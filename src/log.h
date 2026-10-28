#pragma once
#include <string>
#include <vector>

// DmCPracticeTool.log next to the exe. Safe to call from any thread (not from DllMain).
namespace logx {
void Write(const char* fmt, ...);
// Last lines for the in-game Log tab.
std::vector<std::string> Tail(size_t maxLines);
}  // namespace logx

#define LOG(...) logx::Write(__VA_ARGS__)
