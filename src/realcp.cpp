#include "realcp.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "game.h"
#include "log.h"
#include "savefile.h"
#include "saveslots.h"
#include "steam_guard.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "unlock.h"
#include "util.h"

using namespace savefile;

namespace realcp {
namespace {

std::mutex g_mu;  // g_list, g_msg
std::vector<Entry> g_list;
std::string g_msg;
std::atomic<bool> g_busy{false};
Created g_created;  // under g_mu
bool g_hasCreated = false;

void Say(const std::string& m) {
  LOG("realcp: %s", m.c_str());
  std::lock_guard<std::mutex> lk(g_mu);
  g_msg = m;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::wstring Root() { return util::InExeDir(L"DmCPT_checkpoints"); }

bool ReadFile(const std::wstring& p, Bytes& out) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(h, nullptr), got = 0;
  out.resize(size);
  bool ok = size == 0 || (::ReadFile(h, out.data(), size, &got, nullptr) && got == size);
  CloseHandle(h);
  return ok;
}

bool WriteFile(const std::wstring& p, const Bytes& b) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = ::WriteFile(h, b.data(), (DWORD)b.size(), &put, nullptr) && put == b.size();
  CloseHandle(h);
  return ok;
}

// The game's encrypted CHECKPNT file -> map / checkpoint / difficulty / date.
bool Describe(const Bytes& enc, Entry& e) {
  Bytes dec = Decrypt(enc);
  Header h;
  if (!ParseHeader(dec, h, nullptr)) return false;
  CheckpointData c = ParseCheckpoint(Payload(dec));
  if (!c.valid || !c.hasCheckpoint || c.map.empty() || c.name.empty()) return false;
  e.map = c.map;
  e.name = c.name;
  e.difficulty = c.difficulty;
  char b[32];
  snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", c.year, c.month, c.day, c.secondsOfDay / 3600,
           c.secondsOfDay / 60 % 60);
  e.saved = b;
  return true;
}

std::vector<Entry> Scan() {
  std::vector<Entry> out;
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW((Root() + L"\\*").c_str(), &d);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == L'.') continue;
    std::wstring dir = Root() + L"\\" + d.cFileName;
    WIN32_FIND_DATAW f;
    HANDLE h2 = FindFirstFileW((dir + L"\\*.chk").c_str(), &f);
    if (h2 == INVALID_HANDLE_VALUE) continue;
    do {
      Entry e;
      e.file = dir + L"\\" + f.cFileName;
      Bytes b;
      if (ReadFile(e.file, b) && Describe(b, e)) out.push_back(e);
    } while (FindNextFileW(h2, &f));
    FindClose(h2);
  } while (FindNextFileW(h, &d));
  FindClose(h);
  return out;
}

// Stores the game's checkpoint file as DmCPT_checkpoints\<map>\<name>.chk.
bool Store(const Bytes& enc, Entry& e, std::string* err) {
  if (!Describe(enc, e)) {
    if (err) *err = "not a checkpoint file";
    return false;
  }
  CreateDirectoryW(Root().c_str(), nullptr);
  std::wstring dir = Root() + L"\\" + util::Utf8ToWide(SafeFileName(Lower(e.map)));
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring file = dir + L"\\" + util::Utf8ToWide(SafeFileName(e.name)) + L".chk";
  if (!WriteFile(file, enc)) {
    if (err) *err = "cannot write " + util::WideToUtf8(file);
    return false;
  }
  return true;
}

bool Start(const char* what) {
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) {
    LOG("realcp: %s ignored (busy)", what);
    return false;
  }
  return true;
}

void Job(std::function<void()> fn) {
  std::thread([fn] {
    fn();
    g_busy = false;
  }).detach();
}

template <class F>
bool WaitFor(F cond, int ms) {
  for (int t = 0; t < ms; t += 250) {
    if (cond()) return true;
    Sleep(250);
  }
  return false;
}

// A call on the first live object of a class (game thread).
bool CallLive(const char* cls, const char* fn) {
  return engine::OnGame<bool>([cls, fn] {
    uintptr_t c = ue3::FindObject("Class", cls);
    for (int i = 0, n = ue3::NumObjects(); c && i < n; ++i) {
      uintptr_t o = ue3::Object(i);
      if (o && ue3::ObjClass(o) == c && ue3::ObjName(o).rfind("Default__", 0) != 0) {
        engine::Call call(o, fn);
        return call.Invoke();
      }
    }
    return false;
  });
}

std::string CheckpointInMemory() {
  return engine::OnGame<std::string>([] {
    const game::World& w = game::W();
    uintptr_t cp = w.engine ? game::GetObj(w.engine, "GCheckPointData") : 0;
    game::Field f = cp ? game::FieldOf(cp, "m_CheckpointName") : game::Field{};
    ue3::TArrayRaw s{};
    if (!f.ok() || !util::Read(cp + f.off, s) || !s.data || s.num <= 0 || s.num > 256) return std::string();
    return util::WideToUtf8(util::ReadWString(s.data, s.num));
  });
}

}  // namespace

std::vector<Entry> List() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_list;
}

bool Find(const std::string& map, const std::string& name, Entry* out) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (const Entry& e : g_list)
    if (Lower(e.map) == Lower(map) && e.name == name) {
      if (out) *out = e;
      return true;
    }
  return false;
}

void Refresh() {
  std::vector<Entry> l = Scan();
  std::lock_guard<std::mutex> lk(g_mu);
  g_list = l;
}

bool AddFile(const std::vector<uint8_t>& enc, std::string* err) {
  Entry e;
  return Store(enc, e, err);
}

bool Busy() { return g_busy; }

bool TakeCreated(Created& out) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_hasCreated) return false;
  out = g_created;
  g_hasCreated = false;
  return true;
}

std::string Message() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_msg;
}

void KeepLast() {
  if (!Start("keep")) return;
  Job([] {
    int slot = slots::ActiveSlotNow();
    Bytes enc;
    Entry e;
    if (slot < 0 || !slots::ReadSlotFile(slot, Checkpoint, enc) || !Describe(enc, e)) {
      Say("No checkpoint saved by the game in this slot yet.");
      return;
    }
    std::string map = game::CurrentStatus().map;
    if (Lower(e.map) != Lower(map)) {
      Say("The game's last checkpoint is in another mission (" + e.map + "): reach a checkpoint here first.");
      return;
    }
    std::string err;
    if (!Store(enc, e, &err)) {
      Say(err);
      return;
    }
    Refresh();
    Created c = engine::OnGame<Created>([] {
      Created r;
      const game::World& w = game::W();
      game::Field l = w.pawn ? game::FieldOf(w.pawn, "Location") : game::Field{};
      game::Field rt = w.pawn ? game::FieldOf(w.pawn, "Rotation") : game::Field{};
      if (l.ok()) {
        game::Vec v = util::ReadOr<game::Vec>(w.pawn + l.off);
        r.hasPos = true;
        r.x = v.x, r.y = v.y, r.z = v.z;
        if (rt.ok()) r.yaw = util::ReadOr<int>(w.pawn + rt.off + 4) & 0xFFFF;
      }
      return r;
    });
    c.map = e.map;
    c.name = e.name;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_created = c;
      g_hasCreated = true;
    }
    Say("Created: " + e.name + " (" + DifficultyName(e.difficulty) + ", saved " + e.saved + ")");
  });
}

void Forget(const std::string& map, const std::string& name) {
  Entry e;
  if (!Find(map, name, &e)) return;
  DeleteFileW(e.file.c_str());
  Refresh();
  Say("Deleted your checkpoint " + name);
}

void Load(const std::string& map, const std::string& name) {
  if (!Start("load")) return;
  Job([map, name] {
    Entry e;
    Bytes enc;
    if (!Find(map, name, &e) || !ReadFile(e.file, enc)) {
      Say("Your checkpoint " + name + " was not found");
      return;
    }
    // The file says whose save it is: make it ours (a copy from another player loads too).
    Bytes dec = Decrypt(enc);
    SetAccount(dec, steam::SaveAccountId());
    enc = Encrypt(dec);
    int slot = slots::ActiveSlotNow();
    if (slot < 0) {
      Say("No save slot is loaded");
      return;
    }
    LOG("realcp: loading kept %s / %s into slot %d", e.map.c_str(), name.c_str(), slot + 1);
    // 1. Main menu (the pause menu's Quit): only there the game reads a slot from disk.
    if (!game::CurrentStatus().frontEnd) {
      CallLive("NTGFxMovieManager", "CloseAllIngameUI");
      if (!CallLive("DVGFxMovieHandler_IngameUI_Callbacks", "IngameMenu_MainMenu_Deferred")) {
        Say("Could not leave the mission");
        return;
      }
      if (!WaitFor([] { int s = unlock::SlotStatus(); return game::CurrentStatus().frontEnd && (s == 3 || s == 4); },
                   60000)) {
        Say("The main menu did not come up");
        return;
      }
      Sleep(1500);  // let the menu settle (it may still save the profile)
    }
    // 2. The kept file into the slot (after leaving: quitting may still write the slot), 3. reload the slot.
    std::string err;
    if (!slots::WriteSlotFile(slot, Checkpoint, enc, &err) || !slots::ReloadSlotNow(slot, &err)) {
      Say("Loading your checkpoint failed: " + err);
      return;
    }
    std::string mem = CheckpointInMemory();
    if (mem != name) {
      Say("The game read a different checkpoint ('" + mem + "')");
      return;
    }
    // 4. Continue, exactly like the main menu button.
    if (!CallLive("DVGFxMovieHandler_Frontend_Callbacks", "ContinueFromCheckpoint")) {
      Say("Continue failed");
      return;
    }
    bool ok = WaitFor([&e] {
      game::Status s = game::CurrentStatus();
      return s.mission && Lower(s.map) == Lower(e.map);
    }, 90000);
    Say(ok ? "Loaded your checkpoint " + name : "The mission did not start");
  });
}

}  // namespace realcp
