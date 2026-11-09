#include "window_mode.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "config.h"
#include "hooks.h"
#include "log.h"

namespace winmode {
namespace {

std::atomic<int> g_mode{kFullscreen};  // this game start (DmCPracticeTool.json "window_mode")
std::atomic<bool> g_active{false};      // the game's device was made windowed and its window styled
std::atomic<HWND> g_wnd{nullptr};       // the game window
std::string g_early;                    // what DllMain did (logged later: no logging under the loader lock)

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                        IDirect3DDevice9**);
using ChangeDisplaySettingsW_t = LONG(WINAPI*)(DEVMODEW*, DWORD);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SetWindowLongW_t = LONG(WINAPI*)(HWND, int, LONG);
using ShowWindow_t = BOOL(WINAPI*)(HWND, int);

Direct3DCreate9_t oCreate9 = nullptr;
CreateDevice_t oCreateDevice = nullptr;
ChangeDisplaySettingsW_t oChangeDisplay = nullptr;
SetWindowPos_t oSetWindowPos = nullptr;
SetWindowLongW_t oSetWindowLong = nullptr;
ShowWindow_t oShowWindow = nullptr;

constexpr LONG kBorderlessStyle = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
constexpr LONG kWindowStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

LONG Style() { return g_mode == kWindowed ? kWindowStyle : kBorderlessStyle; }

MONITORINFO Monitor(HWND w) {
  MONITORINFO mi{sizeof(mi)};
  if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi)) {
    mi.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi.rcWork = mi.rcMonitor;
  }
  return mi;
}

bool IsOurDummy(HWND w) {  // d3d9hook's throwaway device (it reads the device vtable)
  wchar_t cls[64] = {};
  GetClassNameW(w, cls, 63);
  return wcscmp(cls, L"DmCPT_Dummy") == 0;
}

// Borderless: the whole monitor. Window: the game's picture size (smaller if it does not fit 85% of the work area,
// same shape), centred on the work area, with a normal frame.
void PlaceWindow(HWND w, UINT bbW, UINT bbH) {
  if (!w) return;
  MONITORINFO mi = Monitor(w);
  (oSetWindowLong ? oSetWindowLong : SetWindowLongW)(w, GWL_STYLE, Style());
  LONG ex = GetWindowLongW(w, GWL_EXSTYLE) & ~WS_EX_TOPMOST;
  (oSetWindowLong ? oSetWindowLong : SetWindowLongW)(w, GWL_EXSTYLE, ex);
  RECT r = mi.rcMonitor;
  if (g_mode == kWindowed) {
    const RECT& wa = mi.rcWork;
    double maxW = (wa.right - wa.left) * 0.85, maxH = (wa.bottom - wa.top) * 0.85;
    double cw = bbW ? bbW : 1280, ch = bbH ? bbH : 720;
    double k = std::min(1.0, std::min(maxW / cw, maxH / ch));
    RECT c{0, 0, (LONG)(cw * k), (LONG)(ch * k)};
    AdjustWindowRectEx(&c, kWindowStyle, FALSE, ex);
    LONG ww = c.right - c.left, wh = c.bottom - c.top;
    r.left = wa.left + ((wa.right - wa.left) - ww) / 2;
    r.top = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    r.right = r.left + ww;
    r.bottom = r.top + wh;
  }
  (oSetWindowPos ? oSetWindowPos : SetWindowPos)(w, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void Windowed(D3DPRESENT_PARAMETERS* pp) {
  if (!pp->Windowed) {
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;  // must be 0 for a windowed device
  }
}

HRESULT WINAPI hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                              D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev) {
  HWND w = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
  if (g_mode != kFullscreen && pp && w && !IsOurDummy(w)) {
    LOG("winmode: the game creates its device %ux%u %s - made windowed (%s)", pp->BackBufferWidth,
        pp->BackBufferHeight, pp->Windowed ? "windowed" : "FULLSCREEN", ModeKey(g_mode));
    Windowed(pp);
    g_wnd = w;
    g_active = true;
    PlaceWindow(w, pp->BackBufferWidth, pp->BackBufferHeight);
  }
  return oCreateDevice(d3d, adapter, type, focus, flags, pp, dev);
}

IDirect3D9* WINAPI hkCreate9(UINT sdk) {
  IDirect3D9* d3d = oCreate9(sdk);
  static bool hooked = false;
  if (d3d && !hooked) {
    hooked = true;
    if (!g_early.empty()) LOG("winmode: %s", g_early.c_str());
    void* fn = (*reinterpret_cast<void***>(d3d))[16];  // IDirect3D9::CreateDevice
    if (hooks::Create(fn, (void*)&hkCreateDevice, (void**)&oCreateDevice, "D3D9.CreateDevice"))
      hooks::Enable(fn, "D3D9.CreateDevice");
  }
  return d3d;
}

LONG WINAPI hkChangeDisplay(DEVMODEW* mode, DWORD flags) {
  if (g_mode != kFullscreen) return DISP_CHANGE_SUCCESSFUL;  // the desktop keeps its own mode
  return oChangeDisplay(mode, flags);
}

BOOL WINAPI hkSetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags) {
  if (g_active && w == g_wnd) {
    if (g_mode == kWindowed) return TRUE;  // the player moves and sizes the window, not the game
    RECT r = Monitor(w).rcMonitor;         // the game places its "fullscreen" window: always the whole monitor
    return oSetWindowPos(w, after == HWND_TOPMOST ? HWND_NOTOPMOST : after, r.left, r.top, r.right - r.left,
                         r.bottom - r.top, (flags & ~(SWP_NOMOVE | SWP_NOSIZE)) | SWP_FRAMECHANGED);
  }
  return oSetWindowPos(w, after, x, y, cx, cy, flags);
}

LONG WINAPI hkSetWindowLong(HWND w, int index, LONG v) {
  if (g_active && w == g_wnd) {
    if (index == GWL_STYLE) v = Style();
    if (index == GWL_EXSTYLE) v &= ~WS_EX_TOPMOST;  // other windows (LiveSplit, OBS) may stay on top
  }
  return oSetWindowLong(w, index, v);
}

BOOL WINAPI hkShowWindow(HWND w, int cmd) {
  // exclusive fullscreen minimises the game when it loses the focus; a window stays where it is
  if (g_active && w == g_wnd && (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE ||
                                 cmd == SW_FORCEMINIMIZE))
    return TRUE;
  return oShowWindow(w, cmd);
}

// The exe's import table entry of dll!func -> detour; returns the original or null.
void* PatchImport(const char* dll, const char* func, void* detour) {
  auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++addrs) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
      DWORD old = 0;
      if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
      void* orig = reinterpret_cast<void*>(addrs->u1.Function);
      addrs->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
      VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
      return orig;
    }
  }
  return nullptr;
}

std::string ConfigValue(const std::string& t, const char* key) {  // "key": "value" or true/false, as text
  size_t k = t.find(std::string("\"") + key + "\"");
  if (k == std::string::npos) return "";
  size_t c = t.find(':', k);
  if (c == std::string::npos) return "";
  size_t v = t.find_first_not_of(" \t\r\n", c + 1);
  if (v == std::string::npos) return "";
  if (t[v] == '"') {
    size_t e = t.find('"', v + 1);
    return e == std::string::npos ? "" : t.substr(v + 1, e - v - 1);
  }
  size_t e = t.find_first_of(",}\r\n", v);
  return t.substr(v, e == std::string::npos ? std::string::npos : e - v);
}

int ReadMode() {  // a plain look into the config (DllMain: no json parser)
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (wchar_t* s = wcsrchr(path, L'\\')) wcscpy_s(s + 1, MAX_PATH - (s + 1 - path), L"DmCPracticeTool.json");
  std::ifstream f(path, std::ios::binary);
  if (!f) return kFullscreen;
  std::stringstream ss;
  ss << f.rdbuf();
  int m = ModeFromKey(ConfigValue(ss.str(), "window_mode").c_str());
  return m < 0 ? kFullscreen : m;
}

BOOL CALLBACK CloseGame(HWND h, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(h)) PostMessageW(h, WM_CLOSE, 0, 0);
  return TRUE;
}

std::wstring PsQuote(const std::wstring& s) {  // 'text' for PowerShell ('' inside)
  std::wstring o = L"'";
  for (wchar_t c : s) o += c == L'\'' ? L"''" : std::wstring(1, c);
  return o + L"'";
}

}  // namespace

const char* ModeKey(int m) { return m == kBorderless ? "borderless" : m == kWindowed ? "windowed" : "fullscreen"; }

int ModeFromKey(const char* s) {
  for (int m = kFullscreen; m <= kWindowed; ++m)
    if (_stricmp(s, ModeKey(m)) == 0) return m;
  return -1;
}

void InstallEarly() {
  g_mode = ReadMode();
  if (g_mode == kFullscreen) return;  // the game is not touched at all
  oCreate9 = (Direct3DCreate9_t)PatchImport("d3d9.dll", "Direct3DCreate9", (void*)&hkCreate9);
  oChangeDisplay = (ChangeDisplaySettingsW_t)PatchImport("user32.dll", "ChangeDisplaySettingsW", (void*)&hkChangeDisplay);
  oSetWindowPos = (SetWindowPos_t)PatchImport("user32.dll", "SetWindowPos", (void*)&hkSetWindowPos);
  oSetWindowLong = (SetWindowLongW_t)PatchImport("user32.dll", "SetWindowLongW", (void*)&hkSetWindowLong);
  oShowWindow = (ShowWindow_t)PatchImport("user32.dll", "ShowWindow", (void*)&hkShowWindow);
  g_early = std::string("window mode ") + ModeKey(g_mode) + ": imports Direct3DCreate9 " +
            (oCreate9 ? "ok" : "MISSING") + ", ChangeDisplaySettingsW " + (oChangeDisplay ? "ok" : "missing") +
            ", SetWindowPos " + (oSetWindowPos ? "ok" : "missing") + ", SetWindowLongW " +
            (oSetWindowLong ? "ok" : "missing") + ", ShowWindow " + (oShowWindow ? "ok" : "missing");
  if (!oCreate9) g_mode = kFullscreen;  // without the device hook nothing else may change
  if (!oChangeDisplay) oChangeDisplay = ChangeDisplaySettingsW;
  if (!oSetWindowPos) oSetWindowPos = SetWindowPos;
  if (!oSetWindowLong) oSetWindowLong = SetWindowLongW;
  if (!oShowWindow) oShowWindow = ShowWindow;
}

int Current() { return g_mode; }

void FixPresent(D3DPRESENT_PARAMETERS* pp) {
  if (!g_active || !pp) return;
  LOG("winmode: device reset %ux%u - kept windowed (%s)", pp->BackBufferWidth, pp->BackBufferHeight,
      ModeKey(g_mode));
  Windowed(pp);
  if (g_mode == kBorderless) PlaceWindow(g_wnd, pp->BackBufferWidth, pp->BackBufferHeight);
}

bool RestartGame() {
  SaveConfig();
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir = dir.substr(0, dir.find_last_of(L'\\'));
  // the game's own arguments (after the exe path in its command line)
  std::wstring cl = GetCommandLineW(), args;
  size_t p = cl[0] == L'"' ? cl.find(L'"', 1) : cl.find(L' ');
  if (p != std::wstring::npos) {
    args = cl.substr(p + 1);
    size_t s = args.find_first_not_of(L' ');
    args = s == std::wstring::npos ? L"" : args.substr(s);
  }
  // the helper writes what happened to DmCPT_logs\restart.txt (the game's log file is gone by then)
  std::wstring logDir = dir + L"\\DmCPT_logs", logFile = logDir + L"\\restart.txt";
  CreateDirectoryW(logDir.c_str(), nullptr);
  std::wstring ps = L"$log = " + PsQuote(logFile) + L"; try { (Get-Date).ToString('HH:mm:ss.fff') + ' waiting for " +
                    std::to_wstring(GetCurrentProcessId()) + L"' | Out-File $log; Wait-Process -Id " +
                    std::to_wstring(GetCurrentProcessId()) +
                    L" -Timeout 60 -ErrorAction SilentlyContinue; Start-Process -FilePath " + PsQuote(exe) +
                    L" -WorkingDirectory " + PsQuote(dir) + (args.empty() ? L"" : L" -ArgumentList " + PsQuote(args)) +
                    L" -ErrorAction Stop; (Get-Date).ToString('HH:mm:ss.fff') + ' started' | Out-File $log -Append } "
                    L"catch { $_ | Out-File $log -Append }";
  std::wstring esc;  // inside -Command "...": a double quote of the arguments must be \"
  for (wchar_t c : ps) esc += c == L'"' ? std::wstring(L"\\\"") : std::wstring(1, c);
  std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + esc + L"\"";
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  // CREATE_NO_WINDOW: a console without a window (PowerShell needs one; DETACHED_PROCESS made it quit at once).
  // Breakaway: a launcher may run the game inside a job that ends its children together with it.
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
                           nullptr, dir.c_str(), &si, &pi);
  if (!ok) ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
  if (!ok) {
    LOG("winmode: restart FAILED - could not start the helper (%lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  LOG("winmode: restart - the game closes now and starts again in mode '%s' (helper pid %lu)",
      ModeKey(g_cfg.windowMode), pi.dwProcessId);
  EnumWindows(CloseGame, 0);
  return true;
}

}  // namespace winmode
