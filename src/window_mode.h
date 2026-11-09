#pragma once
#include <d3d9.h>

// Settings > GAME WINDOW (#16, user): three modes, read at game start.
//  - Fullscreen: the game as it is (nothing is touched).
//  - Borderless: fullscreen in a window without borders over the whole monitor - Alt+Tab is instant, other windows
//    (LiveSplit, OBS) can stay on top.
//  - Window: a normal window with a frame that can be moved and resized (the picture is stretched to it).
// The game still believes it is fullscreen: only its D3D9 device is created windowed and its window is styled.
//  - DllMain: the exe's imports are redirected (before the game's WinMain runs): Direct3DCreate9 (then
//    IDirect3D9::CreateDevice is hooked), ChangeDisplaySettingsW (no desktop mode change), SetWindowPos /
//    SetWindowLongW / ShowWindow on the game window (our style and place, not minimized on Alt+Tab).
//  - A mode picked in the menu takes effect at the next start; RestartGame() does that start at once.
namespace winmode {

enum Mode { kFullscreen = 0, kBorderless = 1, kWindowed = 2 };
const char* ModeKey(int m);     // config value: "fullscreen" / "borderless" / "windowed"
int ModeFromKey(const char* s); // -1 = unknown

void InstallEarly();  // DllMain (loader lock: only reads a file and writes import table entries)
int Current();        // the mode of this game start
// d3d9hook::Reset: the device stays windowed after a reset (a resolution change in the game's options).
void FixPresent(D3DPRESENT_PARAMETERS* pp);
// Saves the config, closes the game the normal way (WM_CLOSE) and starts it again from its exe once this process
// has ended (a hidden PowerShell waits for it). Render thread.
bool RestartGame();

}  // namespace winmode
