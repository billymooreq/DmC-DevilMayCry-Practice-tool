#pragma once
#include <string>

// Compatibility report for game builds the tool does not know yet: one text file next to the game exe
// (DmCPT_compat_report.txt) with the exe identity, every signature, the UE3 layout and the classes / functions
// our features use. Read-only: nothing in the game is changed.
namespace compat {

void CreateReportAsync();
bool Busy();
std::string LastResult();  // one line for the menu
std::wstring ReportPath();

}  // namespace compat
