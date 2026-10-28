#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Save unlocks: locating the player profile & co, main-menu check, game-thread call sequences.
namespace unlock {

struct Refs {
  uintptr_t engine = 0;    // DVGameEngine instance
  uintptr_t profile = 0;   // DVGamerProfile (GGamerProfileData)
  uintptr_t upgrades = 0;  // NTCombat_UpgradesComponent (profile.m_UpgradesUnlocked)
  uintptr_t pc = 0;        // live player controller
  uintptr_t cheat = 0;     // pc.CheatManager (DVExecFunctions)
  uintptr_t shop = 0;      // live DVShopComponent of the current DVGameInfo
  uintptr_t game = 0;      // live DVGameInfo of the current map
  std::string map;         // persistent level package of the player controller
  std::string error;
};

Refs Locate();  // read-only, any thread
bool IsFrontEnd(const std::string& map);

// Read-only calls (GetTotalScore, IsBloodyPalaceUnlocked, HasUpgrade...) to validate ProcessEvent calls.
void RunTestCalls();

// Steps of "Save Unlocks". Each one uses the game's own functions where they exist. Steps before StepCount
// make up "Unlock All" (in this order: later steps rely on earlier ones, e.g. completion % needs the keys).
// RanksSSS is separate: mission ranks are the player's results, so it is never part of "Unlock All".
enum Step {
  Missions, StoryItems, SecretMissions, MundusKeys, Collectibles, Upgrades, HealthUps, TriggerUps, Costumes,
  Completion, ConceptArt, StepCount,
  RanksSSS = StepCount
};
// "missions", "story", "secret", "keys", "collectibles", "upgrades", "hp", "dt", "costumes", "completion",
// "conceptart", "ranks_sss"
const char* StepId(Step s);
const char* StepLabel(Step s);  // menu text

// Runs on a worker thread: checks (front end, save slot loaded) -> save backup -> achievements blocked ->
// steps on the game thread -> optional native save -> profile dump. withSave=false + no steps = "Save now".
void Run(std::vector<Step> steps, bool withSave);
// Test fixture (autorun "fixture", for the save export/import accuracy test): a "tournament" profile with a
// partial build - all missions/difficulties, some story items, 14 skills, +1 HP / +1 DT, a few items, 3 unspent
// upgrade points - then saved. Never used by the menu.
void RunFixture();
// Autorun entry: "all", "save", or a step id (incl. "ranks_sss").
void RunFromScript(const std::string& what);
// Save slot status as the game reports it (game-thread query, waits up to 3 s): -1 unknown.
int SlotStatus();

// Why the unlock buttons are (not) available. Only Ready allows a run.
enum class State { Initializing, NoHook, Loading, InMission, NoSave, Ready };
struct Status {
  State state = State::Initializing;
  std::string map;
  std::string detail;  // reason / extra info for the menu
};
const char* StateText(State s);  // "UE3 initializing...", "in mission", "main menu, save not loaded", "ready"...
// Cached, refreshed on a worker thread every ~2 s (never blocks the render thread). The refresh also calls
// ue3::Init(background) when UE3 is not ready yet, so the menu does not depend on the auto-init thread.
Status CurrentStatus();

std::vector<std::string> Report();  // lines for the menu (last run)
bool Busy();

}  // namespace unlock
