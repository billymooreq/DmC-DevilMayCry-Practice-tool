#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Cheats -> CAMERA "Unlock camera" (research: docs/camera.md).
//
// DmC's camera is one chase camera (DVCam_Chase_Dante) under a camera manager that picks the camera with the
// highest priority. Levels restrict it by: chase-settings contexts (corridor, platforming, boss) on a settings
// stack, fixed / rail cameras of NTCam_Mappable actors and LockCamera above the chase camera, lock-on targets,
// scripted SetYaw / SetTilt turns (some with stick input off) and a camera ceiling. The feature undoes those every
// frame outside cutscenes and puts them back when it turns off.
namespace features {
class Feature;
}

namespace camera {

std::vector<features::Feature*> CreateFeatures();  // registered by feature_list.cpp

// Tests / Dev (autorun "cam"): camera controller, camera stack, chase settings stack. Game thread.
std::vector<std::string> DebugLines();

}  // namespace camera
