#pragma once
#include <cstdint>
#include <string>

// Steam achievements guard + save-folder helpers.
namespace steam {

// Hooks ISteamUserStats (STEAMUSERSTATS_INTERFACE_VERSION011): SetStat[3,4], SetAchievement[7], StoreStats[10],
// IndicateAchievementProgress[13]. Achievement calls are always logged; while Blocking() they are swallowed.
bool InstallGuard();             // idempotent, thread-safe
bool GuardInstalled();
void BeginBlock();               // start/extend blocking
void EndBlockAfter(int ms);      // keep blocking a bit longer (the game may save/award asynchronously)
bool Blocking();
void BlockForSession();     // from now until the game exits (a cheat was used this session)
int BlockedCount();              // calls swallowed so far

// <Steam>\userdata\<ActiveUser>\220440\remote (fallback: newest userdata\*\220440\remote with DMC5 saves).
std::wstring SaveDir(std::string* how = nullptr);
// Copies the save folder (+ remotecache.vdf) to <exe dir>\DmCPT_backups\<time>_<tag>\. Empty on failure.
std::wstring BackupSaves(const char* tag, std::string* error);
// Steam account id (32-bit) of the save folder in use: the userdata\<id> folder name. 0 if unknown.
uint32_t SaveAccountId();

// ISteamRemoteStorage of the game's steam_api.dll (STEAMREMOTESTORAGE_INTERFACE_VERSION008): FileWrite [0],
// FileRead [1]. Before the first write the layout is verified by reading an existing save through FileRead and
// comparing it with the file on disk (calls go through an ESP-restoring thunk, so a wrong guess cannot break
// the stack). Writing through Steam keeps its cloud bookkeeping (remotecache.vdf) in sync with the new files.
bool CloudReady(std::string* why);
bool CloudWrite(const std::string& name, const void* data, int size);

}  // namespace steam
