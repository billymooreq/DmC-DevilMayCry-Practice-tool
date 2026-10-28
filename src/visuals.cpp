// Visuals: collision wireframes over the picture. Research: research/visuals.md.
#include "visuals.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "enemies.h"
#include "imgui.h"
#include "log.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace visuals {
namespace {

using game::GetBool;
using game::GetFloat;
using game::GetInt;
using game::GetObj;
using game::Vec;

constexpr float kPi = 3.14159265f;
constexpr float kUnrealToRad = 6.2831853f / 65536.0f;

// ------------------------------------------------------------------ math (UE3: X forward, Y right, Z up)
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(Vec a) { return std::sqrt(Dot(a, a)); }
Vec Norm(Vec a) {
  float l = Len(a);
  return l > 1e-6f ? a * (1.0f / l) : Vec{1, 0, 0};
}

// Affine transform as rows (UE3 row vectors): p -> o + x*p.x + y*p.y + z*p.z. Axes may carry scale.
struct Xf {
  Vec x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1}, o{};
  Vec P(Vec p) const { return o + x * p.x + y * p.y + z * p.z; }
  Vec D(Vec p) const { return x * p.x + y * p.y + z * p.z; }
};
Xf Then(const Xf& a, const Xf& b) {  // a first, then b
  Xf r;
  r.x = b.D(a.x);
  r.y = b.D(a.y);
  r.z = b.D(a.z);
  r.o = b.P(a.o);
  return r;
}
// FRotationMatrix
Xf FromRot(const int rot[3], Vec o = {}) {
  float sp = std::sin(rot[0] * kUnrealToRad), cp = std::cos(rot[0] * kUnrealToRad);
  float sy = std::sin(rot[1] * kUnrealToRad), cy = std::cos(rot[1] * kUnrealToRad);
  float sr = std::sin(rot[2] * kUnrealToRad), cr = std::cos(rot[2] * kUnrealToRad);
  Xf t;
  t.x = {cp * cy, cp * sy, sp};
  t.y = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
  t.z = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
  t.o = o;
  return t;
}
// FQuatRotationTranslationMatrix, rows scaled (BoneAtom)
Xf FromQuat(const float q[4], Vec o, float s) {
  float x2 = q[0] + q[0], y2 = q[1] + q[1], z2 = q[2] + q[2];
  float xx = q[0] * x2, xy = q[0] * y2, xz = q[0] * z2, yy = q[1] * y2, yz = q[1] * z2, zz = q[2] * z2;
  float wx = q[3] * x2, wy = q[3] * y2, wz = q[3] * z2;
  Xf t;
  t.x = Vec{1 - (yy + zz), xy + wz, xz - wy} * s;
  t.y = Vec{xy - wz, 1 - (xx + zz), yz + wx} * s;
  t.z = Vec{xz + wy, yz - wx, 1 - (xx + yy)} * s;
  t.o = o;
  return t;
}
Xf FromMatrix(const float m[16]) {
  Xf t;
  t.x = {m[0], m[1], m[2]};
  t.y = {m[4], m[5], m[6]};
  t.z = {m[8], m[9], m[10]};
  t.o = {m[12], m[13], m[14]};
  return t;
}

// ------------------------------------------------------------------ frame data
struct Line {
  Vec a, b;
  uint32_t col;
  bool occlude = false;  // hidden behind the level's geometry when the scene depth is available
  uint32_t group = 0;    // lines of one trigger: its label is shown while any of them is visible
};
struct Label {
  Vec p;
  uint32_t col;
  std::string text;
  uint32_t group = 0;  // hidden while all lines of this group are hidden behind walls
};
struct Frame {
  bool valid = false;
  ULONGLONG at = 0;  // real time it was collected
  game::Pov cam;
  std::vector<Line> lines;
  std::vector<Label> labels;
};

uint32_t Rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
uint32_t WithAlpha(uint32_t col, float a) {
  int alpha = (int)(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
  return (col & ~IM_COL32_A_MASK) | ((uint32_t)alpha << IM_COL32_A_SHIFT);
}

// Lines in world space, all with one colour.
struct Sink {
  std::vector<Line>* out;
  uint32_t col;
  bool occlude = false;
  uint32_t group = 0;
  void L(Vec a, Vec b) const { out->push_back({a, b, col, occlude, group}); }
};

// Circle (or arc a0..a1) around c in the plane (u, v) of the local space of t.
void Ring(const Sink& s, const Xf& t, Vec c, Vec u, Vec v, float r, int segs, float a0 = 0, float a1 = 2 * kPi) {
  Vec prev = t.P(c + u * (std::cos(a0) * r) + v * (std::sin(a0) * r));
  for (int i = 1; i <= segs; ++i) {
    float a = a0 + (a1 - a0) * i / segs;
    Vec p = t.P(c + u * (std::cos(a) * r) + v * (std::sin(a) * r));
    s.L(prev, p);
    prev = p;
  }
}

void Basis(Vec axis, Vec& u, Vec& v) {
  Vec ref = std::fabs(axis.z) < 0.9f ? Vec{0, 0, 1} : Vec{1, 0, 0};
  u = Norm(Cross(axis, ref));
  v = Cross(axis, u);
}

void Sphere(const Sink& s, const Xf& t, Vec c, float r) {
  Ring(s, t, c, {1, 0, 0}, {0, 1, 0}, r, 16);
  Ring(s, t, c, {1, 0, 0}, {0, 0, 1}, r, 16);
  Ring(s, t, c, {0, 1, 0}, {0, 0, 1}, r, 16);
}

// Capsule around the segment a..b (local space of t).
void Capsule(const Sink& s, const Xf& t, Vec a, Vec b, float r) {
  Vec d = b - a;
  if (Len(d) < 0.01f) return Sphere(s, t, a, r);
  Vec ax = Norm(d), u, v;
  Basis(ax, u, v);
  Ring(s, t, a, u, v, r, 16);
  Ring(s, t, b, u, v, r, 16);
  for (Vec side : {u, v, u * -1.0f, v * -1.0f}) s.L(t.P(a + side * r), t.P(b + side * r));
  // end caps: half circles through the axis
  Ring(s, t, b, u, ax, r, 8, 0, kPi);
  Ring(s, t, b, v, ax, r, 8, 0, kPi);
  Ring(s, t, a, u, ax * -1.0f, r, 8, 0, kPi);
  Ring(s, t, a, v, ax * -1.0f, r, 8, 0, kPi);
}

void Box(const Sink& s, const Xf& t, Vec c, Vec h) {
  Vec p[8];
  for (int i = 0; i < 8; ++i)
    p[i] = t.P(c + Vec{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
  static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (auto& k : e) s.L(p[k[0]], p[k[1]]);
}

// Upright cylinder (world Z), half height h.
void Cylinder(const Sink& s, Vec c, float r, float h) {
  Xf id;
  Ring(s, id, c + Vec{0, 0, h}, {1, 0, 0}, {0, 1, 0}, r, 20);
  Ring(s, id, c - Vec{0, 0, h}, {1, 0, 0}, {0, 1, 0}, r, 20);
  for (int i = 0; i < 4; ++i) {
    float a = i * kPi * 0.5f;
    Vec o{std::cos(a) * r, std::sin(a) * r, 0};
    s.L(c + o + Vec{0, 0, h}, c + o - Vec{0, 0, h});
  }
}

// Prism over a polygon of the local XY plane, from z0 to z1 (vertical edges only at its corners).
void Prism(const Sink& s, const Xf& t, const std::vector<std::pair<float, float>>& poly, float z0, float z1) {
  size_t n = poly.size();
  if (n < 2) return;
  for (size_t i = 0; i < n; ++i) {
    auto [ax, ay] = poly[i];
    auto [bx, by] = poly[(i + 1) % n];
    s.L(t.P({ax, ay, z0}), t.P({bx, by, z0}));
    s.L(t.P({ax, ay, z1}), t.P({bx, by, z1}));
    auto [px, py] = poly[(i + n - 1) % n];
    float d1x = ax - px, d1y = ay - py, d2x = bx - ax, d2y = by - ay;
    float l = std::sqrt((d1x * d1x + d1y * d1y) * (d2x * d2x + d2y * d2y));
    if (l < 1e-6f || (d1x * d2x + d1y * d2y) / l < 0.94f) s.L(t.P({ax, ay, z0}), t.P({ax, ay, z1}));  // > 20 degrees
  }
}

// Clip a polygon to the half plane a*x + b*y <= c (Sutherland-Hodgman).
std::vector<std::pair<float, float>> ClipPoly(const std::vector<std::pair<float, float>>& in, float a, float b, float c) {
  std::vector<std::pair<float, float>> out;
  for (size_t i = 0; i < in.size(); ++i) {
    auto p = in[i], q = in[(i + 1) % in.size()];
    float dp = a * p.first + b * p.second - c, dq = a * q.first + b * q.second - c;
    if (dp <= 0) out.push_back(p);
    if ((dp < 0) != (dq < 0) && dp != dq) {
      float k = dp / (dp - dq);
      out.push_back({p.first + (q.first - p.first) * k, p.second + (q.second - p.second) * k});
    }
  }
  return out;
}

// NTCombat_StrikeVolume_Arc: the game queries a box (GetPhysXCollisionParameters: centre = Range/2 forward and
// Height/2 up, extent = Range/2, BoxWidth/2, Height/2) and keeps what lies within Range and the Angle. Drawn: the
// pie slice (radius r, +-half around X) cut by that box, from the strike's location up to its height.
void ArcVolume(const Sink& s, const Xf& t, float r, float half, float width, float h) {
  std::vector<std::pair<float, float>> poly;
  int segs = std::max(4, (int)(half * 2 / (kPi / 16)));
  if (half < kPi - 0.01f) poly.push_back({0.0f, 0.0f});
  for (int i = 0; i <= segs; ++i) {
    float a = -half + 2 * half * i / segs;
    poly.push_back({std::cos(a) * r, std::sin(a) * r});
  }
  if (half >= kPi - 0.01f) poly.pop_back();  // full circle: no repeated point
  poly = ClipPoly(poly, -1, 0, 0);  // x >= 0
  if (width > 0) {
    poly = ClipPoly(poly, 0, 1, width * 0.5f);
    poly = ClipPoly(poly, 0, -1, width * 0.5f);
  }
  Prism(s, t, poly, 0, h);
}

// Cone along local X: radius rn at 0, rf at h.
void Cone(const Sink& s, const Xf& t, float h, float rn, float rf) {
  Ring(s, t, {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, std::max(rn, 0.5f), 16);
  Ring(s, t, {h, 0, 0}, {0, 1, 0}, {0, 0, 1}, rf, 16);
  for (int i = 0; i < 4; ++i) {
    float a = i * kPi * 0.5f;
    Vec d{0, std::cos(a), std::sin(a)};
    s.L(t.P(d * rn), t.P(Vec{h, 0, 0} + d * rf));
  }
}

// ------------------------------------------------------------------ settings
std::atomic<bool> g_on[kCats];
std::atomic<bool> g_moveCapsule[2];  // enemies, player
std::atomic<float> g_linger{0.3f};
std::mutex g_optMu;
Options g_opt;

std::mutex g_frameMu;
std::shared_ptr<const Frame> g_frame = std::make_shared<Frame>();
std::mutex g_statMu;
Stats g_stats;
std::atomic<float> g_drawMs{0};

// ------------------------------------------------------------------ game thread state
unsigned g_gen = ~0u;
double g_clock = 0;  // world seconds (stops in pause, follows slow motion)
ULONGLONG g_lastEvent = 0;

// Characters: physics asset bodies, parsed once per asset.
struct Elem {
  int type;  // 0 sphere, 1 box, 2 sphyl
  Xf tm;
  float a, b, c;
};
struct Body {
  ue3::FNameRaw bone;
  std::vector<Elem> elems;
};
std::unordered_map<uintptr_t, std::vector<Body>> g_assets;
std::map<std::tuple<uintptr_t, int, int>, int> g_boneIdx;  // (skeletal mesh, name) -> bone index

// Strikes and projectiles.
struct Ghost {
  uintptr_t key;
  double t;
  uint32_t col;
  std::vector<Line> lines;
  Vec labelAt;
  std::string label;
  float minLife = 0;  // instant events (direct hits) stay at least this long, whatever the slider says
};
std::deque<Ghost> g_ghosts;
struct Strike {
  uintptr_t owner = 0;
  double lastCapture = -1;
  std::string label;
};
std::unordered_map<uintptr_t, Strike> g_strikes;      // running strike processes
std::unordered_map<uintptr_t, double> g_projectiles;  // projectile -> last capture
struct Marker {
  Vec p;
  double t;
  bool dante;
};
std::deque<Marker> g_markers;
struct VolumeHit {
  double t = -1;
  uintptr_t owner = 0;
} g_lastVolumeHit;  // the last strike volume / projectile that found a victim
int g_probe = 0;  // Dev: log this many strikes / projectiles in detail (their first shape)
std::unordered_set<uintptr_t> g_probed;

// Triggers (Kismet Touch / Used events with an actor in the level as their originator).
enum class TKind { Other, Cutscene, Arena };
struct Trigger {
  uintptr_t ev = 0, orig = 0;
  TKind kind = TKind::Other;
  std::string label, path;
};
std::vector<Trigger> g_triggers;
// Check of the classification in the log: a trigger fired -> what the game did in the next seconds.
std::unordered_map<uintptr_t, int> g_trigCount;
struct Fired {
  std::string what;
  ULONGLONG at;
  int enemies;
  bool cinematic;
};
std::vector<Fired> g_fired;
struct Scan {
  bool running = false;
  int next = 0;
  std::vector<uintptr_t> events;
  std::unordered_multimap<int, uintptr_t> remote;  // SeqEvent_RemoteEvent by EventName index
  std::vector<uintptr_t> walls;
} g_scan;
std::vector<uintptr_t> g_walls;  // blocking volumes of the loaded levels (last finished scan)
ULONGLONG g_nextScan = 0;
// BrushComponent -> its convex elements in local space: outline edges + a grid on the faces (invisible walls)
struct ConvexShape {
  std::vector<std::pair<Vec, Vec>> edges, grid;
};
std::unordered_map<uintptr_t, ConvexShape> g_convex;
std::atomic<bool> g_wallGrid{true};
// hidden behind walls (render thread): lines for the D3D9 hook's layer, groups seen by its occlusion queries
std::atomic<bool> g_occLive{false};
Occluded g_occ;
std::unordered_map<uint32_t, bool> g_groupVisible;
std::atomic<bool> g_occludeAll{false};  // Dev: every category hidden behind walls (to check the depth test)
game::Pov g_lastCam;       // render thread
bool g_lastCamOk = false;
std::atomic<float> g_testPlane{0};  // Dev

// Class caches.
enum ClassFlag : uint32_t {
  kStrikeProc = 1, kProjectile = 2, kTouchEvent = 4, kRemoteEvent = 8, kArena = 16, kCutscene = 32,
  kInterp = 64, kSequence = 128, kActivateRemote = 256, kFinishSequence = 512, kSeqActivated = 1024,
  kVolArc = 2048, kVolCapsule = 4096, kVolCone = 8192, kVolTwoArcs = 16384, kCylinderComp = 32768,
  kBrushComp = 65536, kDirector = 131072, kSequenceEvent = 262144, kWall = 524288,
};
std::unordered_map<uintptr_t, uint32_t> g_classFlags;
uint32_t Flags(uintptr_t obj) {
  uintptr_t cls = ue3::ObjClass(obj);
  if (!cls) return 0;
  auto it = g_classFlags.find(cls);
  if (it != g_classFlags.end()) return it->second;
  uint32_t f = 0;
  auto is = [&](const char* n) { return ue3::IsChildOf(cls, n); };
  if (is("NTCombat_ProcessStrike")) f |= kStrikeProc;
  if (is("NTCombat_Projectile")) f |= kProjectile;
  if (is("SequenceEvent")) f |= kSequenceEvent;
  if (is("SeqEvent_Touch") || is("SeqEvent_Used")) f |= kTouchEvent;
  if (is("SeqEvent_RemoteEvent")) f |= kRemoteEvent;
  if (is("SeqEvent_SequenceActivated")) f |= kSeqActivated;
  // the level script starting a fight: an encounter (arena lock + waves) or a wave spawn
  if (is("DVSeqAct_BeginEncounter") || is("DVSeqAct_Encounter") || is("DVSeqAct_EncounterWave") ||
      is("DVSeqAct_EnemyWaveManager") || is("DVSeqAct_EncounterUnmanaged") || is("NTSeqAct_SpawnWave"))
    f |= kArena;
  // ... or a cutscene: the game's cinematic actions; a matinee counts only with a director (camera cuts)
  if (is("NTSeqAct_PlayCinematic") || is("DVSeqAct_PlayCinematicAndMatinee") || is("NTSeqAct_CinematicNode") ||
      is("SeqAct_ToggleCinematicMode") || is("NTSeqAct_ShowCutsceneTextScreen"))
    f |= kCutscene;
  if (is("SeqAct_Interp")) f |= kInterp;
  if (is("Sequence")) f |= kSequence;
  if (is("SeqAct_ActivateRemoteEvent")) f |= kActivateRemote;
  if (is("SeqAct_FinishSequence")) f |= kFinishSequence;
  if (is("NTCombat_StrikeVolume_TwoArcs")) f |= kVolTwoArcs;
  else if (is("NTCombat_StrikeVolume_Arc")) f |= kVolArc;
  else if (is("NTCombat_StrikeVolume_Capsule")) f |= kVolCapsule;
  else if (is("NTCombat_StrikeVolume_Cone")) f |= kVolCone;
  if (is("CylinderComponent")) f |= kCylinderComp;
  if (is("BrushComponent")) f |= kBrushComp;
  if (is("InterpGroupDirector")) f |= kDirector;
  // invisible walls: what pawns collide with (camera blocking volumes are a separate tree and stay out)
  if (is("BlockingVolume") || is("DynamicBlockingVolume") || is("NTCollisionVolume")) f |= kWall;
  g_classFlags[cls] = f;
  return f;
}

// Function names the observer reacts to (name index, resolved once).
struct Names {
  int started = -1, stopped = -1, tick = -1, processTick = -1, contacted = -1, strikeCb = -1, contact = -1;
  bool ok = false;
  void Resolve() {
    started = ue3::FindNameIndex("ProcessStarted");
    stopped = ue3::FindNameIndex("ProcessStopped");
    processTick = ue3::FindNameIndex("ProcessTick");
    tick = ue3::FindNameIndex("Tick");
    contacted = ue3::FindNameIndex("Callback_Contacted");
    strikeCb = ue3::FindNameIndex("StrikeCallback");
    contact = ue3::FindNameIndex("ContactInternal");
    ok = started >= 0 && stopped >= 0 && processTick >= 0;
  }
} g_names;

template <class T>
bool ReadAt(uintptr_t obj, const char* field, T& out) {
  game::Field f = game::FieldOf(obj, field);
  return f.ok() && util::Read(obj + f.off, out);
}
bool ReadArr(uintptr_t obj, const char* field, ue3::TArrayRaw& a) {
  return ReadAt(obj, field, a) && a.num >= 0 && a.num < 100000 && (a.num == 0 || a.data);
}
bool ActorLoc(uintptr_t a, Vec& v) { return ReadAt(a, "Location", v); }

Options Opt() {
  std::lock_guard<std::mutex> lk(g_optMu);
  return g_opt;
}

bool InRange(const game::Pov& cam, Vec p, float maxDist, float radius = 0) {
  return Len(p - cam.loc) - radius <= maxDist;
}

std::string Shorten(std::string s, size_t n = 40) { return s.size() > n ? s.substr(0, n - 1) + "~" : s; }

// ------------------------------------------------------------------ characters
const std::vector<Body>& BodiesOf(uintptr_t asset) {
  auto it = g_assets.find(asset);
  if (it != g_assets.end()) return it->second;
  std::vector<Body> out;
  ue3::TArrayRaw setups{};
  if (ReadArr(asset, "BodySetup", setups))
    for (int i = 0; i < setups.num && i < 256; ++i) {
      uintptr_t bs = util::ReadOr<uintptr_t>(setups.data + i * 4);
      if (!ue3::IsObject(bs) || GetBool(bs, "bNoCollision")) continue;
      Body b;
      if (!ReadAt(bs, "BoneName", b.bone)) continue;
      game::Field g = game::FieldOf(bs, "AggGeom");
      if (!g.ok()) continue;
      // KAggregateGeom: SphereElems +0 (0x48 each), BoxElems +0xC (0x50), SphylElems +0x18 (0x4C); TM first
      struct Kind {
        int off, size, type;
      };
      for (Kind k : {Kind{0x0, 0x48, 0}, Kind{0xC, 0x50, 1}, Kind{0x18, 0x4C, 2}}) {
        ue3::TArrayRaw a{};
        if (!util::Read(bs + g.off + k.off, a) || a.num <= 0 || a.num > 32 || !a.data) continue;
        for (int e = 0; e < a.num; ++e) {
          uintptr_t p = a.data + e * k.size;
          float m[16], v[3] = {0, 0, 0};
          if (!util::SafeRead(p, m, sizeof m) || !util::SafeRead(p + 0x40, v, k.type == 1 ? 12 : k.type == 2 ? 8 : 4))
            continue;
          b.elems.push_back({k.type, FromMatrix(m), v[0], v[1], v[2]});
        }
      }
      if (!b.elems.empty()) out.push_back(std::move(b));
    }
  return g_assets[asset] = std::move(out);
}

int BoneIndex(uintptr_t mesh, uintptr_t skel, const ue3::FNameRaw& bone) {
  auto key = std::make_tuple(skel, bone.index, bone.number);
  auto it = g_boneIdx.find(key);
  if (it != g_boneIdx.end()) return it->second;
  int idx = -1;
  engine::Call c(mesh, "MatchRefBone");
  if (c.Valid()) {
    c.Raw(c.ParamName(0).c_str(), &bone, sizeof bone);
    if (c.Invoke()) idx = c.RetInt();
  }
  return g_boneIdx[key] = idx;
}

int DrawCharacter(const Sink& s, uintptr_t pawn, bool moveCapsule) {
  int n = 0;
  uintptr_t mesh = GetObj(pawn, "Mesh");
  uintptr_t asset = mesh ? GetObj(mesh, "PhysicsAsset") : 0;
  uintptr_t skel = mesh ? GetObj(mesh, "SkeletalMesh") : 0;
  ue3::TArrayRaw bases{};
  float l2w[16];
  game::Field lf = mesh ? game::FieldOf(mesh, "LocalToWorld") : game::Field{};
  if (asset && skel && lf.ok() && util::SafeRead(mesh + lf.off, l2w, sizeof l2w) && ReadArr(mesh, "SpaceBases", bases)) {
    Xf comp = FromMatrix(l2w);
    for (const Body& b : BodiesOf(asset)) {
      int bi = BoneIndex(mesh, skel, b.bone);
      if (bi < 0 || bi >= bases.num) continue;
      struct {
        float q[4];
        Vec t;
        float s;
      } atom;
      if (!util::Read(bases.data + bi * 0x20, atom)) continue;
      Xf bone = Then(FromQuat(atom.q, atom.t, atom.s != 0 ? atom.s : 1.0f), comp);
      for (const Elem& e : b.elems) {
        Xf t = Then(e.tm, bone);
        if (e.type == 0) Sphere(s, t, {}, e.a);
        else if (e.type == 1) Box(s, t, {}, Vec{e.a, e.b, e.c} * 0.5f);
        else Capsule(s, t, {0, 0, -e.b * 0.5f}, {0, 0, e.b * 0.5f}, e.a);
        ++n;
      }
    }
  }
  if (moveCapsule) {
    // NTPawn.worldCollision { bool UprightCapsule; Vector LocationOffset; float CollisionHeight, CollisionRadius }
    game::Field wc = game::FieldOf(pawn, "worldCollision");
    Vec loc{}, off{};
    float hr[2];
    if (wc.ok() && ActorLoc(pawn, loc) && util::Read(pawn + wc.off + 4, off) && util::SafeRead(pawn + wc.off + 0x10, hr, 8) &&
        hr[1] > 1 && hr[0] > 1 && hr[0] < 5000) {
      Sink dim{s.out, WithAlpha(s.col, 0.45f)};
      // the character's location is at its feet (DmC pawns): the capsule stands on it
      Cylinder(dim, loc + off + Vec{0, 0, hr[0]}, hr[1], hr[0]);
      ++n;
    }
  }
  return n;
}

// ------------------------------------------------------------------ strike volumes
// One strike volume at a location/rotation (the strike process's m_Location / m_Rotation; projectiles: the same
// values their Tick passes to IntersectCheck).
bool VolumeLines(const Sink& s, uintptr_t vol, Vec loc, const int rot[3], std::string* info = nullptr) {
  if (!ue3::IsObject(vol)) return false;
  uint32_t f = Flags(vol);
  Xf base = FromRot(rot, loc);
  char b[256];
  if (f & kVolCapsule) {
    // the capsule starts at the (offset) location and reaches Length forward (GetThreatCylinder: range =
    // Length + the attacker's radius); projectiles centre it themselves (GetStrikeVolumeLocation)
    float r = GetFloat(vol, "Radius"), len = GetFloat(vol, "Length");
    Vec offLoc{};
    int offRot[3] = {0, 0, 0};
    ReadAt(vol, "m_LocationOffset", offLoc);
    game::Field fr = game::FieldOf(vol, "m_RotationOffset");
    if (fr.ok()) util::SafeRead(vol + fr.off, offRot, 12);
    Xf t = Then(FromRot(offRot, offLoc), base);
    Capsule(s, t, {0, 0, 0}, {len, 0, 0}, r);
    if (info) snprintf(b, sizeof b, "capsule r %.0f len %.0f off (%.0f %.0f %.0f) rot (%d %d %d)", r, len, offLoc.x, offLoc.y, offLoc.z, offRot[0], offRot[1], offRot[2]);
  } else if (f & kVolArc) {
    float r = GetFloat(vol, "Range"), ang = GetFloat(vol, "Angle"), h = GetFloat(vol, "Height"), w = GetFloat(vol, "BoxWidth");
    ArcVolume(s, base, r, std::clamp(ang, 0.5f, 360.0f) * 0.5f * kPi / 180.0f, w, h);
    if (info) snprintf(b, sizeof b, "arc range %.0f angle %.0f height %.0f boxwidth %.0f", r, ang, h, w);
  } else if (f & kVolCone) {
    float h = GetFloat(vol, "m_Height"), rf = GetFloat(vol, "m_FarRadius"), rn = GetFloat(vol, "m_NearRadius");
    Cone(s, base, h, rn, rf);
    if (info) snprintf(b, sizeof b, "cone h %.0f near %.0f far %.0f", h, rn, rf);
  } else if (f & kVolTwoArcs) {
    bool any = false;
    std::string sub;
    for (const char* which : {"One", "Two"}) {
      char fa[48], fo[48];
      snprintf(fa, sizeof fa, "m_Arc%s", which);
      snprintf(fo, sizeof fo, "m_Arc%sRotationOffset", which);
      int off[3] = {0, 0, 0};
      game::Field fr = game::FieldOf(vol, fo);
      if (fr.ok()) util::SafeRead(vol + fr.off, off, 12);
      int r2[3] = {rot[0] + off[0], rot[1] + off[1], rot[2] + off[2]};  // Add_RotatorRotator, as its DrawDebug
      std::string si;
      any |= VolumeLines(s, GetObj(vol, fa), loc, r2, info ? &si : nullptr);
      sub += " [" + si + "]";
    }
    if (info) snprintf(b, sizeof b, "two arcs%s", sub.c_str());
    if (!any) return false;
  } else {
    if (info) *info = "unknown volume " + ue3::ClassNameOf(vol);
    return false;
  }
  if (info) *info = b;
  return true;
}

// Dev probe: what the game's own natives say about a volume at this location (arc: the PhysX box it queries;
// capsule: the location/rotation after its offsets).
void Probe(uintptr_t vol, Vec loc, const int rot[3], const std::string& mine) {
  uint32_t f = Flags(vol);
  std::string extra;
  char b[256];
  if (f & kVolArc) {
    engine::Call c(vol, "GetPhysXCollisionParameters");
    if (c.Valid()) {
      c.Raw(c.ParamName(2).c_str(), &loc, 12).Raw(c.ParamName(3).c_str(), rot, 12);
      if (c.Invoke()) {
        const float* ce = (const float*)c.ParamData(0);
        const float* ex = (const float*)c.ParamData(1);
        const float* al = (const float*)c.ParamData(2);
        const int* ar = (const int*)c.ParamData(3);
        snprintf(b, sizeof b, " | physx centre (%.0f %.0f %.0f) extent (%.0f %.0f %.0f) arcloc (%.0f %.0f %.0f) arcrot (%d %d %d)",
                 ce[0] - loc.x, ce[1] - loc.y, ce[2] - loc.z, ex[0], ex[1], ex[2], al[0] - loc.x, al[1] - loc.y, al[2] - loc.z, ar[0], ar[1], ar[2]);
        extra = b;
      } else {
        extra = " | physx call failed: " + c.Error();
      }
    }
  } else if (f & kVolCapsule) {
    engine::Call c(vol, "TransformWithOffsets");
    if (c.Valid()) {
      c.Raw(c.ParamName(0).c_str(), &loc, 12).Raw(c.ParamName(1).c_str(), rot, 12);
      if (c.Invoke()) {
        const float* l = (const float*)c.ParamData(0);
        const int* r = (const int*)c.ParamData(1);
        snprintf(b, sizeof b, " | game offsets -> loc (%.1f %.1f %.1f) rot (%d %d %d)", l[0] - loc.x, l[1] - loc.y, l[2] - loc.z, r[0], r[1], r[2]);
        extra = b;
      }
    }
  }
  LOG("visuals: probe %s at (%.0f %.0f %.0f) rot (%d %d %d): %s%s", ue3::PathName(vol).c_str(), loc.x, loc.y, loc.z, rot[0], rot[1], rot[2],
      mine.c_str(), extra.c_str());
}

std::string StrikeLabel(uintptr_t proc) {
  // DVCombat_StrikeDef lives inside its action: Dante_Melee.Actions.Action_Dante_Rebellion_Combo1.DVCombat_StrikeDef_0
  uintptr_t def = GetObj(proc, "m_Def");
  std::string a = def ? ue3::ObjName(ue3::ObjOuter(def)) : "";
  for (const char* pre : {"Action_Dante_", "Action_", "R_Attack_", "Attack_"})
    if (a.rfind(pre, 0) == 0) {
      a = a.substr(strlen(pre));
      break;
    }
  return Shorten(a, 32);
}

bool IsDante(uintptr_t owner) { return owner && owner == game::W().pawn; }

void AddGhost(uintptr_t key, uintptr_t owner, uintptr_t vol, Vec loc, const int rot[3], const std::string& label) {
  Ghost g;
  g.key = key;
  g.t = g_clock;
  g.col = IsDante(owner) ? Color(Cat::Hitbox) : EnemyHitboxColor();
  g.label = label;
  g.labelAt = loc;
  Sink s{&g.lines, g.col};
  std::string info;
  bool probe = g_probe > 0 && g_probed.insert(key).second;
  if (!VolumeLines(s, vol, loc, rot, probe ? &info : nullptr)) {
    if (probe) LOG("visuals: probe %s: no shape (%s)", ue3::ObjName(key).c_str(), info.c_str());
    return;
  }
  if (probe) {
    --g_probe;
    LOG("visuals: strike %s owner %s label '%s'", ue3::ObjName(key).c_str(), ue3::ObjName(owner).c_str(), label.c_str());
    Probe(vol, loc, rot, info);
  }
  g_ghosts.push_back(std::move(g));
  if (g_ghosts.size() > 600) g_ghosts.pop_front();
}

void CaptureStrike(uintptr_t proc, bool force) {
  auto it = g_strikes.find(proc);
  if (it == g_strikes.end()) return;
  // one shape per 1/60 s of game time is enough for the trail (high frame rates would pile up copies)
  if (!force && it->second.lastCapture >= 0 && g_clock - it->second.lastCapture < 1.0 / 60.0) return;
  uintptr_t vol = GetObj(proc, "m_StrikeVolume");
  Vec loc{};
  int rot[3];
  game::Field fr = game::FieldOf(proc, "m_Rotation");
  if (!vol || !ReadAt(proc, "m_Location", loc) || !fr.ok() || !util::SafeRead(proc + fr.off, rot, 12)) return;
  if (loc.x == 0 && loc.y == 0 && loc.z == 0) return;  // not placed yet
  it->second.lastCapture = g_clock;
  AddGhost(proc, it->second.owner, vol, loc, rot, it->second.label);
}

void CaptureProjectile(uintptr_t proj) {
  double& last = g_projectiles[proj];
  if (last > 0 && g_clock - last < 1.0 / 60.0) return;
  last = g_clock;
  uintptr_t vol = GetObj(proj, "m_StrikeVolume");
  if (!vol) vol = GetObj(proj, "m_StrikeVolume_Auto");
  Vec loc{};
  int rot[3];
  game::Field fr = game::FieldOf(proj, "Rotation");
  if (!vol || !ActorLoc(proj, loc) || !fr.ok() || !util::SafeRead(proj + fr.off, rot, 12)) {
    if (g_probe > 0 && g_probed.insert(proj).second)
      LOG("visuals: probe projectile %s: no strike volume (m_StrikeVolume %s, auto %s)", ue3::ObjName(proj).c_str(),
          ue3::ObjName(GetObj(proj, "m_StrikeVolume")).c_str(), ue3::ObjName(GetObj(proj, "m_StrikeVolume_Auto")).c_str());
    return;
  }
  // NTCombat_Projectile.GetStrikeVolumeRotation / GetStrikeVolumeLocation: the automatic capsule stands up
  // (pitch +90) and is centred on the projectile
  uintptr_t params = GetObj(proj, "m_Parameters");
  if (!(params && GetBool(params, "m_UseCustomStrikeVolume"))) {
    rot[0] += 16384;
    loc = loc - FromRot(rot).x * (GetFloat(vol, "Length") * 0.5f);
  }
  AddGhost(proj, GetObj(proj, "m_OwningPawn"), vol, loc, rot, "");
}

void AddMarker(Vec p, bool dante) {
  for (const Marker& m : g_markers)
    if (g_clock - m.t < 0.1 && Len(m.p - p) < 8) return;  // the same hit reported twice
  g_markers.push_back({p, g_clock, dante});
  if (g_markers.size() > 200) g_markers.pop_front();
}

// Middle of a character's mesh bounds (chest height), else its location.
Vec MidOf(uintptr_t actor) {
  Vec p{};
  ActorLoc(actor, p);
  if (uintptr_t mesh = GetObj(actor, "Mesh")) {
    Vec o{};
    if (ReadAt(mesh, "Bounds", o) && Len(o - p) < 1000) p = o;
  }
  return p;
}

// Every hit ends in the victim's NTCombat_Nucleus.ContactInternal. Hits that came from a strike volume or a
// projectile were seen just before (Callback_Contacted / StrikeCallback in the same frame); the others are
// "direct": Ebony & Ivory at a locked target, grabs, some special moves. They have no hitbox: a line from the
// attacker to the victim shows them.
void OnContact(uintptr_t nucleus, uintptr_t contact, bool markers, bool hitbox) {
  uintptr_t attacker = GetObj(contact, "OriginatorActor"), victim = GetObj(nucleus, "Owner");
  if (!victim) return;
  Vec p{};
  bool point = GetBool(contact, "bContactPointActive") && ReadAt(contact, "ContactPointOnContactee", p);
  bool direct = !(g_lastVolumeHit.t == g_clock && g_lastVolumeHit.owner == attacker);
  if (markers && (point || direct)) AddMarker(point ? p : MidOf(victim), IsDante(attacker));
  if (!hitbox || !direct || !attacker || attacker == victim) return;
  Ghost g;
  g.key = contact;
  g.t = g_clock;
  g.col = IsDante(attacker) ? Color(Cat::Hitbox) : EnemyHitboxColor();
  Vec a = MidOf(attacker), b = point ? p : MidOf(victim);
  Sink s{&g.lines, g.col};
  s.L(a, b);
  Xf id;
  Box(s, id, b, {6, 6, 6});
  std::string style = game::GetName(contact, "StyleDisplayName");
  g.label = (style.empty() ? std::string("Direct hit") : style) + " (no hitbox)";
  g.labelAt = b;
  g.minLife = 0.4f;
  g_ghosts.push_back(std::move(g));
}

// ------------------------------------------------------------------ triggers
// What a Kismet event leads to: breadth-first through output links, sub-sequences and remote events; the
// nearest arena / cutscene action decides.
TKind Classify(uintptr_t ev, std::string& path, std::vector<std::string>* visited = nullptr) {
  std::deque<std::pair<uintptr_t, int>> q{{ev, 0}};
  std::unordered_set<uintptr_t> seen{ev};
  auto push = [&](uintptr_t op, int d) {
    if (op && d < 40 && seen.size() < 600 && seen.insert(op).second && ue3::IsObject(op)) q.push_back({op, d});
  };
  auto pushOutputs = [&](uintptr_t op, int d) {
    ue3::TArrayRaw outs{};
    if (!ReadArr(op, "OutputLinks", outs)) return;
    for (int i = 0; i < outs.num && i < 32; ++i) {
      ue3::TArrayRaw links{};  // SeqOpOutputLink (0x38): Links TArray<SeqOpOutputInputLink (8)> first
      if (!util::Read(outs.data + i * 0x38, links) || links.num <= 0 || links.num > 64) continue;
      for (int k = 0; k < links.num; ++k) push(util::ReadOr<uintptr_t>(links.data + k * 8), d + 1);
    }
  };
  while (!q.empty()) {
    auto [op, d] = q.front();
    q.pop_front();
    if (visited) visited->push_back(ue3::ClassNameOf(op));
    uint32_t f = Flags(op);
    if (f & kArena) {
      path = ue3::ClassNameOf(op);
      return TKind::Arena;
    }
    if (f & kCutscene) {
      path = ue3::ClassNameOf(op);
      return TKind::Cutscene;
    }
    if (f & kInterp) {
      uintptr_t data = GetObj(op, "InterpData");
      ue3::TArrayRaw groups{};
      if (data && ReadArr(data, "InterpGroups", groups))
        for (int i = 0; i < groups.num && i < 128; ++i)
          if (Flags(util::ReadOr<uintptr_t>(groups.data + i * 4)) & kDirector) {
            path = "matinee with camera cuts " + ue3::ObjName(op);
            return TKind::Cutscene;
          }
    }
    if (f & kSequence) {  // into a sub-sequence: its "Sequence Activated" events
      ue3::TArrayRaw objs{};
      if (op != ev && ReadArr(op, "SequenceObjects", objs))
        for (int i = 0; i < objs.num && i < 4000; ++i) {
          uintptr_t o = util::ReadOr<uintptr_t>(objs.data + i * 4);
          if (o && (Flags(o) & kSeqActivated)) push(o, d + 1);
        }
    }
    if (f & kFinishSequence) pushOutputs(GetObj(op, "ParentSequence"), d);  // back out to the parent's outputs
    if (f & kActivateRemote) {
      ue3::FNameRaw n{};
      if (ReadAt(op, "EventName", n)) {
        auto range = g_scan.remote.equal_range(n.index);
        for (auto it = range.first; it != range.second; ++it) push(it->second, d + 1);
      }
    }
    pushOutputs(op, d);
  }
  path = "nothing found in " + std::to_string(seen.size()) + " script nodes";
  return TKind::Other;
}

// The levels that are loaded and visible now: the persistent level + visible streaming levels.
std::unordered_set<uintptr_t> VisibleLevels(const game::World& w) {
  std::unordered_set<uintptr_t> out;
  if (!w.world) return out;
  out.insert(ue3::ObjOuter(w.world));
  ue3::TArrayRaw a{};
  if (ReadArr(w.world, "StreamingLevels", a))
    for (int i = 0; i < a.num && i < 512; ++i) {
      uintptr_t ls = util::ReadOr<uintptr_t>(a.data + i * 4);
      if (!ue3::IsObject(ls) || !GetBool(ls, "bIsVisible")) continue;
      if (uintptr_t lvl = GetObj(ls, "LoadedLevel")) out.insert(lvl);
    }
  return out;
}

// GObjects walk in slices (~25k objects per frame), then classification of the touch events found.
void ScanTick(const game::World& w) {
  ULONGLONG now = GetTickCount64();
  if (!g_scan.running) {
    if (now < g_nextScan) return;
    g_scan = Scan{};
    g_scan.running = true;
  }
  int n = ue3::NumObjects();
  int end = std::min(n, g_scan.next + 25000);
  for (int i = g_scan.next; i < end; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o) continue;
    uint32_t f = Flags(o);
    if (!(f & (kSequenceEvent | kWall))) continue;
    if (ue3::ObjName(o).rfind("Default__", 0) == 0) continue;
    if (f & kWall) g_scan.walls.push_back(o);
    if (f & kTouchEvent) g_scan.events.push_back(o);
    if (f & kRemoteEvent) {
      ue3::FNameRaw nm{};
      if (ReadAt(o, "EventName", nm)) g_scan.remote.insert({nm.index, o});
    }
  }
  g_scan.next = end;
  if (end < n) return;
  g_scan.running = false;
  g_nextScan = now + 3000;  // streaming levels come and go: look again every few seconds

  std::unordered_set<uintptr_t> levels = VisibleLevels(w);
  g_walls.clear();
  for (uintptr_t a : g_scan.walls)
    if (game::Alive(a) && levels.count(ue3::ObjOuter(a))) g_walls.push_back(a);
  std::vector<Trigger> out;
  for (uintptr_t ev : g_scan.events) {
    uintptr_t orig = GetObj(ev, "Originator");
    if (!orig || !game::Alive(orig) || GetBool(orig, "bDeleteMe") || !levels.count(ue3::ObjOuter(orig))) continue;
    Trigger t;
    t.ev = ev;
    t.orig = orig;
    // keep the classification of triggers we already know (the level script does not change)
    auto old = std::find_if(g_triggers.begin(), g_triggers.end(), [&](const Trigger& x) { return x.ev == ev; });
    if (old != g_triggers.end()) {
      t.kind = old->kind;
      t.path = old->path;
    } else {
      t.kind = Classify(ev, t.path);
    }
    std::string name = game::GetStr(ev, "ObjComment");
    if (name.empty()) name = ue3::ObjName(orig);
    // an encounter starts the fight; wave actions add enemies to a fight (often a later part of the same arena)
    const char* kind = t.kind == TKind::Cutscene                                      ? "Cutscene"
                       : t.kind == TKind::Arena && t.path.find("Wave") != std::string::npos ? "Arena wave"
                       : t.kind == TKind::Arena                                         ? "Arena"
                                                                                        : "Trigger";
    t.label = std::string(kind) + ": " + Shorten(name, 36);
    out.push_back(std::move(t));
  }
  if (out.size() != g_triggers.size()) LOG("visuals: %zu level triggers on %s", out.size(), w.map.c_str());
  g_triggers = std::move(out);
}

// Grid lines on one flat face (points of a convex polygon in local space, normal n): lines every ~1 m along the
// face's horizontal and vertical directions, clipped to the face.
void FaceGrid(const std::vector<Vec>& pts, Vec n, std::vector<std::pair<Vec, Vec>>& out) {
  Vec u = std::fabs(n.z) < 0.7f ? Norm(Cross(Vec{0, 0, 1}, n)) : Norm(Vec{1, 0, 0} - n * n.x);
  Vec v = Cross(n, u);
  float d = Dot(n, pts[0]);
  using P2 = std::pair<float, float>;
  std::vector<P2> p2;
  for (const Vec& p : pts) p2.push_back({Dot(p, u), Dot(p, v)});
  // convex hull (monotone chain): the face's outline, whatever order its triangles came in
  std::sort(p2.begin(), p2.end());
  p2.erase(std::unique(p2.begin(), p2.end()), p2.end());
  if (p2.size() < 3) return;
  auto cross = [](const P2& o, const P2& a, const P2& b) {
    return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
  };
  std::vector<P2> h(2 * p2.size());
  size_t k = 0;
  for (size_t i = 0; i < p2.size(); ++i) {
    while (k >= 2 && cross(h[k - 2], h[k - 1], p2[i]) <= 0) --k;
    h[k++] = p2[i];
  }
  for (size_t i = p2.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && cross(h[k - 2], h[k - 1], p2[i - 1]) <= 0) --k;
    h[k++] = p2[i - 1];
  }
  h.resize(k - 1);
  if (h.size() < 3) return;
  auto to3 = [&](float x, float y) { return n * d + u * x + v * y; };
  for (int axis = 0; axis < 2; ++axis) {
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (const P2& q : h) {
      float c = axis ? q.second : q.first;
      lo = std::min(lo, c);
      hi = std::max(hi, c);
    }
    float step = std::max(100.0f, (hi - lo) / 40.0f);  // 1 m, at most ~40 lines across a face
    for (float c = std::ceil((lo + 5) / step) * step; c < hi - 5; c += step) {
      float t0 = FLT_MAX, t1 = -FLT_MAX;
      for (size_t e = 0; e < h.size(); ++e) {
        const P2& a = h[e];
        const P2& b = h[(e + 1) % h.size()];
        float ac = axis ? a.second : a.first, bc = axis ? b.second : b.first;
        if ((ac - c) * (bc - c) > 0 || ac == bc) continue;
        float t = (c - ac) / (bc - ac);
        float o = axis ? a.first + (b.first - a.first) * t : a.second + (b.second - a.second) * t;
        t0 = std::min(t0, o);
        t1 = std::max(t1, o);
      }
      if (t1 - t0 < 1) continue;
      if (axis) out.push_back({to3(t0, c), to3(t1, c)});
      else out.push_back({to3(c, t0), to3(c, t1)});
    }
  }
}

const ConvexShape& ConvexOf(uintptr_t brush) {
  auto it = g_convex.find(brush);
  if (it != g_convex.end()) return it->second;
  ConvexShape shape;
  game::Field g = game::FieldOf(brush, "BrushAggGeom");
  ue3::TArrayRaw cv{};
  if (g.ok() && util::Read(brush + g.off + 0x24, cv) && cv.num > 0 && cv.num < 64 && cv.data) {
    for (int e = 0; e < cv.num; ++e) {
      // KConvexElem (0x64): VertexData TArray<Vector> +0, FaceTriData TArray<int> +0x18
      ue3::TArrayRaw vs{}, tris{};
      uintptr_t p = cv.data + e * 0x64;
      if (!util::Read(p, vs) || !util::Read(p + 0x18, tris) || vs.num <= 0 || vs.num > 4096 || tris.num % 3 || tris.num > 30000) continue;
      std::vector<Vec> v(vs.num);
      std::vector<int> idx(tris.num);
      if (!util::SafeRead(vs.data, v.data(), v.size() * 12) || !util::SafeRead(tris.data, idx.data(), idx.size() * 4)) continue;
      // keep only edges between faces that are not coplanar (no triangle diagonals); group triangles into faces
      std::map<std::pair<int, int>, std::vector<Vec>> normals;
      struct Face {
        Vec n;
        float d;
        std::vector<Vec> pts;
      };
      std::vector<Face> faces;
      for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        int a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a < 0 || b < 0 || c < 0 || a >= vs.num || b >= vs.num || c >= vs.num) continue;
        Vec nrm = Norm(Cross(v[b] - v[a], v[c] - v[a]));
        for (auto [i, j] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) normals[{std::min(i, j), std::max(i, j)}].push_back(nrm);
        float dist = Dot(nrm, v[a]);
        auto fc = std::find_if(faces.begin(), faces.end(),
                               [&](const Face& x) { return Dot(x.n, nrm) > 0.999f && std::fabs(x.d - dist) < 1; });
        if (fc == faces.end()) {
          faces.push_back({nrm, dist, {}});
          fc = faces.end() - 1;
        }
        for (int i : {a, b, c}) fc->pts.push_back(v[i]);
      }
      for (auto& [k, ns] : normals)
        if (ns.size() != 2 || Dot(ns[0], ns[1]) < 0.999f) shape.edges.push_back({v[k.first], v[k.second]});
      for (const Face& fc : faces) FaceGrid(fc.pts, fc.n, shape.grid);
    }
  }
  return g_convex[brush] = std::move(shape);
}

// Shape of a trigger / wall actor: its collision component (cylinder / brush), else its bounds. fade: the farther
// from the camera, the fainter (walls of other rooms show through the near ones otherwise).
// grid: the 1 m grid on the faces too (walls nearer than 25 m).
bool DrawVolume(const Sink& sink, uintptr_t actor, const game::Pov& cam, float maxDist, Vec& top, bool fade = false,
                bool grid = false) {
  uintptr_t comp = GetObj(actor, "CollisionComponent");
  if (!comp) return false;
  // PrimitiveComponent.Bounds = BoxSphereBounds { Origin, BoxExtent, SphereRadius }
  struct {
    Vec o, e;
    float r;
  } bounds;
  if (!ReadAt(comp, "Bounds", bounds) || bounds.r <= 0 || bounds.r > 200000) return false;
  if (!InRange(cam, bounds.o, maxDist, bounds.r)) return false;
  top = bounds.o + Vec{0, 0, bounds.e.z};
  Sink s = sink;
  float dist = std::max(0.0f, Len(bounds.o - cam.loc) - bounds.r);
  if (fade) s.col = WithAlpha(sink.col, std::clamp(1.0f - 0.85f * dist / std::max(maxDist, 1.0f), 0.15f, 1.0f));
  uint32_t f = Flags(comp);
  if (f & kCylinderComp) {
    float h = GetFloat(comp, "CollisionHeight"), r = GetFloat(comp, "CollisionRadius");
    Vec loc{};
    ActorLoc(actor, loc);
    Cylinder(s, loc, r, h);
    return true;
  }
  if (f & kBrushComp) {
    float m[16];
    game::Field lf = game::FieldOf(comp, "LocalToWorld");
    const ConvexShape& shape = ConvexOf(comp);
    if (!shape.edges.empty() && lf.ok() && util::SafeRead(comp + lf.off, m, sizeof m)) {
      Xf t2 = FromMatrix(m);
      for (auto& [a, b] : shape.edges) s.L(t2.P(a), t2.P(b));
      if (grid && dist < 2500) {
        Sink gs = s;
        gs.col = WithAlpha(s.col, 0.45f);
        for (auto& [a, b] : shape.grid) gs.L(t2.P(a), t2.P(b));
      }
      return true;
    }
  }
  Xf id;
  Box(s, id, bounds.o, bounds.e);
  return true;
}

void Publish(std::shared_ptr<Frame> f, const Stats& st) {
  {
    std::lock_guard<std::mutex> lk(g_frameMu);
    g_frame = std::move(f);
  }
  std::lock_guard<std::mutex> lk(g_statMu);
  float drawMs = g_stats.drawMs;
  int hidden = g_stats.labelsHidden;
  g_stats = st;
  g_stats.drawMs = drawMs;
  g_stats.labelsHidden = hidden;
}

bool AnyOn() {
  for (int i = 0; i < kCats; ++i)
    if (g_on[i]) return true;
  return false;
}

void ClearWorld() {
  g_assets.clear();
  g_boneIdx.clear();
  g_ghosts.clear();
  g_strikes.clear();
  g_projectiles.clear();
  g_markers.clear();
  g_triggers.clear();
  g_walls.clear();
  g_trigCount.clear();
  g_fired.clear();
  g_convex.clear();
  g_scan = Scan{};
  g_nextScan = 0;
}

}  // namespace

// ------------------------------------------------------------------ public
uint32_t Color(Cat c) {
  switch (c) {
    case Cat::Enemy: return Rgba(255, 150, 40);
    case Cat::Player: return Rgba(70, 200, 255);
    case Cat::Hitbox: return Rgba(255, 80, 210);  // Dante's attacks
    case Cat::HitMarker: return Rgba(255, 255, 255);
    case Cat::Cutscene: return Rgba(175, 120, 255);
    case Cat::Arena: return Rgba(70, 235, 100);
    case Cat::OtherTrigger: return Rgba(175, 175, 175);
    case Cat::Walls: return Rgba(255, 225, 40);
    default: return Rgba(255, 255, 255);
  }
}
uint32_t EnemyHitboxColor() { return Rgba(255, 50, 50); }
uint32_t HitOnDanteColor() { return Rgba(255, 130, 130); }

void SetOn(Cat c, bool on) { g_on[(int)c] = on; }
bool On(Cat c) { return g_on[(int)c]; }
void SetMoveCapsule(bool enemies, bool on) { g_moveCapsule[enemies ? 0 : 1] = on; }
void SetLinger(float s) { g_linger = std::clamp(s, 0.0f, 3.0f); }
void SetWallGrid(bool on) { g_wallGrid = on; }
void SetOcclusionLive(bool ok) { g_occLive = ok; }
bool OcclusionLive() { return g_occLive; }
Occluded& OccludedLines() { return g_occ; }
void SetGroupVisible(uint32_t group, bool v) { g_groupVisible[group] = v; }
void SetOccludeAll(bool on) { g_occludeAll = on; }
void SetDepthTestPlane(float m) { g_testPlane = m; }
bool LastCamera(game::Pov& out) {
  out = g_lastCam;
  return g_lastCamOk;
}

Options GetOptions() { return Opt(); }
void SetOptions(const Options& o) {
  Options c = o;
  c.maxDistance = std::clamp(c.maxDistance, 5.0f, 200.0f);
  c.thickness = std::clamp(c.thickness, 1.0f, 5.0f);
  std::lock_guard<std::mutex> lk(g_optMu);
  g_opt = c;
}
void LoadJson(const nlohmann::json& j) {
  Options o;
  o.labels = j.value("labels", o.labels);
  o.maxDistance = j.value("max_distance", o.maxDistance);
  o.thickness = j.value("thickness", o.thickness);
  o.occlude = j.value("occlude", o.occlude);
  SetOptions(o);
}
nlohmann::json SaveJson() {
  Options o = Opt();
  return {{"labels", o.labels}, {"max_distance", o.maxDistance}, {"thickness", o.thickness}, {"occlude", o.occlude}};
}

bool WantsEvents() { return g_on[(int)Cat::Hitbox] || g_on[(int)Cat::HitMarker]; }

void OnEvent(uintptr_t self, uintptr_t fn, void* parms, bool after) {
  if (!WantsEvents() || !game::W().mission) return;
  if (!g_names.ok) {
    g_names.Resolve();
    if (!g_names.ok) return;
  }
  int n = ue3::ObjNameIndex(fn);
  bool hitbox = g_on[(int)Cat::Hitbox];
  if (!after) {
    if (n == g_names.started && hitbox && (Flags(self) & kStrikeProc)) {
      Strike st;
      st.owner = GetObj(self, "m_OwningPawn");
      st.label = StrikeLabel(self);
      g_strikes[self] = st;
    } else if (n == g_names.stopped && (Flags(self) & kStrikeProc)) {
      CaptureStrike(self, true);  // the last position, if no frame captured it yet
      g_strikes.erase(self);
    } else if (parms) {
      bool markers = g_on[(int)Cat::HitMarker];
      // where the strike volume met the victim's body: NTCombat_IntersectInfo.IntersectionPoint (+0xC, valid +0x18)
      if (n == g_names.contacted) {
        ue3::TArrayRaw a = *reinterpret_cast<const ue3::TArrayRaw*>(parms);
        uintptr_t proc = *reinterpret_cast<const uintptr_t*>((const uint8_t*)parms + 0x10);
        uintptr_t owner = ue3::IsObject(proc) ? GetObj(proc, "m_OwningPawn") : 0;
        g_lastVolumeHit = {g_clock, owner};
        for (int i = 0; markers && i < a.num && i < 32 && a.data; ++i) {
          uint32_t valid = util::ReadOr<uint32_t>(a.data + i * 0x28 + 0x18);
          Vec p{};
          if ((valid & 1) && util::Read(a.data + i * 0x28 + 0xC, p)) AddMarker(p, IsDante(owner));
        }
      } else if (n == g_names.strikeCb && (Flags(self) & kProjectile)) {
        uintptr_t owner = GetObj(self, "m_OwningPawn");
        g_lastVolumeHit = {g_clock, owner};
        uint32_t valid = *reinterpret_cast<const uint32_t*>((const uint8_t*)parms + 0x18);
        if (markers && (valid & 1)) AddMarker(*reinterpret_cast<const Vec*>((const uint8_t*)parms + 0xC), IsDante(owner));
      } else if (n == g_names.contact) {
        uintptr_t contact = *reinterpret_cast<const uintptr_t*>(parms);
        if (ue3::IsObject(contact)) OnContact(self, contact, markers, hitbox);
      }
    }
  } else if (hitbox) {
    if (n == g_names.processTick) {
      if (g_strikes.count(self)) CaptureStrike(self, false);
    } else if (n == g_names.tick && (Flags(self) & kProjectile)) {
      CaptureProjectile(self);
    }
  }
}

void OnFrame(const game::World& w) {
  if (!AnyOn() || !w.mission) {
    if (g_gen != ~0u) {
      ClearWorld();
      g_gen = ~0u;
      Publish(std::make_shared<Frame>(), Stats{});
    }
    return;
  }
  LARGE_INTEGER q0, q1, qf;
  QueryPerformanceCounter(&q0);
  if (w.gen != g_gen) {  // another map, checkpoint or restart: everything game-side is new
    ClearWorld();
    g_gen = w.gen;
  }
  g_clock += w.gameDt;
  auto f = std::make_shared<Frame>();
  Stats st;
  if (!game::CameraPov(w, f->cam)) {
    Publish(f, st);
    return;
  }
  Options opt = Opt();
  float maxDist = opt.maxDistance * 100.0f;  // metres -> game units
  f->valid = true;
  f->at = GetTickCount64();

  // characters
  auto character = [&](uintptr_t pawn, Cat c, bool capsule) {
    Vec loc{};
    if (!pawn || !ActorLoc(pawn, loc) || !InRange(f->cam, loc, maxDist, 300)) return;
    st.bodies += DrawCharacter(Sink{&f->lines, Color(c)}, pawn, capsule);
  };
  if (g_on[(int)Cat::Enemy])
    for (uintptr_t e : enemies::ActiveEnemies(w)) character(e, Cat::Enemy, g_moveCapsule[0]);
  if (g_on[(int)Cat::Player]) character(w.pawn, Cat::Player, g_moveCapsule[1]);

  // attack hitboxes: shapes captured at every check of running strikes, kept a little and faded
  float linger = g_linger;
  for (auto it = g_strikes.begin(); it != g_strikes.end();)
    it = game::Alive(it->first) ? std::next(it) : g_strikes.erase(it);
  for (auto it = g_projectiles.begin(); it != g_projectiles.end();)
    it = game::Alive(it->first) && g_clock - it->second < 0.5 ? std::next(it) : g_projectiles.erase(it);
  if (!g_on[(int)Cat::Hitbox]) g_ghosts.clear();
  std::unordered_map<uintptr_t, double> newest;
  for (const Ghost& g : g_ghosts) newest[g.key] = std::max(newest[g.key], g.t);
  std::deque<Ghost> keep;
  for (Ghost& g : g_ghosts) {
    bool active = g_strikes.count(g.key) || g_projectiles.count(g.key);
    bool latest = newest[g.key] == g.t;
    double age = g_clock - g.t;
    float life = std::max(linger, g.minLife);
    float alpha;
    if (active && latest) alpha = 1.0f;  // still checking hits
    else if (life > 0 && age <= life) alpha = 0.85f * (1.0f - (float)(age / life)) + 0.1f;
    else continue;
    if (InRange(f->cam, g.labelAt, maxDist, 500)) {
      for (const Line& l : g.lines) f->lines.push_back({l.a, l.b, WithAlpha(l.col, alpha)});
      if (opt.labels && latest && !g.label.empty()) f->labels.push_back({g.labelAt, WithAlpha(g.col, alpha), g.label});
      if (latest) ++st.strikes;
    }
    keep.push_back(std::move(g));
  }
  g_ghosts.swap(keep);

  // hit markers: a small star where the hit was counted, for a second
  if (!g_on[(int)Cat::HitMarker]) g_markers.clear();
  while (!g_markers.empty() && g_clock - g_markers.front().t > 1.0) g_markers.pop_front();
  for (const Marker& m : g_markers) {
    if (!InRange(f->cam, m.p, maxDist)) continue;
    float a = 1.0f - (float)((g_clock - m.t) / 1.0);
    Sink s{&f->lines, WithAlpha(m.dante ? Color(Cat::HitMarker) : HitOnDanteColor(), a)};
    const float r = 7;
    s.L(m.p - Vec{r, 0, 0}, m.p + Vec{r, 0, 0});
    s.L(m.p - Vec{0, r, 0}, m.p + Vec{0, r, 0});
    s.L(m.p - Vec{0, 0, r}, m.p + Vec{0, 0, r});
    Xf id;
    Sphere(s, id, m.p, 4);
    ++st.markers;
  }

  // triggers and invisible walls (one scan finds both; a level that stops being visible hides them at once)
  bool trig = g_on[(int)Cat::Cutscene] || g_on[(int)Cat::Arena] || g_on[(int)Cat::OtherTrigger];
  bool walls = g_on[(int)Cat::Walls];
  std::unordered_set<uintptr_t> levels;
  if (trig || walls) {
    ScanTick(w);
    levels = VisibleLevels(w);
  }
  if (walls) {
    uintptr_t dante = w.pawn ? ue3::ObjClass(w.pawn) : 0;
    Sink s{&f->lines, Color(Cat::Walls), opt.occlude};
    for (uintptr_t a : g_walls) {
      if (!game::Alive(a) || !levels.count(ue3::ObjOuter(a)) || GetBool(a, "bDeleteMe")) continue;
      // only what stops Dante now: collision on (dynamic walls are switched by the level script) and, for walls
      // meant for some classes only, Dante's class among them
      if (!GetBool(a, "bCollideActors") || !GetBool(a, "bBlockActors")) continue;
      ue3::TArrayRaw only{};
      if (dante && ReadArr(a, "m_aBlockedClassList", only) && only.num > 0) {
        bool blocks = false;
        for (int i = 0; i < only.num && i < 32 && !blocks; ++i) {
          uintptr_t cls = util::ReadOr<uintptr_t>(only.data + i * 4);
          blocks = cls && ue3::IsObject(cls) && ue3::IsChildOf(dante, ue3::ObjName(cls).c_str());
        }
        if (!blocks) continue;
      }
      ++st.walls;
      Vec top{};
      DrawVolume(s, a, f->cam, maxDist, top, true, g_wallGrid);
    }
  }
  if (trig) {
    bool cinematic = GetBool(w.pc, "bCinematicMode");
    int enemyCount = -1;
    for (const Trigger& t : g_triggers) {
      Cat c = t.kind == TKind::Arena ? Cat::Arena : t.kind == TKind::Cutscene ? Cat::Cutscene : Cat::OtherTrigger;
      ++st.triggers[t.kind == TKind::Cutscene ? 0 : t.kind == TKind::Arena ? 1 : 2];
      if (!game::Alive(t.ev) || !game::Alive(t.orig) || !levels.count(ue3::ObjOuter(t.orig))) continue;
      int count = GetInt(t.ev, "TriggerCount");
      auto known = g_trigCount.find(t.ev);
      if (known != g_trigCount.end() && count > known->second) {
        if (enemyCount < 0) enemyCount = (int)enemies::ActiveEnemies(w).size();
        LOG("visuals: trigger fired: %s (%s, %s) - enemies %d, cutscene %d", t.label.c_str(), ue3::ObjName(t.ev).c_str(),
            t.path.c_str(), enemyCount, cinematic);
        g_fired.push_back({t.label, GetTickCount64(), enemyCount, cinematic});
      }
      g_trigCount[t.ev] = count;
      if (!g_on[(int)c]) continue;
      // switched off by the level script for now, or fired as often as it may: drawn faint
      int maxCount = GetInt(t.ev, "MaxTriggerCount");
      bool off = !GetBool(t.ev, "bEnabled"), used = maxCount > 0 && count >= maxCount;
      uint32_t col = WithAlpha(Color(c), off || used ? 0.3f : 1.0f);
      Vec top{};
      // triggers are few and matter far away too (where the next fight starts): no distance limit
      uint32_t group = (uint32_t)t.orig;
      if (DrawVolume(Sink{&f->lines, col, opt.occlude, group}, t.orig, f->cam, FLT_MAX, top) && opt.labels)
        f->labels.push_back({top, col, used ? t.label + " (used)" : off ? t.label + " (off)" : t.label, group});
    }
    for (auto it = g_fired.begin(); it != g_fired.end();) {
      if (GetTickCount64() - it->at < 5000) {
        ++it;
        continue;
      }
      LOG("visuals: 5 s after '%s': enemies %d -> %d, cutscene %d -> %d", it->what.c_str(), it->enemies,
          (int)enemies::ActiveEnemies(w).size(), it->cinematic, cinematic);
      it = g_fired.erase(it);
    }
  } else if (!walls && !g_triggers.empty()) {  // the wall scan keeps the list: no rebuild every scan
    g_triggers.clear();
  }
  if (!trig && !walls && (g_scan.running || g_nextScan)) {
    g_scan = Scan{};
    g_nextScan = 0;
    g_walls.clear();
  }

  QueryPerformanceCounter(&q1);
  QueryPerformanceFrequency(&qf);
  st.collectMs = (float)((q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart);
  st.lines = (int)f->lines.size();
  st.labels = (int)f->labels.size();
  Publish(std::move(f), st);
}

// Render thread. Camera of the frame the shapes were collected in; segments are clipped against the near
// plane before projecting, so nothing behind the camera turns into a line across the screen.
void Draw() {
  g_occ.lines.clear();
  g_occ.planeOn = false;
  std::shared_ptr<const Frame> f;
  {
    std::lock_guard<std::mutex> lk(g_frameMu);
    f = g_frame;
  }
  if (!f || !f->valid || (f->lines.empty() && f->labels.empty())) return;
  if (GetTickCount64() - f->at > 300) return;  // loading screen / pause of the game thread: no stale shapes
  g_lastCam = f->cam;
  g_lastCamOk = true;
  LARGE_INTEGER q0, q1, qf;
  QueryPerformanceCounter(&q0);
  ImVec2 ds = ImGui::GetIO().DisplaySize;
  Xf cam = FromRot(f->cam.rot, f->cam.loc);
  float fov = (f->cam.fov > 5 && f->cam.fov < 170 ? f->cam.fov : 90.0f) * kPi / 180.0f;
  float cx = ds.x * 0.5f, cy = ds.y * 0.5f, focal = cx / std::tan(fov * 0.5f);
  const float nearZ = 5.0f;
  auto toView = [&](Vec p) {
    Vec d = p - cam.o;
    return Vec{Dot(d, cam.x), Dot(d, cam.y), Dot(d, cam.z)};  // depth, right, up
  };
  auto toScreen = [&](Vec v) { return ImVec2(cx + v.y * focal / v.x, cy - v.z * focal / v.x); };
  Options opt = Opt();
  float th = opt.thickness * std::max(1.0f, ds.y / 1080.0f);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under the menu and the damage log
  float margin = std::max(ds.x, ds.y) * 2.0f;
  // Lines hidden behind walls go to the D3D9 hook's layer in world space, pulled 8 units toward the camera so lines
  // lying on a floor or in a wall are not lost to depth rounding.
  bool occ = g_occLive;
  g_occ.thickness = opt.thickness;
  g_occ.cam[0] = f->cam.loc.x, g_occ.cam[1] = f->cam.loc.y, g_occ.cam[2] = f->cam.loc.z;
  auto pulled = [&](Vec p, float* out) {
    Vec d = f->cam.loc - p;
    float len = Len(d);
    if (len > 1) p = p + d * (std::min(8.0f, len * 0.5f) / len);
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
  };
  bool all = g_occludeAll;
  for (const Line& l : f->lines) {
    if ((l.occlude || all) && occ) {
      OccLine o;
      pulled(l.a, o.a);
      pulled(l.b, o.b);
      uint32_t c = l.col;  // ImGui ABGR -> D3D ARGB
      o.argb = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
      o.group = l.group;
      g_occ.lines.push_back(o);
      continue;
    }
    Vec a = toView(l.a), b = toView(l.b);
    if (a.x < nearZ && b.x < nearZ) continue;
    if (a.x < nearZ) a = a + (b - a) * ((nearZ - a.x) / (b.x - a.x));
    else if (b.x < nearZ) b = b + (a - b) * ((nearZ - b.x) / (a.x - b.x));
    ImVec2 pa = toScreen(a), pb = toScreen(b);
    if ((pa.x < 0 && pb.x < 0) || (pa.x > ds.x && pb.x > ds.x) || (pa.y < 0 && pb.y < 0) || (pa.y > ds.y && pb.y > ds.y)) continue;
    if (std::fabs(pa.x) > margin || std::fabs(pa.y) > margin || std::fabs(pb.x) > margin || std::fabs(pb.y) > margin) {
      // very long after projection (an endpoint just in front of the camera): cut to the screen neighbourhood
      ImVec2 d(pb.x - pa.x, pb.y - pa.y);
      float t0 = 0, t1 = 1;
      auto clip = [&](float p, float q) {  // Liang-Barsky
        if (std::fabs(p) < 1e-6f) return q >= 0;
        float r = q / p;
        if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
        else { if (r < t0) return false; if (r < t1) t1 = r; }
        return true;
      };
      if (!(clip(-d.x, pa.x + margin) && clip(d.x, ds.x + margin - pa.x) && clip(-d.y, pa.y + margin) && clip(d.y, ds.y + margin - pa.y)))
        continue;
      pb = ImVec2(pa.x + d.x * t1, pa.y + d.y * t1);
      pa = ImVec2(pa.x + d.x * t0, pa.y + d.y * t0);
    }
    dl->AddLine(pa, pb, l.col, th);
  }
  if (g_testPlane > 0 && occ) {
    // Dev: what the depth test hides at this distance - the plane covers only what is farther than it
    Xf c = FromRot(f->cam.rot, f->cam.loc);
    Vec p = f->cam.loc + c.x * (g_testPlane * 100.0f);
    g_occ.planeOn = true;
    g_occ.plane[0] = p.x, g_occ.plane[1] = p.y, g_occ.plane[2] = p.z;
  }
  if (!occ) g_groupVisible.clear();
  int labelsHidden = 0;
  if (!f->labels.empty()) {
    ImFont* font = ImGui::GetFont();
    float px = ImGui::GetFontSize() * 0.95f;
    for (const Label& l : f->labels) {
      if (occ && l.group) {  // a trigger hidden behind walls: no label either (not known yet: shown)
        auto it = g_groupVisible.find(l.group);
        if (it != g_groupVisible.end() && !it->second) {
          ++labelsHidden;
          continue;
        }
      }
      Vec v = toView(l.p);
      if (v.x < nearZ) continue;
      ImVec2 s = toScreen(v);
      if (s.x < -200 || s.x > ds.x + 200 || s.y < -50 || s.y > ds.y + 50) continue;
      ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0, l.text.c_str());
      ImVec2 pos(std::floor(s.x - ts.x * 0.5f), std::floor(s.y - ts.y - 2));
      uint32_t shadow = WithAlpha(IM_COL32(0, 0, 0, 255), ((l.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * 0.85f);
      dl->AddText(font, px, ImVec2(pos.x + 1, pos.y + 1), shadow, l.text.c_str());
      dl->AddText(font, px, pos, l.col, l.text.c_str());
    }
  }
  QueryPerformanceCounter(&q1);
  QueryPerformanceFrequency(&qf);
  std::lock_guard<std::mutex> lk(g_statMu);
  g_stats.drawMs = (float)((q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart);
  g_stats.labelsHidden = labelsHidden;
}

Stats GetStats() {
  std::lock_guard<std::mutex> lk(g_statMu);
  return g_stats;
}

std::vector<std::string> DebugLines(const std::string& what) {
  std::vector<std::string> out;
  if (what.rfind("probe", 0) == 0) {
    g_probe = what.size() > 6 ? atoi(what.c_str() + 6) : 10;
    g_probed.clear();
    out.push_back("probing the next " + std::to_string(g_probe) + " strike shapes");
    return out;
  }
  Stats st = GetStats();
  char b[512];
  snprintf(b, sizeof b,
           "lines %d labels %d (hidden behind walls %d) bodies %d strikes %d markers %d triggers c%d a%d o%d walls %d "
           "collect %.2f ms draw %.2f ms hiding %s",
           st.lines, st.labels, st.labelsHidden, st.bodies, st.strikes, st.markers, st.triggers[0], st.triggers[1],
           st.triggers[2], st.walls, st.collectMs, st.drawMs, g_occLive ? "works" : "not available");
  out.push_back(b);
  if (what == "reach") {  // what the "other" triggers lead to: script node classes, by number of triggers
    std::map<std::string, int> hist;
    int n = 0;
    for (const Trigger& t : g_triggers) {
      if (t.kind != TKind::Other || !game::Alive(t.ev)) continue;
      ++n;
      std::vector<std::string> v;
      std::string path;
      Classify(t.ev, path, &v);
      std::sort(v.begin(), v.end());
      v.erase(std::unique(v.begin(), v.end()), v.end());
      for (const std::string& c : v) ++hist[c];
    }
    std::vector<std::pair<int, std::string>> top;
    for (auto& [c, k] : hist) top.push_back({k, c});
    std::sort(top.rbegin(), top.rend());
    std::string line = "reach of " + std::to_string(n) + " other triggers:";
    for (size_t i = 0; i < top.size() && i < 40; ++i) line += " " + top[i].second + "=" + std::to_string(top[i].first);
    out.push_back(line);
  }
  if (what == "walls")
    for (uintptr_t a : g_walls) {
      Vec loc{};
      ActorLoc(a, loc);
      ue3::TArrayRaw only{};
      ReadArr(a, "m_aBlockedClassList", only);
      std::string cls;
      for (int i = 0; i < only.num && i < 8; ++i) cls += " " + ue3::ObjName(util::ReadOr<uintptr_t>(only.data + i * 4));
      snprintf(b, sizeof b, "wall %s %s at (%.0f %.0f %.0f) collide %d block %d only:%s", ue3::ClassNameOf(a).c_str(),
               ue3::ObjName(a).c_str(), loc.x, loc.y, loc.z, GetBool(a, "bCollideActors"), GetBool(a, "bBlockActors"),
               cls.c_str());
      out.push_back(b);
    }
  if (what == "triggers")
    for (const Trigger& t : g_triggers) {
      Vec loc{};
      ActorLoc(t.orig, loc);
      snprintf(b, sizeof b, "%-9s %s <- %s %s at (%.0f %.0f %.0f) via %s enabled %d count %d/%d",
               t.kind == TKind::Arena ? "ARENA" : t.kind == TKind::Cutscene ? "CUTSCENE" : "other", ue3::ObjName(t.ev).c_str(),
               ue3::ClassNameOf(t.orig).c_str(), ue3::ObjName(t.orig).c_str(), loc.x, loc.y, loc.z, t.path.c_str(),
               GetBool(t.ev, "bEnabled"), GetInt(t.ev, "TriggerCount"), GetInt(t.ev, "MaxTriggerCount"));
      out.push_back(b);
    }
  return out;
}

}  // namespace visuals
