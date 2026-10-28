// Enemy spawn / kill all / despawn / health labels. Research: research/enemies.md.
#include "enemies.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

#include "log.h"
#include "nav.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace enemies {
namespace {

using game::GetBool;
using game::GetFloat;
using game::GetObj;
using game::SetBool;

// What players call the game's enemy ids (the game's own names, DevilGame/Localization/INT/GFxUI.int
// "NewEnemy_*"), in the order the menu lists them. Ids the game has that are missing here are added at the end
// under their raw name.
struct KnownName {
  const char* id;
  const char* name;
};
const KnownName kNames[] = {
    {"WarriorDroneWeak", "Lesser Stygian"},
    {"WarriorDrone", "Stygian"},
    {"WarriorDroneBasic", "Stygian (basic)"},
    {"WarriorDroneHeavy", "Elite Stygian"},
    {"FlyingDrone", "Harpy"},
    {"StingerDroneNoShield", "Pathos"},
    {"StingerDrone", "Shielded Pathos"},
    {"GrenadeDroneNoShield", "Bathos"},
    {"GrenadeDrone", "Shielded Bathos"},
    {"ChainsawDrone", "Ravager"},
    {"ChainsawDroneNoShield", "Ravager (no shield)"},
    {"Summoner", "Witch"},
    {"Rage", "Rage"},
    {"BabyRage", "Rage Spawn"},
    {"AngelRage", "Ghost Rage"},
    {"DemonRage", "Blood Rage"},
    {"TankDrone", "Tyrant"},
    {"BuzzsawTank", "Butcher"},
    {"ShieldDrone", "Death Knight"},
    {"BlueShieldDrone", "Frost Knight"},
    {"RedShieldDrone", "Hell Knight"},
    {"DreamRunner", "Dreamrunner"},
    {"DreamRunnerBoss", "Drekavac"},
};

constexpr int kMaxCount = 10;
constexpr int kMaxAlive = 40;            // never more enemies than this at once from our spawns
constexpr ULONGLONG kLoadTimeoutMs = 20000;

std::mutex g_mu;  // g_snap, g_sel
Snapshot g_snap;
Selection g_sel;
std::mutex g_hpMu;
HpView g_hp;
std::atomic<bool> g_showHp{false};

// ---- game thread
std::vector<Kind> g_kinds;
uintptr_t g_waveClass = 0;
struct Found {
  uintptr_t tmpl = 0;   // NTCharacterTemplate, checked
};
std::map<std::string, Found> g_templates;           // kind id -> template in memory now
std::map<std::string, std::string> g_tmplToKind;    // template object name -> kind id (pawns carry a copy)
std::vector<Avail> g_avail;
unsigned g_scanGen = ~0u;
ULONGLONG g_nextScan = 0;
struct Spawned {
  uintptr_t pawn;
  int index;  // GObjects index, a recycled address is not ours
};
std::vector<Spawned> g_spawned;
unsigned g_spawnGen = ~0u;
struct Pending {
  bool on = false;
  std::string kind;
  int count = 0;
  game::Vec center;
  int faceYaw = 0;
  bool faceDante = true;
  ULONGLONG deadline = 0, readyAt = 0;
} g_pending;
std::string g_msg;
std::unordered_map<uintptr_t, bool> g_pawnClass;  // class -> is an NTPawn
std::vector<std::string> g_failed;                // kinds that did not load on this map

// Boss health bar from the level script (DVSeqAct_BossHealthBar.Activated). Game thread.
std::atomic<uintptr_t> g_fnBossBar{0};
struct ScriptBar {
  float cur = 0, max = 0;
  ULONGLONG changed = 0;
  unsigned gen = ~0u;
} g_scriptBar;
struct PhaseBar {
  uintptr_t ctrl = 0;
  float cur = -1;
  ULONGLONG changed = 0;
} g_phaseBar;

const Kind* FindKind(const std::string& id) {
  for (const Kind& k : g_kinds)
    if (k.id == id) return &k;
  return nullptr;
}

void SetMsg(const std::string& m) {
  g_msg = m;
  LOG("enemies: %s", m.c_str());
}

// The game's list: Params.EnemyEncounterParams.m_EnemyDataParams (struct EnemyDataParams, 0x2C bytes:
// +0x00 EnemyName, +0x08 Level, ...). Read once, it is a package object that lives for the whole session.
void LoadKinds() {
  if (!g_kinds.empty()) return;
  uintptr_t params = ue3::FindObject("DVEnemyEncounterParameters", "EnemyEncounterParams", "Params");
  if (!params) return;
  game::Field f = game::FieldOf(params, "m_EnemyDataParams");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(params + f.off, a) || a.num <= 0 || a.num > 200 || !a.data) return;
  constexpr int kElem = 0x2C;
  std::vector<Kind> raw;
  for (int i = 0; i < a.num; ++i) {
    Kind k;
    k.id = ue3::ReadFName(a.data + i * kElem);
    k.level = ue3::ReadFName(a.data + i * kElem + 8);
    if (k.id.empty() || k.id == "None") continue;
    raw.push_back(k);
  }
  for (const KnownName& n : kNames)
    for (Kind& k : raw)
      if (k.id == n.id) {
        k.name = n.name;
        g_kinds.push_back(k);
      }
  for (Kind& k : raw)
    if (k.name.empty()) {
      k.name = k.id;
      g_kinds.push_back(k);
    }
  g_waveClass = ue3::FindObject("Class", "DVSeqVar_EnemyWaveData");
  LOG("enemies: %zu kinds from %s, wave data class %s", g_kinds.size(), ue3::PathName(params).c_str(),
      util::Hex(g_waveClass).c_str());
}

// A template we may pass to DVEnemyManager.SpawnEnemy: a live NTCharacterTemplate with a pawn class (an empty
// template crashes the game).
bool GoodTemplate(uintptr_t t) {
  return t && ue3::IsObject(t) && game::Alive(t) && ue3::IsA(t, "NTCharacterTemplate") && GetObj(t, "PawnClass");
}

// Templates in memory: every live DVSeqVar_EnemyWaveData (Kismet variable of the loaded levels) names its enemy.
void ScanTemplates() {
  g_templates.clear();
  if (!g_waveClass) return;
  int n = ue3::NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o || ue3::ObjClass(o) != g_waveClass) continue;
    std::string nm = ue3::ObjName(o);
    if (nm.rfind("Default__", 0) == 0) continue;
    std::string id = game::GetName(o, "EnemyCharacterName");
    uintptr_t t = GetObj(o, "EnemyCharacterTemplate");
    if (id.empty() || !GoodTemplate(t) || g_templates.count(id)) continue;
    g_templates[id].tmpl = t;
    g_tmplToKind[ue3::ObjName(t)] = id;
  }
}

// Spawn levels this map can stream (WorldInfo.StreamingLevels[].PackageName).
std::vector<std::string> StreamingLevels(const game::World& w) {
  std::vector<std::string> out;
  game::Field f = game::FieldOf(w.world, "StreamingLevels");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(w.world + f.off, a) || a.num <= 0 || a.num > 4096 || !a.data) return out;
  for (int i = 0; i < a.num; ++i) {
    uintptr_t ls = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (ls && ue3::IsObject(ls)) out.push_back(game::GetName(ls, "PackageName"));
  }
  return out;
}

void Rescan(const game::World& w) {
  LoadKinds();
  ScanTemplates();
  g_avail.assign(g_kinds.size(), Avail::Unknown);
  for (size_t i = 0; i < g_kinds.size(); ++i) {
    const Kind& k = g_kinds[i];
    if (!w.mission) continue;
    // DVSeqAct_EnemyLevelLoader.ActivateLevels adds the spawn level to the world's streaming list itself, so any
    // enemy can be loaded on any map; only one that failed to load here is shown as not available
    if (g_templates.count(k.id))
      g_avail[i] = Avail::Ready;
    else
      g_avail[i] = std::find(g_failed.begin(), g_failed.end(), k.id) != g_failed.end() ? Avail::Missing : Avail::Loadable;
  }
}

Avail AvailOf(const std::string& id) {
  for (size_t i = 0; i < g_kinds.size() && i < g_avail.size(); ++i)
    if (g_kinds[i].id == id) return g_avail[i];
  return Avail::Unknown;
}

int ToUnrealAngle(float rad) { return (int)std::lround(rad * 65536.0f / 6.2831853f) & 0xFFFF; }
float FromUnrealAngle(int a) { return (a & 0xFFFF) * 6.2831853f / 65536.0f; }

bool ActorLoc(uintptr_t a, game::Vec& v) {
  game::Field f = game::FieldOf(a, "Location");
  return f.ok() && util::Read(a + f.off, v);
}

bool CameraPov(const game::World& w, game::Vec& loc, int rot[3], float& fov) {
  game::Pov p;
  if (!game::CameraPov(w, p)) return false;
  loc = p.loc;
  for (int i = 0; i < 3; ++i) rot[i] = p.rot[i];
  fov = p.fov;
  return true;
}

void PruneSpawned(const game::World& w) {
  if (w.gen != g_spawnGen) {  // another map / checkpoint: those enemies are gone with the old world
    g_spawned.clear();
    g_spawnGen = w.gen;
  }
  g_spawned.erase(std::remove_if(g_spawned.begin(), g_spawned.end(),
                                 [](const Spawned& s) {
                                   return !game::Alive(s.pawn) || ue3::ObjIndexOf(s.pawn) != s.index ||
                                          GetBool(s.pawn, "m_IsDestroyed");
                                 }),
                  g_spawned.end());
}

int DoSpawn(const game::World& w, const Pending& p) {
  auto it = g_templates.find(p.kind);
  if (it == g_templates.end() || !GoodTemplate(it->second.tmpl)) return -1;
  uintptr_t em = GetObj(w.gameInfo, "m_EnemyManager");
  if (!em) return -1;
  game::Vec dante{};
  ActorLoc(w.pawn, dante);
  int made = 0;
  int alive = (int)ActiveEnemies(w).size();
  for (int i = 0; i < p.count && alive < kMaxAlive; ++i) {
    // a row across the view, a second row behind it from the 6th enemy on
    int row = i / 5, col = i % 5, inRow = std::min(5, p.count - row * 5);
    float side = (col - (inRow - 1) * 0.5f) * 170.0f, back = row * 220.0f;
    float yawRad = FromUnrealAngle(p.faceYaw);  // the direction the row faces away from
    float fx = std::cos(yawRad), fy = std::sin(yawRad);
    game::Vec pos{p.center.x - fy * side + fx * back, p.center.y + fx * side + fy * back, p.center.z + 40.0f};
    int rot[3] = {0, 0, 0};
    rot[1] = p.faceDante ? ToUnrealAngle(std::atan2(dante.y - pos.y, dante.x - pos.x)) : p.faceYaw;
    engine::Call c(em, "SpawnEnemy");
    if (!c.Valid()) return made;
    c.Obj(c.ParamName(0).c_str(), it->second.tmpl)
        .Raw(c.ParamName(1).c_str(), &pos, sizeof pos)
        .Raw(c.ParamName(2).c_str(), rot, sizeof rot);
    if (!c.Invoke() || !c.RetData()) {
      LOG("enemies: SpawnEnemy failed: %s", c.Error().c_str());
      break;
    }
    uintptr_t pawn = *reinterpret_cast<const uintptr_t*>(c.RetData());
    if (!pawn || !ue3::IsObject(pawn)) {
      LOG("enemies: SpawnEnemy returned nothing (%.0f %.0f %.0f)", pos.x, pos.y, pos.z);
      continue;
    }
    g_spawned.push_back({pawn, ue3::ObjIndexOf(pawn)});
    ++made;
    ++alive;
  }
  return made;
}

void PublishSnapshot(const game::World& w) {
  Snapshot s;
  s.mission = w.mission;
  s.kinds = g_kinds;
  s.avail = g_avail;
  s.active = w.mission ? (int)ActiveEnemies(w).size() : 0;
  s.spawned = (int)g_spawned.size();
  s.loading = g_pending.on;
  s.message = g_msg;
  std::lock_guard<std::mutex> lk(g_mu);
  g_snap = std::move(s);
}

void CollectHp(const game::World& w) {
  HpView v;
  if (g_showHp && w.mission && CameraPov(w, *reinterpret_cast<game::Vec*>(&v.camX), &v.pitch, v.fov)) {
    v.valid = true;
    for (uintptr_t e : ActiveEnemies(w)) {
      float hp = 0, mx = 0;
      if (!Health(e, &hp, &mx) || mx <= 0 || hp <= 0) continue;
      game::Vec top{};
      if (!ActorLoc(e, top)) continue;
      // above the head: the top of the mesh bounds (BoxSphereBounds { Origin, BoxExtent, SphereRadius })
      top.z += 120.0f;
      if (uintptr_t mesh = GetObj(e, "Mesh")) {
        game::Field b = game::FieldOf(mesh, "Bounds");
        game::Vec o{}, ext{};
        if (b.ok() && util::Read(mesh + b.off, o) && util::Read(mesh + b.off + 12, ext) && ext.z > 10 && ext.z < 2000)
          top = {o.x, o.y, o.z + ext.z + 25.0f};
      }
      v.labels.push_back({top.x, top.y, top.z, hp, mx});
    }
  }
  std::lock_guard<std::mutex> lk(g_hpMu);
  g_hp = std::move(v);
}

bool IsNTPawn(uintptr_t o) {
  uintptr_t cls = ue3::ObjClass(o);
  auto it = g_pawnClass.find(cls);
  if (it != g_pawnClass.end()) return it->second;
  bool r = ue3::IsChildOf(cls, "NTPawn") && ue3::ClassNameOf(o) != "Class";
  g_pawnClass[cls] = r;
  return r;
}

}  // namespace

const char* AvailText(Avail a) {
  switch (a) {
    case Avail::Loadable: return "loads first";
    case Avail::Missing: return "not available on this map";
    default: return "";
  }
}

Snapshot Now() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_snap;
}

Selection GetSel() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_sel;
}

void SetSel(const Selection& s) {
  Selection c = s;
  c.count = std::clamp(c.count, 1, kMaxCount);
  c.where = std::clamp(c.where, 0, 1);
  c.distance = std::clamp(c.distance, 1.0f, 20.0f);
  c.posSlot = std::clamp(c.posSlot, 0, nav::kPosSlots - 1);
  std::lock_guard<std::mutex> lk(g_mu);
  g_sel = c;
}

void LoadJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  Selection s;
  s.kind = j.value("kind", s.kind);
  s.count = j.value("count", s.count);
  s.where = j.value("where", s.where);
  s.distance = j.value("distance", s.distance);
  s.posSlot = j.value("pos_slot", s.posSlot);
  SetSel(s);
}

nlohmann::json SaveJson() {
  Selection s = GetSel();
  return {{"kind", s.kind}, {"count", s.count}, {"where", s.where}, {"distance", s.distance}, {"pos_slot", s.posSlot}};
}

std::string Blocked(Action a) {
  Snapshot s = Now();
  if (!s.mission) return "in a mission only";
  if (a == Action::Spawn) {
    if (s.loading) return "loading the enemy...";
    if (nav::Now().cutscene) return "not during a cutscene";
    Selection sel = GetSel();
    for (size_t i = 0; i < s.kinds.size() && i < s.avail.size(); ++i)
      if (s.kinds[i].id == sel.kind && s.avail[i] == Avail::Missing) return "not available on this map";
    if (sel.where == 1) {
      nav::Snapshot ns = nav::Now();
      if (!ns.slot[sel.posSlot]) return "nothing saved in this position slot";
    }
  }
  if (a == Action::KillAll && s.active == 0) return "no enemies";
  if (a == Action::Despawn && s.spawned == 0) return "none spawned";
  return "";
}

std::vector<uintptr_t> ActiveEnemies(const game::World& w) {
  std::vector<uintptr_t> out;
  uintptr_t em = GetObj(w.gameInfo, "m_EnemyManager");  // DVGameInfo.m_EnemyManager.m_ActiveEnemies
  if (!em) return out;
  game::Field f = game::FieldOf(em, "m_ActiveEnemies");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(em + f.off, a) || a.num <= 0 || a.num > 512 || !a.data) return out;
  for (int i = 0; i < a.num; ++i) {
    uintptr_t p = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (p && p != w.pawn && ue3::IsObject(p)) out.push_back(p);
  }
  return out;
}

void OnFrame(const game::World& w) {
  ULONGLONG now = GetTickCount64();
  if (w.gen != g_scanGen) {
    g_scanGen = w.gen;
    g_nextScan = 0;
    g_failed.clear();
    g_msg.clear();
    g_scriptBar = {};
    g_phaseBar = {};
    if (!g_fnBossBar) g_fnBossBar = ue3::FindObject("Function", "Activated", "DVSeqAct_BossHealthBar");
    if (g_pending.on) SetMsg("Spawn cancelled: the level changed.");
    g_pending = {};
  }
  PruneSpawned(w);
  // the list is cheap to keep fresh (one pass over the objects, by class pointer): every 1 s, every 0.25 s while
  // a spawn waits for its enemy to load
  if (now >= g_nextScan && ue3::Ready()) {
    Rescan(w);
    g_nextScan = now + (g_pending.on ? 250 : 1000);
  }
  if (g_pending.on) {
    if (!w.mission) {
      g_pending = {};
    } else if (g_templates.count(g_pending.kind)) {
      if (!g_pending.readyAt) g_pending.readyAt = now + 500;  // let the freshly streamed level finish setting up
      if (now >= g_pending.readyAt) {
        Pending p = g_pending;
        g_pending = {};
        int made = DoSpawn(w, p);
        const Kind* k = FindKind(p.kind);
        SetMsg(made > 0 ? "Spawned " + std::to_string(made) + " x " + (k ? k->name : p.kind) + "."
                        : "Could not spawn " + std::string(k ? k->name : p.kind) + " here.");
      }
    } else if (now > g_pending.deadline) {
      const Kind* k = FindKind(g_pending.kind);
      SetMsg("Could not load " + std::string(k ? k->name : g_pending.kind) + " on this map.");
      g_failed.push_back(g_pending.kind);
      g_pending = {};
    }
  }
  CollectHp(w);
  PublishSnapshot(w);
}

void Spawn(const game::World& w) {
  std::string why = Blocked(Action::Spawn);
  if (!why.empty()) {
    LOG("enemies: spawn refused (%s)", why.c_str());
    return;
  }
  Rescan(w);
  Selection sel = GetSel();
  const Kind* k = FindKind(sel.kind);
  if (!k) {
    SetMsg("Unknown enemy.");
    return;
  }
  Pending p;
  p.on = true;
  p.kind = k->id;
  p.count = sel.count;
  game::Vec dante{};
  if (!ActorLoc(w.pawn, dante)) return;
  if (sel.where == 1) {
    int yaw = 0;
    if (!nav::SavedPosition(sel.posSlot, &p.center, &yaw)) {
      SetMsg("Nothing saved in this position slot.");
      return;
    }
    p.faceYaw = yaw;      // a row across the saved view direction
    p.faceDante = false;  // looking the way the saved position looked
  } else {
    // in front of Dante, along the camera's view (what the player sees ahead)
    game::Vec cl;
    int cr[3];
    float fov;
    float yaw = CameraPov(w, cl, cr, fov) ? FromUnrealAngle(cr[1]) : 0.0f;
    float d = sel.distance * 100.0f;
    p.center = {dante.x + std::cos(yaw) * d, dante.y + std::sin(yaw) * d, dante.z};
    p.faceYaw = ToUnrealAngle(yaw);
  }
  if (g_templates.count(k->id)) {
    int made = DoSpawn(w, p);
    SetMsg(made > 0 ? "Spawned " + std::to_string(made) + " x " + k->name + "." : "Could not spawn " + k->name + " here.");
    return;
  }
  // not in memory: stream its spawn level in (what DVExecFunctions.LoadSingleSpawnLevel does) and spawn when the
  // template shows up
  engine::Call c(w.cheat, "LoadSingleSpawnLevel");
  if (!c.Valid() || !c.Name(c.ParamName(0).c_str(), k->id.c_str()).Invoke()) {
    SetMsg("Could not load " + k->name + " on this map.");
    return;
  }
  p.deadline = GetTickCount64() + kLoadTimeoutMs;
  g_pending = p;
  g_nextScan = 0;
  SetMsg("Loading " + k->name + "...");
}

void KillAll(const game::World& w) {
  std::vector<uintptr_t> foes = ActiveEnemies(w);
  if (foes.empty()) return;
  // The game's dev command NTKillAll gives every AI character a lethal hit. Allies (Kat) are AI characters too:
  // they are made unkillable for that one call; enemies made unkillable by Infinite enemy HP lose it for it.
  std::vector<std::pair<uintptr_t, bool>> saved;
  auto setGod = [&](uintptr_t pawn, bool god) {
    uintptr_t ctrl = GetObj(pawn, "m_Controller");
    if (!ctrl || !game::FieldOf(ctrl, "bGodMode").ok()) return;
    saved.push_back({ctrl, GetBool(ctrl, "bGodMode")});
    SetBool(ctrl, "bGodMode", god);
  };
  for (uintptr_t e : foes) setGod(e, false);
  int n = ue3::NumObjects(), allies = 0;
  for (int i = 0; i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o || o == w.pawn || !IsNTPawn(o) || std::find(foes.begin(), foes.end(), o) != foes.end()) continue;
    if (ue3::ObjName(o).rfind("Default__", 0) == 0 || GetBool(o, "m_IsDestroyed")) continue;
    uintptr_t ctrl = GetObj(o, "m_Controller");
    if (!ctrl || !ue3::IsA(ctrl, "NTAIController")) continue;
    setGod(o, true);
    ++allies;
  }
  engine::Call c(w.cheat, "NTKillAll");
  bool ok = c.Valid() && c.Invoke();
  for (auto it = saved.rbegin(); it != saved.rend(); ++it)
    if (game::Alive(it->first)) SetBool(it->first, "bGodMode", it->second);
  SetMsg(ok ? "Killed " + std::to_string(foes.size()) + " enemies." : "Kill all did not work here.");
  if (allies) LOG("enemies: kill all spared %d other AI character(s)", allies);
}

void DespawnSpawned(const game::World& w) {
  PruneSpawned(w);
  int n = 0;
  for (const Spawned& s : g_spawned) {
    // DVPawn.CleanupOnscreen: the game's own removal of an enemy (despawn effect + Destroy; DVPawn.Destroyed takes
    // it out of the enemy manager)
    engine::Call c(s.pawn, "CleanupOnscreen");
    if (c.Valid() && c.Invoke()) ++n;
  }
  g_spawned.clear();
  SetMsg("Removed " + std::to_string(n) + " spawned enemies.");
}

std::string ActorName(const game::World& w, uintptr_t a) {
  if (!a) return "?";
  if (a == w.pawn) return "Dante";
  if (uintptr_t t = GetObj(a, "m_Template")) {
    std::string tn = ue3::ObjName(t);
    if (size_t d = tn.find("_Duplicate"); d != std::string::npos) tn = tn.substr(0, d);
    auto it = g_tmplToKind.find(tn);
    if (it != g_tmplToKind.end())
      if (const Kind* k = FindKind(it->second)) return k->name;
  }
  std::string cn = game::GetName(a, "m_CharacterName");
  if (const Kind* k = FindKind(cn)) return k->name;
  // bosses: the game's names (GFxUI.int NewEnemy_*), by character name or pawn class (DVPawn_<id>...)
  static const KnownName kBosses[] = {{"KillingFace", "Mundus' Spawn"}, {"Poison", "Succubus"},
                                      {"Hunter", "Hunter"},             {"BobBarbas", "Bob Barbas"},
                                      {"Mundus", "Mundus"},             {"Vergil", "Vergil"}};
  std::string cls = ue3::ClassNameOf(a);
  for (const KnownName& b : kBosses)
    if (cn == b.id || cls.rfind(std::string("DVPawn_") + b.id, 0) == 0) return b.name;
  if (!cn.empty()) return cn;
  std::string n = ue3::ClassNameOf(a);
  for (const char* pre : {"DVPawn_", "NTPawn_", "DV", "NT"})
    if (n.rfind(pre, 0) == 0) {
      n = n.substr(strlen(pre));
      break;
    }
  return n;
}

void OnEventAfter(uintptr_t self, uintptr_t fn) {
  if (!fn || fn != g_fnBossBar || !ue3::IsObject(self)) return;
  // DVSeqAct_BossHealthBar inputs: 0 show (Setup), 1 hide, 2 set health; values from its linked variables
  float cur = GetFloat(self, "CurrentHealth"), mx = GetFloat(self, "MaximumHealth");
  if (mx <= 0) return;
  // A bar the level sets up full at the start of the fight is not what the player is hitting yet: it counts once
  // its value moves (a hit on the part it belongs to).
  unsigned gen = game::W().gen;
  if (g_scriptBar.gen != gen || g_scriptBar.max != mx)
    g_scriptBar = {cur, mx, cur < mx ? GetTickCount64() : 0, gen};
  else if (cur != g_scriptBar.cur)
    g_scriptBar.cur = cur, g_scriptBar.changed = GetTickCount64();
}

bool Health(uintptr_t a, float* hp, float* maxHp, bool* boss) {
  if (boss) *boss = false;
  if (!a || !ue3::IsObject(a) || !game::FieldOf(a, "m_Health").ok()) return false;
  uintptr_t ctrl = GetObj(a, "m_Controller");
  if (ctrl && game::FieldOf(ctrl, "m_PhaseManager").ok()) {
    if (boss) *boss = true;
    // the boss's own bar: DVHealthBarComponent keeps the fraction it last showed and the phase's maximum
    float pcur = -1, pmax = 0;
    if (uintptr_t bar = GetObj(ctrl, "m_HealthBarComp")) {
      pmax = GetFloat(bar, "m_MaxHealth");
      pcur = GetFloat(bar, "cache_CurrentHealth") * pmax;
    }
    if (pmax <= 0)
      if (uintptr_t phase = GetObj(GetObj(ctrl, "m_PhaseManager"), "m_CurrentPhase")) {
        pmax = GetFloat(phase, "m_fTotalHealth");
        pcur = GetFloat(phase, "m_fHealth");
      }
    if (g_phaseBar.ctrl != ctrl || std::fabs(g_phaseBar.cur - pcur) > 0.01f) g_phaseBar = {ctrl, pcur, GetTickCount64()};
    bool script = g_scriptBar.max > 0 && g_scriptBar.gen == game::W().gen && g_scriptBar.changed > g_phaseBar.changed;
    if (script) {
      *hp = g_scriptBar.cur;
      *maxHp = g_scriptBar.max;
      return true;
    }
    if (pmax > 0) {
      *hp = std::max(0.0f, pcur);
      *maxHp = pmax;
      return true;
    }
  }
  *hp = GetFloat(a, "m_Health");
  *maxHp = GetFloat(a, "m_MaxHealth");
  return true;
}

void SetShowHp(bool on) { g_showHp = on; }

HpView HpLabels() {
  std::lock_guard<std::mutex> lk(g_hpMu);
  return g_hp;
}

std::vector<std::string> DebugLines() {
  std::vector<std::string> out;
  const game::World& w = game::W();
  Rescan(w);
  for (size_t i = 0; i < g_kinds.size(); ++i) {
    const Kind& k = g_kinds[i];
    auto it = g_templates.find(k.id);
    char b[256];
    snprintf(b, sizeof b, "%-22s %-20s %-28s %s %s", k.id.c_str(), k.name.c_str(), k.level.c_str(),
             i < g_avail.size() ? (g_avail[i] == Avail::Ready ? "READY" : g_avail[i] == Avail::Loadable ? "loadable"
                                   : g_avail[i] == Avail::Missing ? "MISSING" : "?") : "?",
             it != g_templates.end() ? ue3::ObjName(it->second.tmpl).c_str() : "-");
    out.push_back(b);
  }
  for (uintptr_t e : ActiveEnemies(w)) {
    char b[256];
    float hp = 0, mx = 0;
    bool boss = false;
    Health(e, &hp, &mx, &boss);
    snprintf(b, sizeof b, "active %s class %s char '%s' template %s -> '%s' HP %g/%g%s", ue3::ObjName(e).c_str(),
             ue3::ClassNameOf(e).c_str(), game::GetName(e, "m_CharacterName").c_str(),
             ue3::ObjName(GetObj(e, "m_Template")).c_str(), ActorName(w, e).c_str(), hp, mx,
             boss ? " (boss phase)" : "");
    out.push_back(b);
  }
  for (const std::string& l : StreamingLevels(w))
    if (l.find("SpawnLevel") != std::string::npos) out.push_back("streaming " + l);
  return out;
}

}  // namespace enemies
