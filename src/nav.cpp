#include "nav.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

#include "config.h"
#include "log.h"
#include "realcp.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"

using nlohmann::json;

namespace nav {
namespace {

std::vector<Mission> g_missions;
std::vector<Mission> g_builtin;  // the built-in catalog, for "Reset this mission" and "Default checkpoints"
std::string g_listName = kDefaultList;  // the list in use, shown on the Practice tab
std::mutex g_catMu;              // points of g_missions: the menu edits them, the game thread copies them
std::string g_source = "not loaded";
std::string g_builtinText;

std::mutex g_mu;  // g_snap, g_sel
Snapshot g_snap;
Selection g_sel;

// Game thread only.
struct PosSlot {
  bool ok = false;
  game::Vec loc;
  int rot[3] = {};
  int ctrlRot[3] = {};
};
PosSlot g_slots[kPosSlots];
std::string g_slotMap;  // saved positions live until the map changes

// A load we started: no second one until the new map is up (double press of a bind, impatient clicks).
struct Busy {
  bool on = false;
  unsigned gen = 0;
  ULONGLONG since = 0;
  std::string what;
} g_busy;
constexpr ULONGLONG kBusyTimeoutMs = 90000;

// Streaming volumes after a checkpoint load. The checkpoint data keeps which LevelStreamingVolumes were switched
// off at the last real save (NTCheckpoint.m_VolumeRecords, applied by CheckpointLoadVolumeState): a save from the
// mission start keeps the volumes of later areas off, so a few seconds after we spawn Dante at a later checkpoint
// the streaming hides the room he stands in and he falls through (M2 Study Room). Levels scripts switch volumes on
// while you play; we switch them all on: in the saved records before the load and on the live volumes for a few
// seconds after it.
struct VolumeFix {
  bool pending = false;  // our checkpoint load is on its way
  ULONGLONG since = 0, until = 0, next = 0;
  std::vector<uintptr_t> vols;
} g_vol;
constexpr ULONGLONG kVolumeFixMs = 5000, kVolumeFixMaxMs = 180000;

std::string Lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

const char* kDiff[kDifficulties] = {"Human",           "Devil Hunter",   "Nephilim",     "Son of Sparda",
                                    "Dante Must Die!", "Heaven or Hell", "Hell and Hell"};

// ------------------------------------------------------------------ catalog
std::string BuiltinJson() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&BuiltinJson), &self);
  HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!r) return {};
  HGLOBAL h = LoadResource(self, r);
  const char* p = h ? static_cast<const char*>(LockResource(h)) : nullptr;
  return p ? std::string(p, SizeofResource(self, r)) : std::string();
}

Kind KindOf(const std::string& s) {
  if (s == "start") return Kind::Start;
  if (s == "arena") return Kind::Arena;
  if (s == "cutscene") return Kind::Cutscene;
  return Kind::Checkpoint;
}

// "list_name" of a saved list ("" in old files: then it is the player's own edited list).
std::string ListNameOf(const std::string& text) {
  try {
    json j = json::parse(text, nullptr, true, true);
    std::string n = j.value("list_name", "");
    return n.empty() ? "My checkpoints" : n;
  } catch (...) {
    return "My checkpoints";
  }
}

bool Parse(const std::string& text, std::vector<Mission>& out, std::string& err) {
  try {
    json j = json::parse(text, nullptr, true, true);  // comments allowed: the file is meant to be edited by hand
    for (const json& mj : j.at("missions")) {
      Mission m;
      m.id = mj.value("id", "");
      m.map = mj.at("map").get<std::string>();
      m.name = mj.value("name", m.map);
      m.secret = mj.value("type", "story") == "secret";
      m.index = mj.value("index", -1);
      if (mj.contains("points"))
        for (const json& pj : mj["points"]) {
          Point p;
          p.id = pj.at("id").get<std::string>();
          p.name = pj.value("name", p.id);
          p.kind = KindOf(pj.value("kind", "checkpoint"));
          if (pj.contains("pos") && pj["pos"].is_array() && pj["pos"].size() == 3) {
            p.hasPos = true;
            p.pos = {pj["pos"][0].get<float>(), pj["pos"][1].get<float>(), pj["pos"][2].get<float>()};
          }
          p.yaw = pj.value("yaw", 0);
          p.cp = p.kind == Kind::Start ? pj.value("checkpoint", "") : p.kind == Kind::Checkpoint ? p.id : "";
          m.points.push_back(p);
        }
      // every mission can at least be started
      if (m.points.empty() || m.points[0].kind != Kind::Start) {
        Point s;
        s.id = "start";
        s.name = "Mission start";
        s.kind = Kind::Start;
        s.cp = mj.value("first_checkpoint", "");
        m.points.insert(m.points.begin(), s);
      }
      out.push_back(m);
    }
    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

// ------------------------------------------------------------------ game helpers (game thread)
uintptr_t Engine(const game::World& w) { return w.engine; }
uintptr_t Profile(const game::World& w) { return w.engine ? game::GetObj(w.engine, "GGamerProfileData") : 0; }
uintptr_t CheckpointData(const game::World& w) { return w.engine ? game::GetObj(w.engine, "GCheckPointData") : 0; }

// A live (non-default) object of exactly this class or a subclass; cached while it stays alive.
uintptr_t FindLive(const char* cls, uintptr_t& cache) {
  if (cache && game::Alive(cache)) return cache;
  cache = 0;
  uintptr_t c = ue3::FindObject("Class", cls);
  if (!c) return 0;
  int n = ue3::NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o || ue3::ObjClass(o) == 0) continue;
    uintptr_t oc = ue3::ObjClass(o);
    bool match = oc == c;
    for (uintptr_t s = oc; !match && s; s = ue3::StructSuper(s)) match = s == c;
    if (!match || ue3::ObjName(o).rfind("Default__", 0) == 0) continue;
    cache = o;
    break;
  }
  return cache;
}

bool CallStatic(uintptr_t context, const char* cls, const char* fn, const char* param, int value) {
  uintptr_t f = ue3::FindObject("Function", fn, cls);
  if (!f || !context) return false;
  engine::Call c(context, f);
  return c.Int(param, value).Invoke();
}

void CloseGameMenus() {
  // NTGFxMovieManager.Get().CloseAllIngameUI(): what the pause menu's Restart does first (pause, death screen)
  static uintptr_t mm = 0;
  if (uintptr_t m = FindLive("NTGFxMovieManager", mm)) {
    engine::Call c(m, "CloseAllIngameUI");
    c.Invoke();
  }
}

void ResetSpecialGoldOrbs(const game::World& w) {
  // DVGameInfo.m_ShopSystem.ResetSpecialGoldOrbCount(): the game does it before every mission start / restart
  if (uintptr_t shop = w.gameInfo ? game::GetObj(w.gameInfo, "m_ShopSystem") : 0) {
    engine::Call c(shop, "ResetSpecialGoldOrbCount");
    c.Invoke();
  }
}

std::string FStringAt(uintptr_t a) {
  ue3::TArrayRaw s{};
  if (!util::Read(a, s) || !s.data || s.num <= 0 || s.num > 1024) return {};
  return util::WideToUtf8(util::ReadWString(s.data, s.num));
}

void EnableVolumeRecords(uintptr_t cp) {
  game::Field f = game::FieldOf(cp, "m_VolumeRecords");  // TArray<StreamingVolumeRecord {FName; bool bDisabled}>
  ue3::TArrayRaw r{};
  if (!f.ok() || !util::Read(cp + f.off, r) || !r.data || r.num <= 0 || r.num > 4096) return;
  for (int i = 0; i < r.num; ++i) *reinterpret_cast<uint32_t*>(r.data + i * 0xC + 8) &= ~1u;
}

void EnableVolumes(const game::World& w) {
  if (g_vol.vols.empty()) {
    uintptr_t cls = ue3::FindObject("Class", "LevelStreamingVolume");
    std::string map = Lower(w.map);
    for (int i = 0, n = ue3::NumObjects(); cls && i < n; ++i) {
      uintptr_t o = ue3::Object(i);
      if (o && ue3::ObjClass(o) == cls && Lower(game::TopPackage(o)) == map) g_vol.vols.push_back(o);
    }
    LOG("nav: %zu streaming volumes switched on after the checkpoint load", g_vol.vols.size());
  }
  for (uintptr_t v : g_vol.vols)
    if (game::Alive(v) && game::GetBool(v, "bDisabled")) game::SetBool(v, "bDisabled", false);
}

bool InCutscene(const game::World& w) { return w.pc && game::GetBool(w.pc, "bCinematicMode"); }

int CurrentDifficulty(const game::World& w) {
  uintptr_t p = Profile(w);
  return p ? game::GetInt(p, "m_iCurrentDifficultyMode", -1) : -1;
}

// Starts a map the way the game's own menus do (DVGFxMovieHandler_*_Callbacks.StartMission / LoadMap):
// clear the saved checkpoint, then "open <map>". With a checkpoint name the level starts at that checkpoint.
bool OpenMap(const game::World& w, const Mission& m, const std::string& checkpoint, bool clear, const char* what) {
  uintptr_t cp = CheckpointData(w);
  if (!w.pc || !cp) {
    LOG("nav: %s refused - no player controller / checkpoint object", what);
    return false;
  }
  CloseGameMenus();
  // NTBaseEngine.StopAnyPlayingCutscene(): what IngameMenu_RestartLevel_Deferred does before a restart
  if (uintptr_t f = ue3::FindObject("Function", "StopAnyPlayingCutscene", "NTBaseEngine")) {
    engine::Call c(w.pc, f);
    c.Invoke();
  }
  ResetSpecialGoldOrbs(w);
  if (clear) {
    engine::Call c(cp, "ClearCheckpoint");
    c.Bool("bClearSnapshots", true).Bool("bClearMissionStats", true).Invoke();
  }
  g_vol.pending = !checkpoint.empty();
  if (!checkpoint.empty()) {
    EnableVolumeRecords(cp);
    engine::Call c(cp, "SetCheckpointName");
    c.Str("MapName", util::Utf8ToWide(m.map)).Str("CheckpointName", util::Utf8ToWide(checkpoint)).Invoke();
  }
  engine::Call open(w.pc, "ConsoleCommand");
  open.Str("Command", util::Utf8ToWide("open " + m.map)).Bool("bWriteToLog", true).Invoke();
  g_busy = {true, w.gen, GetTickCount64(), what};
  LOG("nav: %s -> open %s%s%s (difficulty %s)", what, m.map.c_str(), checkpoint.empty() ? "" : ", checkpoint ",
      checkpoint.c_str(), DifficultyName(CurrentDifficulty(w)));
  return true;
}

// Difficulty for the next map: what DVGFxMovieHandler_Frontend_Callbacks.SetMissionAndDifficultyMode does.
// Heaven or Hell / Hell and Hell are modes on top of Son of Sparda (engine level 3).
void SetDifficulty(const game::World& w, int mode, int storyIndex) {
  uintptr_t prof = Profile(w);
  if (!prof) return;
  int level = mode >= 5 ? 3 : mode;
  CallStatic(prof, "NTDifficultyHelpers", "SetDifficultyLevel", "NewLevel", level);
  if (storyIndex >= 0) {
    game::SetInt(prof, "m_iMapNumber", storyIndex);
    game::SetInt(prof, "m_iLastMapNumber", storyIndex);
  }
  game::SetInt(prof, "m_iCurrentDifficultyLevel", level);
  game::SetInt(prof, "m_iCurrentDifficultyMode", mode);
}

struct Rot {
  int pitch = 0, yaw = 0, roll = 0;
};

// Like NTGameInfo_SP.RevertCheckpoint: TeleportToLocationReset + component update + camera points of interest,
// then the camera is snapped behind Dante (NTCam_CameraController.TeleportReset).
void DoTeleport(const game::World& w, const game::Vec& loc, const Rot& rot, const Rot* ctrl) {
  uintptr_t pawn = w.pawn;
  engine::Call t(pawn, "TeleportToLocationReset");
  t.Raw(t.ParamName(0).c_str(), &loc, sizeof loc).Raw(t.ParamName(1).c_str(), &rot, sizeof rot).Invoke();
  engine::Call u(pawn, "ForceUpdateComponents");
  u.Bool("bCollisionUpdate", false).Bool("bTransformOnly", false).Invoke();
  if (uintptr_t poi = game::GetObj(pawn, "m_POIcomponent")) {
    engine::Call c(poi, "TeleportComponent");
    c.Invoke();
  }
  game::Field vel = game::FieldOf(pawn, "Velocity");
  if (vel.ok()) *reinterpret_cast<game::Vec*>(pawn + vel.off) = {};
  Rot cr = ctrl ? *ctrl : Rot{0, rot.yaw, 0};
  game::Field r = game::FieldOf(w.pc, "Rotation");  // Actor.SetRotation is native (not callable through events)
  if (r.ok()) *reinterpret_cast<Rot*>(w.pc + r.off) = cr;
  if (uintptr_t cam = game::GetObj(w.pc, "PlayerCamera"); cam && ue3::IsA(cam, "NTCam_CameraController")) {
    engine::Call c(cam, "TeleportReset");
    c.Invoke();
  }
}

// Game thread: a copy, the menu may reorder the points at the same time.
bool CopyMission(int i, Mission& out) {
  if (i < 0 || i >= (int)g_missions.size()) return false;
  std::lock_guard<std::mutex> lk(g_catMu);
  out = g_missions[i];
  return true;
}

bool CurrentMission(const game::World& w, Mission& out) { return CopyMission(FindMission(w.map), out); }

std::string PointLine(const Point& p) {
  json j = {{"id", p.id}, {"name", p.name}, {"kind", KindName(p.kind)}};
  if (p.kind == Kind::Start && !p.cp.empty()) j["checkpoint"] = p.cp;
  if (p.hasPos) {
    j["pos"] = json::array({p.pos.x, p.pos.y, p.pos.z});
    j["yaw"] = p.yaw;
  }
  return j.dump();
}

// Same layout as tools/missions_build.py: one point per line, easy to read and to merge back.
std::string CatalogTextLocked();

bool SaveUserCatalog() {
  if (g_listName == kDefaultList) g_listName = "My checkpoints";  // an edited default list is the player's own now
  std::string out;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    out = CatalogTextLocked();
  }
  std::wstring path = util::InExeDir(L"DmCPT_missions.json");
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    LOG("nav: cannot write DmCPT_missions.json");
    return false;
  }
  f << out;
  g_source = "DmCPT_missions.json";
  return true;
}

std::string CatalogTextLocked() {
  std::string out = "{\n  \"version\": 1,\n  \"list_name\": " + json(g_listName).dump() +
                    ",\n  \"_help\": \"Edited in the game (Practice tab). Delete this file to go back to the built-in "
                    "list.\",\n  \"missions\": [\n";
  {
    for (size_t mi = 0; mi < g_missions.size(); ++mi) {
      const Mission& m = g_missions[mi];
      json head = {{"id", m.id}, {"map", m.map}, {"name", m.name}, {"type", m.secret ? "secret" : "story"},
                   {"index", m.index}};
      std::string h = head.dump();
      h.pop_back();
      out += "    " + h + ", \"points\": [\n";
      for (size_t pi = 0; pi < m.points.size(); ++pi)
        out += "      " + PointLine(m.points[pi]) + (pi + 1 < m.points.size() ? ",\n" : "\n");
      out += std::string("    ]}") + (mi + 1 < g_missions.size() ? ",\n" : "\n");
    }
  }
  out += "  ]\n}\n";
  return out;
}

std::string BlockedNow(Action a, const game::World* w, int slot) {
  Snapshot s;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    s = g_snap;
  }
  if (!FeaturesAllowed()) return "unknown game version";
  if (!s.ready) return "starting up";
  if (s.busy || realcp::Busy()) return "loading";
  if (a == Action::MissionSelect) return s.frontEnd || s.mission ? "" : "loading";
  if (!s.mission) return s.frontEnd ? "in a mission only" : "loading";
  // Loads may stop a cutscene (the pause menu's Restart does the same); moving Dante inside one may not.
  bool moves = a == Action::Teleport || a == Action::SavePos || a == Action::LoadPos;
  if (moves && s.cutscene) return "not during a cutscene";
  if (a == Action::Keep) return s.missionIndex < 0 ? "not a story mission" : "";
  if ((a == Action::Restart || a == Action::LoadCheckpoint || a == Action::Teleport) && s.missionIndex < 0)
    return "not a story or secret mission";
  if (a == Action::LoadPos && (slot < 0 || slot >= kPosSlots || !s.slot[slot])) return "nothing saved in this slot";
  (void)w;
  return "";
}

bool Refuse(Action a, const char* what, int slot = 0) {
  // game thread: a load started earlier in this same frame is not in the snapshot yet (two presses in one frame)
  std::string why = g_busy.on && a != Action::SavePos ? std::string("loading") : BlockedNow(a, nullptr, slot);
  if (why.empty()) return false;
  LOG("nav: %s ignored (%s)", what, why.c_str());
  return true;
}

// ------------------------------------------------------------------ crawl helpers (game thread)
json Vec3(const game::Vec& v) { return json::array({std::round(v.x), std::round(v.y), std::round(v.z)}); }

bool ActorPlace(uintptr_t a, game::Vec& loc, Rot& rot) {
  game::Field l = game::FieldOf(a, "Location"), r = game::FieldOf(a, "Rotation");
  if (!l.ok() || !r.ok()) return false;
  loc = util::ReadOr<game::Vec>(a + l.off);
  rot = util::ReadOr<Rot>(a + r.off);
  return true;
}

struct LiveCp {
  std::string name, friendly, object;
  bool notReal = false, hasPos = false;
  game::Vec pos;
  int yaw = 0;
  int objX = 0, objY = 0;
};

std::vector<LiveCp> GatherCheckpoints(const game::World& w) {
  std::vector<LiveCp> out;
  uintptr_t cls = ue3::FindObject("Class", "SeqAct_NT_SetCheckPoint");
  if (!cls || w.map.empty()) return out;
  std::string map = Lower(w.map);
  int n = ue3::NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o || ue3::ObjClass(o) != cls || Lower(game::TopPackage(o)) != map) continue;
    LiveCp c;
    c.object = ue3::ObjName(o);
    game::Field nf = game::FieldOf(o, "nCheckpointName");
    if (nf.ok()) c.name = ue3::ReadFName(o + nf.off);
    c.notReal = game::GetBool(o, "NotARealCheckpoint");
    c.objX = game::GetInt(o, "ObjPosX");
    c.objY = game::GetInt(o, "ObjPosY");
    {
      engine::Call f(o, "GetFriendlyCheckpointName");
      if (f.Invoke() && f.RetData()) {
        ue3::FNameRaw fn;
        memcpy(&fn, f.RetData(), sizeof fn);
        c.friendly = ue3::NameOf(fn);
      }
    }
    engine::Call ps(o, "GetPlayerStart");
    if (ps.Invoke()) {
      uintptr_t st = (uintptr_t)(uint32_t)ps.RetInt();
      Rot r;
      if (st && ue3::IsObject(st) && ActorPlace(st, c.pos, r)) {
        c.hasPos = true;
        c.yaw = r.yaw;
      }
    }
    out.push_back(c);
  }
  return out;
}

json CrawlMission(const game::World& w, const Mission& m) {
  json j = {{"id", m.id}, {"map", m.map}, {"name", m.name}, {"type", m.secret ? "secret" : "story"},
            {"index", m.index}};
  std::string first;
  if (w.world) {
    uintptr_t mi = game::GetObj(w.world, "MyMapInfo");
    game::Field f = mi ? game::FieldOf(mi, "m_FirstCheckpointName") : game::Field{};
    if (f.ok()) first = FStringAt(mi + f.off);
  }
  j["first_checkpoint"] = first;
  game::Vec start;
  Rot sr;
  if (w.pawn && ActorPlace(w.pawn, start, sr)) {
    j["spawn"] = Vec3(start);
    j["spawn_yaw"] = sr.yaw & 0xFFFF;
  }
  json cps = json::array();
  for (const LiveCp& c : GatherCheckpoints(w)) {
    json cj = {{"name", c.name}, {"friendly", c.friendly}, {"object", c.object}, {"not_real", c.notReal},
               {"obj_pos", json::array({c.objX, c.objY})}};
    if (c.hasPos) {
      cj["pos"] = Vec3(c.pos);
      cj["yaw"] = c.yaw & 0xFFFF;
    }
    cps.push_back(cj);
  }
  j["checkpoints"] = cps;
  // arenas and cutscenes of the level script (research for "before arena N" points)
  int enc = 0, cine = 0;
  uintptr_t ce = ue3::FindObject("Class", "DVSeqAct_BeginEncounter"), cc = ue3::FindObject("Class", "DVSeqAct_PlayCinematicAndMatinee");
  std::string map = Lower(w.map);
  for (int i = 0, n = ue3::NumObjects(); i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o) continue;
    uintptr_t oc = ue3::ObjClass(o);
    if ((oc == ce || oc == cc) && Lower(game::TopPackage(o)) == map) (oc == ce ? enc : cine)++;
  }
  j["encounters"] = enc;
  j["cinematics"] = cine;
  return j;
}

}  // namespace

// ------------------------------------------------------------------ public
const char* KindName(Kind k) {
  switch (k) {
    case Kind::Start: return "start";
    case Kind::Checkpoint: return "checkpoint";
    case Kind::Arena: return "arena";
    case Kind::Cutscene: return "cutscene";
  }
  return "?";
}

const char* DifficultyName(int mode) { return mode >= 0 && mode < kDifficulties ? kDiff[mode] : "?"; }

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  g_builtinText = BuiltinJson();
  std::wstring user = util::InExeDir(L"DmCPT_missions.json");
  std::string err;
  std::ifstream f(user, std::ios::binary);
  if (f) {
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<Mission> m;
    if (Parse(ss.str(), m, err)) {
      g_missions = m;
      g_source = "DmCPT_missions.json";
      g_listName = ListNameOf(ss.str());
    } else {
      LOG("nav: DmCPT_missions.json not used: %s", err.c_str());
      g_source = "built in (DmCPT_missions.json has an error: " + err + ")";
    }
  }
  Parse(g_builtinText, g_builtin, err);
  if (g_missions.empty()) {
    if (Parse(g_builtinText, g_missions, err)) {
      if (g_source == "not loaded") g_source = "built in";
    } else {
      g_source = "built-in catalog broken: " + err;
    }
  }
  // the mission start's checkpoint name (older edited catalogs do not have it): from the built-in one
  for (Mission& m : g_missions)
    for (const Mission& b : g_builtin)
      if (Lower(b.map) == Lower(m.map) && !m.points.empty() && m.points[0].kind == Kind::Start && m.points[0].cp.empty() &&
          !b.points.empty())
        m.points[0].cp = b.points[0].cp;
  realcp::Refresh();
  size_t pts = 0;
  for (const Mission& m : g_missions) pts += m.points.size();
  LOG("nav: catalog %s - %zu missions, %zu points", g_source.c_str(), g_missions.size(), pts);
}

const std::vector<Mission>& Missions() { return g_missions; }

std::string CatalogText() {
  std::lock_guard<std::mutex> lk(g_catMu);
  return CatalogTextLocked();
}

std::string ListName() { return g_listName; }

bool UseBuiltin() {
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    g_missions = g_builtin;
  }
  DeleteFileW(util::InExeDir(L"DmCPT_missions.json").c_str());
  g_source = "built in";
  g_listName = kDefaultList;
  LOG("nav: back to the built-in checkpoint list");
  return true;
}

bool ReplaceCatalog(const std::string& text, std::string* err) {
  std::vector<Mission> m;
  std::string e;
  if (!Parse(text, m, e) || m.empty()) {
    if (err) *err = e.empty() ? "no missions in the list" : e;
    return false;
  }
  for (Mission& x : m)  // the mission start's checkpoint name, as Init does
    for (const Mission& b : g_builtin)
      if (Lower(b.map) == Lower(x.map) && !x.points.empty() && x.points[0].kind == Kind::Start && x.points[0].cp.empty() &&
          !b.points.empty())
        x.points[0].cp = b.points[0].cp;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    g_missions = m;
  }
  g_listName = ListNameOf(text);
  LOG("nav: catalog replaced by '%s' (%zu missions)", g_listName.c_str(), m.size());
  return SaveUserCatalog();
}

int FindMission(const std::string& map) {
  std::lock_guard<std::mutex> lk(g_catMu);
  std::string m = Lower(map);
  for (size_t i = 0; i < g_missions.size(); ++i)
    if (Lower(g_missions[i].map) == m) return (int)i;
  return -1;
}

std::string CatalogSource() { return g_source; }

bool ExportCatalog(std::string* msg) {
  std::wstring path = util::InExeDir(L"DmCPT_missions.json");
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (msg) *msg = "DmCPT_missions.json already exists (delete it to get the built-in one again)";
    return false;
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f || g_builtinText.empty()) {
    if (msg) *msg = "cannot write DmCPT_missions.json";
    return false;
  }
  f << g_builtinText;
  if (msg) *msg = "written next to the game exe: edit the names, restart the game";
  LOG("nav: built-in catalog exported to DmCPT_missions.json");
  return true;
}

bool MovePoint(int mission, int point, int dir) {
  if (mission < 0 || mission >= (int)g_missions.size()) return false;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    std::vector<Point>& p = g_missions[mission].points;
    int to = point + dir;
    if (point < 1 || to < 1 || point >= (int)p.size() || to >= (int)p.size()) return false;
    std::swap(p[point], p[to]);
    LOG("nav: %s: '%s' moved to place %d", g_missions[mission].id.c_str(), p[to].id.c_str(), to + 1);
  }
  return SaveUserCatalog();
}

bool DeletePoint(int mission, int point) {
  if (mission < 0 || mission >= (int)g_missions.size()) return false;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    std::vector<Point>& p = g_missions[mission].points;
    if (point < 1 || point >= (int)p.size()) return false;
    LOG("nav: %s: '%s' deleted", g_missions[mission].id.c_str(), p[point].id.c_str());
    p.erase(p.begin() + point);
  }
  return SaveUserCatalog();
}

int FindPoint(int mission, const std::string& checkpoint) {
  if (mission < 0 || mission >= (int)g_missions.size()) return -1;
  const std::vector<Point>& p = g_missions[mission].points;
  for (size_t i = 0; i < p.size(); ++i)
    if (p[i].cp == checkpoint) return (int)i;
  return -1;
}

int AddPoint(int mission, const Point& pt, int after) {
  if (mission < 0 || mission >= (int)g_missions.size()) return -1;
  int at;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    std::vector<Point>& p = g_missions[mission].points;
    at = std::clamp(after + 1, 1, (int)p.size());
    p.insert(p.begin() + at, pt);
    LOG("nav: %s: '%s' added at place %d", g_missions[mission].id.c_str(), pt.id.c_str(), at + 1);
  }
  SaveUserCatalog();
  return at;
}

std::string NiceName(const std::string& id) {
  // the same rules as tools/missions_build.py (short version): drop the "c" prefix, split words and numbers
  std::string s = id, extra;
  if (size_t b = s.find('('); b != std::string::npos) {
    extra = " " + s.substr(b);
    s = s.substr(0, b);
  }
  if (s.size() > 1 && s[0] == 'c' && (isupper((unsigned char)s[1]) || isdigit((unsigned char)s[1]))) s = s.substr(1);
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i], prev = i ? s[i - 1] : ' ';
    if (c == '_') c = ' ';
    bool split = i && c != ' ' && prev != ' ' && prev != '_' &&
                 ((isupper((unsigned char)c) && islower((unsigned char)prev)) ||
                  (isdigit((unsigned char)c) && isalpha((unsigned char)prev)) ||
                  (isalpha((unsigned char)c) && isdigit((unsigned char)prev)));
    if (split) out += ' ';
    out += (out.empty() || out.back() == ' ') ? (char)toupper((unsigned char)c) : c;
  }
  return out + extra;
}

bool RenamePoint(int mission, int point, const std::string& name) {
  if (mission < 0 || mission >= (int)g_missions.size() || name.empty()) return false;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    std::vector<Point>& p = g_missions[mission].points;
    if (point < 0 || point >= (int)p.size()) return false;
    LOG("nav: %s: '%s' renamed to '%s'", g_missions[mission].id.c_str(), p[point].id.c_str(), name.c_str());
    p[point].name = name;
  }
  return SaveUserCatalog();
}

bool RenameMission(int mission, const std::string& name) {
  if (mission < 0 || mission >= (int)g_missions.size() || name.empty()) return false;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    LOG("nav: %s renamed to '%s'", g_missions[mission].id.c_str(), name.c_str());
    g_missions[mission].name = name;
  }
  return SaveUserCatalog();
}

bool ResetMission(int mission) {
  if (mission < 0 || mission >= (int)g_missions.size()) return false;
  {
    std::lock_guard<std::mutex> lk(g_catMu);
    for (const Mission& b : g_builtin)
      if (Lower(b.map) == Lower(g_missions[mission].map)) {
        g_missions[mission].points = b.points;
        g_missions[mission].name = b.name;
      }
    LOG("nav: %s: points back to the built-in list", g_missions[mission].id.c_str());
  }
  return SaveUserCatalog();
}

bool Edited(int mission) {
  if (mission < 0 || mission >= (int)g_missions.size()) return false;
  for (const Mission& b : g_builtin) {
    if (Lower(b.map) != Lower(g_missions[mission].map)) continue;
    const std::vector<Point>& a = g_missions[mission].points;
    if (a.size() != b.points.size() || g_missions[mission].name != b.name) return true;
    for (size_t i = 0; i < a.size(); ++i)
      if (a[i].id != b.points[i].id || a[i].name != b.points[i].name) return true;
    return false;
  }
  return false;
}

Snapshot Now() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_snap;
}

std::string Blocked(Action a, int slot) { return BlockedNow(a, nullptr, slot); }

Selection GetSel() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_sel;
}

void SetSel(const Selection& s) {
  bool changed;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    changed = s.point != g_sel.point || s.pointMap != g_sel.pointMap || s.selMission != g_sel.selMission ||
              s.selDifficulty != g_sel.selDifficulty || s.selPoint != g_sel.selPoint || s.slot != g_sel.slot ||
              s.useReal != g_sel.useReal;
    g_sel = s;
  }
  if (changed) MarkConfigDirty();
}

int PointFor(const Selection& s, const std::string& map) { return Lower(s.pointMap) == Lower(map) ? s.point : 0; }

void LoadJson(const json& j) {
  if (!j.is_object()) return;
  Selection s;
  s.point = j.value("point", 0);
  s.pointMap = j.value("point_map", "");
  s.selMission = j.value("mission", 0);
  s.selDifficulty = std::clamp(j.value("difficulty", 0), 0, kDifficulties - 1);
  s.selPoint = j.value("mission_point", 0);
  s.slot = std::clamp(j.value("slot", 0), 0, kPosSlots - 1);
  s.useReal = j.value("use_real", true);
  std::lock_guard<std::mutex> lk(g_mu);
  g_sel = s;
}

json SaveJson() {
  Selection s = GetSel();
  return {{"point", s.point},           {"point_map", s.pointMap},       {"mission", s.selMission},
          {"difficulty", s.selDifficulty}, {"mission_point", s.selPoint}, {"slot", s.slot},
          {"use_real", s.useReal}};
}

void OnFrame(const game::World& w) {
  game::Status st = game::CurrentStatus();
  if (g_busy.on) {
    bool arrived = w.gen != g_busy.gen && w.mission;
    if (arrived || GetTickCount64() - g_busy.since > kBusyTimeoutMs) {
      LOG("nav: %s finished%s (%s)", g_busy.what.c_str(), arrived ? "" : " - TIMEOUT", w.map.c_str());
      g_busy.on = false;
      if (arrived && g_vol.pending) {
        g_vol.since = GetTickCount64();
        g_vol.until = g_vol.since + kVolumeFixMs;
        g_vol.vols.clear();
      }
      g_vol.pending = false;
    }
  }
  if (g_vol.until) {
    ULONGLONG now = GetTickCount64();
    // A checkpoint may start with a cutscene (M2 Grand Foyer Fight Won): its end switches streaming again, so
    // the window runs until 5 s after the last cutscene frame (at most 3 minutes after the load).
    if (InCutscene(w) && now - g_vol.since < kVolumeFixMaxMs) g_vol.until = std::max(g_vol.until, now + kVolumeFixMs);
    if (now > g_vol.until || !w.mission) {
      LOG("nav: streaming volume fix done");
      g_vol.until = 0;
      g_vol.vols.clear();
    } else if (now >= g_vol.next) {
      EnableVolumes(w);
      g_vol.next = now + 100;
    }
  }
  if (Lower(w.map) != Lower(g_slotMap) && !w.map.empty()) {
    for (PosSlot& s : g_slots) s = {};
    g_slotMap = w.map;
  }
  Snapshot s;
  s.ready = st.ready;
  s.mission = w.mission;
  s.frontEnd = w.frontEnd && st.ready;
  s.cutscene = w.mission && InCutscene(w);
  s.busy = g_busy.on || realcp::Busy();
  s.map = w.map;
  s.missionIndex = w.mission ? FindMission(w.map) : -1;
  s.difficulty = st.ready ? CurrentDifficulty(w) : -1;
  for (int i = 0; i < kPosSlots; ++i) s.slot[i] = g_slots[i].ok;
  std::lock_guard<std::mutex> lk(g_mu);
  g_snap = s;
}

void RestartMission(const game::World& w) {
  if (Refuse(Action::Restart, "restart mission")) return;
  Mission mm;
  if (!CurrentMission(w, mm)) return;
  const Mission* m = &mm;
  OpenMap(w, *m, "", true, "restart mission");
}

void LoadPoint(const game::World& w, int point) {
  if (Refuse(Action::LoadCheckpoint, "load checkpoint")) return;
  Mission mm;
  if (!CurrentMission(w, mm)) return;
  const Mission* m = &mm;
  if (point < 0 || point >= (int)m->points.size() || !m->points[point].Loadable()) {
    LOG("nav: load checkpoint ignored (point %d is not a checkpoint of %s)", point, m->map.c_str());
    return;
  }
  const Point& p = m->points[point];
  if (GetSel().useReal && !p.cp.empty() && realcp::Find(m->map, p.cp)) {  // a real save of this point: Continue
    LOG("nav: load checkpoint %s from the kept real save", p.cp.c_str());
    realcp::Load(m->map, p.cp);
    return;
  }
  OpenMap(w, *m, p.kind == Kind::Start ? "" : p.id, true, ("load checkpoint " + p.id).c_str());
}

void MissionSelect(const game::World& w, int mission, int difficulty, int point) {
  if (Refuse(Action::MissionSelect, "mission select")) return;
  Mission m;
  if (!CopyMission(mission, m)) return;
  if (point < 0 || point >= (int)m.points.size() || !m.points[point].Loadable()) point = 0;
  if (const Point& rp = m.points[point]; GetSel().useReal && !rp.cp.empty() && realcp::Find(m.map, rp.cp)) {
    LOG("nav: mission select %s %s from the kept real save (its own difficulty)", m.id.c_str(), rp.cp.c_str());
    realcp::Load(m.map, rp.cp);
    return;
  }
  if (m.secret) {
    // DVGFxMovieHandler_Frontend_Callbacks.StartSecretMission: the difficulty comes with the secret mission and
    // the story checkpoint is kept (LoadMap(map, false)).
    uintptr_t smm = w.gameInfo ? game::GetObj(w.gameInfo, "m_SecretMissionsManager") : 0;
    if (smm && ue3::FindNameIndex(m.id.c_str()) >= 0) {
      engine::Call c(smm, "GetMissionDifficulty");
      if (c.Name(c.ParamName(0).c_str(), m.id.c_str()).Invoke())
        CallStatic(Profile(w), "NTDifficultyHelpers", "SetDifficultyLevel", "NewLevel", c.RetInt());
    } else {
      LOG("nav: mission select: no secret mission manager here, difficulty unchanged");
    }
  } else {
    SetDifficulty(w, std::clamp(difficulty, 0, kDifficulties - 1), m.index);
  }
  const Point& p = m.points[point];
  OpenMap(w, m, p.kind == Kind::Start ? "" : p.id, !m.secret, ("mission select " + m.id).c_str());
}

void Teleport(const game::World& w, int point) {
  if (Refuse(Action::Teleport, "teleport")) return;
  Mission mm;
  if (!CurrentMission(w, mm)) return;
  const Mission* m = &mm;
  if (point < 0 || point >= (int)m->points.size() || !m->points[point].hasPos) {
    LOG("nav: teleport ignored (no position for point %d)", point);
    return;
  }
  const Point& p = m->points[point];
  DoTeleport(w, p.pos, Rot{0, p.yaw, 0}, nullptr);
  LOG("nav: teleport to %s (%.0f %.0f %.0f)", p.id.c_str(), p.pos.x, p.pos.y, p.pos.z);
}

void SavePos(const game::World& w, int slot) {
  if (Refuse(Action::SavePos, "save position") || slot < 0 || slot >= kPosSlots) return;
  PosSlot s;
  Rot r;
  if (!ActorPlace(w.pawn, s.loc, r)) return;
  memcpy(s.rot, &r, sizeof r);
  game::Field cr = game::FieldOf(w.pc, "Rotation");
  if (cr.ok()) util::SafeRead(w.pc + cr.off, s.ctrlRot, sizeof s.ctrlRot);
  s.ok = true;
  g_slots[slot] = s;
  g_slotMap = w.map;
  LOG("nav: position %d saved (%.0f %.0f %.0f)", slot + 1, s.loc.x, s.loc.y, s.loc.z);
}

void LoadPos(const game::World& w, int slot) {
  if (Refuse(Action::LoadPos, "load position", slot)) return;
  const PosSlot& s = g_slots[slot];
  Rot r, c;
  memcpy(&r, s.rot, sizeof r);
  memcpy(&c, s.ctrlRot, sizeof c);
  DoTeleport(w, s.loc, r, &c);
  LOG("nav: position %d loaded", slot + 1);
}

bool SavedPosition(int slot, game::Vec* loc, int* yaw) {
  if (slot < 0 || slot >= kPosSlots || !g_slots[slot].ok) return false;
  *loc = g_slots[slot].loc;
  *yaw = g_slots[slot].rot[1];
  return true;
}

std::vector<std::string> LiveCheckpoints() {
  std::vector<std::string> out;
  for (const LiveCp& c : GatherCheckpoints(game::W())) {
    char b[256];
    snprintf(b, sizeof b, "%s friendly '%s' %s%s pos %s yaw %d obj %s (%d,%d)", c.name.c_str(), c.friendly.c_str(),
             c.notReal ? "NOT-REAL " : "", c.hasPos ? "" : "no-start", c.hasPos ? Vec3(c.pos).dump().c_str() : "-",
             c.yaw & 0xFFFF, c.object.c_str(), c.objX, c.objY);
    out.push_back(b);
  }
  return out;
}

void Crawl(int first, int last) {
  if (g_missions.empty()) return;
  first = std::max(0, first);
  last = std::min((int)g_missions.size() - 1, last < 0 ? (int)g_missions.size() - 1 : last);
  std::wstring path = util::InExeDir(L"DmCPT_dumps\\missions_crawl.json");
  json all = json::array();
  {
    std::ifstream f(path);  // a crawl in parts adds to the previous result
    if (f) try {
        all = json::parse(f);
      } catch (...) {
        all = json::array();
      }
  }
  for (int i = first; i <= last; ++i) {
    Mission m;
    CopyMission(i, m);
    unsigned gen0 = engine::OnGame<unsigned>([] { return game::W().gen; });
    bool ok = engine::OnGame<bool>([&m] {
      const game::World& w = game::W();
      g_busy.on = false;  // the crawl waits on its own
      return OpenMap(w, m, "", !m.secret, ("crawl " + m.id).c_str());
    });
    if (!ok) continue;
    bool there = false;
    for (int t = 0; t < 480 && !there; ++t) {
      Sleep(250);
      there = engine::OnGame<bool>([&m, gen0] {
        const game::World& w = game::W();
        return w.gen != gen0 && w.mission && Lower(w.map) == Lower(m.map);
      }, 2000);
    }
    if (!there) {
      LOG("nav: crawl %s: map did not come up", m.map.c_str());
      continue;
    }
    Sleep(3000);
    json j = engine::OnGame<json>([&m] { return CrawlMission(game::W(), m); });
    LOG("nav: crawl %s: %zu checkpoints, first '%s'", m.map.c_str(), j.value("checkpoints", json::array()).size(),
        j.value("first_checkpoint", "").c_str());
    for (auto it = all.begin(); it != all.end();)
      it = it->value("map", "") == m.map ? all.erase(it) : std::next(it);
    all.push_back(j);
    std::ofstream f(path, std::ios::trunc);
    f << all.dump(1);
  }
  LOG("nav: crawl done -> DmCPT_dumps\\missions_crawl.json");
}

}  // namespace nav
