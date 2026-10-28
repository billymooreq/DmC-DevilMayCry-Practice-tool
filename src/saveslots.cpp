#include "saveslots.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <map>
#include <mutex>
#include <thread>

#include "log.h"
#include "steam_guard.h"
#include "ue3/dumper.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "unlock.h"
#include "util.h"
#include "version.h"

using namespace savefile;
using engine::OnGame;

namespace slots {
namespace {

std::mutex g_mu;  // guards everything below
std::vector<SlotInfo> g_slots;
std::vector<ImportEntry> g_imports;
std::vector<std::string> g_report;
std::wstring g_lastExport;
int g_numSlots = 0, g_active = -1;
std::atomic<bool> g_busy{false}, g_refreshing{false};

void Rep(const char* fmt, ...) {
  char b[768];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  LOG("slots: %s", b);
  std::lock_guard<std::mutex> lk(g_mu);
  g_report.emplace_back(b);
}

void ClearReport() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_report.clear();
}

bool ReadAll(const std::wstring& path, Bytes& out) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? n : 0);
  bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

bool WriteAll(const std::wstring& path, const Bytes& data) {
  std::wstring tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = fclose(f) == 0 && ok;
  return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void MakeDirs(const std::wstring& dir) {
  for (size_t p = dir.find(L'\\', 3); p != std::wstring::npos; p = dir.find(L'\\', p + 1))
    CreateDirectoryW(dir.substr(0, p).c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
}

std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = (wchar_t)towlower(c);
  return s;
}

// Encrypted files are decrypted; a file that already is a decrypted save (valid header) is taken as is.
bool LoadSaveFile(const std::wstring& path, Bytes& dec, std::string* err) {
  Bytes raw;
  if (!ReadAll(path, raw)) {
    if (err) *err = "cannot read " + util::WideToUtf8(path);
    return false;
  }
  Header h;
  dec = Decrypt(raw);
  if (ParseHeader(dec, h, nullptr)) return true;
  if (ParseHeader(raw, h, nullptr)) {
    dec = raw;
    return true;
  }
  std::string e;
  ParseHeader(dec, h, &e);
  if (err) *err = util::WideToUtf8(path.substr(path.find_last_of(L'\\') + 1)) + ": " + e;
  return false;
}

bool LoadSlotFromDisk(const std::wstring& dir, int slot, SlotFiles& s, std::string* err) {
  for (int k = 0; k < KindCount; ++k) {
    std::wstring p = dir + L"\\" + SlotFileName(slot, (Kind)k);
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    if (!LoadSaveFile(p, s.dec[k], err)) return false;
  }
  return CheckSet(s, err);
}

// dir\<base>.dmcpt, or <base>_2.dmcpt, _3... if taken.
std::wstring FreePath(const std::wstring& dir, const std::string& base) {
  std::wstring b = util::Utf8ToWide(base), path = dir + L"\\" + b + L".dmcpt";
  for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
    path = dir + L"\\" + b + L"_" + std::to_wstring(i) + L".dmcpt";
  return path;
}

std::string NowIso() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  TIME_ZONE_INFORMATION tz;
  DWORD r = GetTimeZoneInformation(&tz);
  long bias = -(long)tz.Bias - (r == TIME_ZONE_ID_DAYLIGHT ? (long)tz.DaylightBias : 0);
  char b[64];
  snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld", st.wYear, st.wMonth, st.wDay, st.wHour,
           st.wMinute, st.wSecond, bias < 0 ? '-' : '+', labs(bias) / 60, labs(bias) % 60);
  return b;
}

// ------------------------------------------------------------------ game side (game thread)
uintptr_t Fn(const char* cls, const char* name) { return ue3::FindObject("Function", name, cls); }

int StaticInt(uintptr_t ctx, const char* fn) {
  engine::Call c(ctx, Fn("DVSaveManager", fn));
  return c.Invoke() ? c.RetInt() : -1;
}
int SlotStatusG(uintptr_t ctx) {
  engine::Call c(ctx, Fn("DVSaveManager", "GetActiveSlotStatus"));
  return c.Invoke() ? c.RetByte() : -1;
}
bool SaveBusyG(uintptr_t ctx) {
  engine::Call c(ctx, Fn("DVSaveManager", "IsBusy"));
  return c.Invoke() && c.RetBool();
}

std::string OuterMost(uintptr_t o) {
  for (int d = 0; d < 32; ++d) {
    uintptr_t outer = ue3::ObjOuter(o);
    if (!outer) break;
    o = outer;
  }
  return ue3::ObjName(o);
}

// The live DVMapInfo of the current (front end) map: it owns the save slot screen logic.
uintptr_t FindMapInfo(const std::string& map) {
  int n = ue3::NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o || ue3::ClassNameOf(o) != "DVMapInfo" || ue3::ObjName(o).rfind("Default__", 0) == 0) continue;
    if (OuterMost(o) == map) return o;
  }
  return 0;
}

struct GameCtx {
  uintptr_t profile = 0, mapInfo = 0;
  std::string map, error;
  bool frontEnd = false;
};

GameCtx Context() {
  GameCtx g;
  if (!ue3::Ready() || !engine::Hooked() || !engine::GameThreadKnown()) {
    g.error = "game not ready (UE3 / game thread)";
    return g;
  }
  unlock::Refs r = unlock::Locate();
  g.profile = r.profile;
  g.map = r.map;
  g.frontEnd = unlock::IsFrontEnd(r.map);
  if (!r.profile) g.error = r.error.empty() ? "profile not found" : r.error;
  return g;
}

bool WaitNotBusy(uintptr_t ctx, int ms) {
  for (int t = 0; t < ms; t += 250) {
    if (!OnGame<bool>([ctx] { return SaveBusyG(ctx); }, 3000)) return true;
    Sleep(250);
  }
  return false;
}

// EnumerateContent (re-reads the slot list from disk) + DVMapInfo.LoadSaveSlot(slot), exactly what the save
// slot screen does: ActivateSaveSlot -> SaveSlotLoaded -> profile restored, main menu reopened.
bool ReloadSlot(const GameCtx& g, int slot, std::string* err) {
  uintptr_t ctx = g.profile;
  bool en = OnGame<bool>([ctx] {
    engine::Call c(ctx, Fn("DVSaveManager", "EnumerateContent"));
    return c.Invoke() && c.RetBool();
  });
  if (!WaitNotBusy(ctx, 15000)) {
    *err = "EnumerateContent did not finish";
    return false;
  }
  uintptr_t mi = FindMapInfo(g.map);
  if (!mi) {
    *err = "DVMapInfo of " + g.map + " not found";
    return false;
  }
  bool called = OnGame<bool>([mi, slot] {
    engine::Call c(mi, "LoadSaveSlot");
    c.Int(c.ParamName(0).c_str(), slot);
    return c.Invoke();
  });
  if (!called) {
    *err = "LoadSaveSlot call failed";
    return false;
  }
  int status = -1, active = -1;
  for (int t = 0; t < 30000; t += 500) {
    Sleep(500);
    struct R { int status, active; bool busy; };
    R r = OnGame<R>([ctx] { return R{SlotStatusG(ctx), StaticInt(ctx, "GetActiveSlotIndex"), SaveBusyG(ctx)}; }, 3000);
    status = r.status;
    active = r.active;
    if (!r.busy && active == slot && (status == 3 || status == 4)) {
      LOG("slots: slot %d loaded (EnumerateContent %d, status %d)", slot + 1, (int)en, status);
      return true;
    }
  }
  *err = "slot did not load (status " + std::to_string(status) + ", active slot " + std::to_string(active + 1) + ")";
  return false;
}

// ------------------------------------------------------------------ scans
void ScanImports(std::vector<ImportEntry>& out) {
  std::wstring root = ImportDir();
  // raw sets: (folder, slot) -> files; packages: *.dmcpt. The import folder and one level of sub-folders.
  std::vector<std::wstring> dirs = {root};
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
        dirs.push_back(root + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  for (const std::wstring& dir : dirs) {
    std::map<int, int> rawSlots;  // slot -> found kinds mask
    h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::wstring name = fd.cFileName, low = Lower(name);
      if (low.size() > 6 && low.substr(low.size() - 6) == L".dmcpt") {
        ImportEntry e;
        e.path = dir + L"\\" + name;
        e.display = util::WideToUtf8(dir == root ? name : dir.substr(root.size() + 1) + L"\\" + name);
        Bytes zip;
        SlotFiles s;
        Manifest m;
        if (!ReadAll(e.path, zip))
          e.error = "cannot read the file";
        else if (ReadPackage(zip, s, m, &e.error)) {
          e.ok = true;
          e.sum = Summarize(s);
          e.infoRebuilt = !e.sum.info.valid;
        }
        e.notes = m.notes;
        e.name = !m.name.empty() ? m.name : util::WideToUtf8(name.substr(0, name.size() - 6));
        e.exportedAt = m.exportedAt;
        e.toolVersion = m.toolVersion;
        e.gameVersion = m.gameVersion;
        if (e.ok && !m.gameVersion.empty() && m.gameVersion != GetGameVersion().name)
          e.error = "made on game version " + VersionLabel(m.gameVersion) + ", you have " + GetGameVersion().label;
        out.push_back(e);
        continue;
      }
      int slot = -1;
      wchar_t kind[16] = {};
      if (swscanf(low.c_str(), L"dmc5_save_%d_%15s", &slot, kind) == 2 && slot >= 0 && slot < 100) {
        std::wstring k = kind;
        rawSlots[slot] |= k == L"profile" ? 1 : k == L"info" ? 2 : k == L"checkpnt" ? 4 : 0;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto [slot, mask] : rawSlots) {
      if (!(mask & 1)) continue;  // a lone CHECKPNT / INFO is not a save
      ImportEntry e;
      e.path = dir;
      e.rawSlot = slot;
      char b[32];
      snprintf(b, sizeof b, "DMC5_SAVE_%02d_*", slot);
      e.display = (dir == root ? std::string() : util::WideToUtf8(dir.substr(root.size() + 1)) + "\\") + b + " (raw)";
      SlotFiles s;
      // File names may be lower case (downloads): find each kind case-insensitively.
      for (int k = 0; k < KindCount && e.error.empty(); ++k) {
        std::wstring want = Lower(SlotFileName(slot, (Kind)k));
        HANDLE h2 = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h2 == INVALID_HANDLE_VALUE) break;
        do {
          if (Lower(fd.cFileName) == want) LoadSaveFile(dir + L"\\" + fd.cFileName, s.dec[k], &e.error);
        } while (FindNextFileW(h2, &fd));
        FindClose(h2);
      }
      if (e.error.empty() && CheckSet(s, &e.error)) {
        e.ok = true;
        e.sum = Summarize(s);
        e.infoRebuilt = !e.sum.info.valid;
        Header hd;
        ParseHeader(s.dec[Profile], hd, nullptr);
        e.rawAccount = hd.account;
      }
      snprintf(b, sizeof b, "Save files, slot %d", slot + 1);
      e.name = (dir == root ? std::string() : util::WideToUtf8(dir.substr(root.size() + 1)) + ": ") + b;
      out.push_back(e);
    }
  }
}

void DoRefresh() {
  std::vector<SlotInfo> sl;
  std::vector<ImportEntry> im;
  MakeDirs(ImportDir());
  MakeDirs(ExportDir());
  std::wstring dir = steam::SaveDir();
  int maxFound = -1;
  for (int i = 0; i < 100 && !dir.empty(); ++i) {
    if (GetFileAttributesW((dir + L"\\" + SlotFileName(i, Profile)).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    maxFound = i;
  }
  int active = -1, num = 0;
  GameCtx g = Context();
  if (g.profile) {
    uintptr_t ctx = g.profile;
    num = OnGame<int>([ctx] { return StaticInt(ctx, "GetNumSaveSlots"); }, 3000);
    active = OnGame<int>([ctx] { return SlotStatusG(ctx) == 3 || SlotStatusG(ctx) == 4 ? StaticInt(ctx, "GetActiveSlotIndex") : -1; }, 3000);
  }
  int count = std::max(num, maxFound + 1);
  if (count <= 0) count = 1;
  for (int i = 0; i < count; ++i) {
    SlotInfo si;
    si.index = i;
    SlotFiles s;
    if (!dir.empty() && GetFileAttributesW((dir + L"\\" + SlotFileName(i, Profile)).c_str()) != INVALID_FILE_ATTRIBUTES) {
      si.used = true;
      if (LoadSlotFromDisk(dir, i, s, &si.error)) si.sum = Summarize(s);
    }
    sl.push_back(si);
  }
  ScanImports(im);
  std::lock_guard<std::mutex> lk(g_mu);
  g_slots = sl;
  g_imports = im;
  if (num > 0) g_numSlots = num;
  g_active = active;
}

bool Begin() {
  if (unlock::Busy()) return false;
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) return false;
  ClearReport();
  return true;
}

// ------------------------------------------------------------------ jobs
void ExportJob(int slot, std::string name, std::string notes, bool saveFirst) {
  std::wstring dir = steam::SaveDir();
  if (dir.empty()) {
    Rep("FAIL: save folder not found");
    return;
  }
  GameCtx g = Context();
  int active = -1, status = -1;
  if (g.profile) {
    uintptr_t ctx = g.profile;
    status = OnGame<int>([ctx] { return SlotStatusG(ctx); }, 3000);
    active = OnGame<int>([ctx] { return StaticInt(ctx, "GetActiveSlotIndex"); }, 3000);
  }
  bool loaded = status == 3 || status == 4;
  if (loaded && active == slot && !g.frontEnd)
    Rep("note: you are in a mission - the last checkpoint is saved, not the exact spot you are at");
  if (saveFirst && loaded && active == slot && g.frontEnd) {
    // SaveContent is asynchronous and IsBusy() may still be false right after the call: wait until all three
    // files on disk are rewritten (or 15 s), otherwise the export reads the files from before the save.
    auto stamps = [&] {
      std::vector<ULONGLONG> t;
      for (int k = 0; k < KindCount; ++k) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW((dir + L"\\" + SlotFileName(slot, (Kind)k)).c_str(), GetFileExInfoStandard, &a);
        t.push_back(((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime);
      }
      return t;
    };
    std::vector<ULONGLONG> before = stamps();
    uintptr_t profile = g.profile;
    steam::BeginBlock();
    bool ok = OnGame<bool>([profile] {
      engine::Call c(profile, "SaveProfile");  // all flags false, as Unlock's "Save now"
      return c.Invoke();
    });
    bool written = false;
    for (int t = 0; ok && t < 15000 && !written; t += 250) {
      Sleep(250);
      std::vector<ULONGLONG> now = stamps();
      written = true;
      for (int k = 0; k < KindCount; ++k) written = written && now[k] != before[k];
    }
    bool done = WaitNotBusy(profile, 15000);
    Sleep(500);  // the last file may still be closing
    steam::EndBlockAfter(5000);
    Rep("%s game saved slot %d first%s", ok && done ? "OK  " : "FAIL", slot + 1,
        written ? "" : " (files did not change - exporting what is on disk)");
    if (!ok || !done) return;
  }
  SlotFiles s;
  std::string err;
  if (GetFileAttributesW((dir + L"\\" + SlotFileName(slot, Profile)).c_str()) == INVALID_FILE_ATTRIBUTES) {
    Rep("FAIL: slot %d is empty", slot + 1);
    return;
  }
  if (!LoadSlotFromDisk(dir, slot, s, &err)) {
    Rep("FAIL: slot %d: %s", slot + 1, err.c_str());
    return;
  }
  Summary sum = Summarize(s);
  if (name.empty()) name = sum.DefaultName();
  Bytes pkg = BuildPackage(s, name, notes, kToolVersion, GetGameVersion().name, NowIso(), slot);
  MakeDirs(ExportDir());
  std::wstring path = FreePath(ExportDir(), SafeFileName(name));
  if (!WriteAll(path, pkg)) {
    Rep("FAIL: cannot write %s", util::WideToUtf8(path).c_str());
    return;
  }
  // Read it back the way an import does: the package must verify before we call it done.
  SlotFiles back;
  Manifest m;
  Bytes again;
  if (!ReadAll(path, again) || !ReadPackage(again, back, m, &err)) {
    Rep("FAIL: written package does not verify: %s", err.c_str());
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_lastExport = path;
  }
  LOG("slots: exported slot %d -> %s (%zu bytes)", slot + 1, util::WideToUtf8(path).c_str(), pkg.size());
  Rep("OK   slot %d saved as \"%s\"", slot + 1, util::WideToUtf8(path.substr(path.find_last_of(L'\\') + 1)).c_str());
}

bool LoadEntryFiles(const ImportEntry& e, SlotFiles& s, std::string* err);

void RenameJob(ImportEntry e, std::string name) {
  std::string err;
  if (e.rawSlot < 0) {
    Bytes zip, renamed;
    if (!ReadAll(e.path, zip)) {
      Rep("FAIL: cannot read %s", e.display.c_str());
      return;
    }
    if (!RenamePackage(zip, name, renamed, &err)) {
      Rep("FAIL: %s: %s", e.display.c_str(), err.c_str());
      return;
    }
    std::wstring dir = e.path.substr(0, e.path.find_last_of(L'\\'));
    std::wstring want = dir + L"\\" + util::Utf8ToWide(SafeFileName(name)) + L".dmcpt";
    std::wstring dst = Lower(want) == Lower(e.path) ? e.path : FreePath(dir, SafeFileName(name));
    // Write the renamed package next to the old one first, then drop the old file: never lose the package.
    if (!WriteAll(dst, renamed)) {
      Rep("FAIL: cannot write %s", util::WideToUtf8(dst).c_str());
      return;
    }
    if (dst != e.path) DeleteFileW(e.path.c_str());
    Rep("OK   renamed to \"%s\" (%s)", name.c_str(), util::WideToUtf8(dst.substr(dst.find_last_of(L'\\') + 1)).c_str());
    return;
  }
  // Raw save files have no place for a name: pack them into a package with this name (same as an export of
  // those files), then move the raw files out of the import folder so the save is listed once.
  SlotFiles s;
  if (!LoadEntryFiles(e, s, &err)) {
    Rep("FAIL: %s: %s", e.display.c_str(), err.c_str());
    return;
  }
  Bytes pkg = BuildPackage(s, name, "", kToolVersion, "", NowIso(), e.rawSlot);
  std::wstring dst = FreePath(ImportDir(), SafeFileName(name));
  SlotFiles back;
  Manifest m;
  if (!WriteAll(dst, pkg) || !ReadPackage(pkg, back, m, &err)) {
    Rep("FAIL: cannot write %s %s", util::WideToUtf8(dst).c_str(), err.c_str());
    return;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t sub[64];
  swprintf(sub, 64, L"%04d%02d%02d_%02d%02d%02d_slot%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond, e.rawSlot);
  std::wstring keep = util::InExeDir(L"DmCPT_saves\\packed_raw\\") + sub;
  MakeDirs(keep);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((e.path + L"\\*").c_str(), &fd);
  int moved = 0;
  if (h != INVALID_HANDLE_VALUE) {
    do {
      int slot = -1;
      wchar_t kind[16] = {};
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          swscanf(Lower(fd.cFileName).c_str(), L"dmc5_save_%d_%15s", &slot, kind) == 2 && slot == e.rawSlot &&
          MoveFileExW((e.path + L"\\" + fd.cFileName).c_str(), (keep + L"\\" + fd.cFileName).c_str(),
                      MOVEFILE_COPY_ALLOWED))
        ++moved;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  Rep("OK   packed as \"%s\" (%s); the %d original files moved to %s", name.c_str(),
      util::WideToUtf8(dst.substr(dst.find_last_of(L'\\') + 1)).c_str(), moved, util::WideToUtf8(keep).c_str());
}

bool LoadEntryFiles(const ImportEntry& e, SlotFiles& s, std::string* err) {
  if (e.rawSlot < 0) {
    Bytes zip;
    Manifest m;
    if (!ReadAll(e.path, zip)) {
      *err = "cannot read the package";
      return false;
    }
    return ReadPackage(zip, s, m, err);
  }
  WIN32_FIND_DATAW fd;
  for (int k = 0; k < KindCount; ++k) {
    std::wstring want = Lower(SlotFileName(e.rawSlot, (Kind)k));
    HANDLE h = FindFirstFileW((e.path + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) break;
    do {
      if (Lower(fd.cFileName) == want && !LoadSaveFile(e.path + L"\\" + fd.cFileName, s.dec[k], err)) {
        FindClose(h);
        return false;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return CheckSet(s, err);
}

void ImportJob(ImportEntry e, int slot) {
  unlock::Status st = unlock::CurrentStatus();
  GameCtx g = Context();
  if (!g.profile || !g.frontEnd) {
    Rep("FAIL: import works only in the main menu (%s)", g.error.empty() ? ("map " + g.map).c_str() : g.error.c_str());
    return;
  }
  uintptr_t ctx = g.profile;
  int status = OnGame<int>([ctx] { return SlotStatusG(ctx); }, 3000);
  if (status != 3 && status != 4) {
    Rep("FAIL: choose any save slot in the game first (slot status %d)", status);
    return;
  }
  if (!steam::InstallGuard()) {
    Rep("FAIL: the tool is not fully started yet - nothing changed, try again in a moment");
    return;
  }
  (void)st;
  SlotFiles s;
  std::string err;
  if (!LoadEntryFiles(e, s, &err)) {
    Rep("FAIL: %s: %s", e.display.c_str(), err.c_str());
    return;
  }
  LOG("slots: %s: integrity checked (hashes + checksums)", e.display.c_str());
  Rep("OK   the file is complete and not damaged");
  uint32_t account = steam::SaveAccountId();
  std::wstring dir = steam::SaveDir();
  if (!account || dir.empty()) {
    Rep("FAIL: your Steam save folder / account id not found");
    return;
  }
  Summary sum = Summarize(s);
  if (!sum.info.valid) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    s.dec[Info] = Wrap(BuildInfo(sum.profile, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute), account);
    LOG("slots: slot info (save slot screen text) rebuilt for this game version");
  }
  Bytes enc[KindCount];
  for (int k = 0; k < KindCount; ++k) {
    SetAccount(s.dec[k], account);
    enc[k] = Encrypt(s.dec[k]);
  }
  std::wstring backup = steam::BackupSaves("before_import", &err);
  if (backup.empty()) {
    Rep("FAIL: save backup failed (%s) - nothing changed", err.c_str());
    return;
  }
  Rep("OK   backup copy of your saves made (DmCPT_backups)");
  LOG("slots: backup %s", util::WideToUtf8(backup).c_str());
  steam::BeginBlock();
  std::string how;
  bool cloud = steam::CloudReady(&how);
  if (!cloud) LOG("slots: Steam remote storage not usable (%s) - writing the files directly", how.c_str());
  bool writeOk = true;
  for (int k = 0; k < KindCount && writeOk; ++k) {
    std::wstring name = SlotFileName(slot, (Kind)k);
    std::wstring path = dir + L"\\" + name;
    bool ok = cloud && steam::CloudWrite(util::WideToUtf8(name), enc[k].data(), (int)enc[k].size());
    if (!ok) ok = WriteAll(path, enc[k]);
    Bytes check;
    ok = ok && ReadAll(path, check) && check == enc[k];
    if (!ok) Rep("FAIL: writing %s", util::WideToUtf8(name).c_str());
    writeOk = ok;
  }
  if (!writeOk) {
    steam::EndBlockAfter(5000);
    Rep("FAIL: import stopped - restore the backup above if the slot does not load");
    return;
  }
  LOG("slots: files written to slot %d (%s), Steam account %u", slot + 1, cloud ? "through Steam Cloud API" : "directly",
      account);
  bool reloaded = ReloadSlot(g, slot, &err);
  steam::EndBlockAfter(30000);
  if (!reloaded) {
    Rep("FAIL: the game did not reload the slot: %s. Restart the game - the files are in place.", err.c_str());
    return;
  }
  Rep("OK   loaded into slot %d - press Continue", slot + 1);
  dumper::RunAsync(dumper::Kind::Profile);
}

template <class F>
void RunJob(F f) {
  std::thread([f] {
    f();
    DoRefresh();
    g_busy = false;
  }).detach();
}

}  // namespace

std::wstring ExportDir() { return util::InExeDir(L"DmCPT_saves\\export"); }
std::wstring ImportDir() { return util::InExeDir(L"DmCPT_saves\\import"); }

void OpenFolder(const std::wstring& dir) {
  MakeDirs(dir);
  ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Refresh() {
  bool expected = false;
  if (!g_refreshing.compare_exchange_strong(expected, true)) return;
  std::thread([] {
    DoRefresh();
    g_refreshing = false;
  }).detach();
}

std::vector<SlotInfo> Slots() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_slots;
}
std::vector<ImportEntry> Imports() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_imports;
}
int NumSlots() {
  std::lock_guard<std::mutex> lk(g_mu);
  return std::max(g_numSlots, (int)g_slots.size());
}
int ActiveSlot() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_active;
}

void Export(int slot, const std::string& name, const std::string& notes, bool saveFirst) {
  if (!Begin()) return;
  RunJob([=] { ExportJob(slot, name, notes, saveFirst); });
}

void Rename(int entry, const std::string& name) {
  ImportEntry e;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (entry < 0 || entry >= (int)g_imports.size() || name.empty()) return;
    e = g_imports[entry];
  }
  if (!Begin()) return;
  RunJob([=] { RenameJob(e, name); });
}

void Import(int entry, int slot) {
  ImportEntry e;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (entry < 0 || entry >= (int)g_imports.size()) return;
    e = g_imports[entry];
  }
  if (!Begin()) return;
  RunJob([=] { ImportJob(e, slot); });
}

void LoadSlot(int slot) {
  if (!Begin()) return;
  RunJob([=] {
    GameCtx g = Context();
    std::string err;
    if (!g.profile || !g.frontEnd)
      Rep("FAIL: main menu only");
    else if (ReloadSlot(g, slot, &err))
      Rep("OK   slot %d loaded", slot + 1);
    else
      Rep("FAIL: %s", err.c_str());
  });
}

int ActiveSlotNow() {
  GameCtx g = Context();
  if (!g.profile) return -1;
  uintptr_t ctx = g.profile;
  return OnGame<int>([ctx] { return StaticInt(ctx, "GetActiveSlotIndex"); }, 3000);
}

bool ReadSlotFile(int slot, Kind k, Bytes& enc) {
  return ReadAll(steam::SaveDir() + L"\\" + SlotFileName(slot, k), enc);
}

bool WriteSlotFile(int slot, Kind k, const Bytes& enc, std::string* err) {
  std::wstring name = SlotFileName(slot, k), path = steam::SaveDir() + L"\\" + name;
  std::string how;
  bool ok = steam::CloudReady(&how) && steam::CloudWrite(util::WideToUtf8(name), enc.data(), (int)enc.size());
  if (!ok) ok = WriteAll(path, enc);
  Bytes check;
  ok = ok && ReadAll(path, check) && check == enc;
  if (!ok && err) *err = "cannot write " + util::WideToUtf8(name);
  return ok;
}

bool ReloadSlotNow(int slot, std::string* err) {
  GameCtx g = Context();
  if (!g.profile || !g.frontEnd) {
    if (err) *err = "main menu only";
    return false;
  }
  std::string e;
  bool ok = ReloadSlot(g, slot, &e);
  if (!ok && err) *err = e;
  return ok;
}

bool Busy() { return g_busy || g_refreshing; }

std::vector<std::string> Report() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_report;
}

std::wstring LastExport() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_lastExport;
}

void ExportFromScript(const std::string& args) {
  int slot = atoi(args.c_str()) - 1;
  size_t sp = args.find(' ');
  std::string notes = sp == std::string::npos ? "" : args.substr(sp + 1);
  if (slot < 0) {
    LOG("slots: export: bad slot '%s'", args.c_str());
    return;
  }
  Export(slot, "", notes, true);
}

void ImportFromScript(const std::string& args) {
  size_t sp = args.find_last_of(' ');
  if (sp == std::string::npos) {
    LOG("slots: import: usage import <name> <slot>");
    return;
  }
  std::string name = args.substr(0, sp);
  int slot = atoi(args.c_str() + sp + 1) - 1;
  DoRefresh();
  std::vector<ImportEntry> im = Imports();
  for (size_t i = 0; i < im.size(); ++i)
    if (im[i].display == name || im[i].display.rfind(name, 0) == 0) {
      Import((int)i, slot);
      return;
    }
  LOG("slots: import: '%s' not found in %s", name.c_str(), util::WideToUtf8(ImportDir()).c_str());
}

}  // namespace slots
