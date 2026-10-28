#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Kept real checkpoints: copies of the checkpoint file (DMC5_SAVE_XX_CHECKPNT) the game itself saved while the
// player played. Loading one is exactly Continue: the file goes back into the active slot, the game reads the
// slot again in the main menu and ContinueFromCheckpoint restores everything the save holds (moving platforms,
// doors, level scripts). Points without a kept copy use nav's own checkpoint load.
// Folder: <exe dir>\DmCPT_checkpoints\<map>\<checkpoint>.chk (the game's encrypted file as it is).
namespace realcp {

struct Entry {
  std::string map, name;  // "Ch2_Main", "GrandFoyerFightWon"
  int difficulty = -1;    // saved on this difficulty (0..6)
  std::string saved;      // "2026-10-27 13:56"
  std::wstring file;
};
std::vector<Entry> List();  // any thread
bool Find(const std::string& map, const std::string& name, Entry* out = nullptr);
void Refresh();             // rescan the folder
// A checkpoint file from a shared set: checked and stored like a kept one (same map + name = replaced).
bool AddFile(const std::vector<uint8_t>& enc, std::string* err);

// Jobs on a worker thread; one at a time. The result line is in Message().
void KeepLast();  // keep the checkpoint the game saved last in the active slot (in a mission)
void Load(const std::string& map, const std::string& name);
void Forget(const std::string& map, const std::string& name);
bool Busy();
std::string Message();

// The checkpoint Create checkpoint just stored, once (menu: put it into the list if it is not there).
struct Created {
  std::string map, name;
  bool hasPos = false;
  float x = 0, y = 0, z = 0;  // where Dante stood when it was created (for Teleport)
  int yaw = 0;
};
bool TakeCreated(Created& out);

}  // namespace realcp
