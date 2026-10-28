#pragma once

// Input injected into the game's DirectInput devices (autorun tests only: in a mission the game ignores window
// messages). dik = DIK_* scan code; mouse button 0 = left, 1 = right.
namespace dinject {
void Key(int dik, bool down);
void MouseButton(int b, bool down);
bool KeyboardSeen();
bool KeyHeld(int dik);  // injected only
}  // namespace dinject
