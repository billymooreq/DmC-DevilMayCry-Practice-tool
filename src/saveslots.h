#pragma once
#include <string>
#include <vector>

#include "savefile.h"

// Save tab: export a slot to a .dmcpt package, import a package or raw DMC5_SAVE_XX_* files into a slot.
// Folders: <exe dir>\DmCPT_saves\export\ and \import\. Import runs in the main menu with a save loaded:
// backup -> Steam guard -> files written through ISteamRemoteStorage (fallback: plain files) -> the game
// re-enumerates its slots and loads the imported one (DVSaveManager.EnumerateContent + DVMapInfo.LoadSaveSlot).
namespace slots {

struct SlotInfo {
  int index = 0;     // 0-based
  bool used = false; // PROFILE present on disk
  std::string error; // unreadable files
  savefile::Summary sum;
};

struct ImportEntry {
  std::wstring path;      // package file, or the folder of a raw set
  int rawSlot = -1;       // raw set: slot number in the file names; -1 = package
  std::string display;    // file / folder name (relative to the import folder)
  std::string name;       // shown to the player: manifest name, else the file name without ".dmcpt"
  uint32_t rawAccount = 0;  // raw set: Steam account the files belong to
  bool ok = false;
  std::string error;      // integrity / format problem
  std::string notes, exportedAt, toolVersion, gameVersion;
  bool infoRebuilt = false;  // old / missing INFO: a current one is built on import
  savefile::Summary sum;
};

std::wstring ExportDir();
std::wstring ImportDir();
void OpenFolder(const std::wstring& dir);

void Refresh();  // rescan the save folder and the import folder (worker thread)
std::vector<SlotInfo> Slots();
std::vector<ImportEntry> Imports();
int NumSlots();    // as the game reports it (cached), at least the slots found on disk
int ActiveSlot();  // -1 unknown

// Export: saves the active slot first when it is the one exported and the game is in the main menu
// (so the file on disk matches what the game shows), then packs what is on disk into "<name>.dmcpt".
void Export(int slot, const std::string& name, const std::string& notes, bool saveFirst);
// Rename an import entry. Package: new name in its manifest + the file renamed to match (hashes cover only the
// save files, they stay valid). Raw DMC5_SAVE_XX_* set: packed into "<name>.dmcpt" in the import folder, the raw
// files move to DmCPT_saves\packed_raw\ (nothing is deleted).
void Rename(int entry, const std::string& name);
// Import entry (index in Imports()) into slot. Main menu with a save loaded only.
void Import(int entry, int slot);
void LoadSlot(int slot);  // reload a slot from disk the way the save slot screen does (dev / autorun)

// For the real checkpoint library (realcp.cpp). Worker threads only.
int ActiveSlotNow();  // the game's active slot, 0-based, -1 unknown
// Writes one encrypted save file of a slot (Steam remote storage, fallback: plain file) and reads it back.
bool WriteSlotFile(int slot, savefile::Kind k, const savefile::Bytes& enc, std::string* err);
bool ReadSlotFile(int slot, savefile::Kind k, savefile::Bytes& enc);
bool ReloadSlotNow(int slot, std::string* err);  // main menu: the game reads the slot from disk again

bool Busy();
std::vector<std::string> Report();
std::wstring LastExport();

// Autorun: slot numbers 1-based like in the menu.
void ExportFromScript(const std::string& args);  // "<slot> [notes...]"
void ImportFromScript(const std::string& args);  // "<file or folder name> <slot>"

}  // namespace slots
