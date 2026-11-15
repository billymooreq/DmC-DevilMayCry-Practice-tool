#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Cheats -> CAMERA "Unlock camera" and Freecam (research: docs/camera.md).
//
// DmC's camera is one chase camera (DVCam_Chase_Dante) under a camera manager that picks the camera with the
// highest priority. Levels restrict it by: chase-settings contexts (corridor, platforming, boss) on a settings
// stack, fixed / rail cameras of NTCam_Mappable actors and LockCamera above the chase camera, lock-on targets,
// scripted SetYaw / SetTilt turns (some with stick input off) and a camera ceiling. The feature undoes those every
// frame outside cutscenes and puts them back when it turns off.
namespace features {
class Feature;
}
namespace game {
struct World;
}

namespace camera {

std::vector<features::Feature*> CreateFeatures();  // registered by feature_list.cpp

// Freecam: the camera flies on its own (Noclip then ignores the keys; while Dante follows the camera, Noclip
// leaves his position alone). Any thread.
bool FreecamFlying();
bool FreecamHoldsDante();
// Menu / keys: Freecam (triggers=false) or Freecam (triggers): one feature, two modes. Any thread.
void ToggleFreecamMode(bool triggers);
// Every frame (game thread, from features::OnFrame): Dante let go by Freecam hangs in the air until he moves.
void OnFrame(const game::World& w);
// ProcessEvent "after" observer (game thread): replaces the view the game renders while Freecam flies.
void OnEventAfter(uintptr_t self, uintptr_t fn, void* parms);
std::vector<std::string> FreecamDebug();  // autorun "freecam"
// autorun "freecam_at x y z [yaw pitch]" (degrees): puts the flying camera there. Game thread.
bool FreecamSetAt(float x, float y, float z, float yawDeg, float pitchDeg, bool rot);

// Tests / Dev (autorun "cam"): camera controller, camera stack, chase settings stack. Game thread.
std::vector<std::string> DebugLines();

}  // namespace camera
