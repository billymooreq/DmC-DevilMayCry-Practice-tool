#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Practice -> CUTSCENES "Skip hints & pop-ups" (research: docs/popups.md).
//
// Closes the game's pop-ups the way their own button does, so whatever the game does after them still happens:
//  - "item / weapon / ability acquired" screens (IngameUI component 36): the OK button's callback
//    DVGFxMovieHandler_IngameUI_Callbacks.Death_ResumeGame() (next queued item, then resume); the gold orb
//    "use it?" choice (Item_GoldOrb_Use) is a choice, not a notice - left alone;
//  - tutorial hints of the level scripts (DVSeqAct_TutorialMessage, also the ones that pause the game until a
//    button): their own Hide() (unpause, "Hidden" output);
//  - secret missions' "Press Back to begin": DVSecretMissionManager.ReadyButtonPressed() once the game allows it.
// Pause menu, results, confirmations and menus are never touched.
namespace features {
class Feature;
}

namespace popups {

features::Feature* CreateFeature();  // "skip_popups" (registered by feature_list.cpp)

// Every ProcessEvent call (game thread, cheap): remembers the tutorial actions the level runs.
void OnEvent(uintptr_t self, uintptr_t fn);

// Tests (autorun "popups"): counters.
std::vector<std::string> DebugLines();

}  // namespace popups
