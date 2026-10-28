#include <windows.h>

#include "autorun.h"
#include "config.h"
#include "d3d9_hook.h"
#include "features.h"
#include "game.h"
#include "input.h"
#include "log.h"
#include "state.h"
#include "steam_guard.h"
#include "ue3/engine.h"
#include "ue3/dumper.h"
#include "ue3/ue3.h"
#include "version.h"

static DWORD WINAPI InitThread(LPVOID) {
  const VersionInfo& v = GetGameVersion();
  LOG("DmC Practice Tool v%s starting. build %s %s", kToolVersion, __DATE__, __TIME__);
  LOG("game version: %s = %s (TimeDateStamp=0x%08X SizeOfImage=0x%08X base=0x%08X)", v.name, v.label,
      v.timeDateStamp, v.sizeOfImage, (unsigned)v.base);
  if (!FeaturesAllowed())
    LOG("WARNING: unknown game build - menu works, all game features are locked");

  features::Init();  // before the config: it holds the feature binds and presets
  LoadConfig();
  if (g_cfg.debugMenuOpenAtStart) state::menuOpen = true;

  input::InstallGlobalHooks();
  if (g_cfg.hookD3D9)
    d3d9hook::Install();
  else
    LOG("d3d9: hook disabled in config (no menu/overlay)");
  // ProcessEvent hook = game-thread task queue for UFunction calls (no effect until something is queued).
  if (engine::InstallProcessEventHook()) engine::SetPeLog(g_cfg.peLog, g_cfg.peLogFilter);
  // UE3 reflection is initialised in the background as soon as the game has loaded its names/objects:
  // the menu, the unlock checks and the PE log must not depend on a button that happens to scan first.
  ue3::StartAutoInit();

  // ISteamUserStats exists only after SteamAPI_Init (steam_api.dll may also load after us): retry for a few
  // minutes. Unlock refuses to run without the guard (see unlock::Run), so a late install is not a risk.
  HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
    for (int i = 0; i < 300 && !steam::InstallGuard(); ++i) Sleep(1000);
    if (!steam::GuardInstalled()) LOG("steam: achievements guard NOT installed after 300 s - unlock disabled");
    return 0;
  }, nullptr, 0, nullptr);
  if (t) CloseHandle(t);

  // Test hook: the offline test host sets this to exercise the UE3 scanner without the game.
  if (GetEnvironmentVariableA("DMCPT_TEST_SCAN", nullptr, 0)) dumper::RunAsync(dumper::Kind::Layout);
  // Test hook of the offline host (screenshots of the menu and the label at other resolutions): shows both.
  // It can only show the label, never hide or move it.
  if (GetEnvironmentVariableA("DMCPT_TEST_UI", nullptr, 0)) {
    state::menuOpen = true;
    features::ForceOverlay();
  }
  autorun::Start();
  LOG("init thread done");
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    // Nothing heavy under the loader lock: everything happens in the init thread.
    HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
