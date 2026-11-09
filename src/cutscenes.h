#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Practice -> Cutscenes -> "Skip cutscenes" (research: docs/cutscenes.md).
//
// The game skips a cutscene when Back (Backspace) is pressed: DVPlayerController.Handle_BackPressed calls the native
// NTPlayerController.TryCancelCutscene(), which ends the playing matinee or Bink video the way the developers made
// it end on a skip (the level script still gets its "skipped/finished" outputs: doors, objectives, the next fight).
// With the feature on we ask the game every frame whether a cutscene is playing (GetCutsceneStatus) and call that
// same TryCancelCutscene() until the game accepts it. Cutscenes the game itself does not let you skip stay as they
// are. Works in missions and in the main menu.
namespace features {
class Feature;
}

namespace cutscenes {

features::Feature* CreateFeature();  // "skip_cutscenes" (registered by feature_list.cpp)

// Every ProcessEvent call (game thread, cheap): a skipped matinee's NTSeqAct_Interp.StartSkip gets FadeTime ~0,
// so the game's 1 s fade to black on a skip becomes a cut.
void OnEvent(uintptr_t self, uintptr_t fn);

// Tests / Dev (autorun "cutscenes"): counters and the last cutscenes seen.
std::vector<std::string> DebugLines();
void SetVerbose(bool on);  // log what is playing at the start of every cutscene
// What is playing now (matinees / Bink actions of the loaded levels). Game thread.
std::vector<std::string> Probe();

}  // namespace cutscenes
