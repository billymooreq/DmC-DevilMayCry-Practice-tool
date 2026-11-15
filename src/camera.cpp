#include "camera.h"

#include <windows.h>
#include <Xinput.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "features.h"
#include "game.h"
#include "hotkeys.h"
#include "input.h"
#include "log.h"
#include "proxy.h"
#include "state.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace camera {
void ToggleFreecamMode(bool triggers);
namespace {

std::string Obj(uintptr_t o) {
  if (!o) return "null";
  if (!ue3::IsObject(o)) return "<bad " + util::Hex(o) + ">";
  return ue3::ClassNameOf(o) + " " + ue3::ObjName(o);
}

ue3::TArrayRaw Arr(uintptr_t obj, const char* name) {
  ue3::TArrayRaw a{};
  game::Field f = game::FieldOf(obj, name);
  if (!f.ok() || !util::Read(obj + f.off, a) || a.num < 0 || a.num > 4096) return {};
  return a;
}

// Camera controller of the player: PlayerController.PlayerCamera (DVCam_CameraControllerDante in missions).
uintptr_t Controller(const game::World& w) { return w.pc ? game::GetObj(w.pc, "PlayerCamera") : 0; }

// A bool field we hold at our value every frame; the game's own value is remembered (also when the level script
// changes it while we hold it) and put back when the feature turns off.
struct HeldBool {
  const char* field;
  uintptr_t obj = 0;
  bool have = false, gameVal = false, wrote = false;
  void Hold(uintptr_t o, bool want) {
    if (!o) return;
    if (o != obj) {
      obj = o;
      have = false;
    }
    bool cur = game::GetBool(o, field);
    if (!have || cur != wrote) {  // first frame, or the game set it since our last write
      gameVal = cur;
      have = true;
    }
    if (cur != want) game::SetBool(o, field, want);
    wrote = want;
  }
  void Restore() {
    if (have && game::Alive(obj) && game::GetBool(obj, field) != gameVal) game::SetBool(obj, field, gameVal);
    have = false;
    obj = 0;
  }
};

// Cameras the level puts over the chase camera: fixed / rail cameras of NTCam_Mappable actors (added as
// NTCam_DecoratorMappable), LockCamera (NTCam_Static), Kismet "look at actor" (NTCam_LookAtActor). Dante's own
// move cameras (Demon Pull, Angel Lift, aerial combat, last enemy) and cutscene cameras are left alone.
bool IsLevelCamera(uintptr_t cam) {
  std::string c = ue3::ClassNameOf(cam);
  return c == "NTCam_DecoratorMappable" || c == "NTCam_Static" || c == "NTCam_LookAtActor" || c == "NTCam_Composite";
}

int Priority(uintptr_t cam) {
  engine::Call c(cam, "GetPriority");
  return c.Invoke() ? c.RetInt() : 0;
}

bool SetPriority(uintptr_t mgr, uintptr_t cam, int prio) {
  // The manager's own way: sets the priority and moves to the new highest camera with the usual transition.
  engine::Call c(mgr, "ChangeCameraPriority");
  return c.Obj("Cam", cam).Int("NewPriority", prio).Invoke();
}

constexpr int kDemoted = -1;  // the chase camera has priority 0

class UnlockCamera : public features::Feature {
 public:
  UnlockCamera() {
    id = "unlock_camera";
    name = "Unlock camera";
    group = features::Group::Camera;
    help = "The camera stays free everywhere, like in open areas: you always turn it yourself with the mouse or "
           "the right stick. Turns off the level's fixed and rail cameras, corridor and boss camera settings, the "
           "pull towards enemies and bosses, scripted camera turns and the camera ceiling.\n"
           "Cutscenes keep their cameras. Turning it off puts everything back.";
  }
  bool Enable(const features::Ctx&) override {
    demoted_.clear();
    LOG("camera: unlock on");
    return true;
  }
  void Tick(const features::Ctx& c) override {
    uintptr_t cc = Controller(c.w);
    if (!cc || !ue3::IsA(cc, "NTCam_CameraController_ThirdPerson")) return;
    if (c.w.pc && game::GetBool(c.w.pc, "bCinematicMode")) return;  // cutscene / QTE: the game's cameras
    uintptr_t chase = game::GetObj(cc, "m_chaseCam");
    uintptr_t mgr = game::GetObj(cc, "m_CamManager");
    // 1) Level cameras over the chase camera: lower their priority below it (the manager cuts back to chase).
    if (mgr) {
      ue3::TArrayRaw a = Arr(mgr, "m_Cameras");
      for (int i = 0; i < a.num; ++i) {
        uintptr_t cam = util::ReadOr<uintptr_t>(a.data + i * 4);
        if (!ue3::IsObject(cam) || cam == chase || !IsLevelCamera(cam)) continue;
        int p = Priority(cam);
        if (p <= kDemoted) continue;
        if (SetPriority(mgr, cam, kDemoted)) {
          demoted_.push_back({cam, p});
          LOG("camera: %s (priority %d) moved below the chase camera", ue3::ObjName(cam).c_str(), p);
        }
      }
      mgr_ = mgr;
    }
    if (!chase) return;
    // 2) Open-area chase settings instead of the corridor / platforming / boss contexts levels push.
    if (uintptr_t sm = game::GetObj(chase, "m_settingsManager")) forceDefault_.Hold(sm, true);
    // 3) No pull towards enemies / bosses (lock-on targets of the chase camera).
    lockon_.Hold(chase, false);
    // 4) Scripted turns (NTCam_SeqAct_SetYaw / SetTilt) and their "no stick input": dropped, not restored.
    for (const char* f : {"m_setYawActive", "m_setYawStarted", "m_setYawDisableInput", "m_setTiltActive",
                          "m_setTiltStarted", "m_setTiltOverrideInput", "m_initialRotationActive",
                          "m_initialRotationOverrideInput"})
      if (game::GetBool(chase, f)) game::SetBool(chase, f, false);
    // 5) Camera ceiling / floor of the level (DVSeqAct_SetCameraCeiling).
    if (ue3::IsA(cc, "DVCam_CameraController")) ceiling_.Hold(cc, false);
  }
  void Disable() override {
    forceDefault_.Restore();
    lockon_.Restore();
    ceiling_.Restore();
    if (game::Alive(mgr_)) {
      ue3::TArrayRaw a = Arr(mgr_, "m_Cameras");
      for (const auto& [cam, prio] : demoted_) {
        bool present = false;
        for (int i = 0; i < a.num && !present; ++i) present = util::ReadOr<uintptr_t>(a.data + i * 4) == cam;
        if (present && game::Alive(cam)) SetPriority(mgr_, cam, prio);
      }
    }
    LOG("camera: unlock off (%zu level camera(s) put back)", demoted_.size());
    demoted_.clear();
    mgr_ = 0;
  }

 private:
  HeldBool forceDefault_{"m_ForcingDefaultContext"};
  HeldBool lockon_{"m_LockonEnabled"};
  HeldBool ceiling_{"m_UseCameraCeiling"};
  std::vector<std::pair<uintptr_t, int>> demoted_;
  uintptr_t mgr_ = 0;
};

// ------------------------------------------------------------------ Freecam
// The view the game renders comes from PlayerController.GetPlayerViewPoint, an event the engine calls through
// ProcessEvent (scene view, sound listener). While the camera flies, its out parameters are
// replaced right after the original ran (OnEventAfter); the game's own camera keeps working underneath and takes
// over again the moment Freecam is off. Our own drawings (Visuals, enemy HP) follow through game::SetPovOverride.
// Level parts stream in around Dante, not around the view (checked #20, M02): far from him the camera sees
// unloaded rooms; "Dante follows camera" carries him along, then rooms load as he passes (and his body sets off
// triggers). The game's keyboard / mouse / gamepad input is muted meanwhile (state::freecamMute, proxy.cpp /
// input.cpp), so Dante stands still; the mouse movement is collected there for the camera.
constexpr float kPi = 3.14159265f;
constexpr float kToUnreal = 65536.0f / (2 * kPi);
constexpr float kMouseRad = 0.11f * kPi / 180.0f;     // per mouse count
constexpr float kPadLookRad = 150.0f * kPi / 180.0f;  // per second at full right stick
// "Dante follows camera" (like playerFly "attach" of the Darksiders II debug menu): he hangs in front of the camera,
// Distance metres ahead along its level heading and 1.5 m below it, facing where the camera looks. If the game moves
// him more than 1.5 m away from where we put him (death, cutscene, a script), he is let go.
constexpr float kBelow = 150.0f, kGameMoved = 150.0f;
constexpr float kEye = 60.0f;  // "Invisible Dante": carried right at the camera, the camera at his head
constexpr ULONGLONG kBlendMs = 300;  // Freecam off: the view glides back to the game's camera

struct FcLive {  // game thread
  bool active = false;  // feature applied
  bool flying = false;  // our view replaces the game's this frame
  uintptr_t pc = 0, fnView = 0, fnUpdate = 0, cam = 0;
  int offLoc = -1, offRot = -1;
  game::Pov pov;
  unsigned viewCalls = 0, updateCalls = 0, framesFlying = 0;
  ULONGLONG blendFrom = 0;  // Freecam off at this tick: blend from pov to the game's view for kBlendMs
} g_live;
std::atomic<bool> g_fcApplied{false}, g_fcHolds{false}, g_fcFlying{false};

bool FcKey(int vk, int dik) {
  if (dinject::KeyHeld(dik)) return true;  // autorun tests
  if (state::menuOpen) return false;
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool NtPhysics(uintptr_t pawn, uint8_t v) {
  engine::Call c(pawn, "SetNTPhysics");
  return c.Byte("newPhysics", v).Invoke();
}

// After Freecam lets Dante go (R pressed again, Freecam (triggers) or Freecam off) he hangs where he is - no
// gravity, no drift - until the player moves him: move, jump, attack, evade, gun (keys, left stick, face buttons).
// On that first press (a fresh one: keys still held from flying do not count) he gets walking physics back at the
// start of the frame, before the game reads the input, so the jump and the first steps happen as usual.
struct Hover {
  uintptr_t pawn = 0;
  unsigned gen = 0;
  game::Vec at;
  bool armed = false;
  unsigned frames = 0;
} g_hover;

bool HoverKeyHeld() {
  for (int vk : {(int)'W', (int)'A', (int)'S', (int)'D', VK_SPACE, VK_LBUTTON, VK_RBUTTON, VK_SHIFT, (int)'F', (int)'V'})
    if (FcKey(vk, 0)) return true;
  XINPUT_STATE xs{};
  if (!state::menuOpen && input::RawPadState(&xs)) {
    const XINPUT_GAMEPAD& g = xs.Gamepad;
    if (std::abs(g.sThumbLX) > 7849 || std::abs(g.sThumbLY) > 7849) return true;
    if (g.wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) return true;
    if (g.bLeftTrigger > 60 || g.bRightTrigger > 60) return true;
  }
  return false;
}

void StartHover(uintptr_t pawn) {
  const game::World& w = game::W();
  if (!game::Alive(pawn) || pawn != w.pawn) return;
  game::Field loc = game::FieldOf(pawn, "Location");
  if (!loc.ok()) return;
  g_hover = {};
  g_hover.pawn = pawn;
  g_hover.gen = w.gen;
  g_hover.at = util::ReadOr<game::Vec>(pawn + loc.off);
  NtPhysics(pawn, 0);
  LOG("camera: Dante hangs at %.0f %.0f %.0f until he moves", g_hover.at.x, g_hover.at.y, g_hover.at.z);
}

// After Dante stops hanging: his height and speed at +0.1 / 0.3 / 0.5 / 1 s in the log (did the jump happen).
struct AfterHover {
  uintptr_t pawn = 0;
  ULONGLONG t0 = 0;
  float z0 = 0;
  int next = 0;
} g_after;

game::Vec PawnVec(uintptr_t pawn, const char* field) {
  game::Field f = game::FieldOf(pawn, field);
  return f.ok() ? util::ReadOr<game::Vec>(pawn + f.off) : game::Vec{};
}

void EndHover(const char* why, bool physics) {
  if (physics && game::Alive(g_hover.pawn)) NtPhysics(g_hover.pawn, 1);  // walking: no floor -> he falls
  LOG("camera: Dante stops hanging (%s, after %u frames)", why, g_hover.frames);
  if (physics) g_after = {g_hover.pawn, GetTickCount64(), g_hover.at.z, 0};
  g_hover = {};
}

void TraceAfterHover() {
  static const ULONGLONG kAt[] = {100, 300, 500, 1000};
  if (!g_after.pawn) return;
  if (!game::Alive(g_after.pawn)) {
    g_after = {};
    return;
  }
  ULONGLONG ms = GetTickCount64() - g_after.t0;
  if (ms < kAt[g_after.next]) return;
  game::Vec l = PawnVec(g_after.pawn, "Location"), v = PawnVec(g_after.pawn, "Velocity");
  LOG("camera: after hanging +%llu ms: height %+.0f, speed up %.0f, sideways %.0f, physics %d", ms, l.z - g_after.z0,
      v.z, std::sqrt(v.x * v.x + v.y * v.y), (int)game::GetByte(g_after.pawn, "NTPhysics"));
  if (++g_after.next >= 4) g_after = {};
}

// A Freecam key held now (its bind from the Hotkeys tab; extra modifiers do not matter, so R still works while
// Shift flies fast). Only with the game in front. Gamepad: all its buttons held.
bool CarryKeyHeld(const char* featureId) {
  hotkeys::Bind b;
  {
    auto lk = features::Lock();
    const auto& all = features::All();
    const features::Data& d = features::D();
    for (size_t i = 0; i < all.size() && i < d.binds.size(); ++i)
      if (std::string(all[i]->id) == featureId) b = d.binds[i];
  }
  if (b.Empty()) return false;
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid != GetCurrentProcessId()) return false;
  if (b.vk && (GetAsyncKeyState(b.vk) & 0x8000)) return true;
  if (b.pad) {
    XINPUT_STATE xs{};
    if (!input::RawPadState(&xs)) return false;
    uint32_t have = xs.Gamepad.wButtons;
    if (xs.Gamepad.bLeftTrigger > 128) have |= hotkeys::kPadLT;
    if (xs.Gamepad.bRightTrigger > 128) have |= hotkeys::kPadRT;
    return (have & b.pad) == b.pad;
  }
  return false;
}

int ParamOffset(uintptr_t fn, const char* name) {
  for (const engine::Param& p : engine::Params(fn))
    if (p.name == name) return p.offset;
  return -1;
}

class Freecam : public features::Feature {
 public:
  Freecam() {
    id = "freecam";
    name = "Freecam";
    group = features::Group::Camera;
    help = "Fly the camera through walls; Dante stands still. R / F: Dante hangs in front of the camera and "
           "flies with it; press again - R: he stays in the air, F: he falls.\n"
           "Keyboard:  Mouse · W A S D · Space / Ctrl up, down · Shift / Alt speed · R / F Dante\n"
           "Gamepad:   Sticks · RT / LT up, down · RB / LB speed";
    params = {{"speed", "Speed", 10.0f, 0.5f, 50.0f, "%.1f m/s", features::ParamKind::Slider, {},
               "How fast the camera flies, in metres per second. The mouse wheel changes it while flying. "
               "Hold Shift (RB) for 4× faster, Alt (LB) for 4× slower.",
               1.0f, 1000.0f},
              {"distance", "Dante distance", 4.0f, 1.0f, 8.0f, "%.1f m", features::ParamKind::Slider, {},
               "How far in front of the camera Dante hangs when you carry him (R / F).", 0.5f, 20.0f},
              {"triggers", "Freecam (triggers)", 0.0f, 0.0f, 1.0f, "", features::ParamKind::Toggle, {},
               "The same free camera, but it sets off triggers (arenas, cutscenes) and the level loads around it: "
               "an invisible Dante flies inside the camera. Turned off, he stays in the air there until you move "
               "him."}};
  }
  bool Enable(const features::Ctx& c) override {
    game::Pov p;
    if (!c.w.pc || !game::CameraPovGame(c.w, p)) return false;
    // Start from the game's camera only once it is with Dante: right after a map change / checkpoint it may still
    // be at 0,0,0 or wherever the last map left it (then we wait), and a cutscene holds its own camera.
    if (game::GetBool(c.w.pc, "bCinematicMode")) return false;
    {
      game::Vec d = PawnVec(c.w.pawn, "Location");
      float dx = d.x - p.loc.x, dy = d.y - p.loc.y, dz = d.z - p.loc.z;
      if (std::sqrt(dx * dx + dy * dy + dz * dz) > 3000.0f) return false;
    }
    uintptr_t fn = engine::FindFunction(c.w.pc, "GetPlayerViewPoint");
    if (!fn) return false;
    g_live = {};  // also ends a glide back from the last time
    g_live.pc = c.w.pc;
    g_live.fnView = fn;
    g_live.offLoc = ParamOffset(fn, "out_Location");
    g_live.offRot = ParamOffset(fn, "out_Rotation");
    g_live.cam = game::GetObj(c.w.pc, "PlayerCamera");
    g_live.fnUpdate = g_live.cam ? engine::FindFunction(g_live.cam, "UpdateCamera") : 0;
    if (g_live.offLoc < 0 || g_live.offRot < 0) return false;
    pos_ = p.loc;
    int pr = p.rot[0] & 0xFFFF;
    if (pr > 32767) pr -= 65536;
    pitch_ = pr / kToUnreal;
    yaw_ = (p.rot[1] & 0xFFFF) / kToUnreal;
    fov_ = p.fov;
    pawn_ = c.w.pawn;
    holding_ = false;
    lastPass_ = false;
    carry_ = Carry::None;
    front_ = prevR_ = prevF_ = false;
    lastKey_ = 0;
    for (auto* a : {&state::fcStateX, &state::fcStateY, &state::fcStateZ, &state::fcDataX, &state::fcDataY,
                    &state::fcDataZ, &state::fcRawX, &state::fcRawY, &state::fcRawZ})
      a->exchange(0);
    g_live.active = true;
    Publish(false);
    g_fcApplied = true;
    LOG("camera: freecam on at %.0f %.0f %.0f yaw %.1f pitch %.1f (view fn %s loc +%d rot +%d, update %s)", pos_.x,
        pos_.y, pos_.z, yaw_ * 180 / kPi, pitch_ * 180 / kPi, ue3::ObjName(ue3::ObjOuter(fn)).c_str(), g_live.offLoc,
        g_live.offRot, g_live.fnUpdate ? ue3::ObjName(ue3::ObjOuter(g_live.fnUpdate)).c_str() : "none");
    return true;
  }
  void Tick(const features::Ctx& c) override {
    bool cine = c.w.pc && game::GetBool(c.w.pc, "bCinematicMode");
    bool dead = game::Alive(pawn_) && game::GetFloat(pawn_, "m_Health", 1.0f) <= 0.0f;
    // The engine's pause (the game's pause menu, our menu pause). Not gameDt: at high frame rates with Game speed
    // slowed down the world clock can stand still for a frame while the game still reads input.
    bool paused = c.w.world && game::GetObj(c.w.world, "Pauser") != 0;
    bool menu = state::menuOpen;
    // Cutscenes / QTE and death (game over menu): the game's camera and input; we go on from here afterwards.
    bool pass = cine || dead;
    if (pass != lastPass_) {
      LOG("camera: freecam %s (%s)", pass ? "gives the view back to the game" : "flies again",
          cine ? "cutscene" : dead ? "Dante died" : "game");
      lastPass_ = pass;
    }
    float dx = 0, dy = 0, dz = 0;
    TakeMouse(dx, dy, dz);
    bool move = !pass && !paused && !menu;
    if (move) {
      float dt = c.w.realDt;  // real time: Game speed does not slow the camera
      yaw_ += dx * kMouseRad;
      pitch_ -= dy * kMouseRad;
      float fwd = 0, right = 0, up = 0, mult = 1;
      if (FcKey('W', 0x11)) fwd += 1;
      if (FcKey('S', 0x1F)) fwd -= 1;
      if (FcKey('D', 0x20)) right += 1;
      if (FcKey('A', 0x1E)) right -= 1;
      if (FcKey(VK_SPACE, 0x39)) up += 1;
      if (FcKey(VK_CONTROL, 0x1D)) up -= 1;
      if (FcKey(VK_SHIFT, 0x2A)) mult *= 4;
      if (FcKey(VK_MENU, 0x38)) mult *= 0.25f;
      XINPUT_STATE xs{};
      if (input::RawPadState(&xs)) {
        auto axis = [](SHORT v, int deadzone) { return std::abs(v) < deadzone ? 0.0f : v / 32767.0f; };
        fwd += axis(xs.Gamepad.sThumbLY, 7849);
        right += axis(xs.Gamepad.sThumbLX, 7849);
        up += xs.Gamepad.bRightTrigger / 255.0f - xs.Gamepad.bLeftTrigger / 255.0f;
        yaw_ += axis(xs.Gamepad.sThumbRX, 8689) * kPadLookRad * dt;
        pitch_ += axis(xs.Gamepad.sThumbRY, 8689) * kPadLookRad * dt;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) mult *= 4;
        if (xs.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) mult *= 0.25f;
      }
      const float lim = 89.0f * kPi / 180.0f;
      pitch_ = std::max(-lim, std::min(lim, pitch_));
      if (yaw_ > kPi) yaw_ -= 2 * kPi;
      if (yaw_ < -kPi) yaw_ += 2 * kPi;
      float speed = c.p[0];
      if (dz != 0) {  // mouse wheel: +-20% per notch
        speed = std::max(0.5f, std::min(1000.0f, speed * std::pow(1.2f, dz / 120.0f)));
        features::SetParam(id, 0, speed);
      }
      float step = speed * 100.0f * mult * dt;  // 100 game units = 1 m
      float cp = std::cos(pitch_), sp = std::sin(pitch_), cy = std::cos(yaw_), sy = std::sin(yaw_);
      // forward along the view (pitch included), sideways level, up / down along the world's vertical
      pos_.x += (cp * cy * fwd - sy * right) * step;
      pos_.y += (cp * sy * fwd + cy * right) * step;
      pos_.z += (sp * fwd + up) * step;
    }
    // Dante along with the camera (triggers fire the game's own way: his collision touches them).
    // Freecam (triggers): invisible, inside the camera. Plain Freecam: R / F press - Dante in front of it; the next
    // press of R lets him go hanging in the air, of F - falling.
    dist_ = c.p[1] * 100.0f;
    bool triggers = c.p[2] != 0;
    {
      bool r = CarryKeyHeld("freecam_carry_hang"), f = CarryKeyHeld("freecam_carry_fall");
      bool live = !menu && !paused && !pass && !triggers;  // the menu / a pause / a cutscene has the input
      const struct {
        bool held, prev;
        char key;
      } keys[] = {{r, prevR_, 'R'}, {f, prevF_, 'F'}};
      for (const auto& k : keys) {
        if (!live || !k.held || k.prev) continue;
        if (front_) {
          front_ = false;
          lastKey_ = k.key;
        } else {
          front_ = true;
        }
        LOG("camera: freecam key %c - Dante %s", k.key,
            front_ ? "carried" : k.key == 'F' ? "let go, falls" : "let go, hangs");
      }
      prevR_ = r;
      prevF_ = f;
    }
    if (triggers) front_ = false;
    Carry want = pass || !game::Alive(pawn_) ? Carry::None
                 : triggers                  ? Carry::Inside
                 : front_                    ? Carry::Front
                                             : Carry::None;
    if (want != Carry::None && holding_) {  // the game moved him (a script, a fall respawn): let go
      game::Vec now = PawnVec(pawn_, "Location");
      float d = std::sqrt((now.x - placed_.x) * (now.x - placed_.x) + (now.y - placed_.y) * (now.y - placed_.y) +
                          (now.z - placed_.z) * (now.z - placed_.z));
      if (d > kGameMoved) {
        LOG("camera: freecam - the game moved Dante by %.0f, he is let go", d);
        Release(false);
        if (triggers) features::SetParam(id, 2, 0.0f);  // plain Freecam from here
        front_ = false;
        want = Carry::None;
      }
    }
    if (want != Carry::None && !holding_) Grab();
    if (want == Carry::None && holding_) {
      // a cutscene / death takes Dante: no hanging; F: he falls; R or Freecam (triggers) off: he stays in the air
      bool hang = !pass && !(carry_ == Carry::Front && lastKey_ == 'F');
      Release(hang);
    }
    if (holding_) {
      carry_ = want;
      bool hide = want == Carry::Inside;
      if (game::GetByte(pawn_, "NTPhysics") != 0) SetPhysics(pawn_, 0);
      if (hide != hidden_ || (hidden_ && !game::GetBool(pawn_, "bHidden"))) Hide(hide);
      PlaceDante(hide ? game::Vec{pos_.x, pos_.y, pos_.z - kEye} : InFront());
    }
    state::freecamMute = !pass && !paused;
    Publish(!pass);
    if (!pass) ++g_live.framesFlying;
  }
  void Disable() override {
    state::freecamMute = false;
    if (holding_) Release(true);  // Freecam off while carrying: he stays in the air there
    if (g_live.flying) g_live.blendFrom = GetTickCount64();  // glide back to Dante's camera
    g_live.active = g_live.flying = false;
    game::SetPovOverride(false, nullptr);
    g_fcApplied = false;
    g_fcFlying = false;
    LOG("camera: freecam off (view replaced %u times, camera updates seen %u, %u frames flying; mouse seen "
        "state %u data %u raw %u)",
        g_live.viewCalls, g_live.updateCalls, g_live.framesFlying, state::fcSeenState.load(),
        state::fcSeenData.load(), state::fcSeenRaw.load());
    pawn_ = 0;
  }

  void SetAt(const game::Vec& at, float yawDeg, float pitchDeg, bool rot) {  // autorun tests
    pos_ = at;
    if (rot) yaw_ = yawDeg * kPi / 180, pitch_ = pitchDeg * kPi / 180;
    Publish(g_live.flying);
  }
  std::vector<std::string> Debug(const game::World& w) const {
    std::vector<std::string> out;
    char b[512];
    game::Vec dl{};
    game::Field loc = game::Alive(w.pawn) ? game::FieldOf(w.pawn, "Location") : game::Field{};
    if (loc.ok()) dl = util::ReadOr<game::Vec>(w.pawn + loc.off);
    game::Pov gp;
    bool g = game::CameraPovGame(w, gp);
    snprintf(b, sizeof b,
             "active %d flying %d mute %d holding %d cam %.0f %.0f %.0f yaw %.1f pitch %.1f | dante %.0f %.0f %.0f "
             "phys %d | game cam %d %.0f %.0f %.0f | view calls %u updates %u | mouse seen state %u data %u raw %u",
             (int)g_live.active, (int)g_live.flying, (int)state::freecamMute.load(), (int)holding_, pos_.x, pos_.y,
             pos_.z, yaw_ * 180 / kPi, pitch_ * 180 / kPi, dl.x, dl.y, dl.z,
             w.pawn ? (int)game::GetByte(w.pawn, "NTPhysics") : -1, (int)g, gp.loc.x, gp.loc.y, gp.loc.z,
             g_live.viewCalls, g_live.updateCalls, state::fcSeenState.load(), state::fcSeenData.load(),
             state::fcSeenRaw.load());
    out.push_back(b);
    snprintf(b, sizeof b, "hover %d armed %d frames %u at %.0f %.0f %.0f | dante hidden %d (ours %d)",
             g_hover.pawn != 0, (int)g_hover.armed, g_hover.frames, g_hover.at.x, g_hover.at.y, g_hover.at.z,
             w.pawn ? (int)game::GetBool(w.pawn, "bHidden") : -1, (int)hidden_);
    out.push_back(b);
    return out;
  }

 private:
  static bool SetPhysics(uintptr_t pawn, uint8_t v) { return NtPhysics(pawn, v); }
  game::Vec InFront() const {
    return {pos_.x + std::cos(yaw_) * dist_, pos_.y + std::sin(yaw_) * dist_, pos_.z - kBelow};
  }
  void PlaceDante(const game::Vec& at) {
    placed_ = at;
    int rot[3] = {0, (int)std::lround(yaw_ * kToUnreal), 0};  // facing where the camera looks
    engine::Call m(pawn_, "ForcedSetLocationAndRotation");
    if (!m.Valid() || !m.Raw("NewLocation", &at, sizeof at).Raw("NewRotation", rot, sizeof rot)
                          .Bool("UpdateComponents", true).Invoke()) {
      engine::Call m2(pawn_, "ForcedSetLocation");
      m2.Raw("NewLocation", &at, sizeof at).Bool("UpdateComponents", true).Invoke();
    }
    game::Field vel = game::FieldOf(pawn_, "Velocity");
    if (vel.ok()) *reinterpret_cast<game::Vec*>(pawn_ + vel.off) = {};
  }
  static void SetHidden(uintptr_t a, bool h) {
    engine::Call c(a, "SetHidden");
    c.Bool("bNewHidden", h).Invoke();
  }
  // Invisible Dante: his model (and what the game hides with him) is not drawn; collision and triggers stay.
  void Hide(bool h) {
    if (h && !hidden_) wasHidden_ = game::GetBool(pawn_, "bHidden");
    SetHidden(pawn_, h ? true : wasHidden_);
    hidden_ = h;
    LOG("camera: freecam - Dante %s", h ? "invisible, carried inside the camera" : "visible again");
  }
  void Grab() {
    placed_ = PawnVec(pawn_, "Location");
    origPhys_ = game::GetByte(pawn_, "NTPhysics");
    if (g_hover.pawn == pawn_) {  // hanging from the last release: carry him again
      origPhys_ = 1;
      g_hover = {};
    }
    SetPhysics(pawn_, 0);  // NTPHYS_None: no gravity, no walking collision (like Noclip)
    holding_ = true;
    g_fcHolds = true;
    LOG("camera: freecam takes Dante along (physics was %d)", origPhys_);
  }
  void Release(bool hover) {
    if (hidden_ && game::Alive(pawn_)) Hide(false);
    if (hover && game::Alive(pawn_))
      StartHover(pawn_);  // hangs until the player moves him
    else if (game::Alive(pawn_))
      SetPhysics(pawn_, origPhys_ ? origPhys_ : 1);  // walking: falls and lands normally
    holding_ = false;
    g_fcHolds = false;
    LOG("camera: freecam lets Dante go");
  }
  // The mouse movement the game did not get, from the source it reads (buffered DirectInput, DirectInput state or
  // Raw Input: whichever is seen; only one, the others may report the same movement).
  static void TakeMouse(float& dx, float& dy, float& dz) {
    int x1 = state::fcDataX.exchange(0), y1 = state::fcDataY.exchange(0), z1 = state::fcDataZ.exchange(0);
    int x2 = state::fcStateX.exchange(0), y2 = state::fcStateY.exchange(0), z2 = state::fcStateZ.exchange(0);
    int x3 = state::fcRawX.exchange(0), y3 = state::fcRawY.exchange(0), z3 = state::fcRawZ.exchange(0);
    if (state::fcSeenData) dx = (float)x1, dy = (float)y1, dz = (float)z1;
    else if (state::fcSeenState) dx = (float)x2, dy = (float)y2, dz = (float)z2;
    else dx = (float)x3, dy = (float)y3, dz = (float)z3;
  }
  void Publish(bool flying) {
    g_live.pov.loc = pos_;
    g_live.pov.rot[0] = (int)std::lround(pitch_ * kToUnreal);
    g_live.pov.rot[1] = (int)std::lround(yaw_ * kToUnreal);
    g_live.pov.rot[2] = 0;
    game::Pov gp;
    if (g_live.pc && game::CameraPovGame(game::W(), gp) && gp.fov > 1) fov_ = gp.fov;  // the game's FOV
    g_live.pov.fov = fov_;
    g_live.flying = flying;
    g_fcFlying = flying;
    game::SetPovOverride(flying, &g_live.pov);
  }

  game::Vec pos_;
  float yaw_ = 0, pitch_ = 0, fov_ = 90;
  uintptr_t pawn_ = 0;
  uint8_t origPhys_ = 1;
  bool holding_ = false, lastPass_ = false, hidden_ = false, wasHidden_ = false;
  enum class Carry { None, Front, Inside } carry_ = Carry::None;
  bool front_ = false, prevR_ = false, prevF_ = false;
  char lastKey_ = 0;
  float dist_ = 200.0f;
  game::Vec placed_;  // where we put Dante last
};

Freecam* g_freecam = nullptr;

// Keys of Freecam (like playerFly's R in the Darksiders II debug menu): Freecam watches them every frame
// (CarryKeyHeld: a press counts with any modifier held, so R still works while Shift flies fast). They work only while
// Freecam flies; otherwise the key is the game's. The features exist for their binds in the Hotkeys tab; running
// them does nothing (the Hotkeys tab fires them too - that would count one press twice).
class FreecamCarryKey : public features::Feature {
 public:
  FreecamCarryKey(const char* i, const char* n, int vk, const char* h) {
    id = i;
    name = n;
    group = features::Group::Camera;
    action = true;
    defBind.vk = vk;
    help = h;
  }
  std::string Blocked() const override { return g_fcApplied ? "" : "turn on Freecam first"; }
};

// Freecam (triggers): the same free camera with an invisible Dante inside it. A switch of its own for the Hotkeys tab.
class FreecamTriggers : public features::Feature {
 public:
  FreecamTriggers() {
    id = "freecam_triggers";
    name = "Freecam (triggers)";
    group = features::Group::Camera;
    action = true;
    frontEndOk = false;
    help = "Turns on / off the free camera that sets off triggers and loads the level (an invisible Dante flies "
           "inside it).";
  }
  void Run(const features::Ctx&) override { ToggleFreecamMode(true); }
};

}  // namespace

// Menu rows and keys: Freecam / Freecam (triggers) are one feature in two modes. Clicking the mode that is on turns
// it off; the other mode switches over (or turns on).
void ToggleFreecamMode(bool triggers) {
  features::View v = features::Get("freecam");
  bool cur = v.manualP[2] != 0;
  if (v.manual && cur == triggers) {
    features::SetManual("freecam", false);
    return;
  }
  features::SetParam("freecam", 2, triggers ? 1.0f : 0.0f);
  if (!v.manual) features::SetManual("freecam", true);
}

namespace {
}  // namespace

std::vector<features::Feature*> CreateFeatures() {
  g_freecam = new Freecam;
  return {new UnlockCamera,
          g_freecam,
          new FreecamTriggers,
          new FreecamCarryKey("freecam_carry_hang", "Freecam: carry Dante / let go (he hangs)", 'R',
                              "Freecam key: Dante hangs in front of the camera and flies with it; press again - he stays "
                              "in the air there until you move him. Only while Freecam flies, otherwise the key is the "
                              "game's."),
          new FreecamCarryKey("freecam_carry_fall", "Freecam: carry Dante / let go (he falls)", 'F',
                              "Freecam key: Dante hangs in front of the camera and flies with it; press again - he falls "
                              "as usual. Only while Freecam flies, otherwise the key is the game's.")};
}

bool FreecamFlying() { return g_fcFlying; }
bool FreecamHoldsDante() { return g_fcHolds; }

// Rotator component a -> b the short way round (65536 = 360 degrees).
int LerpAngle(int a, int b, float t) {
  int d = ((b - a) & 0xFFFF);
  if (d > 32767) d -= 65536;
  return a + (int)std::lround(d * t);
}

void OnEventAfter(uintptr_t self, uintptr_t fn, void* parms) {
  if (!parms) return;
  if (!g_live.active && g_live.blendFrom) {  // Freecam just turned off: glide to the game's own view
    if (fn != g_live.fnView || self != g_live.pc) return;
    ULONGLONG ms = GetTickCount64() - g_live.blendFrom;
    if (ms >= kBlendMs || !game::Alive(g_live.pc)) {
      g_live.blendFrom = 0;
      return;
    }
    float t = ms / (float)kBlendMs;
    t = t * t * (3 - 2 * t);
    auto* p = (uint8_t*)parms;
    auto* loc = reinterpret_cast<game::Vec*>(p + g_live.offLoc);
    int* r = reinterpret_cast<int*>(p + g_live.offRot);
    const game::Pov& a = g_live.pov;
    *loc = {a.loc.x + (loc->x - a.loc.x) * t, a.loc.y + (loc->y - a.loc.y) * t, a.loc.z + (loc->z - a.loc.z) * t};
    for (int i = 0; i < 3; ++i) r[i] = LerpAngle(a.rot[i], r[i], t);
    return;
  }
  if (!g_live.active) return;
  if (fn == g_live.fnView && self == g_live.pc) {
    if (!g_live.flying) return;
    auto* p = (uint8_t*)parms;
    *reinterpret_cast<game::Vec*>(p + g_live.offLoc) = g_live.pov.loc;
    int* r = reinterpret_cast<int*>(p + g_live.offRot);
    r[0] = g_live.pov.rot[0], r[1] = g_live.pov.rot[1], r[2] = g_live.pov.rot[2];
    ++g_live.viewCalls;
  } else if (fn == g_live.fnUpdate && self == g_live.cam) {
    ++g_live.updateCalls;
  }
}

void OnFrame(const game::World& w) {
  TraceAfterHover();
  if (!g_hover.pawn) return;
  uintptr_t p = g_hover.pawn;
  if (!game::Alive(p) || p != w.pawn || w.gen != g_hover.gen) return EndHover("world changed", false);
  if (w.pc && game::GetBool(w.pc, "bCinematicMode")) return EndHover("cutscene", true);
  if (game::GetFloat(p, "m_Health", 1.0f) <= 0.0f) return EndHover("Dante died", true);
  if (features::Get("noclip").applied) return EndHover("Noclip flies him", false);
  ++g_hover.frames;
  bool muted = state::freecamMute || state::menuOpen;  // the keys fly the camera / work the menu
  bool held = !muted && HoverKeyHeld();
  if (!muted && !held) g_hover.armed = true;
  if (g_hover.armed && held) return EndHover("the player moves him", true);
  if (game::GetByte(p, "NTPhysics") != 0) NtPhysics(p, 0);
  game::Field vel = game::FieldOf(p, "Velocity");
  if (vel.ok()) *reinterpret_cast<game::Vec*>(p + vel.off) = {};
  game::Field loc = game::FieldOf(p, "Location");
  if (loc.ok()) {
    game::Vec now = util::ReadOr<game::Vec>(p + loc.off);
    float d = std::fabs(now.x - g_hover.at.x) + std::fabs(now.y - g_hover.at.y) + std::fabs(now.z - g_hover.at.z);
    if (d > 1000) {
      g_hover.at = now;  // moved on purpose (Teleport, Load position, Noclip): hang there
    } else if (d > 1) {
      engine::Call m(p, "ForcedSetLocation");
      m.Raw("NewLocation", &g_hover.at, sizeof g_hover.at).Bool("UpdateComponents", true).Invoke();
    }
  }
}

bool FreecamSetAt(float x, float y, float z, float yawDeg, float pitchDeg, bool rot) {
  if (!g_freecam || !g_live.active) return false;
  g_freecam->SetAt({x, y, z}, yawDeg, pitchDeg, rot);
  return true;
}

std::vector<std::string> FreecamDebug() {
  if (!g_freecam) return {"no freecam"};
  return g_freecam->Debug(game::W());
}

std::vector<std::string> DebugLines() {
  std::vector<std::string> out;
  const game::World& w = game::W();
  uintptr_t cc = Controller(w);
  char b[512];
  if (!cc) {
    out.push_back("no camera controller (map '" + w.map + "')");
    return out;
  }
  snprintf(b, sizeof b, "controller %s cinematic %d map %s", Obj(cc).c_str(),
           w.pc ? (int)game::GetBool(w.pc, "bCinematicMode") : -1, w.map.c_str());
  out.push_back(b);
  game::Pov pov;
  if (game::CameraPov(w, pov)) {
    snprintf(b, sizeof b, "pov loc %.0f %.0f %.0f rot p %d y %d r %d fov %.1f", pov.loc.x, pov.loc.y, pov.loc.z,
             pov.rot[0], pov.rot[1], pov.rot[2], pov.fov);
    out.push_back(b);
  }
  if (!ue3::IsA(cc, "NTCam_CameraController")) return out;
  out.push_back("current " + Obj(game::GetObj(cc, "m_CurrentCamera")) + " | forced " +
                Obj(game::GetObj(cc, "m_ForcedCamera")) + " | locked " + Obj(game::GetObj(cc, "m_lockedCamera")));
  uintptr_t mgr = game::GetObj(cc, "m_CamManager");
  if (mgr) {
    out.push_back("manager highest " + Obj(game::GetObj(mgr, "m_HighestPriorityCamera")) + " | main " +
                  Obj(game::GetObj(mgr, "m_MainCamera")) + " | transition " +
                  Obj(game::GetObj(mgr, "m_ActiveTransitionCamera")));
    ue3::TArrayRaw a = Arr(mgr, "m_Cameras");
    for (int i = 0; i < a.num; ++i) {
      uintptr_t c = util::ReadOr<uintptr_t>(a.data + i * 4);
      if (!ue3::IsObject(c)) continue;
      snprintf(b, sizeof b, "  cam[%d] %s prio %d active %d paused %d", i, Obj(c).c_str(), game::GetInt(c, "m_Priority"),
               game::GetBool(c, "m_Active"), game::GetBool(c, "m_IsPaused"));
      out.push_back(b);
    }
  }
  uintptr_t chase = game::GetObj(cc, "m_chaseCam");
  if (chase) {
    uintptr_t sm = game::GetObj(chase, "m_settingsManager");
    uintptr_t st = game::GetObj(chase, "m_settings");
    snprintf(b, sizeof b,
             "chase %s settings %s combat %d lockon %d kismetNoAutoRot %d setYaw %d setTilt %d curvePos %.3f",
             Obj(chase).c_str(), Obj(st).c_str(), game::GetBool(chase, "m_CombatMode"),
             game::GetBool(chase, "m_LockonEnabled"), game::GetBool(chase, "m_KismetDisableAutoRotation"),
             game::GetBool(chase, "m_setYawActive"), game::GetBool(chase, "m_setTiltActive"),
             game::GetFloat(chase, "m_CurrentPositionOnCurve"));
    out.push_back(b);
    {
      ue3::TArrayRaw lk = Arr(chase, "m_lockonTargets");
      game::Field lf = game::FieldOf(chase, "m_lockonTargets");
      snprintf(b, sizeof b,
               "  lockon entries %d (field %d) setYawDisableInput %d autoTilt %d/%d initialRot %d combatRot %d "
               "combatZoom %d combatPitch %d wallSlide %d superClose %d obstructRot %d tracking %d",
               lk.num, lf.ok(), game::GetBool(chase, "m_setYawDisableInput"), game::GetBool(chase, "m_AutoTiltEnabled"),
               game::GetBool(chase, "m_KismetAutoTiltEnabled"), game::GetBool(chase, "m_initialRotationActive"),
               game::GetBool(chase, "m_combatRotationActive"), game::GetBool(chase, "m_combatZoomActive"),
               game::GetBool(chase, "m_combatPitchActive"), game::GetBool(chase, "m_wallSlidingYawActive"),
               game::GetBool(chase, "m_SuperCloseModeActive"), game::GetBool(chase, "m_ObstructionRotate_Active"),
               game::GetBool(chase, "m_usingTracking"));
      out.push_back(b);
      snprintf(b, sizeof b, "  tracking dist %.0f cur %.0f prev %.0f curveZoom %.2f collision r %.1f",
               game::GetFloat(chase, "m_TrackingDistance"), game::GetFloat(chase, "m_currentTrackingDistance"),
               game::GetFloat(chase, "m_previousTrackingDistance"), game::GetFloat(chase, "m_CurrentCurveZoom"),
               game::GetFloat(chase, "m_collisionRadius"));
      out.push_back(b);
    }
    if (st) {
      snprintf(b, sizeof b, "  settings: stick %d curveLimit %d [%.2f..%.2f] inputOverride %d autoRot %d/%d lockon %d/%d",
               game::GetBool(st, "m_AllowStickInput"), game::GetBool(st, "CurveLimitOverride"),
               game::GetFloat(st, "m_fCurveNormalMinLimit"), game::GetFloat(st, "m_fCurveNormalMaxLimit"),
               game::GetBool(st, "InputOverride"), game::GetBool(st, "NewAutoRotateOverride"),
               game::GetBool(st, "NewAutoRotationEnable"), game::GetBool(st, "LockonOverride"),
               game::GetBool(st, "LockOnAllowed"));
      out.push_back(b);
    }
    if (sm) {
      snprintf(b, sizeof b, "  manager %s default %s blend %s command %s '%s' forcingDefault %d locked %d",
               Obj(sm).c_str(), Obj(game::GetObj(sm, "m_DefaultSettings")).c_str(),
               Obj(game::GetObj(sm, "m_StackBlendSetting")).c_str(),
               Obj(game::GetObj(sm, "m_CurrentCommandSettings")).c_str(),
               game::GetStr(sm, "m_CurrentCommandContextName").c_str(), game::GetBool(sm, "m_ForcingDefaultContext"),
               game::GetBool(sm, "m_ChaseSettingsLocked"));
      out.push_back(b);
      game::Field f = game::FieldOf(sm, "m_SettingsStack");
      ue3::TArrayRaw a = Arr(sm, "m_SettingsStack");
      int esz = f.ok() ? ue3::PropElementSize(ue3::PropExtra(f.prop)) : 0;
      for (int i = 0; i < a.num && esz > 0; ++i) {
        std::string line = "  stack[" + std::to_string(i) + "]";
        for (int k = 0; k < esz; k += 4) {
          uint32_t v = util::ReadOr<uint32_t>(a.data + i * esz + k);
          line += " " + (ue3::IsObject(v) ? Obj(v) : util::Hex(v));
        }
        out.push_back(line);
      }
    }
  }
  if (ue3::IsA(cc, "DVCam_CameraController")) {
    snprintf(b, sizeof b, "dv: ceiling %d %.0f floor %.0f combatAllowed %d angelGlideCam %d lastEnemyCam %s",
             game::GetBool(cc, "m_UseCameraCeiling"), game::GetFloat(cc, "m_CameraCeiling"),
             game::GetFloat(cc, "m_CameraFloor"), game::GetBool(cc, "m_CombatModeAllowed"),
             game::GetBool(cc, "m_UseAngelGlideCamera"), Obj(game::GetObj(cc, "m_LastEnemyCamera")).c_str());
    out.push_back(b);
  }
  ue3::TArrayRaw d = Arr(cc, "m_DynamicDecorators");
  std::string decs = "decorators:";
  for (int i = 0; i < d.num; ++i) decs += " " + ue3::ClassNameOf(util::ReadOr<uintptr_t>(d.data + i * 4));
  out.push_back(decs);
  return out;
}

}  // namespace camera
