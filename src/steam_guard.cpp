#include "steam_guard.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "hooks.h"
#include "log.h"
#include "savefile.h"
#include "util.h"
#include "version.h"

namespace steam {
namespace {

std::atomic<int> g_blockDepth{0};
std::atomic<ULONGLONG> g_blockUntil{0};
std::atomic<int> g_blocked{0};

using SetStat_t = bool(__thiscall*)(void*, const char*, uint32_t);  // int32 and float overloads (4 bytes)
using SetAch_t = bool(__thiscall*)(void*, const char*);
using Store_t = bool(__thiscall*)(void*);
using Indicate_t = bool(__thiscall*)(void*, const char*, uint32_t, uint32_t);
SetStat_t oSetStatA = nullptr, oSetStatB = nullptr;
SetAch_t oSetAchievement = nullptr;
Store_t oStoreStats = nullptr;
Indicate_t oIndicate = nullptr;

std::string SafeName(const char* s) { return s ? util::ReadCString((uintptr_t)s, 128) : "(null)"; }

bool __fastcall hkSetStatA(void* self, void*, const char* name, uint32_t v) {
  if (Blocking()) {
    ++g_blocked;
    LOG("steam: BLOCKED SetStat(%s, 0x%X)", SafeName(name).c_str(), v);
    return true;
  }
  return oSetStatA(self, name, v);
}
bool __fastcall hkSetStatB(void* self, void*, const char* name, uint32_t v) {
  if (Blocking()) {
    ++g_blocked;
    LOG("steam: BLOCKED SetStat(%s, 0x%X)", SafeName(name).c_str(), v);
    return true;
  }
  return oSetStatB(self, name, v);
}
bool __fastcall hkSetAchievement(void* self, void*, const char* name) {
  if (Blocking()) {
    ++g_blocked;
    LOG("steam: BLOCKED SetAchievement(%s)", SafeName(name).c_str());
    return true;
  }
  LOG("steam: SetAchievement(%s) - passed (not in unlock window)", SafeName(name).c_str());
  return oSetAchievement(self, name);
}
bool __fastcall hkStoreStats(void* self, void*) {
  if (Blocking()) {
    ++g_blocked;
    LOG("steam: BLOCKED StoreStats()");
    return true;
  }
  return oStoreStats(self);
}
bool __fastcall hkIndicate(void* self, void*, const char* name, uint32_t cur, uint32_t max) {
  if (Blocking()) {
    ++g_blocked;
    LOG("steam: BLOCKED IndicateAchievementProgress(%s, %u/%u)", SafeName(name).c_str(), cur, max);
    return true;
  }
  return oIndicate(self, name, cur, max);
}

bool RegString(HKEY root, const wchar_t* key, const wchar_t* value, std::wstring& out) {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof buf;
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return false;
  out = buf;
  for (auto& c : out)
    if (c == L'/') c = L'\\';
  return true;
}

bool HasSaves(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\DMC5_SAVE_*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  FindClose(h);
  return true;
}

ULONGLONG FileTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
  return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

std::atomic<bool> g_guardInstalled{false};

}  // namespace

bool InstallGuard() {
  static std::mutex mu;  // the retry thread and an unlock run may both call this
  std::lock_guard<std::mutex> lk(mu);
  static bool done = false, warned = false;
  if (done) return true;
  HMODULE api = GetModuleHandleW(L"steam_api.dll");
  auto getStats = api ? (void*(__cdecl*)())GetProcAddress(api, "SteamUserStats") : nullptr;
  void* stats = getStats ? getStats() : nullptr;
  if (!stats) {
    if (!warned) LOG("steam: SteamUserStats() not available yet - achievements guard NOT installed (retrying)");
    warned = true;
    return false;
  }
  void** vt = *reinterpret_cast<void***>(stats);
  LOG("steam: ISteamUserStats %p vtable %p", stats, vt);
  bool ok = hooks::Create(vt[3], (void*)&hkSetStatA, (void**)&oSetStatA, "Steam.SetStat#3") &&
            hooks::Create(vt[4], (void*)&hkSetStatB, (void**)&oSetStatB, "Steam.SetStat#4") &&
            hooks::Create(vt[7], (void*)&hkSetAchievement, (void**)&oSetAchievement, "Steam.SetAchievement") &&
            hooks::Create(vt[10], (void*)&hkStoreStats, (void**)&oStoreStats, "Steam.StoreStats") &&
            hooks::Create(vt[13], (void*)&hkIndicate, (void**)&oIndicate, "Steam.IndicateAchievementProgress");
  if (!ok) return false;
  for (int i : {3, 4, 7, 10, 13}) hooks::Enable(vt[i], "Steam.UserStats");
  done = true;
  g_guardInstalled = true;
  LOG("steam: achievements guard installed");
  return true;
}

bool GuardInstalled() { return g_guardInstalled; }

void BeginBlock() {
  ++g_blockDepth;
  LOG("steam: achievement/stat blocking ON");
}

void EndBlockAfter(int ms) {
  g_blockUntil = GetTickCount64() + ms;
  if (g_blockDepth > 0) --g_blockDepth;
  LOG("steam: achievement/stat blocking ends in %d ms", ms);
}

std::atomic<bool> g_sessionBlock{false};
bool Blocking() { return g_sessionBlock || g_blockDepth > 0 || GetTickCount64() < g_blockUntil; }
void BlockForSession() {
  if (!g_sessionBlock.exchange(true)) LOG("steam: achievements and stats blocked until the game exits (cheats used)");
}
int BlockedCount() { return g_blocked; }

namespace {

std::wstring EnvDir(const wchar_t* var) {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring ExeDir() {
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring d(exe, n);
  return d.substr(0, d.find_last_of(L'\\'));
}

// Folders Steam emulators keep "Steam Cloud" files in, most likely first. RELOADED / 3DM (steam_api.ini):
// %ProgramData%\Steam\<UserName>\<AppId>\storage.
std::vector<std::wstring> EmulatorDirs() {
  std::vector<std::wstring> out;
  auto add = [&](const std::wstring& d) {
    for (const auto& x : out)
      if (_wcsicmp(x.c_str(), d.c_str()) == 0) return;
    out.push_back(d);
  };
  std::wstring pd = EnvDir(L"ProgramData"), ad = EnvDir(L"APPDATA"), pub = EnvDir(L"PUBLIC");
  wchar_t user[256] = {};
  std::wstring ini = ExeDir() + L"\\steam_api.ini";
  if (GetPrivateProfileStringW(L"Settings", L"UserName", L"", user, 256, ini.c_str()) && user[0] && !pd.empty())
    add(pd + L"\\Steam\\" + user + L"\\220440\\storage");
  auto scan = [&](const std::wstring& root, const wchar_t* tail) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
        add(root + L"\\" + fd.cFileName + tail);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  };
  if (!pd.empty()) scan(pd + L"\\Steam", L"\\220440\\storage");
  if (!pub.empty()) scan(pub + L"\\Documents\\Steam", L"\\220440\\remote");  // CODEX
  if (!ad.empty()) add(ad + L"\\Goldberg SteamEmu Saves\\220440\\remote");
  if (!ad.empty()) add(ad + L"\\SmartSteamEmu\\220440\\remote");
  add(ExeDir() + L"\\steam_settings\\remote");
  return out;
}

bool SameThroughApi(const std::wstring& dir);  // below: a save in dir reads the same through FileRead

// Steam's own save folder: <Steam>\userdata\<ActiveUser>\220440\remote, else the newest userdata folder with saves.
std::wstring SteamUserdataDir(std::string* how) {
  std::wstring steamPath;
  DWORD user = 0, size = sizeof user;
  bool havePath = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steamPath);
  bool haveUser = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser",
                               RRF_RT_REG_DWORD, nullptr, &user, &size) == ERROR_SUCCESS && user != 0;
  if (havePath && haveUser) {
    std::wstring dir = steamPath + L"\\userdata\\" + std::to_wstring(user) + L"\\220440\\remote";
    if (HasSaves(dir)) {
      if (how) *how = "registry ActiveUser " + std::to_string(user);
      return dir;
    }
  }
  if (!havePath) return {};
  // Fallback: the userdata folder with the most recently written DmC profile.
  std::wstring best;
  ULONGLONG bestTime = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((steamPath + L"\\userdata\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
      std::wstring dir = steamPath + L"\\userdata\\" + fd.cFileName + L"\\220440\\remote";
      ULONGLONG t = FileTime(dir + L"\\DMC5_SAVE_00_PROFILE");
      if (HasSaves(dir) && t >= bestTime) {
        best = dir;
        bestTime = t;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (how) *how = "newest userdata folder";
  return best;
}

// The game's steam_api.dll is Valve's (not an emulator's): a repack that runs through the real Steam client.
bool ValveSteamApi() {
  HMODULE api = GetModuleHandleW(L"steam_api.dll");
  wchar_t path[MAX_PATH];
  if (!api || !GetModuleFileNameW(api, path, MAX_PATH)) return false;
  DWORD dummy = 0, n = GetFileVersionInfoSizeW(path, &dummy);
  if (!n) return false;
  std::vector<uint8_t> v(n);
  if (!GetFileVersionInfoW(path, 0, n, v.data())) return false;
  struct Lang { WORD lang, cp; }* tr = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(v.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(Lang)) return false;
  wchar_t key[64];
  swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", tr->lang, tr->cp);
  wchar_t* company = nullptr;
  if (!VerQueryValueW(v.data(), key, (void**)&company, &len) || !company) return false;
  return wcsstr(company, L"Valve") != nullptr;
}

// A non-Steam build: the folder whose save files the game's steam_api.dll returns (an emulator's folder, or Steam's
// own when a repack runs through the real Steam client); else, unchecked, Steam's folder for Valve's steam_api.dll
// and the emulator's for an emulator.
std::wstring NonSteamSaveDir(std::string* how) {
  std::vector<std::wstring> dirs = EmulatorDirs();
  std::string sh;
  std::wstring steamDir = SteamUserdataDir(&sh);
  for (const auto& d : dirs)
    if (HasSaves(d) && SameThroughApi(d)) {
      if (how) *how = "Steam emulator folder (checked through the game's steam_api)";
      return d;
    }
  if (!steamDir.empty() && SameThroughApi(steamDir)) {
    if (how) *how = "Steam's own folder (checked through the game's steam_api: " + sh + ")";
    return steamDir;
  }
  if (ValveSteamApi()) {
    if (how) *how = "Steam's own folder (Valve's steam_api.dll, not checked: " + sh + ")";
    return steamDir;
  }
  wchar_t user[256] = {};
  if (GetPrivateProfileStringW(L"Settings", L"UserName", L"", user, 256, (ExeDir() + L"\\steam_api.ini").c_str()) &&
      user[0] && !dirs.empty()) {
    if (how) *how = "Steam emulator folder from steam_api.ini";
    return dirs[0];
  }
  for (const auto& d : dirs)
    if (HasSaves(d)) {
      if (how) *how = "Steam emulator folder with saves (not checked)";
      return d;
    }
  if (how) *how = "no Steam emulator save folder found";
  return {};
}

}  // namespace

std::wstring SaveDir(std::string* how) {
  if (!GetGameVersion().nonSteam) return SteamUserdataDir(how);
  std::string h;
  std::wstring d = NonSteamSaveDir(&h);
  static std::wstring logged;
  if (d != logged) {
    logged = d;
    LOG("steam: non-Steam game, save folder %s (%s)", d.empty() ? "-" : util::WideToUtf8(d).c_str(), h.c_str());
  }
  if (how) *how = h;
  return d;
}

std::wstring BackupSaves(const char* tag, std::string* error) {
  std::string how;
  std::wstring src = SaveDir(&how);
  if (src.empty()) {
    if (error) *error = "save folder not found (Steam registry / userdata)";
    return {};
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t stamp[64];
  swprintf(stamp, 64, L"%04d%02d%02d_%02d%02d%02d_", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  std::wstring root = util::InExeDir(L"DmCPT_backups\\");
  CreateDirectoryW(root.c_str(), nullptr);
  std::wstring dst = root + stamp + util::Utf8ToWide(tag);
  std::wstring dstRemote = dst + L"\\remote";
  if (!CreateDirectoryW(dst.c_str(), nullptr) || !CreateDirectoryW(dstRemote.c_str(), nullptr)) {
    if (error) *error = "cannot create backup folder";
    return {};
  }
  int copied = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (!CopyFileW((src + L"\\" + fd.cFileName).c_str(), (dstRemote + L"\\" + fd.cFileName).c_str(), TRUE)) {
        if (error) *error = "copy failed: " + util::WideToUtf8(fd.cFileName);
        FindClose(h);
        return {};
      }
      ++copied;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  // remotecache.vdf sits one level up (Steam Cloud bookkeeping) - useful for a manual restore.
  std::wstring parent = src.substr(0, src.find_last_of(L'\\'));
  CopyFileW((parent + L"\\remotecache.vdf").c_str(), (dst + L"\\remotecache.vdf").c_str(), TRUE);
  if (copied < 3) {
    if (error) *error = "only " + std::to_string(copied) + " file(s) found in the save folder";
    return {};
  }
  FILE* f = _wfopen((dst + L"\\SOURCE.txt").c_str(), L"w");
  if (f) {
    fprintf(f, "%s\nfound via %s\n", util::WideToUtf8(src).c_str(), how.c_str());
    fclose(f);
  }
  LOG("backup: %d save file(s) %s -> %s", copied, util::WideToUtf8(src).c_str(), util::WideToUtf8(dst).c_str());
  return dst;
}

uint32_t AccountFromApi();  // below

uint32_t SaveAccountId() {
  std::wstring dir = SaveDir();  // ...\userdata\<id>\220440\remote
  if (GetGameVersion().nonSteam && dir.find(L"\\userdata\\") == std::wstring::npos) {
    // The emulator's own (fake) account id: the one in its saves, else what its ISteamUser reports.
    WIN32_FIND_DATAW fd;
    HANDLE h = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"\\DMC5_SAVE_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        FILE* f = _wfopen((dir + L"\\" + fd.cFileName).c_str(), L"rb");
        if (!f) continue;
        savefile::Bytes enc(fd.nFileSizeLow);
        bool ok = !enc.empty() && fread(enc.data(), 1, enc.size(), f) == enc.size();
        fclose(f);
        savefile::Header hd{};
        if (ok && savefile::ParseHeader(savefile::Decrypt(enc), hd, nullptr) && hd.account) {
          FindClose(h);
          return hd.account;
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    return AccountFromApi();
  }
  size_t a = dir.find(L"\\userdata\\");
  if (a == std::wstring::npos) return 0;
  return (uint32_t)wcstoul(dir.c_str() + a + 10, nullptr, 10);
}

// ------------------------------------------------------------------ ISteamRemoteStorage
namespace {

// __thiscall with n dword arguments; ESP is restored from ESI (callee-saved) after the call, so a callee that
// pops a different number of bytes cannot corrupt our stack.
uint32_t SafeThiscall(void* self, void* fn, const uint32_t* args, int n) {
  uint32_t result = 0;
  __asm {
    mov esi, esp
    mov ecx, n
    mov edx, args
  push_loop:
    test ecx, ecx
    jz pushed
    dec ecx
    push dword ptr [edx + ecx * 4]
    jmp push_loop
  pushed:
    mov ecx, self
    call fn
    mov esp, esi
    mov result, eax
  }
  return result;
}

void* RemoteStorage() {
  HMODULE api = GetModuleHandleW(L"steam_api.dll");
  auto get = api ? (void*(__cdecl*)())GetProcAddress(api, "SteamRemoteStorage") : nullptr;
  return get ? get() : nullptr;
}

std::mutex g_cloudMu;
int g_cloudState = 0;  // 0 unknown, 1 verified, -1 failed
std::string g_cloudWhy;

// FileRead [1] of name into a buffer of the disk file's size: returns true if it matches the bytes on disk.
bool ReadMatches(void* rs, const std::string& name, const std::vector<uint8_t>& disk, int* got) {
  void** vt = *reinterpret_cast<void***>(rs);
  std::vector<uint8_t> buf(disk.size() + 16, 0xCC);
  uint32_t args[3] = {(uint32_t)(uintptr_t)name.c_str(), (uint32_t)(uintptr_t)buf.data(), (uint32_t)disk.size()};
  int n = (int)SafeThiscall(rs, vt[1], args, 3);
  if (got) *got = n;
  return n == (int)disk.size() && memcmp(buf.data(), disk.data(), disk.size()) == 0;
}

std::vector<uint8_t> ReadDisk(const std::wstring& path) {
  std::vector<uint8_t> d;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return d;
  fseek(f, 0, SEEK_END);
  d.resize(ftell(f));
  fseek(f, 0, SEEK_SET);
  if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
  fclose(f);
  return d;
}

bool SameThroughApi(const std::wstring& dir) {
  void* rs = RemoteStorage();
  if (!rs) return false;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\DMC5_SAVE_*_INFO").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  FindClose(h);
  std::vector<uint8_t> disk = ReadDisk(dir + L"\\" + fd.cFileName);
  return !disk.empty() && ReadMatches(rs, util::WideToUtf8(fd.cFileName), disk, nullptr);
}

}  // namespace

// ISteamUser::GetSteamID [2] (returns CSteamID through a hidden pointer, MSVC x86): the low 32 bits = account id.
uint32_t AccountFromApi() {
  HMODULE api = GetModuleHandleW(L"steam_api.dll");
  auto get = api ? (void*(__cdecl*)())GetProcAddress(api, "SteamUser") : nullptr;
  void* user = get ? get() : nullptr;
  if (!user) return 0;
  uint64_t id = 0;
  uint32_t args[1] = {(uint32_t)(uintptr_t)&id};
  SafeThiscall(user, (*reinterpret_cast<void***>(user))[2], args, 1);
  uint32_t acc = (uint32_t)id;
  // SteamID64 of an individual account: universe 1, type 1, instance 1 in the high 32 bits (0x01100001).
  bool sane = (uint32_t)(id >> 32) == 0x01100001u && acc != 0;
  LOG("steam: account id from ISteamUser::GetSteamID: %u (0x%016llX) %s", acc, (unsigned long long)id,
      sane ? "" : "- not a normal Steam id, not used");
  return sane ? acc : 0;
}

bool CloudReady(std::string* why) {
  std::lock_guard<std::mutex> lk(g_cloudMu);
  if (g_cloudState == 0) {
    void* rs = RemoteStorage();
    std::wstring dir = SaveDir();
    std::vector<uint8_t> disk;
    std::string probe;
    WIN32_FIND_DATAW fd;
    HANDLE h = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"\\DMC5_SAVE_*_INFO").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      probe = util::WideToUtf8(fd.cFileName);
      FindClose(h);
      FILE* f = _wfopen((dir + L"\\" + fd.cFileName).c_str(), L"rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        disk.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        if (fread(disk.data(), 1, disk.size(), f) != disk.size()) disk.clear();
        fclose(f);
      }
    }
    if (!rs) {
      g_cloudState = -1;
      g_cloudWhy = "SteamRemoteStorage() not available";
    } else if (disk.empty()) {
      g_cloudState = -1;
      g_cloudWhy = "no save file to verify the interface with";
    } else {
      void** vt = *reinterpret_cast<void***>(rs);
      std::vector<uint8_t> buf(disk.size() + 16, 0xCC);
      uint32_t args[3] = {(uint32_t)(uintptr_t)probe.c_str(), (uint32_t)(uintptr_t)buf.data(), (uint32_t)disk.size()};
      int got = (int)SafeThiscall(rs, vt[1], args, 3);
      bool same = got == (int)disk.size() && memcmp(buf.data(), disk.data(), disk.size()) == 0;
      g_cloudState = same ? 1 : -1;
      g_cloudWhy = same ? "verified with " + probe
                        : "FileRead check failed (" + std::to_string(got) + " of " + std::to_string(disk.size()) + " bytes)";
    }
    LOG("steam: remote storage %p: %s", rs, g_cloudWhy.c_str());
  }
  if (why) *why = g_cloudWhy;
  return g_cloudState == 1;
}

bool CloudWrite(const std::string& name, const void* data, int size) {
  if (!CloudReady(nullptr)) return false;
  void* rs = RemoteStorage();
  void** vt = *reinterpret_cast<void***>(rs);
  uint32_t args[3] = {(uint32_t)(uintptr_t)name.c_str(), (uint32_t)(uintptr_t)data, (uint32_t)size};
  bool ok = (SafeThiscall(rs, vt[0], args, 3) & 0xFF) != 0;
  LOG("steam: FileWrite(%s, %d bytes) -> %d", name.c_str(), size, (int)ok);
  return ok;
}

}  // namespace steam
