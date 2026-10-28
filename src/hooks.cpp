#include "hooks.h"

#include <windows.h>

#include <mutex>

#include "MinHook.h"
#include "log.h"

namespace hooks {

static std::once_flag g_once;
static bool g_ok = false;

bool EnsureInit() {
  std::call_once(g_once, [] {
    MH_STATUS s = MH_Initialize();
    g_ok = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED);
    LOG("minhook: init %s", g_ok ? "ok" : MH_StatusToString(s));
  });
  return g_ok;
}

bool Create(void* target, void* detour, void** original, const char* name) {
  if (!EnsureInit() || !target) return false;
  MH_STATUS s = MH_CreateHook(target, detour, original);
  if (s != MH_OK) {
    LOG("hook %s @%p: create failed: %s", name, target, MH_StatusToString(s));
    return false;
  }
  return true;
}

bool Enable(void* target, const char* name) {
  MH_STATUS s = MH_EnableHook(target);
  LOG("hook %s @%p: %s", name, target, s == MH_OK ? "enabled" : MH_StatusToString(s));
  return s == MH_OK;
}

}  // namespace hooks
