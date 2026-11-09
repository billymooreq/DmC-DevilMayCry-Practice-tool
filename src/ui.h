#pragma once
#include <string>
#include <vector>

#include "imgui.h"

// The look of the tool (v0.11 "list like a debug menu"): palette, fonts built into the DLL, scale, and the row
// widgets every tab is made of. Render thread only. Rules: ui_guidelines.md.
namespace ui {

// ---- palette
constexpr ImU32 kAccent = IM_COL32(0xD7, 0x14, 0x1A, 255);     // red: active tab, hover, headings
constexpr ImU32 kText = IM_COL32(0xEC, 0xEC, 0xEC, 255);
constexpr ImU32 kText2 = IM_COL32(0x8A, 0x8A, 0x8A, 255);      // secondary text
constexpr ImU32 kOff = IM_COL32(0x7A, 0x7A, 0x7A, 255);        // [off]
constexpr ImU32 kLocked = IM_COL32(0x5F, 0x5F, 0x5F, 255);     // locked / unavailable
constexpr ImU32 kErr = IM_COL32(0xFF, 0x5A, 0x5F, 255);        // "... has priority", errors
constexpr ImU32 kToast = IM_COL32(0xFF, 0x6B, 0x70, 255);      // message in the status line
constexpr ImU32 kOk = IM_COL32(0x5F, 0xD3, 0x8A, 255);
constexpr ImU32 kWarn = IM_COL32(0xF2, 0xC1, 0x4E, 255);
constexpr ImU32 kFieldBg = IM_COL32(0x1A, 0x1A, 0x1A, 255);
constexpr ImU32 kFieldBorder = IM_COL32(0x3A, 0x3A, 0x3A, 255);
constexpr ImU32 kBtnBg = IM_COL32(0x1C, 0x1C, 0x1C, 255);
constexpr ImU32 kBtnBorder = IM_COL32(0x2E, 0x2E, 0x2E, 255);
constexpr ImU32 kSelBg = IM_COL32(0x3A, 0x0D, 0x10, 255);      // picked item, dropdown hover
constexpr ImU32 kWindowBg = IM_COL32(8, 8, 10, 224);           // rgba(8,8,10,0.88)
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kWeak = IM_COL32(0xFF, 0x3B, 0x41, 255);       // damage log: weak spot
constexpr ImU32 kSoft = IM_COL32(0xD6, 0xD6, 0xD6, 255);       // hint text, options
constexpr ImU32 kTabIdle = IM_COL32(0x8F, 0x8F, 0x8F, 255);

// The menu and the damage log are drawn kBoost times the prototype's sizes at the default zoom (1080p: 13 px text
// -> 21 px, menu 560 -> 896 px wide). The user found the prototype sizes too small to read in the game.
constexpr float kBoost = 1.6f;

// ---- fonts: JetBrains Mono Regular/Bold (all text), Barlow Condensed Bold/ExtraBold (title, tabs, log title, HP)
enum class F { Mono, MonoBold, Title, Heavy };
void InitFonts();       // once, after ImGui::CreateContext (before the DX9 backend builds the texture)
// Every frame: the screen scale (height / 1080) and the menu scale (screen scale x window zoom).
void SetScales(float screen, float menu);
bool FontsWanted();     // the sizes for these scales are not built yet (and the mouse is not held)
void RebuildFonts();    // between frames, after ImGui_ImplDX9_InvalidateDeviceObjects (the backend rebuilds)
ImFont* Get(F f, float px);  // the built font closest to px; draw it at px
ImFont* Overlay();           // "Practice Tool Active" (Segoe UI Bold from Windows, as before)
ImTextureID Razium(float* w, float* h);  // the Easter egg picture (decoded on first use), nullptr if not available
float Screen();  // screen scale
float S();       // menu scale: css px -> screen px
inline float P(float css) { return css * S(); }

// ---- text
ImVec2 Measure(F f, float px, const char* s, float spacing = 0, float wrap = 0);
void Text(ImDrawList* dl, F f, float px, ImVec2 pos, ImU32 col, const char* s, float spacing = 0, float wrap = 0);
std::string Ellipsize(F f, float px, const std::string& s, float maxW);
std::string Upper(std::string s);

// ---- hint box (bottom of the menu): the description of what the mouse points at
void SetHint(const std::string& s);
const std::string& Hint();
void ClearHint();

// ---- rows (inside the menu's list child; css px x S())
struct RowOpt {
  bool sub = false;              // nested: 34 px from the left, grey label
  bool locked = false;           // grey, no hover, click does nothing
  std::string lockText;          // right side when locked: "[Infinite HP has priority]"
  std::string desc;              // hint
  bool picked = false;           // list rows: picked item (dark red, "› ")
};
void Group(const char* label);
// [ON] / [off] at the right; returns true on a click (never when locked).
bool Toggle(const char* id, const char* label, bool on, const RowOpt& o = {});
// A row with any right text (actions, list items). Returns true on a click.
bool Plain(const char* id, const char* label, const std::string& right, ImU32 rightCol, const RowOpt& o = {});
struct NumSpec {
  float min = 0, max = 1, step = 0.1f, def = 1, hardMax = 1000;
  std::string unit;   // "×", " m", " s", ""
  bool integer = false;
};
std::string NumText(float v, const NumSpec& n);
// Slider + < number >; a click on the number types a value (min..hardMax). Returns true when *v changed.
bool Number(const char* id, const char* label, float* v, const NumSpec& n, bool hot, const RowOpt& o = {});
struct DropItem {
  std::string text;
  ImU32 col = 0;  // 0 = normal
};
// Value field with a red caret; the list drops below. Returns the picked index or -1.
int Drop(const char* id, const char* label, const std::vector<DropItem>& items, int cur, const RowOpt& o = {});
// Infinite Glide: mode boxes + [ON]/[off]. Returns the new mode or -1; *toggled = the row was clicked elsewhere.
int Modes(const char* id, const char* label, const std::vector<const char*>& modes, int cur, bool on,
          const RowOpt& o, bool* toggled);
enum class BtnKind { Normal, Primary, Big, Disabled, Small };
struct Btn {
  std::string label;
  BtnKind kind = BtnKind::Normal;
  std::string desc;  // hint
  std::string why;   // Disabled: why (hint + status line on click)
};
struct Slots {
  const char* label = "Slot";
  int first = 1, count = 3, picked = 0;  // numbers shown: first..first+count-1; picked = 0-based
  unsigned saved = 0;                    // bit i: slot i holds something
  std::string desc;
};
// A row of buttons that wraps; with slots on the left the buttons go to the right end. Returns the clicked
// button (-1); *slot = the clicked slot (0-based) or -1.
int Buttons(const char* id, const std::vector<Btn>& b, const Slots* slots = nullptr, int* slot = nullptr);
void Info(const std::string& text, ImU32 col = kText2);
// A list row: label (+ right text) and small buttons at its right end. Returns -2 = the row was clicked,
// i = button i was clicked, -1 = nothing.
int ListRow(const char* id, const std::string& label, const std::string& right, ImU32 rightCol,
            const std::vector<Btn>& btns, const RowOpt& o = {});
// Label + text field (300 px). Returns true when the text changed.
bool TextField(const char* id, const char* label, char* buf, size_t size, const char* placeholder,
               const std::string& desc, bool multiline = false);
struct LegendItem {
  ImU32 col;
  const char* text;
};
void Legend(const std::vector<LegendItem>& items);
enum class KeyState { None, Set, Conflict, Waiting };
// Hotkeys tab: name, group, [key], Bind, x. Returns 1 = Bind clicked, 2 = x clicked, 0.
int KeyRow(const char* id, const char* label, const char* group, const std::string& key, KeyState st, bool canClear,
           const std::string& desc);
bool Picture(const char* id, ImTextureID tex, float w, float h, const char* caption);  // true = clicked
void Space(float css);
bool AnyPopupOpen();  // a dropdown is open (the menu must not react to the rows under it)

// Style of ImGui's own windows (confirmations, popups) in the new look. Call after scale changes.
void ApplyImGuiStyle(float s);

}  // namespace ui
