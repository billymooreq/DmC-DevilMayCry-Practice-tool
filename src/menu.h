#pragma once
#include <string>

// The tool's menu + "Practice Tool Active" overlay + enemy health numbers. Render thread, except where noted.
namespace menu {
void SetupStyle();  // after ImGui::CreateContext (fonts: ui::InitFonts)
void Draw();        // between NewFrame and Render
// "Cheats", "Practice", "Enemies", "Visuals", "Runs", "Unlocks", "Saves", "Hotkeys", "Settings", "Dev" (any thread)
void SelectTab(const std::string& name);
// The message on the right of the status line (fades after 5 s): results of actions. Any thread.
void Toast(const std::string& text);
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
