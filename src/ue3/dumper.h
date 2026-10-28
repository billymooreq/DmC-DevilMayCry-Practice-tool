#pragma once
#include <string>

// Text dumps into <exe dir>\DmCPT_dumps\. Each runs on its own worker thread, read-only.
namespace dumper {
enum class Kind { Names, Objects, Classes, Profile, Layout, Scripts };
// arg: Profile = comma-separated class names (live objects of those classes, all bases); Scripts =
// "Class1,Class2;keyword1,keyword2". Empty = the built-in profile/unlock sets.
void RunAsync(Kind kind, const std::string& arg = {});
bool Busy();
std::string LastResult();
}  // namespace dumper
