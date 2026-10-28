#pragma once
#include <windows.h>
#include <Xinput.h>

namespace input {
void InstallGlobalHooks();     // cursor + XInput hooks (init thread)
void AttachWindow(HWND hwnd);  // WndProc subclass (render thread, once)
void OnFrame(HWND hwnd);       // menu toggle, key capture, cursor state (render thread)
void SetBackBufferSize(int w, int h);  // mouse coordinates are scaled from client to back buffer

const char* KeyName(int vk);
// Gamepad 0 as it really is (the game's view is muted while the menu is open). False = no pad.
bool RawPadState(XINPUT_STATE* st);
}  // namespace input
