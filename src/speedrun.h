#pragma once
#include <string>
#include <vector>

#include "json.hpp"

// Runs > Speedrun: a switch that makes the game leaderboard-clean, and a run start the community LiveSplit
// autosplitter sees (research: docs/timers.md, "Speedrun").
//  - Speedrun mode: every cheat off (features::GoLegit: features, presets, the menu pause, the game's debug menu), no
//    "Practice Tool Active", cheats locked in the menu, their keys and presets do nothing. Off: nothing turns on.
//  - Start run: the run timers from zero + the picked save slot loaded the way the slot screen does + the picked
//    mission / difficulty started like the tool's Mission select (the game's own map name, so LiveSplit starts).
//    It switches nothing else on (user, #16): not Speedrun mode, not a timer. With a timer of the tool on, it waits
//    until "Run time (LiveSplit)" really runs.
//  - Restart run: the same again.
namespace features {
class Feature;
}

namespace speedrun {

struct Pick {
  int slot = 0;          // save slot, 0-based
  int difficulty = 0;    // 0 Human .. 6 Hell and Hell
  std::string mission = "M01";
};
Pick GetPick();
void SetPick(const Pick& p);
void LoadJson(const nlohmann::json& j);  // DmCPracticeTool.json "speedrun"
nlohmann::json SaveJson();

void GoLegit();   // any thread
void Leave();     // any thread
void StartRun(bool reset);  // any thread: the job runs on its own thread (one at a time)
bool Busy();

// The result line under the buttons: what the last Speedrun mode switch / Start run did. kind: 0 working, 1 ok, 2 failed.
struct Status {
  std::string text;
  int kind = 0;
};
Status LastStatus();

std::vector<features::Feature*> CreateFeatures();  // registered by feature_list.cpp (Hotkeys: group Speedrun)
std::vector<std::string> DebugLines();             // autorun "speedrun"

}  // namespace speedrun
