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
#include "util.h"

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

std::wstring SaveDir(std::string* how) {
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

uint32_t SaveAccountId() {
  std::wstring dir = SaveDir();  // ...\userdata\<id>\220440\remote
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

}  // namespace

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
