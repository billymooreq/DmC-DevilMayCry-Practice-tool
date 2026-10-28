#pragma once
#include <string>
#include <vector>

// Checkpoint sets: share a checkpoint list with another player. One file "<name>.dmcptset" (a zip) holds the
// player's point list (missions.json: order, names, deleted points) and the real checkpoints they kept
// (checkpoints/<map>/<name>.chk). Folder: <exe dir>\DmCPT_sets\ - exported sets land there, received ones are
// put there. Import replaces the point list and adds the
// real checkpoints (no backups: "Default checkpoints", the built-in list, is always there to go back to).
// Render thread (small files).
namespace cpsets {

struct SetInfo {
  std::wstring path;
  std::string file;  // file name
  std::string name, created, toolVersion;
  int missions = 0, points = 0, kept = 0;
  bool ok = false;
  std::string error;
};

std::wstring Dir();
std::vector<SetInfo> List();  // rescans the folder
bool Export(const std::string& name, std::string* msg);
bool Import(const std::wstring& path, std::string* msg);

}  // namespace cpsets
