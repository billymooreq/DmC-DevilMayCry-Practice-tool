#include "game.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "log.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"

using util::ReadOr;

namespace game {
namespace {

std::mutex g_fieldMu;
std::map<std::pair<uintptr_t, std::string>, Field> g_fields;

std::atomic<uintptr_t> g_engine{0};
std::atomic<bool> g_engineSearch{false};

World g_w;  // game thread
uintptr_t g_lastWorld = 0, g_lastPawn = 0;
LARGE_INTEGER g_lastQpc{};
float g_lastTimeSeconds = -1;

std::mutex g_stMu;
Status g_status;

void SetStatus(const Status& s) {
  std::lock_guard<std::mutex> lk(g_stMu);
  g_status = s;
}

bool IsFrontEndMap(const std::string& map) {
  std::string m = map;
  for (auto& c : m) c = (char)tolower((unsigned char)c);
  return m.rfind("mainmenu", 0) == 0;
}

// The engine object lives as long as the process: found once on a worker thread (full GObjects walk).
void StartEngineSearch() {
  bool expected = false;
  if (!g_engineSearch.compare_exchange_strong(expected, true)) return;
  std::thread([] {
    for (int attempt = 0; attempt < 600 && !g_engine; ++attempt) {
      if (ue3::Ready()) {
        int n = ue3::NumObjects();
        for (int i = 0; i < n; ++i) {
          uintptr_t o = ue3::Object(i);
          if (!o || ue3::ClassNameOf(o) == "Class") continue;
          if (!ue3::IsA(o, "GameEngine") || ue3::ObjName(o).rfind("Default__", 0) == 0) continue;
          g_engine = o;
          LOG("game: engine %s @%p", ue3::FullName(o).c_str(), (void*)o);
          break;
        }
      }
      if (!g_engine) Sleep(1000);
    }
  }).detach();
}

uintptr_t FirstLocalPlayerController(uintptr_t engine) {
  Field f = FieldOf(engine, "GamePlayers");
  if (!f.ok()) return 0;
  ue3::TArrayRaw a{};
  if (!util::Read(engine + f.off, a) || a.num <= 0 || a.num > 8 || !a.data) return 0;
  uintptr_t lp = ReadOr<uintptr_t>(a.data);
  if (!ue3::IsObject(lp)) return 0;
  return GetObj(lp, "Actor");
}

}  // namespace

// ------------------------------------------------------------------ reflection
Field FieldOf(uintptr_t obj, const char* name) {
  uintptr_t cls = ue3::ObjClass(obj);
  if (!cls) return {};
  std::pair<uintptr_t, std::string> key{cls, name};
  {
    std::lock_guard<std::mutex> lk(g_fieldMu);
    auto it = g_fields.find(key);
    if (it != g_fields.end()) return it->second;
  }
  Field r;
  int ni = ue3::FindNameIndex(name);
  for (uintptr_t s = cls; s && ni >= 0 && !r.ok(); s = ue3::StructSuper(s)) {
    int guard = 0;
    for (uintptr_t f = ue3::StructChildren(s); f && guard < 10000; f = ue3::FieldNext(f), ++guard) {
      if (ue3::ObjNameIndex(f) != ni || ReadOr<int32_t>(f + ue3::L().objName + 4) != 0) continue;
      std::string c = ue3::ClassNameOf(f);
      if (c.size() < 8 || c.compare(c.size() - 8, 8, "Property") != 0) continue;
      r.off = ue3::PropOffset(f);
      r.prop = f;
      if (c == "BoolProperty") r.mask = ue3::PropExtraU32(f);
      break;
    }
  }
  if (!r.ok()) LOG("game: field %s.%s not found", ue3::ObjName(cls).c_str(), name);
  std::lock_guard<std::mutex> lk(g_fieldMu);
  g_fields[key] = r;
  return r;
}

std::string GetStr(uintptr_t obj, const char* name) {
  Field f = FieldOf(obj, name);
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(obj + f.off, a) || a.num <= 1 || a.num > 1024 || !a.data) return {};
  std::wstring w((size_t)a.num - 1, L' ');
  if (!util::SafeRead(a.data, w.data(), w.size() * sizeof(wchar_t))) return {};
  return util::WideToUtf8(w);
}
std::string GetName(uintptr_t obj, const char* name) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return {};
  std::string n = ue3::ReadFName(obj + f.off);
  return n == "None" ? std::string() : n;
}

float GetFloat(uintptr_t obj, const char* name, float def) {
  Field f = FieldOf(obj, name);
  return f.ok() ? ReadOr<float>(obj + f.off, def) : def;
}
bool SetFloat(uintptr_t obj, const char* name, float v) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return false;
  *reinterpret_cast<float*>(obj + f.off) = v;
  return true;
}
int GetInt(uintptr_t obj, const char* name, int def) {
  Field f = FieldOf(obj, name);
  return f.ok() ? ReadOr<int32_t>(obj + f.off, def) : def;
}
bool SetInt(uintptr_t obj, const char* name, int v) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return false;
  *reinterpret_cast<int32_t*>(obj + f.off) = v;
  return true;
}
uint8_t GetByte(uintptr_t obj, const char* name, uint8_t def) {
  Field f = FieldOf(obj, name);
  return f.ok() ? ReadOr<uint8_t>(obj + f.off, def) : def;
}
bool SetByte(uintptr_t obj, const char* name, uint8_t v) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return false;
  *reinterpret_cast<uint8_t*>(obj + f.off) = v;
  return true;
}
bool GetBool(uintptr_t obj, const char* name, bool def) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return def;
  return (ReadOr<uint32_t>(obj + f.off) & (f.mask ? f.mask : 1)) != 0;
}
bool SetBool(uintptr_t obj, const char* name, bool v) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return false;
  uint32_t* d = reinterpret_cast<uint32_t*>(obj + f.off);
  uint32_t m = f.mask ? f.mask : 1;
  *d = v ? (*d | m) : (*d & ~m);
  return true;
}
uintptr_t GetObj(uintptr_t obj, const char* name) {
  Field f = FieldOf(obj, name);
  if (!f.ok()) return 0;
  uintptr_t v = ReadOr<uintptr_t>(obj + f.off);
  return v && ue3::IsObject(v) ? v : 0;
}

bool Alive(uintptr_t obj) {
  if (!obj || !ue3::IsObject(obj)) return false;
  int idx = ue3::ObjIndexOf(obj);
  return idx >= 0 && ue3::Object(idx) == obj;
}

std::string TopPackage(uintptr_t o) {
  uintptr_t top = o;
  for (int d = 0; d < 32; ++d) {
    uintptr_t outer = ue3::ObjOuter(top);
    if (!outer) break;
    top = outer;
  }
  return ue3::ObjName(top);
}

// ------------------------------------------------------------------ world
const World& W() { return g_w; }

// PlayerCamera.CameraCache = { float TimeStamp; TPOV POV { Vector Location; Rotator Rotation; float FOV } }
bool CameraPov(const World& w, Pov& out) {
  uintptr_t cam = GetObj(w.pc, "PlayerCamera");
  if (!cam) return false;
  Field f = FieldOf(cam, "CameraCache");
  if (!f.ok()) return false;
  uintptr_t p = cam + f.off + 4;
  return util::Read(p, out.loc) && util::SafeRead(p + 12, out.rot, 12) && util::Read(p + 24, out.fov);
}

void OnFrame() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  float dt = g_lastQpc.QuadPart ? (float)(now.QuadPart - g_lastQpc.QuadPart) / (float)freq.QuadPart : 0.0f;
  g_lastQpc = now;
  if (dt > 0.25f) dt = 0.25f;  // hitch / loading screen: never teleport a whole second of movement

  Status st;
  World w;
  w.gen = g_w.gen;
  w.realDt = dt;
  if (!FeaturesAllowed()) {
    st.reason = "unknown game version";
  } else if (!ue3::Ready() || !engine::Hooked()) {
    st.reason = "starting up";
  } else if (!(w.engine = g_engine)) {
    StartEngineSearch();
    st.reason = "starting up";
  } else {
    st.ready = true;
    w.pc = FirstLocalPlayerController(w.engine);
    if (w.pc) {
      w.world = GetObj(w.pc, "WorldInfo");
      // Ninja Theory pawns are not engine Pawns: the player character is NTPlayerController.m_NTPawn.
      w.pawn = GetObj(w.pc, "m_NTPawn");
      if (!w.pawn) w.pawn = GetObj(w.pc, "Pawn");
      w.cheat = GetObj(w.pc, "CheatManager");
    }
    if (w.world) w.gameInfo = GetObj(w.world, "Game");
    if (w.world == g_lastWorld && !g_w.map.empty())
      w.map = g_w.map;
    else if (w.world)
      w.map = TopPackage(w.world);
    w.frontEnd = w.map.empty() || IsFrontEndMap(w.map);
    if (w.pawn && !ue3::IsA(w.pawn, "DVPawn_Player")) w.pawn = 0;  // cinematic / possession of something else
    w.mission = !w.frontEnd && w.pawn && w.world;
    if (w.world) {
      float t = GetFloat(w.world, "TimeSeconds", -1);
      w.gameDt = (g_lastTimeSeconds >= 0 && w.world == g_lastWorld) ? std::max(0.0f, t - g_lastTimeSeconds) : 0.0f;
      if (w.gameDt > 0.25f) w.gameDt = 0.25f;
      g_lastTimeSeconds = t;
    }
    if (w.world != g_lastWorld || w.pawn != g_lastPawn) {
      ++w.gen;
      LOG("game: world %s map '%s' pawn %s (gen %u)", util::Hex(w.world).c_str(), w.map.c_str(),
          w.pawn ? ue3::ObjName(w.pawn).c_str() : "none", w.gen);
      g_lastWorld = w.world;
      g_lastPawn = w.pawn;
    }
    st.map = w.map;
    st.frontEnd = w.frontEnd;
    st.mission = w.mission;
    if (!w.mission) st.reason = w.frontEnd ? "main menu - works in a mission" : "loading";
  }
  g_w = w;
  SetStatus(st);
}

Status CurrentStatus() {
  std::lock_guard<std::mutex> lk(g_stMu);
  return g_status;
}

std::vector<uintptr_t> Pawns() {
  // DVGameInfo.m_EnemyManager.m_ActiveEnemies (WorldInfo.PawnList stays empty: NT pawns are not Pawns).
  std::vector<uintptr_t> out;
  uintptr_t em = g_w.gameInfo ? GetObj(g_w.gameInfo, "m_EnemyManager") : 0;
  Field f = em ? FieldOf(em, "m_ActiveEnemies") : Field{};
  ue3::TArrayRaw a{};
  if (f.ok() && util::Read(em + f.off, a) && a.num > 0 && a.num <= 512 && a.data)
    for (int i = 0; i < a.num; ++i) {
      uintptr_t p = ReadOr<uintptr_t>(a.data + i * 4);
      if (p && ue3::IsObject(p)) out.push_back(p);
    }
  return out;
}

bool IsEnemy(uintptr_t pawn) {
  if (!pawn || pawn == g_w.pawn || !ue3::IsA(pawn, "NTPawn")) return false;
  uintptr_t ctrl = GetObj(pawn, "m_Controller");
  return ctrl && ue3::IsA(ctrl, "NTAIController");
}

// ------------------------------------------------------------------ tokens
bool HasToken(uintptr_t pawn, const char* token, const char* owner) {
  uintptr_t tm = GetObj(pawn, "m_TokenManager");
  if (!tm) return false;
  if (owner) {
    engine::Call c(tm, "OwnerHasToken");
    c.Name("TokenName", token).Name("TokenOwner", owner);
    return c.Invoke() && c.RetBool();
  }
  engine::Call c(tm, "HasToken");
  c.Name("TokenName", token);
  return c.Invoke() && c.RetBool();
}

bool CreateToken(uintptr_t pawn, const char* token, const char* owner) {
  uintptr_t tm = GetObj(pawn, "m_TokenManager");
  if (!tm) return false;
  engine::Call c(tm, "CreateToken");
  c.Name("TokenName", token).Name("TokenOwner", owner);
  return c.Invoke();
}

bool DeleteToken(uintptr_t pawn, const char* token, const char* owner) {
  uintptr_t tm = GetObj(pawn, "m_TokenManager");
  if (!tm) return false;
  engine::Call c(tm, "DeleteToken");
  c.Name("TokenName", token).Name("TokenOwner", owner);
  return c.Invoke();
}

}  // namespace game
