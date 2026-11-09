#include "devtools.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "features.h"
#include "game.h"
#include "log.h"
#include "steam_guard.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"

namespace devtools {
namespace {

// ENTButton (NTInput): the developers' build checked these in NTPlayerController.Handle_PausePressed.
constexpr int kLeftTrigger = 10;   // Angel (keyboard Q by default)
constexpr int kRightTrigger = 8;   // Demon (keyboard E by default)

struct Fns {
  bool resolved = false;
  uintptr_t unlockAll = 0;                      // DVExecFunctions.UnlockAllUpgrades
  uintptr_t execNT = 0, execDV = 0;             // the debug menu's buttons: NTGFxCallbacks.ExecConsoleCommand (+ DV)
  uintptr_t pcConsole = 0, actorConsole = 0;    // ConsoleCommand called through ProcessEvent (our tests)
  uintptr_t pausePressed = 0;                   // NTPlayerController.Handle_PausePressed
  int offIsDebug = -1, offRet = -1;
  uint32_t maskIsDebug = 1, maskRet = 1;
} g_f;

int g_consoleDepth = 0;  // game thread: inside a console command (made by hand, not by a level script)
std::atomic<int> g_blocked{0}, g_opened{0};
std::atomic<int> g_available{-1};  // the game has DisplayDebugMenu: -1 not known yet

void Resolve() {
  g_f.resolved = true;
  g_f.unlockAll = ue3::FindObject("Function", "UnlockAllUpgrades", "DVExecFunctions");
  g_f.execNT = ue3::FindObject("Function", "ExecConsoleCommand", "NTGFxCallbacks");
  g_f.execDV = ue3::FindObject("Function", "ExecConsoleCommand", "DVGFxMovieHandler_IngameUI_Callbacks");
  g_f.pcConsole = ue3::FindObject("Function", "ConsoleCommand", "PlayerController");
  g_f.actorConsole = ue3::FindObject("Function", "ConsoleCommand", "Actor");
  g_f.pausePressed = ue3::FindObject("Function", "Handle_PausePressed", "NTPlayerController");
  if (g_f.pausePressed) {
    for (const engine::Param& p : engine::Params(g_f.pausePressed)) {
      if (p.name == "bIsDebug") g_f.offIsDebug = p.offset, g_f.maskIsDebug = p.boolMask ? p.boolMask : 1;
      if (p.name == "ReturnValue") g_f.offRet = p.offset, g_f.maskRet = p.boolMask ? p.boolMask : 1;
    }
  }
  bool menu = g_f.pausePressed && g_f.offIsDebug >= 0 && g_f.offRet >= 0 &&
              ue3::FindObject("Function", "DisplayDebugMenu", "NTPlayerController") &&
              ue3::FindObject("Function", "IsHeld", "NTPlayerController");
  g_available = menu ? 1 : 0;
  LOG("devtools: UnlockAllUpgrades %s, debug menu %s", g_f.unlockAll ? "found" : "MISSING",
      menu ? "available" : "NOT available");
}

bool IsConsoleFn(uintptr_t f) {
  return f && (f == g_f.execNT || f == g_f.execDV || f == g_f.pcConsole || f == g_f.actorConsole);
}

bool Held(uintptr_t pc, int button) {
  engine::Call c(pc, "IsHeld");
  return c.Valid() && c.Int(c.ParamName(0).c_str(), button).Invoke() && c.RetBool();
}

// Pause pressed with Angel + Demon held: the debug menu instead of the pause menu (what the developers' build did
// with its debug buttons). The call to Handle_PausePressed is skipped and reports "handled".
bool OpenDebugMenu(uintptr_t pc, void* parms) {
  uint32_t isDebug = 0;
  std::memcpy(&isDebug, (uint8_t*)parms + g_f.offIsDebug, 4);
  if (isDebug & g_f.maskIsDebug) return false;
  if (!ue3::IsA(pc, "NTPlayerController")) return false;
  if (!Held(pc, kLeftTrigger) || !Held(pc, kRightTrigger)) return false;
  engine::Call close(pc, "CloseAllIngameUIMenus");
  close.Invoke();
  engine::Call open(pc, "DisplayDebugMenu");
  if (!open.Invoke()) {
    LOG("devtools: DisplayDebugMenu failed: %s", open.Error().c_str());
    return false;
  }
  // The debug menu has commands that unlock achievements and upgrades: like any cheat, Steam stats stop here.
  steam::BlockForSession();
  uint32_t* ret = reinterpret_cast<uint32_t*>((uint8_t*)parms + g_f.offRet);
  *ret |= g_f.maskRet;
  ++g_opened;
  LOG("devtools: debug menu opened (pause + Angel + Demon)");
  return true;
}

bool Veto(void* self, void* fn, void* parms) {
  if (!g_f.resolved) {
    if (!ue3::Ready()) return false;
    Resolve();
  }
  uintptr_t f = (uintptr_t)fn;
  if (IsConsoleFn(f)) {
    ++g_consoleDepth;
    return false;
  }
  if (f && f == g_f.unlockAll && g_consoleDepth == 0) {
    // The map that owns the caller (the room's own CheatManager), not game::W(): the frame world may still be
    // the previous map while the new level's script starts.
    std::string map = game::TopPackage((uintptr_t)self);
    if (IsDevRoomMap(map)) {
      ++g_blocked;
      LOG("devtools: %s: the level script's UnlockAllUpgrades skipped (keeps your save's abilities)", map.c_str());
      return true;
    }
    return false;
  }
  if (f && f == g_f.pausePressed && parms && g_cfg.debugMenu && !features::SpeedrunMode() && g_available == 1)
    return OpenDebugMenu((uintptr_t)self, parms);
  return false;
}

}  // namespace

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  engine::SetEventVeto(&Veto);
}

// Called by the features' "after" hook (game thread): leaves a console command.
void AfterEvent(void* /*self*/, void* fn) {
  if (g_consoleDepth > 0 && IsConsoleFn((uintptr_t)fn)) --g_consoleDepth;
}

bool IsDevRoomMap(const std::string& map) { return _stricmp(map.c_str(), "Combat") == 0; }

bool DebugMenuOn() { return g_cfg.debugMenu; }
void SetDebugMenu(bool on) { g_cfg.debugMenu = on; }
int DebugMenuAvailable() { return g_available; }

int UnlockAllBlocked() { return g_blocked; }
int DebugMenuOpened() { return g_opened; }

}  // namespace devtools
