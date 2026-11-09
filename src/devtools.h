#pragma once
#include <string>

// The developers' leftovers in the shipped game (research: docs/debug.md).
//
// Dev room: the test map "Combat" (Practice -> Mission select). Its level script runs
// DVSeqAct_ToggleUpgrades -> DVExecFunctions.UnlockAllUpgrades, and the checkpoint it saves right away writes every
// ability into the save slot for good. While the loaded map is a dev room we skip that one call when the level
// script makes it; the same command run by hand (the game's debug menu, a console command) still works.
//
// Debug menu: NTPlayerController.DisplayDebugMenu (Scaleform "DEBUG MENU": Resume Game, Jump Menu, Console
// Commands). The game opens it from Handle_PausePressed only when NTWorldInfo.IsNTGoldMaster() is false, which it
// never is in the shipped game. With the Settings option on we do what the developers' build did with another
// button: Pause pressed while Angel + Demon (left + right trigger) are held opens the debug menu instead of the
// pause menu. Nothing else in the game changes; switched off, the pause button is the game's own again.
namespace devtools {

void Init();  // registers the ProcessEvent veto (once)
void AfterEvent(void* self, void* fn);  // from the features' "after" ProcessEvent hook

bool IsDevRoomMap(const std::string& map);  // "Combat"

// Settings -> Other -> "Unlock the game's debug menu" (config "debug_menu", off by default).
bool DebugMenuOn();
void SetDebugMenu(bool on);
int DebugMenuAvailable();  // 1 yes, 0 this game build has no debug menu, -1 not known yet (game not started)

// Counters for tests (autorun "devtools").
int UnlockAllBlocked();
int DebugMenuOpened();

}  // namespace devtools
