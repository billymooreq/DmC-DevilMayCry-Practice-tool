// dinput8.dll proxy: forwards every export to the system dinput8.dll and, while the menu is open,
// mutes keyboard/mouse DirectInput data so the game does not react to menu clicks.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <atomic>
#include <deque>
#include <mutex>

#include "config.h"
#include "proxy.h"
#include "hooks.h"
#include "log.h"
#include "state.h"

namespace {

HMODULE g_real = nullptr;
std::once_flag g_loadOnce;

FARPROC RealProc(const char* name) {
  std::call_once(g_loadOnce, [] {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);  // SysWOW64 for a 32-bit process via redirection
    std::wstring path = std::wstring(sys) + L"\\dinput8.dll";
    g_real = LoadLibraryW(path.c_str());
    LOG("proxy: real dinput8 %s (%p)", g_real ? "loaded" : "NOT loaded", g_real);
  });
  return g_real ? GetProcAddress(g_real, name) : nullptr;
}

// --- DirectInput muting -----------------------------------------------------
using CreateDevice_t = HRESULT(WINAPI*)(void* self, REFGUID guid, void** device, IUnknown* outer);
using GetDeviceState_t = HRESULT(WINAPI*)(void* self, DWORD cb, void* data);
using GetDeviceData_t = HRESULT(WINAPI*)(void* self, DWORD cbObj, DIDEVICEOBJECTDATA* rgdod,
                                         DWORD* inOut, DWORD flags);

void HookDevice(void* device, REFGUID guid);

// ---- injected input (autorun tests: the game reads DirectInput, window messages do not reach it in a mission)
std::atomic<void*> g_keyboard{nullptr}, g_mouse{nullptr};
std::atomic<uint8_t> g_injKey[256];
std::atomic<uint8_t> g_injMouse[8];
std::mutex g_evMu;
std::deque<std::pair<DWORD, DWORD>> g_kbEvents, g_mouseEvents;  // (offset, data) for buffered readers
DWORD g_evSeq = 0;

void InjectState(void* self, DWORD cb, void* data) {
  if (self == g_keyboard && cb == 256) {
    auto* k = (uint8_t*)data;
    for (int i = 0; i < 256; ++i)
      if (g_injKey[i]) k[i] = 0x80;
  } else if (self == g_mouse && (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2))) {
    auto* m = (uint8_t*)data;
    int n = cb == sizeof(DIMOUSESTATE) ? 4 : 8;
    for (int i = 0; i < n; ++i)
      if (g_injMouse[i]) m[offsetof(DIMOUSESTATE, rgbButtons) + i] = 0x80;
  }
}

void InjectData(void* self, DWORD cbObj, DIDEVICEOBJECTDATA* rgdod, DWORD* inOut, DWORD flags, DWORD capacity) {
  if (!rgdod || (flags & DIGDD_PEEK) || cbObj < 16) return;
  std::deque<std::pair<DWORD, DWORD>>* q = self == g_keyboard ? &g_kbEvents : self == g_mouse ? &g_mouseEvents : nullptr;
  if (!q) return;
  std::lock_guard<std::mutex> lk(g_evMu);
  while (!q->empty() && *inOut < capacity) {
    auto* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rgdod + *inOut * cbObj);
    memset(e, 0, cbObj);
    e->dwOfs = q->front().first;
    e->dwData = q->front().second;
    e->dwTimeStamp = GetTickCount();
    e->dwSequence = ++g_evSeq;
    q->pop_front();
    ++*inOut;
  }
}

// A and W interfaces have separate vtables, so two independent slots each.
template <int I>
struct CreateDeviceHook {
  static inline void* target = nullptr;
  static inline CreateDevice_t orig = nullptr;
  static HRESULT WINAPI Detour(void* self, REFGUID guid, void** device, IUnknown* outer) {
    HRESULT hr = orig(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device) HookDevice(*device, guid);
    return hr;
  }
};

template <int I>
struct DeviceHooks {
  static inline void* stateTarget = nullptr;
  static inline GetDeviceState_t origState = nullptr;
  static inline GetDeviceData_t origData = nullptr;
  static HRESULT WINAPI State(void* self, DWORD cb, void* data) {
    HRESULT hr = origState(self, cb, data);
    // Keyboard (256 bytes) and mouse states only: zeroed joystick axes would mean full deflection.
    if (SUCCEEDED(hr) && data && state::menuOpen &&
        (cb == 256 || cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)))
      memset(data, 0, cb);
    else if (SUCCEEDED(hr) && data)
      InjectState(self, cb, data);
    return hr;
  }
  static HRESULT WINAPI Data(void* self, DWORD cbObj, DIDEVICEOBJECTDATA* rgdod, DWORD* inOut,
                             DWORD flags) {
    DWORD capacity = inOut ? *inOut : 0;
    HRESULT hr = origData(self, cbObj, rgdod, inOut, flags);
    if (SUCCEEDED(hr) && inOut && state::menuOpen) *inOut = 0;  // drop buffered events
    else if (SUCCEEDED(hr) && inOut) InjectData(self, cbObj, rgdod, inOut, flags, capacity);
    return hr;
  }
};

std::mutex g_hookMu;

template <int I>
bool TryHookDeviceSlot(void** vtbl) {
  using H = DeviceHooks<I>;
  if (H::stateTarget == vtbl[9]) return true;  // already hooked
  if (H::stateTarget) return false;            // slot busy with another vtable
  H::stateTarget = vtbl[9];
  bool ok = hooks::Create(vtbl[9], (void*)&H::State, (void**)&H::origState, "DI.GetDeviceState") &&
            hooks::Create(vtbl[10], (void*)&H::Data, (void**)&H::origData, "DI.GetDeviceData");
  if (ok) {
    hooks::Enable(vtbl[9], "DI.GetDeviceState");
    hooks::Enable(vtbl[10], "DI.GetDeviceData");
  }
  return true;
}

void HookDevice(void* device, REFGUID guid) {
  if (!g_cfg.hookDInput) return;
  if (guid == GUID_SysKeyboard) g_keyboard = device;
  if (guid == GUID_SysMouse) g_mouse = device;
  std::lock_guard<std::mutex> lk(g_hookMu);
  void** vtbl = *reinterpret_cast<void***>(device);
  if (!TryHookDeviceSlot<0>(vtbl)) TryHookDeviceSlot<1>(vtbl);
}

template <int I>
bool TryHookCreateSlot(void** vtbl) {
  using H = CreateDeviceHook<I>;
  if (H::target == vtbl[3]) return true;
  if (H::target) return false;
  H::target = vtbl[3];
  if (hooks::Create(vtbl[3], (void*)&H::Detour, (void**)&H::orig, "DI.CreateDevice"))
    hooks::Enable(vtbl[3], "DI.CreateDevice");
  return true;
}

void OnDirectInputCreated(void* di) {
  if (!g_cfg.hookDInput) return;
  std::lock_guard<std::mutex> lk(g_hookMu);
  void** vtbl = *reinterpret_cast<void***>(di);
  if (!TryHookCreateSlot<0>(vtbl)) TryHookCreateSlot<1>(vtbl);
}

}  // namespace

namespace dinject {
void Key(int dik, bool down) {
  if (dik <= 0 || dik > 255) return;
  if (g_injKey[dik].exchange(down ? 1 : 0) == (down ? 1 : 0)) return;
  std::lock_guard<std::mutex> lk(g_evMu);
  g_kbEvents.push_back({(DWORD)dik, down ? 0x80u : 0u});
}
void MouseButton(int b, bool down) {
  if (b < 0 || b > 7) return;
  if (g_injMouse[b].exchange(down ? 1 : 0) == (down ? 1 : 0)) return;
  std::lock_guard<std::mutex> lk(g_evMu);
  g_mouseEvents.push_back({(DWORD)(offsetof(DIMOUSESTATE, rgbButtons) + b), down ? 0x80u : 0u});
}
bool KeyboardSeen() { return g_keyboard != nullptr; }
bool KeyHeld(int dik) { return dik > 0 && dik < 256 && g_injKey[dik] != 0; }
}  // namespace dinject

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out,
                                        LPUNKNOWN outer) {
  using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  auto fn = (Fn)RealProc("DirectInput8Create");
  if (!fn) return E_FAIL;
  HRESULT hr = fn(hinst, version, riid, out, outer);
  LOG("proxy: DirectInput8Create(version=0x%X) -> 0x%08X", version, (unsigned)hr);
  if (SUCCEEDED(hr) && out && *out) OnDirectInputCreated(*out);
  return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
  auto fn = (HRESULT(WINAPI*)())RealProc("DllCanUnloadNow");
  return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
  auto fn = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))RealProc("DllGetClassObject");
  return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
  auto fn = (HRESULT(WINAPI*)())RealProc("DllRegisterServer");
  return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
  auto fn = (HRESULT(WINAPI*)())RealProc("DllUnregisterServer");
  return fn ? fn() : E_FAIL;
}

LPCDIDATAFORMAT WINAPI Proxy_GetdfDIJoystick() {
  auto fn = (LPCDIDATAFORMAT(WINAPI*)())RealProc("GetdfDIJoystick");
  return fn ? fn() : nullptr;
}

}  // extern "C"
