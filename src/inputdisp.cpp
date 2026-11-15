#include "inputdisp.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>

#include "config.h"
#include "features.h"
#include "imgui.h"
#include "log.h"
#include "state.h"
#include "ui.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace inputdisp {
namespace {

// ------------------------------------------------------------------ the game's buttons
// NTInput.ENTButton: the game's own buttons (pad layout). Keyboard keys and pad buttons are mapped to these first
// (DMCKbdInputConfig.ini / DMCGamepadInputConfig.ini - the game's rebinding), then DVButtonMap maps actions to them.
// The numbers differ between game versions (v1.0 has NTB_V_GAME9 and no NTB_DEVILTRIGGER, so SWITCH_TO_*,
// FORWARD... are one higher there): every number comes from the game's own enum, by name.
constexpr int kMaxNtb = 64;
constexpr uint8_t kNo = 0xFF;      // no button
std::vector<std::string> g_enum;   // game number -> name ("NTB_A_CROSS"); written once by the game thread
std::atomic<bool> g_enumReady{false};
int g_none = -1, g_v1 = -1;        // NTB_NONE, NTB_V_GAME1 (V_GAME1..8 follow it)

int Idx(const char* name) {
  if (!name) return -1;
  for (size_t i = 0; i < g_enum.size(); ++i)
    if (g_enum[i] == name) return (int)i;
  return -1;
}
uint8_t Btn(const char* name) {
  int i = Idx(name);
  return i >= 0 && i < kMaxNtb ? (uint8_t)i : kNo;
}
bool Valid(int b) { return b >= 0 && b < (int)g_enum.size() && b < kMaxNtb && b != g_none; }
bool IsVirtual(int b) { return g_v1 >= 0 && b >= g_v1 && b < g_v1 + 8; }

// A pad button's short name when the game's gamepad config does not say (Xbox layout).
const std::pair<const char*, const char*> kPadDefault[] = {
    {"NTB_DPADUP", "D\xE2\x86\x91"}, {"NTB_DPADLEFT", "D\xE2\x86\x90"}, {"NTB_DPADRIGHT", "D\xE2\x86\x92"},
    {"NTB_DPADDOWN", "D\xE2\x86\x93"}, {"NTB_A_CROSS", "A"}, {"NTB_B_CIRCLE", "B"}, {"NTB_X_SQUARE", "X"},
    {"NTB_Y_TRIANGLE", "Y"}, {"NTB_RIGHT_TRIGGER", "RT"}, {"NTB_RIGHT_SHOULDER", "RB"}, {"NTB_LEFT_TRIGGER", "LT"},
    {"NTB_LEFT_SHOULDER", "LB"}, {"NTB_BACK", "Back"}, {"NTB_START", "Start"}, {"NTB_RIGHT_STICK", "R3"},
    {"NTB_LEFT_STICK", "L3"}};

// ------------------------------------------------------------------ actions shown (the game's names: Options >
// Controls, GFxUI.int [ControllerRemappingScreen]). Order = left to right in a row.
enum Act {
  kMelee, kSpecial, kShoot, kJump, kAngel, kDemon, kEvade1, kEvade2, kDT, kGun, kAngelWpn, kDemonWpn, kTarget,
  kCamera, kToAngel, kToDemon, kToHuman, kActCount
};
struct ActDef {
  const char* field;   // DVButtonMap property: the button of this action
  const char* field2;  // a second button that also does it (Devil Trigger: the keyboard's own DT key)
  const char* def;     // DefaultInput.ini, when the map cannot be read (NTB name; nullptr = none)
  const char* def2;
  const char* text;    // Text style
  ImU32 col;
};
#define C(r, g, b) IM_COL32(r, g, b, 255)
const ActDef kActs[kActCount] = {
    {"LIGHT_MELEE", nullptr, "NTB_Y_TRIANGLE", nullptr, "Melee", C(0xF2, 0xC1, 0x4E)},     // Basic Melee Attack
    {"STRONG_MELEE", nullptr, "NTB_B_CIRCLE", nullptr, "Special", C(0xFF, 0x5A, 0x5F)},    // Special Melee Attack
    {"RANGE_ATTACK", nullptr, "NTB_X_SQUARE", nullptr, "Shoot", C(0x4F, 0xA3, 0xFF)},      // Shoot
    {"Jump", nullptr, "NTB_A_CROSS", nullptr, "Jump", C(0x5F, 0xD3, 0x8A)},                // Jump
    {"ANGEL_MODE", nullptr, "NTB_LEFT_TRIGGER", nullptr, "Angel", C(0x3D, 0xE3, 0xDA)},    // Angel Mode (hold)
    {"DEMON_MODE", nullptr, "NTB_RIGHT_TRIGGER", nullptr, "Demon", C(0xFF, 0x8A, 0x3D)},   // Demon Mode (hold)
    {"EVADE1", nullptr, "NTB_RIGHT_SHOULDER", nullptr, "Evade", C(0xB9, 0x92, 0xFF)},      // Evade 1
    {"EVADE2", nullptr, "NTB_LEFT_SHOULDER", nullptr, "Evade", C(0xB9, 0x92, 0xFF)},       // Evade 2
    // Devil Trigger: the pad's pair (V_GAME3 = L3 + R3); v1.3's keyboard also has its own key (DEVILTRIGGER2)
    {"DEVILTRIGGER", "DEVILTRIGGER2", "NTB_V_GAME3", "NTB_DEVILTRIGGER", "DT", C(0xFF, 0x4F, 0xD8)},
    {"CYCLE_RANGED_WEAPON", nullptr, "NTB_DPADUP", nullptr, "Gun", C(0xCF, 0xCF, 0xCF)},        // Cycle Gun
    {"CYCLE_ANGEL_WEAPON", nullptr, "NTB_DPADLEFT", nullptr, "A.wpn", C(0x9F, 0xE9, 0xE4)},     // Cycle Angel Weapon
    {"CYCLE_DEMON_WEAPON", nullptr, "NTB_DPADRIGHT", nullptr, "D.wpn", C(0xFF, 0xC2, 0x9A)},    // Cycle Demon Weapon
    {"CYCLE_RANGED_TARGET", nullptr, "NTB_LEFT_STICK", nullptr, "Target", C(0xA9, 0xB2, 0xC0)}, // Cycle Ranged Target
    {"CENTRE_CAMERA", nullptr, "NTB_RIGHT_STICK", nullptr, "Camera", C(0xA9, 0xB2, 0xC0)},      // Center Camera
    {"SWITCH_TO_ANGEL_MODE", nullptr, "NTB_SWITCH_TO_ANGEL", nullptr, ">Angel", C(0x3D, 0xE3, 0xDA)},  // keys 1/2/3
    {"SWITCH_TO_DEMON_MODE", nullptr, "NTB_SWITCH_TO_DEMON", nullptr, ">Demon", C(0xFF, 0x8A, 0x3D)},
    {"SWITCH_TO_HUMAN_MODE", nullptr, "NTB_SWITCH_TO_HUMAN", nullptr, ">Human", C(0xEC, 0xEC, 0xEC)},
};
#undef C

// ------------------------------------------------------------------ the list
struct Row {
  int dir = 5;          // numpad notation: 8 up (forward), 2 down, 4 left, 6 right, 5 neutral
  uint32_t acts = 0;    // bit per Act
  double t = 0;         // game seconds it was held
  int frames = 0;       // real frames it was held
  bool Same(const Row& o) const { return dir == o.dir && acts == o.acts; }
  bool Neutral() const { return dir == 5 && acts == 0; }
};
constexpr size_t kKeep = 64;

std::mutex g_mu;            // g_rows, g_opt, labels
std::deque<Row> g_rows;     // newest first
Options g_opt;
std::atomic<bool> g_on{false};
std::atomic<bool> g_clear{false};
float g_idle = 0;           // game thread: real seconds the list showed no input (neutral)
unsigned g_counted = 0, g_zeroDt = 0;  // frames counted / of them with the world's time standing (autorun log)
std::atomic<float> g_alpha{1.0f};
float g_x = -1, g_y = -1;   // the player's place (screen fractions); -1 = default
bool g_replace = false;

// Buttons of each action (from the game's DVButtonMap) and the pairs of its two-button "virtual" buttons.
// Game numbers (kNo = none).
struct Map {
  uint8_t btn[kActCount][2];
  uint8_t pair[8][2];       // NTB_V_GAMEk = VIRTUAL_k_1 + VIRTUAL_k_2
  bool fromGame = false;
} g_map;

// Short names of the keys / pad buttons bound to each NTB button (the game's ini files), render thread reads.
std::string g_keyName[kMaxNtb];
std::string g_padName[kMaxNtb];

// ------------------------------------------------------------------ game's ini files (the player's rebinding)
std::wstring IniDir() {
  wchar_t docs[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) return L"";
  return std::wstring(docs) + L"\\My Games\\UnrealEngine3\\DevilGame\\";
}

std::string Narrow(const wchar_t* w) {
  char b[128];
  WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
  return b;
}

// UE3 key name -> short label ("LeftMouseButton" -> "LMB").
std::string ShortKey(const std::string& k) {
  static const std::pair<const char*, const char*> kNames[] = {
      {"LeftMouseButton", "LMB"}, {"RightMouseButton", "RMB"}, {"MiddleMouseButton", "MMB"},
      {"ThumbMouseButton", "M4"}, {"ThumbMouseButton2", "M5"}, {"MouseScrollUp", "Wh\xE2\x86\x91"},
      {"MouseScrollDown", "Wh\xE2\x86\x93"}, {"SpaceBar", "Space"}, {"LeftShift", "Shift"}, {"RightShift", "RShift"},
      {"LeftControl", "Ctrl"}, {"RightControl", "RCtrl"}, {"LeftAlt", "Alt"}, {"RightAlt", "RAlt"},
      {"Escape", "Esc"}, {"BackSpace", "Bksp"}, {"Enter", "Enter"}, {"Tab", "Tab"}, {"CapsLock", "Caps"},
      {"Up", "\xE2\x86\x91"}, {"Down", "\xE2\x86\x93"}, {"Left", "\xE2\x86\x90"}, {"Right", "\xE2\x86\x92"},
      {"Zero", "0"}, {"One", "1"}, {"Two", "2"}, {"Three", "3"}, {"Four", "4"}, {"Five", "5"}, {"Six", "6"},
      {"Seven", "7"}, {"Eight", "8"}, {"Nine", "9"}, {"Tilde", "~"}, {"Insert", "Ins"}, {"Delete", "Del"},
      {"PageUp", "PgUp"}, {"PageDown", "PgDn"}, {"Home", "Home"}, {"End", "End"}, {"Semicolon", ";"},
      {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Backslash", "\\"}, {"LeftBracket", "["},
      {"RightBracket", "]"}, {"Quote", "'"}, {"Underscore", "-"}, {"Equals", "="}};
  for (const auto& n : kNames)
    if (k == n.first) return n.second;
  if (k.rfind("NumPad", 0) == 0) {
    std::string r = ShortKey(k.substr(6));
    return "Num" + r;
  }
  return k.size() > 6 ? k.substr(0, 6) : k;
}

// DirectInput button name of the game's gamepad config -> short label ("Left Shoulder" -> "LB").
std::string ShortPad(const std::string& n) {
  static const std::pair<const char*, const char*> kNames[] = {
      {"Left Shoulder", "LB"}, {"Right Shoulder", "RB"}, {"Left trigger", "LT"}, {"Right trigger", "RT"},
      {"Left thumb stick", "L3"}, {"Right thumb stick", "R3"}, {"DPad Up", "D\xE2\x86\x91"},
      {"DPad Down", "D\xE2\x86\x93"}, {"DPad Left", "D\xE2\x86\x90"}, {"DPad Right", "D\xE2\x86\x92"}};
  for (const auto& p : kNames)
    if (n == p.first) return p.second;
  return n.size() > 6 ? n.substr(0, 6) : n;
}

FILETIME g_kbdTime{}, g_padTime{};

FILETIME FileTime(const std::wstring& p) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return FILETIME{};
  return d.ftLastWriteTime;
}

// Reads the two ini files again when they changed (the game writes them when the player rebinds).
void ReadIni(bool force) {
  std::wstring dir = IniDir();
  if (dir.empty()) return;
  std::wstring kbd = dir + L"DMCKbdInputConfig.ini", pad = dir + L"DMCGamepadInputConfig.ini";
  FILETIME kt = FileTime(kbd), pt = FileTime(pad);
  bool kch = force || CompareFileTime(&kt, &g_kbdTime) != 0, pch = force || CompareFileTime(&pt, &g_padTime) != 0;
  if (!kch && !pch) return;
  g_kbdTime = kt, g_padTime = pt;
  if (!g_enumReady) return;
  std::string keys[kMaxNtb], pads[kMaxNtb];
  wchar_t b[128];
  int n = std::min((int)g_enum.size(), kMaxNtb);
  for (int i = 0; i < n; ++i) {
    std::wstring nm(g_enum[i].begin(), g_enum[i].end());
    GetPrivateProfileStringW(L"Keyboard mappings", nm.c_str(), L"", b, 128, kbd.c_str());
    keys[i] = b[0] ? ShortKey(Narrow(b)) : "";
    for (const auto& d : kPadDefault)
      if (g_enum[i] == d.first) pads[i] = d.second;
    if (i < 16) {  // the pad's 16 buttons come first in both versions
      std::wstring k = L"NinjaInputIdToDirectInputId" + std::to_wstring(i);
      int id = (int)GetPrivateProfileIntW(L"ControllerId 0", k.c_str(), -2, pad.c_str());
      if (id >= 0) {
        std::wstring sname = L"DirectInputIdToString" + std::to_wstring(id);
        GetPrivateProfileStringW(L"ControllerId 0", sname.c_str(), L"", b, 128, pad.c_str());
        if (b[0] && wcscmp(b, L"Unused") != 0) pads[i] = ShortPad(Narrow(b));
      } else if (id == -1) {
        pads[i] = "";
      }
    }
  }
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < kMaxNtb; ++i) g_keyName[i] = keys[i], g_padName[i] = pads[i];
  auto at = [](const std::string* v, uint8_t i) { return i < kMaxNtb ? v[i].c_str() : "-"; };
  LOG("inputdisp: key names read (Melee key '%s', pad '%s'; Jump key '%s', pad '%s'; DT key '%s')",
      at(keys, g_map.btn[kMelee][0]), at(pads, g_map.btn[kMelee][0]), at(keys, g_map.btn[kJump][0]),
      at(pads, g_map.btn[kJump][0]), at(keys, g_map.btn[kDT][1]));
}

// ------------------------------------------------------------------ reading the game (game thread)
// The game's ENTButton names (once): every button number below comes from them.
bool LoadEnum() {
  uintptr_t e = ue3::FindObject("Enum", "ENTButton");
  if (!e) return false;
  std::vector<std::string> names = ue3::EnumNames(e);
  if (names.size() < 20) return false;
  g_enum = names;
  g_none = Idx("NTB_NONE");
  g_v1 = Idx("NTB_V_GAME1");
  for (int a = 0; a < kActCount; ++a) g_map.btn[a][0] = Btn(kActs[a].def), g_map.btn[a][1] = Btn(kActs[a].def2);
  static const char* const kPairs[4][2] = {{"NTB_Y_TRIANGLE", "NTB_B_CIRCLE"}, {"NTB_A_CROSS", "NTB_X_SQUARE"},
                                           {"NTB_LEFT_STICK", "NTB_RIGHT_STICK"}, {"NTB_X_SQUARE", "NTB_Y_TRIANGLE"}};
  for (int k = 0; k < 8; ++k)
    for (int i = 0; i < 2; ++i) g_map.pair[k][i] = k < 4 ? Btn(kPairs[k][i]) : kNo;
  g_enumReady = true;
  LOG("inputdisp: %zu game buttons (NTB_NONE %d, NTB_V_GAME1 %d, NTB_DEVILTRIGGER %d)", g_enum.size(), g_none, g_v1,
      Idx("NTB_DEVILTRIGGER"));
  return true;
}

void ReadMap(uintptr_t input) {
  uintptr_t m = game::GetObj(input, "m_CurrentButtonMap");
  if (!m) return;
  auto get = [m](const char* f, uint8_t def) -> uint8_t {
    if (!f) return kNo;
    game::Field fl = game::FieldOf(m, f);
    if (!fl.ok()) return def;  // v1.0 has no DEVILTRIGGER2 / STRONG_MELEE2
    uint8_t v = def;
    util::Read(m + fl.off, v);
    return Valid(v) ? v : kNo;
  };
  Map n = g_map;
  for (int a = 0; a < kActCount; ++a) {
    n.btn[a][0] = get(kActs[a].field, n.btn[a][0]);
    n.btn[a][1] = get(kActs[a].field2, n.btn[a][1]);
  }
  for (int k = 0; k < 8; ++k) {
    char f1[16], f2[16];
    snprintf(f1, sizeof f1, "VIRTUAL_%d_1", k + 1);
    snprintf(f2, sizeof f2, "VIRTUAL_%d_2", k + 1);
    n.pair[k][0] = get(f1, n.pair[k][0]);
    n.pair[k][1] = get(f2, n.pair[k][1]);
  }
  if (!g_map.fromGame) LOG("inputdisp: button map from the game (%s): melee %d special %d shoot %d jump %d dt %d/%d",
                           ue3::ObjName(m).c_str(), n.btn[kMelee][0], n.btn[kSpecial][0], n.btn[kShoot][0],
                           n.btn[kJump][0], n.btn[kDT][0], n.btn[kDT][1]);
  n.fromGame = true;
  g_map = n;
}

struct Raw {
  uint64_t held = 0;  // bit per NTB button, held now
  float sx = 0, sy = 0, mag = 0;
};

// The buttons the game uses this frame: NTInput.m_VirtualButtonsHeld, bit = 1 << ENTButton (probe #23: the same as
// IsHeld(); m_HardwareButtonsThisFrame is the raw buttons - with two buttons of a pair held, the logical one has
// the pair's virtual button instead, e.g. C + Z on the keyboard = NTB_V_GAME3 = Devil Trigger). Movement (keyboard
// W/A/S/D too - they set no button bits) is the left stick m_vLeftStick_Raw (NTStick): +12 forward (-1..1),
// +16 right, +24 how far it is pushed (0..1; +12/+16 keep the last direction after letting go).
bool ReadRaw(uintptr_t input, Raw& r) {
  static game::Field held, stick;
  static uintptr_t cls = 0;
  uintptr_t c = util::ReadOr<uintptr_t>(input + 0x30);
  if (c != cls) {
    cls = c;
    held = game::FieldOf(input, "m_VirtualButtonsHeld");
    stick = game::FieldOf(input, "m_vLeftStick_Raw");
  }
  if (!held.ok()) return false;
  uint32_t b = 0;
  if (!util::Read(input + held.off, b)) return false;
  r.held = b;
  if (stick.ok()) {
    float f[7] = {};
    if (util::SafeRead(input + stick.off, f, sizeof f)) r.sy = f[3], r.sx = f[4], r.mag = f[6];
  }
  return true;
}

bool Held(uint64_t held, int b) { return Valid(b) && (held >> b & 1); }

Row Sample(const Raw& r) {
  Row s;
  const Map& m = g_map;
  // A virtual button other than Devil Trigger's (Gun Special = Jump + Shoot, ...): show the two buttons pressed.
  uint64_t held = r.held;
  for (int k = 0; k < 8 && g_v1 >= 0; ++k)
    if (Held(held, g_v1 + k) && m.btn[kDT][0] != g_v1 + k)
      for (int i = 0; i < 2; ++i)
        if (Valid(m.pair[k][i])) held |= 1ull << m.pair[k][i];
  for (int a = 0; a < kActCount; ++a)
    if (Held(held, m.btn[a][0]) || Held(held, m.btn[a][1])) s.acts |= 1u << a;
  int x = 0, y = 0;
  if (r.mag > 0.5f) {  // the stick (and W/A/S/D): 8 directions of 45 degrees
    float ang = std::atan2(r.sy, r.sx);  // 0 = right, counter-clockwise
    int sec = (int)std::floor((ang + 3.14159265f / 8) / (3.14159265f / 4));
    static const int kX[8] = {1, 1, 0, -1, -1, -1, 0, 1}, kY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    sec = ((sec % 8) + 8) % 8;
    x = kX[sec], y = kY[sec];
  }
  s.dir = 5 + x + 3 * y;
  return s;
}

// ------------------------------------------------------------------ probe (research, autorun)
std::atomic<double> g_probeUntil{0};

double NowS() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return (double)c.QuadPart / (double)f.QuadPart;
}

void ProbeFrame(const game::World& w, uintptr_t input) {
  static std::string last;
  static bool header = false;
  if (!header) {
    header = true;
    LOG("inputprobe: PlayerInput %s class %s", ue3::ObjName(input).c_str(),
        ue3::ObjName(util::ReadOr<uintptr_t>(input + 0x30)).c_str());
    uintptr_t m = game::GetObj(input, "m_CurrentButtonMap");
    if (m) {
      std::string s;
      for (int i = 0; i < 0x31; ++i) {
        char b[8];
        snprintf(b, sizeof b, "%d ", util::ReadOr<uint8_t>(m + 0x38 + i));
        s += b;
      }
      LOG("inputprobe: map %s bytes +0x38: %s", ue3::ObjName(m).c_str(), s.c_str());
    }
    if (uintptr_t st = ue3::FindObject("ScriptStruct", "NTStick")) {
      std::string s;
      for (uintptr_t f = ue3::StructChildren(st); f && s.size() < 600; f = ue3::FieldNext(f)) {
        char b[96];
        snprintf(b, sizeof b, "+%d %s %s; ", ue3::PropOffset(f), ue3::ClassNameOf(f).c_str(), ue3::ObjName(f).c_str());
        s += b;
      }
      LOG("inputprobe: struct NTStick (%d bytes): %s", ue3::StructSize(st), s.c_str());
    }
  }
  auto gi = [&](const char* f) { return game::GetInt(input, f, -1); };
  uint64_t isHeld = 0;
  for (int i = 0; i < (int)g_enum.size() && i < kMaxNtb; ++i) {
    engine::Call c(input, "IsHeld");
    c.Byte("checkButton", (uint8_t)i);
    if (c.Invoke() && c.RetBool()) isHeld |= 1ull << i;
  }
  int dv = -1;
  {
    engine::Call c(input, "GetDVButtonsHeld");
    if (c.Invoke()) dv = c.RetInt();
  }
  auto stick = [&](const char* f) {
    game::Field fl = game::FieldOf(input, f);
    std::string s;
    if (!fl.ok()) return s;
    for (int i = 0; i < 9; ++i) {
      char b[16];
      snprintf(b, sizeof b, "%.2f ", util::ReadOr<float>(input + fl.off + i * 4));
      s += b;
    }
    return s;
  };
  char b[700];
  snprintf(b, sizeof b,
           "hw %08X last %08X newLast %08X | vPress %08X vHeld %08X vRel %08X | IsHeld %09llX | DVHeld %08X | "
           "flags %08X | L [%s] mov [%s] menu %d",
           (unsigned)gi("m_HardwareButtonsThisFrame"), (unsigned)gi("m_HardwareButtonsLastFrame"),
           (unsigned)gi("m_HardwareButtonsNewLastFrame"), (unsigned)gi("m_VirtualButtonsPressed"),
           (unsigned)gi("m_VirtualButtonsHeld"), (unsigned)gi("m_VirtualButtonsReleased"),
           (unsigned long long)isHeld, (unsigned)dv, (unsigned)gi("m_InputDisableFlags"), stick("m_vLeftStick_Raw").c_str(),
           stick("m_vMovement").c_str(), (int)state::menuOpen);
  if (last != b) {
    last = b;
    LOG("inputprobe: %s", b);
  }
}

// ------------------------------------------------------------------ features
class DisplayFeature : public features::Feature {
 public:
  DisplayFeature() {
    id = "input_display";
    name = "Input display";
    group = features::Group::Visuals;
    help = "Shows your inputs like a fighting game's training mode: newest on top, with how many frames each was "
           "held. Not a cheat: works in Speedrun mode. Drag it while the menu is open.";
    anywhere = true;
    noOverlay = true;
  }
  bool Enable(const features::Ctx&) override {
    g_on = true;
    return true;
  }
  void Disable() override { g_on = false; }
};

class DisplayAction : public features::Feature {
 public:
  DisplayAction(const char* i, const char* n, const char* h, void (*run)()) : run_(run) {
    id = i;
    name = n;
    help = h;
    group = features::Group::Visuals;
    action = true;
    anywhere = true;
    frontEndOk = true;
    noOverlay = true;
  }
  void Run(const features::Ctx&) override { run_(); }

 private:
  void (*run_)();
};

}  // namespace

std::vector<features::Feature*> CreateFeatures() {
  return {new DisplayFeature,
          new DisplayAction("input_display_clear", "Clear input display", "Empties the input list.", [] { Clear(); }),
          new DisplayAction("input_display_reset_position", "Reset input display position",
                            "Puts the input list back on the left of the screen. Drag it anywhere while the menu is open.",
                            [] { ResetPosition(); })};
}

void OnFrame(const game::World& w) {
  bool probe = g_probeUntil.load() > 0;
  if (!g_on && !probe) {
    g_idle = 0;
    return;
  }
  if (g_clear.exchange(false)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_rows.clear();
    g_idle = 0;
  }
  if (!g_enumReady) {
    if (!LoadEnum()) return;
    ReadIni(true);
  }
  static int iniTick = 0;
  if (iniTick-- <= 0) {
    iniTick = 120;
    ReadIni(false);
  }
  uintptr_t input = w.pc ? game::GetObj(w.pc, "PlayerInput") : 0;
  if (input && !ue3::IsA(input, "NTInput")) input = 0;
  if (probe) {
    if (NowS() > g_probeUntil.load()) {
      g_probeUntil = 0;
      LOG("inputprobe: end");
    } else if (input) {
      ProbeFrame(w, input);
    }
  }
  if (!g_on) return;
  static uintptr_t mapFor = 0;
  static int mapTick = 0;
  if (input && (input != mapFor || mapTick-- <= 0)) {
    mapFor = input;
    mapTick = 300;
    ReadMap(input);
  }
  // Frozen (nothing counts, no new rows): no mission (menus, loading), the game paused (its pause menu, the tool's
  // menu pause) or the tool's menu open (the game gets no input then), the world's time not running.
  bool paused = w.world && game::GetObj(w.world, "Pauser") != 0;
  if (!w.mission || !input || paused || state::menuOpen) return;
  Raw raw;
  if (!ReadRaw(input, raw)) return;
  Row s = Sample(raw);
  // Time: the world's time of this frame without the world's slow-downs (TimeDilation: the game's hit stops and the
  // tool's Game speed), times the tool's Game speed (Cheats > Speed): 0.5x counts half as many frames for the same
  // real hold. Not the frame's real time: a loading hitch inside a mission would count. Not "world time did not move
  // = paused": at 60 fps the world's TimeSeconds sometimes stays one frame and moves two the next (#23: every 8th
  // frame was lost, every hold showed 1 frame short).
  float speed = 1.0f;
  {
    features::View v = features::Get("game_speed");
    if (v.applied) speed = v.p[0];
  }
  float td = w.world ? game::GetFloat(w.world, "TimeDilation", 1.0f) : 1.0f;
  if (!(td > 0.001f)) td = 1.0f;
  double dt = (double)w.gameDt / td * speed;
  ++g_counted;
  if (w.gameDt <= 0) ++g_zeroDt;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_rows.empty()) {  // the frame that just ended belongs to the state of the last sample
    Row& top = g_rows.front();
    top.t += dt;
    top.frames += 1;
  }
  if (g_rows.empty() || !g_rows.front().Same(s)) {
    g_rows.push_front(s);
    if (g_rows.size() > kKeep) g_rows.pop_back();
    g_idle = 0;
  } else if (s.Neutral()) {
    g_idle += w.realDt;
  } else {
    g_idle = 0;
  }
  float a = 1.0f;
  if (g_opt.hideAfter > 0 && g_idle > g_opt.hideAfter) a = std::max(0.0f, 1.0f - (g_idle - g_opt.hideAfter) / 0.4f);
  g_alpha = a;
}

// ------------------------------------------------------------------ drawing (render thread)
namespace {

ImU32 Alpha(ImU32 c, float a) {
  int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a + 0.5f);
  return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}

ImU32 Dark(ImU32 c, float k) {  // the colour k times as bright
  int r = (int)((c >> IM_COL32_R_SHIFT & 0xFF) * k), g = (int)((c >> IM_COL32_G_SHIFT & 0xFF) * k),
      b = (int)((c >> IM_COL32_B_SHIFT & 0xFF) * k);
  return IM_COL32(r, g, b, (c >> IM_COL32_A_SHIFT) & 0xFF);
}

struct Pen {
  ImDrawList* dl;
  float sc, a;
  float P(float css) const { return css * sc; }
};

// One action as a gamepad icon (Xbox layout, own simple shapes: circles for A B X Y, bumpers, triggers, stick
// clicks as rings, the d-pad as a cross with the pressed arm filled). Returns its width.
float PadIcon(const Pen& p, ImVec2 at, float h, const std::string& lab, ImU32 col, bool draw) {
  ImDrawList* dl = p.dl;
  ImU32 c = Alpha(col, p.a), ink = Alpha(IM_COL32(16, 16, 18, 255), p.a);
  float px = h * 0.66f;
  auto text = [&](float x0, float w, const char* s, ImU32 tc, float tpx) {
    ImVec2 m = ui::Measure(ui::F::MonoBold, tpx, s);
    ui::Text(dl, ui::F::MonoBold, tpx, ImVec2(x0 + (w - m.x) * 0.5f, at.y + (h - tpx) * 0.5f - p.P(0.5f)), tc, s);
  };
  if (lab == "A" || lab == "B" || lab == "X" || lab == "Y") {
    if (draw) {
      ImVec2 ctr(at.x + h * 0.5f, at.y + h * 0.5f);
      dl->AddCircleFilled(ctr, h * 0.5f, c, 20);
      text(at.x, h, lab.c_str(), ink, px);
    }
    return h;
  }
  if (lab == "L3" || lab == "R3") {
    if (draw) {
      ImVec2 ctr(at.x + h * 0.5f, at.y + h * 0.5f);
      dl->AddCircle(ctr, h * 0.5f - p.P(1), c, 20, std::max(1.5f, p.P(1.8f)));
      text(at.x, h, lab.c_str(), c, h * 0.5f);
    }
    return h;
  }
  if (lab.rfind("D\xE2\x86", 0) == 0 && lab.size() == 4) {
    // D-pad: a box in the action's colour with a dark arrow (the stick's direction is a white arrow without a box)
    if (draw) {
      unsigned char k = (unsigned char)lab[3];  // 0x91 up, 0x90 left, 0x92 right, 0x93 down
      float dx = k == 0x90 ? -1.0f : k == 0x92 ? 1.0f : 0.0f, dy = k == 0x91 ? -1.0f : k == 0x93 ? 1.0f : 0.0f;
      dl->AddRectFilled(at, ImVec2(at.x + h, at.y + h), c, p.P(2.5f));
      ImVec2 ctr(at.x + h * 0.5f, at.y + h * 0.5f);
      float r = h * 0.30f;
      ImVec2 tip(ctr.x + dx * r, ctr.y + dy * r), b(ctr.x - dx * r * 0.7f, ctr.y - dy * r * 0.7f);
      ImVec2 n(-dy * r * 0.9f, dx * r * 0.9f);
      dl->AddTriangleFilled(tip, ImVec2(b.x + n.x, b.y + n.y), ImVec2(b.x - n.x, b.y - n.y), ink);
    }
    return h;
  }
  // bumpers / triggers / anything else: a rounded box with the name
  float tpx = h * 0.58f;
  float w = std::max(h * 1.25f, ui::Measure(ui::F::MonoBold, tpx, lab.c_str()).x + p.P(6));
  if (draw) {
    bool trig = lab == "LT" || lab == "RT";
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), c, trig ? h * 0.45f : p.P(2),
                      trig ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    text(at.x, w, lab.c_str(), ink, tpx);
  }
  return w;
}

float KeyIcon(const Pen& p, ImVec2 at, float h, const std::string& lab, ImU32 col, bool draw) {
  float tpx = h * 0.6f;
  float w = std::max(h, ui::Measure(ui::F::MonoBold, tpx, lab.c_str()).x + p.P(6));
  if (draw) {
    ImU32 c = Alpha(col, p.a);
    p.dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), Alpha(Dark(col, 0.28f), p.a * 0.95f), p.P(2.5f));
    p.dl->AddRect(at, ImVec2(at.x + w, at.y + h), c, p.P(2.5f), 0, std::max(1.0f, p.P(1.2f)));
    ImVec2 m = ui::Measure(ui::F::MonoBold, tpx, lab.c_str());
    ui::Text(p.dl, ui::F::MonoBold, tpx, ImVec2(at.x + (w - m.x) * 0.5f, at.y + (h - tpx) * 0.5f - p.P(0.5f)),
             Alpha(IM_COL32(255, 255, 255, 255), p.a), lab.c_str());
  }
  return w;
}

float TextIcon(const Pen& p, ImVec2 at, float h, const std::string& lab, ImU32 col, bool draw) {
  float tpx = h * 0.72f;
  ImVec2 m = ui::Measure(ui::F::MonoBold, tpx, lab.c_str());
  if (draw) {
    ImVec2 t(at.x, at.y + (h - tpx) * 0.5f - p.P(0.5f));
    float off = std::max(1.0f, std::round(p.P(1)));
    ui::Text(p.dl, ui::F::MonoBold, tpx, ImVec2(t.x + off, t.y + off), Alpha(IM_COL32(0, 0, 0, 200), p.a), lab.c_str());
    ui::Text(p.dl, ui::F::MonoBold, tpx, t, Alpha(col, p.a), lab.c_str());
  }
  return m.x;
}

// The label of an action in a style ("" = draw the text name).
std::string LabelOf(int act, int style) {
  const uint8_t* b = g_map.btn[act];
  if (style == 2 || !g_enumReady) return kActs[act].text;
  auto nameOf = [&](int n) -> std::string {
    if (!Valid(n)) return "";
    if (IsVirtual(n)) {  // a two-button virtual button: "L3+R3"
      const uint8_t* pr = g_map.pair[n - g_v1];
      if (!Valid(pr[0]) || !Valid(pr[1])) return "";
      std::string x = style == 1 ? g_keyName[pr[0]] : g_padName[pr[0]], y = style == 1 ? g_keyName[pr[1]] : g_padName[pr[1]];
      return x.empty() || y.empty() ? "" : x + "+" + y;
    }
    return style == 1 ? g_keyName[n] : g_padName[n];
  };
  // Devil Trigger: the keyboard has its own key (DEVILTRIGGER2), the pad uses the pair
  if (act == kDT) {
    std::string k = style == 1 ? nameOf(b[1]) : nameOf(b[0]);
    if (k.empty()) k = nameOf(style == 1 ? b[0] : b[1]);
    return k;
  }
  return nameOf(b[0]);
}

// The direction in a cell: an arrow (8 ways) or a dot for neutral.
void DirIcon(const Pen& p, ImVec2 at, float h, int dir) {
  ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
  if (dir == 5) {
    p.dl->AddCircleFilled(c, std::max(1.5f, h * 0.09f), Alpha(IM_COL32(140, 140, 145, 255), p.a), 10);
    return;
  }
  int dx = (dir - 1) % 3 - 1, dy = (dir - 1) / 3 - 1;  // numpad: 1 = down-left
  float len = std::sqrt((float)(dx * dx + dy * dy));
  float ux = dx / len, uy = -dy / len;  // screen y grows down
  float r = h * 0.40f, head = h * 0.30f, th = std::max(2.0f, h * 0.13f);
  ImVec2 tip(c.x + ux * r, c.y + uy * r), tail(c.x - ux * r, c.y - uy * r);
  ImVec2 base(tip.x - ux * head, tip.y - uy * head);
  ImVec2 nrm(-uy, ux);
  ImU32 col = Alpha(IM_COL32(255, 255, 255, 255), p.a);
  p.dl->AddLine(tail, base, col, th);
  p.dl->AddTriangleFilled(tip, ImVec2(base.x + nrm.x * head * 0.62f, base.y + nrm.y * head * 0.62f),
                          ImVec2(base.x - nrm.x * head * 0.62f, base.y - nrm.y * head * 0.62f), col);
}

int ShownFrames(const Row& r, int mode) {
  long n = mode == 1 ? r.frames : (long)std::floor(r.t * 60.0 + 0.5);
  return (int)std::clamp(n, 1L, 999L);
}

}  // namespace

void Draw(bool menuOpen) {
  if (!g_on) return;
  std::vector<Row> rows;
  Options o;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    o = g_opt;
    for (const Row& r : g_rows) {
      if (!o.neutral && r.Neutral()) continue;
      rows.push_back(r);
      if ((int)rows.size() >= o.rows) break;
    }
  }
  bool topIsCurrent = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    topIsCurrent = !g_rows.empty() && !rows.empty() && g_rows.front().Same(rows.front());
  }
  float alpha = menuOpen ? 1.0f : g_alpha.load();
  if (!menuOpen && (rows.empty() || alpha <= 0.001f)) return;
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  if (ds.x <= 0 || ds.y <= 0) return;
  Pen pen{nullptr, ui::Screen() * ui::kBoost * std::clamp(o.size, 0.5f, 2.0f), alpha};
  const float rowH = pen.P(19), iconH = pen.P(15.5f), padX = pen.P(5), padY = pen.P(3), gap = pen.P(3);
  const float numPx = pen.P(13);
  const float numW = ui::Measure(ui::F::MonoBold, numPx, "999").x;
  const float dirX = padX + numW + pen.P(6);
  const float iconsX = dirX + rowH + pen.P(4);
  // width: room for 5 icons at least, more when a row needs it (so the box does not jump all the time)
  auto rowW = [&](const Row& r) {
    float x = 0;
    for (int a = 0; a < kActCount; ++a) {
      if (!(r.acts >> a & 1)) continue;
      std::string lab = LabelOf(a, o.style);
      float w = o.style == 0 && !lab.empty() ? PadIcon(pen, {}, iconH, lab, 0, false)
                : o.style == 1 && !lab.empty() ? KeyIcon(pen, {}, iconH, lab, 0, false)
                                               : TextIcon(pen, {}, iconH, lab.empty() || o.style == 2 ? kActs[a].text : lab, 0, false);
      x += w + gap;
    }
    return x;
  };
  float need = pen.P(5 * 19);
  {
    std::lock_guard<std::mutex> lk(g_mu);  // labels are written by the game thread when the ini files change
    for (const Row& r : rows) need = std::max(need, rowW(r));
  }
  int shown = std::max(1, (int)rows.size());
  ImVec2 size(iconsX + need + padX, shown * rowH + padY * 2);
  static bool held = false;
  if (!held || g_replace) {
    g_replace = false;
    ImVec2 pos = g_x >= 0 ? ImVec2(g_x * ds.x, g_y * ds.y) : ImVec2(ds.x * 0.012f, ds.y * 0.27f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  }
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground;
  if (!menuOpen) fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  bool open = ImGui::Begin("##InputDisplay", nullptr, fl);
  ImGui::PopStyleVar(3);
  if (open) {
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 np(std::max(0.0f, std::min(p.x, ds.x - sz.x)), std::max(0.0f, std::min(p.y, ds.y - sz.y)));
    if (np.x != p.x || np.y != p.y) ImGui::SetWindowPos(np);
    if (menuOpen && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) held = true;
    if (held && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      held = false;
      g_x = np.x / ds.x, g_y = np.y / ds.y;
      MarkConfigDirty();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    pen.dl = dl;
    float bg = o.bg;
    if (menuOpen) bg = std::max(bg, ImGui::IsWindowHovered() ? 0.67f : 0.43f);
    if (bg > 0) dl->AddRectFilled(np, ImVec2(np.x + sz.x, np.y + sz.y), IM_COL32(8, 8, 10, (int)(bg * alpha * 255 + 0.5f)), pen.P(2));
    if (menuOpen) dl->AddRect(np, ImVec2(np.x + sz.x, np.y + sz.y), IM_COL32(255, 255, 255, 60), pen.P(2));
    if (rows.empty()) {
      ui::Text(dl, ui::F::Mono, numPx, ImVec2(np.x + padX, np.y + padY + (rowH - numPx) * 0.5f), ui::kText2, "Input display");
    }
    std::lock_guard<std::mutex> lk(g_mu);
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      float y = np.y + padY + i * rowH;
      if (i > 0) dl->AddLine(ImVec2(np.x + padX, y), ImVec2(np.x + sz.x - padX, y), Alpha(IM_COL32(255, 255, 255, 22), alpha));
      char num[8];
      snprintf(num, sizeof num, "%d", ShownFrames(r, o.frames));
      bool cur = i == 0 && topIsCurrent;
      ImVec2 m = ui::Measure(ui::F::MonoBold, numPx, num);
      float off = std::max(1.0f, std::round(pen.P(1)));
      ImVec2 tp(np.x + padX + numW - m.x, y + (rowH - numPx) * 0.5f - pen.P(0.5f));
      ui::Text(dl, ui::F::MonoBold, numPx, ImVec2(tp.x + off, tp.y + off), Alpha(IM_COL32(0, 0, 0, 200), alpha), num);
      ui::Text(dl, cur ? ui::F::MonoBold : ui::F::Mono, numPx, tp,
               Alpha(cur ? IM_COL32(255, 255, 255, 255) : IM_COL32(0xBE, 0xBE, 0xBE, 255), alpha), num);
      float ih = iconH, iy = y + (rowH - ih) * 0.5f;
      DirIcon(pen, ImVec2(np.x + dirX, y + (rowH - rowH * 0.86f) * 0.5f), rowH * 0.86f, r.dir);
      float x = np.x + iconsX;
      for (int a = 0; a < kActCount; ++a) {
        if (!(r.acts >> a & 1)) continue;
        std::string lab = LabelOf(a, o.style);
        ImU32 col = kActs[a].col;
        float w = o.style == 0 && !lab.empty()   ? PadIcon(pen, ImVec2(x, iy), ih, lab, col, true)
                  : o.style == 1 && !lab.empty() ? KeyIcon(pen, ImVec2(x, iy), ih, lab, col, true)
                                                 : TextIcon(pen, ImVec2(x, iy), ih, kActs[a].text, col, true);
        x += w + gap;
      }
    }
    if (menuOpen && ImGui::IsWindowHovered())
      ui::SetHint("Input display: drag to move (only while the menu is open). Numbers: frames each input was held, "
                  "newest on top. Reset position on the Visuals tab puts it back.");
  }
  ImGui::End();
}

// ------------------------------------------------------------------ settings
Options GetOptions() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_opt;
}

void SetOptions(const Options& o) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_opt = o;
    g_opt.style = std::clamp(g_opt.style, 0, 2);
    g_opt.frames = std::clamp(g_opt.frames, 0, 1);
    g_opt.rows = std::clamp(g_opt.rows, 5, 30);
    g_opt.size = std::clamp(g_opt.size, 0.5f, 2.0f);
    g_opt.bg = std::clamp(g_opt.bg, 0.0f, 1.0f);
    g_opt.hideAfter = std::clamp(g_opt.hideAfter, 0.0f, 60.0f);
  }
  MarkConfigDirty();
}

void Clear() {
  g_clear = true;
  if (!g_on) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_rows.clear();
  }
}

void ResetPosition() {
  g_x = g_y = -1;
  g_replace = true;
  MarkConfigDirty();
}

void LoadJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  Options o;
  o.style = j.value("style", o.style);
  o.frames = j.value("frames", o.frames);
  o.rows = j.value("rows", o.rows);
  o.neutral = j.value("neutral", o.neutral);
  o.size = j.value("size", o.size);
  o.bg = j.value("bg", o.bg);
  o.hideAfter = j.value("hide_after", o.hideAfter);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_opt = o;
  }
  SetOptions(o);  // clamps
  g_x = j.value("x", -1.0f);
  g_y = j.value("y", -1.0f);
}

nlohmann::json SaveJson() {
  Options o = GetOptions();
  nlohmann::json j = {{"style", o.style}, {"frames", o.frames}, {"rows", o.rows}, {"neutral", o.neutral},
                      {"size", o.size},   {"bg", o.bg},         {"hide_after", o.hideAfter}};
  if (g_x >= 0) j["x"] = g_x, j["y"] = g_y;
  return j;
}

bool SetByName(const std::string& k, float v) {
  Options o = GetOptions();
  if (k == "style") o.style = (int)v;
  else if (k == "frames") o.frames = (int)v;
  else if (k == "rows") o.rows = (int)v;
  else if (k == "neutral") o.neutral = v != 0;
  else if (k == "size") o.size = v;
  else if (k == "bg") o.bg = v;
  else if (k == "hide") o.hideAfter = v;
  else return false;
  SetOptions(o);
  return true;
}

void Probe(float seconds) {
  g_probeUntil = NowS() + seconds;
  LOG("inputprobe: start for %.1f s", seconds);
}

std::vector<std::string> DebugLines(int n) {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lk(g_mu);
  char b[300];
  snprintf(b, sizeof b, "on %d style %d frames %d rows %d neutral %d size %.2f bg %.2f hide %.1f | map from game %d | rows %zu alpha %.2f idle %.2f | frames counted %u, world time standing %u",
           (int)g_on.load(), g_opt.style, g_opt.frames, g_opt.rows, (int)g_opt.neutral, g_opt.size, g_opt.bg, g_opt.hideAfter,
           (int)g_map.fromGame, g_rows.size(), g_alpha.load(), g_idle, g_counted, g_zeroDt);
  out.push_back(b);
  for (int i = 0; i < (int)g_rows.size() && i < n; ++i) {
    const Row& r = g_rows[i];
    std::string acts;
    for (int a = 0; a < kActCount; ++a)
      if (r.acts >> a & 1) acts += std::string(acts.empty() ? "" : "+") + kActs[a].text;
    snprintf(b, sizeof b, "row %d: %3d f60 (%.4f s) %3d real | dir %d | %s", i, ShownFrames(r, 0), r.t, r.frames, r.dir,
             acts.empty() ? "-" : acts.c_str());
    out.push_back(b);
  }
  return out;
}

}  // namespace inputdisp
