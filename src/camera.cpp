#include "camera.h"

#include <windows.h>

#include <cstdio>

#include "features.h"
#include "game.h"
#include "log.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace camera {
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

}  // namespace

std::vector<features::Feature*> CreateFeatures() { return {new UnlockCamera}; }

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
