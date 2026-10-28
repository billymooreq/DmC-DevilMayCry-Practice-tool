#pragma once
#include <string>

// ImGui menu + "Practice Tool Active" overlay. Render thread only.
namespace menu {
void SetupFonts();  // after ImGui::CreateContext
void Draw();        // between NewFrame and Render
void SelectTab(const std::string& name);  // "Cheats", "Unlocks", "Saves", "Hotkeys", "Settings", "Dev" (any thread)
// Autorun screenshots: open every "Details" block; open the Rename / Import window of import entry <index>.
void ForceDetails(bool open);
void OpenSavePopup(const std::string& which, int index);
}  // namespace menu
