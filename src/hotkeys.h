#pragma once
#include <cstdint>
#include <string>

#include "json.hpp"

// Key bindings: a keyboard key (+ Ctrl/Shift/Alt) and/or a gamepad button combination. Render thread only.
namespace hotkeys {

enum Mod : uint8_t { kCtrl = 1, kShift = 2, kAlt = 4 };
constexpr uint32_t kPadLT = 0x10000, kPadRT = 0x20000;  // analog triggers as buttons, above the XInput bits

struct Bind {
  int vk = 0;          // keyboard / mouse virtual key, 0 = none
  uint8_t mods = 0;    // Mod bits that must be held with vk
  uint32_t pad = 0;    // XInput button bits (+ kPadLT/kPadRT); all must be held
  bool Empty() const { return vk == 0 && pad == 0; }
  bool operator==(const Bind& o) const { return vk == o.vk && mods == o.mods && pad == o.pad; }
};
std::string Text(const Bind& b);          // "Ctrl+F5", "LB+A", "F5 / LB+A", "" when empty
bool Overlaps(const Bind& a, const Bind& b);  // same key part or same pad part = conflict
nlohmann::json ToJson(const Bind& b);
Bind FromJson(const nlohmann::json& j);

// Every frame (input::OnFrame): fires features/presets on a fresh press. Nothing fires while the menu is open,
// a key is being captured or the game window is in the background.
void Poll(bool gameForeground);

// Capture into *target: the next keyboard key (with held modifiers) or gamepad combination (all buttons held
// before the first release). Esc cancels. keyboardOnly for the menu key.
void BeginCapture(Bind* target, bool keyboardOnly = false);
bool Capturing();
const Bind* CaptureTarget();
void CancelCapture();

// Tests (autorun): pretend vk is held for a few frames, as if pressed on the keyboard.
void FakePress(int vk);

}  // namespace hotkeys
