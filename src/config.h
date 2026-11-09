#pragma once
#include <string>

#include "hotkeys.h"
#include "json.hpp"

// DmCPracticeTool.json next to the exe. Auto-loaded at startup, saved automatically on changes (binds, sliders,
// presets, window) and by Save/Load in Settings. The "Practice Tool Active" label has no settings on purpose:
// old "overlay" / "hold_hotkey" / "debug.force_overlay" entries are ignored and dropped on the next save.
struct Config {
  hotkeys::Bind menuKey{0x2D, 0, 0};  // open / close the menu: any key (+ modifiers) or gamepad combination
  bool showDevTab = false;
  bool pauseInMenu = true;  // the game stands still while the menu is open (in a mission)
  bool hideHints = false;   // "Disable hints": no description box at the bottom of the menu
  bool debugMenu = false;   // "Unlock the game's debug menu": Pause + Angel + Demon opens it (devtools.cpp)
  int windowMode = 0;       // Settings > GAME WINDOW: 0 fullscreen, 1 fullscreen window, 2 window (winmode::Mode);
                            // read at game start (window_mode.cpp, DllMain); picking one restarts the game

  // Menu window, fractions of the screen (w <= 0 = not saved yet).
  float winX = 0, winY = 0, winW = 0, winH = 0;
  // Height of the save list on the Saves tab in text lines (the player drags its bottom edge).
  float savesListRows = 6;

  // Hooks (switch off to narrow down a crash).
  bool hookD3D9 = true;
  std::string renderHook = "vtable";  // "vtable" | "present" | "endscene" (see d3d9_hook.cpp)
  bool hookWndProc = true;
  bool hookDInput = true;   // mute keyboard/mouse DirectInput while the menu is open
  bool hookXInput = true;   // mute the gamepad while the menu is open
  bool hookCursor = true;   // stop the game from re-centering/clipping the cursor while the menu is open

  bool debugMenuOpenAtStart = false;
  int dumpArrayLimit = 256;
  bool peLog = false;  // log ProcessEvent calls matching peLogFilter (comma separated substrings)
  std::string peLogFilter = "Save,Upgrade,Health,DevilTrigger,Achievement,Stat,Costume,DLC,Pickup,Unlock,Mission";

  // Feature binds/parameters and presets, owned by features::LoadJson/SaveJson.
  nlohmann::json features = nlohmann::json::object();
  nlohmann::json presets = nlohmann::json::array();
};

extern Config g_cfg;

bool LoadConfig();   // false = no file / parse error (defaults kept)
bool SaveConfig();
void MarkConfigDirty();     // any thread
void ConfigAutosaveTick();  // render thread, every frame: saves ~1 s after the last change
