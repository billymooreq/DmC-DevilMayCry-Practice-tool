#include "hotkeys.h"

#include <windows.h>
#include <Xinput.h>

#include <atomic>
#include <map>

#include "features.h"
#include "input.h"
#include "log.h"
#include "state.h"

namespace hotkeys {
namespace {

Bind* g_capture = nullptr;
bool g_captureKeyboardOnly = false;
bool g_captureArmed = false;  // all keys/buttons were released after the capture started
uint32_t g_capturePadMax = 0;
std::atomic<int> g_fake[256];  // vk -> frames left (autorun tests, set from another thread)
std::map<std::string, bool> g_prev;

bool Down(int vk) {
  if (vk <= 0) return false;
  if (vk < 256 && g_fake[vk] > 0) return true;
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

uint8_t HeldMods() {
  return (Down(VK_CONTROL) ? kCtrl : 0) | (Down(VK_SHIFT) ? kShift : 0) | (Down(VK_MENU) ? kAlt : 0);
}

bool IsModifier(int vk) {
  switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
      return true;
  }
  return false;
}

uint32_t PadButtons() {
  XINPUT_STATE st{};
  if (!input::RawPadState(&st)) return 0;
  uint32_t b = st.Gamepad.wButtons;
  if (st.Gamepad.bLeftTrigger > 128) b |= kPadLT;
  if (st.Gamepad.bRightTrigger > 128) b |= kPadRT;
  return b;
}

bool Satisfied(const Bind& b, uint8_t mods, uint32_t pad) {
  if (b.vk && Down(b.vk) && mods == b.mods) return true;
  if (b.pad && (pad & b.pad) == b.pad) return true;
  return false;
}

void Capture(uint32_t pad) {
  bool anyKey = false;
  int pressed = 0;
  for (int vk = 3; vk < 255; ++vk) {
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || IsModifier(vk)) continue;
    if (!Down(vk)) continue;
    anyKey = true;
    if (!pressed) pressed = vk;
  }
  if (!g_captureArmed) {
    if (!anyKey && !pad) g_captureArmed = true;  // wait until the click on "Bind" is released
    return;
  }
  if (pressed == VK_ESCAPE) {
    LOG("hotkeys: capture cancelled");
    g_capture = nullptr;
    return;
  }
  if (pressed) {
    g_capture->vk = pressed;
    g_capture->mods = HeldMods();
    LOG("hotkeys: captured key %s", Text(*g_capture).c_str());
    g_capture = nullptr;
    features::MarkDirty();
    return;
  }
  if (g_captureKeyboardOnly) return;
  // Gamepad: the combination is every button held together; it is taken when the first one is released.
  if (pad) {
    g_capturePadMax |= pad;
  } else if (g_capturePadMax) {
    g_capture->pad = g_capturePadMax;
    LOG("hotkeys: captured pad %s", Text(*g_capture).c_str());
    g_capture = nullptr;
    features::MarkDirty();
  }
}

const char* PadName(uint32_t bit) {
  switch (bit) {
    case XINPUT_GAMEPAD_DPAD_UP: return "DPad Up";
    case XINPUT_GAMEPAD_DPAD_DOWN: return "DPad Down";
    case XINPUT_GAMEPAD_DPAD_LEFT: return "DPad Left";
    case XINPUT_GAMEPAD_DPAD_RIGHT: return "DPad Right";
    case XINPUT_GAMEPAD_START: return "Start";
    case XINPUT_GAMEPAD_BACK: return "Back";
    case XINPUT_GAMEPAD_LEFT_THUMB: return "LS";
    case XINPUT_GAMEPAD_RIGHT_THUMB: return "RS";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB";
    case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
    case XINPUT_GAMEPAD_A: return "A";
    case XINPUT_GAMEPAD_B: return "B";
    case XINPUT_GAMEPAD_X: return "X";
    case XINPUT_GAMEPAD_Y: return "Y";
    case kPadLT: return "LT";
    case kPadRT: return "RT";
  }
  return "?";
}

}  // namespace

std::string Text(const Bind& b) {
  std::string k, p;
  if (b.vk) {
    if (b.mods & kCtrl) k += "Ctrl+";
    if (b.mods & kShift) k += "Shift+";
    if (b.mods & kAlt) k += "Alt+";
    k += input::KeyName(b.vk);
  }
  static const uint32_t order[] = {kPadLT, kPadRT, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                   XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
                                   XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
                                   XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_A,
                                   XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
  for (uint32_t bit : order)
    if (b.pad & bit) p += (p.empty() ? "" : "+") + std::string(PadName(bit));
  if (!k.empty() && !p.empty()) return k + " / " + p;
  return k + p;
}

bool Overlaps(const Bind& a, const Bind& b) {
  return (a.vk && a.vk == b.vk && a.mods == b.mods) || (a.pad && a.pad == b.pad);
}

nlohmann::json ToJson(const Bind& b) { return {{"key", b.vk}, {"mods", b.mods}, {"pad", b.pad}}; }

Bind FromJson(const nlohmann::json& j) {
  Bind b;
  if (!j.is_object()) return b;
  b.vk = j.value("key", 0);
  b.mods = (uint8_t)j.value("mods", 0);
  b.pad = j.value("pad", 0u);
  return b;
}

void Poll(bool gameForeground) {
  uint32_t pad = PadButtons();
  if (g_capture) {
    if (gameForeground) Capture(pad);
  } else {
    bool live = gameForeground && !state::menuOpen;
    uint8_t mods = HeldMods();
    auto edge = [&](const std::string& key, const Bind& b) {
      bool now = !b.Empty() && Satisfied(b, mods, pad);
      bool& prev = g_prev[key];
      bool fire = live && now && !prev;
      prev = now;  // updated while the menu is open too: closing it never fires a held bind
      return fire;
    };
    std::vector<std::string> toggle;
    std::vector<int> presets;
    {
      auto lk = features::Lock();
      features::Data& d = features::D();
      const auto& all = features::All();
      for (size_t i = 0; i < all.size() && i < d.binds.size(); ++i)
        if (!all[i]->noBind && edge(std::string("f:") + all[i]->id, d.binds[i])) toggle.push_back(all[i]->id);
      for (size_t i = 0; i < d.presets.size(); ++i)
        if (edge("p:" + d.presets[i].name, d.presets[i].bind)) presets.push_back((int)i);
    }
    for (const std::string& id : toggle) {
      features::Feature* f = features::Find(id);
      LOG("hotkeys: %s", id.c_str());
      if (f && f->action)
        features::Trigger(id);
      else
        features::ToggleManual(id);
    }
    for (int i : presets) features::TogglePreset(i);
  }
  for (auto& f : g_fake)
    if (f > 0) --f;
}

void BeginCapture(Bind* target, bool keyboardOnly) {
  g_capture = target;
  g_captureKeyboardOnly = keyboardOnly;
  g_captureArmed = false;
  g_capturePadMax = 0;
}

bool Capturing() { return g_capture != nullptr; }
const Bind* CaptureTarget() { return g_capture; }
void CancelCapture() { g_capture = nullptr; }

void FakePress(int vk) {
  if (vk > 0 && vk < 256) g_fake[vk] = 6;
}

}  // namespace hotkeys
