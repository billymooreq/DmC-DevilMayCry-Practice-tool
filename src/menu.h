#pragma once
#include <string>

// The tool's menu + "Practice Tool Active" overlay + enemy health numbers. Render thread, except where noted.
namespace menu {
void SetupStyle();  // after ImGui::CreateContext (fonts: ui::InitFonts)
void Draw();        // between NewFrame and Render
// "Cheats", "Practice", "Enemies", "Visuals", "Runs", "Save states" ("savestates"), "Unlocks", "Saves", "Hotkeys",
// "Settings", "Dev" (any thread)
void SelectTab(const std::string& name);
// The message on the right of the status line (fades after 5 s): results of actions. Any thread.
void Toast(const std::string& text);
// #29: a short message on the game screen while the menu is closed (2.6 s; green / red mark). Any thread.
void Notice(const std::string& text, bool ok);
// #30 tests (autorun labelwatch): the label in the last drawn frame - 0 none (no cheat / Speedrun mode), 1 hidden
// by "only on loading screens", 2 drawn; and how many frames the menu drew so far.
int LabelState();
unsigned DrawnFrames();
// Autorun screenshots: open every "Details" block; open the Rename / Import window of import entry <index>.
void ForceDetails(bool open);
void OpenSavePopup(const std::string& which, int index);
void SetWindow(float x, float y, float w, float h);  // autorun ui_win: menu window, fractions of the screen
// KEY BINDINGS window (#21), autorun: open 0/1; place (fractions of the screen, w <= 0 = default); columns drawn
// last frame; check its layout against the rows the tabs draw (logged "bindlayout: ...").
void SetBindsWindow(int open);
void PlaceBindsWindow(float x, float y, float w, float h);
int BindsWindowColumns();
void CheckBindsLayout();
}  // namespace menu
