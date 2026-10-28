#include "input.h"

#include <Xinput.h>

#include <cstdio>

#include "config.h"
#include "hooks.h"
#include "hotkeys.h"
#include "imgui.h"
#include "log.h"
#include "state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace input {
namespace {

WNDPROC g_origWndProc = nullptr;
bool g_unicodeWnd = true;
int g_bbW = 0, g_bbH = 0;

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
SetCursorPos_t oSetCursorPos = nullptr;
ClipCursor_t oClipCursor = nullptr;
XInputGetState_t oXInputGetState = nullptr;
XInputGetState_t g_ownXInput = nullptr;  // when the game's xinput is not hooked

BOOL WINAPI hkSetCursorPos(int x, int y) {
  if (state::menuOpen) return TRUE;  // the game keeps re-centering the cursor for mouse look
  return oSetCursorPos(x, y);
}

BOOL WINAPI hkClipCursor(const RECT* r) {
  if (state::menuOpen) return oClipCursor(nullptr);
  return oClipCursor(r);
}

DWORD WINAPI hkXInputGetState(DWORD idx, XINPUT_STATE* st) {
  DWORD r = oXInputGetState(idx, st);
  if (r == ERROR_SUCCESS && st && state::menuOpen) memset(&st->Gamepad, 0, sizeof(st->Gamepad));
  return r;
}

bool IsInputMessage(UINT msg) {
  return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
         msg == WM_INPUT || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR;
}

LRESULT CallOrig(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return g_unicodeWnd ? CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp)
                      : CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (state::menuOpen) {
    LPARAM imLp = lp;
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) {
      RECT rc;
      GetClientRect(hwnd, &rc);
      if (g_bbW > 0 && rc.right > 0 && rc.bottom > 0 && (rc.right != g_bbW || rc.bottom != g_bbH)) {
        int x = (short)LOWORD(lp) * g_bbW / rc.right, y = (short)HIWORD(lp) * g_bbH / rc.bottom;
        imLp = MAKELPARAM(x, y);
      }
    }
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, imLp);
    if (IsInputMessage(msg)) {
      if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp);  // frees the raw input buffer
      if (msg == WM_SYSKEYDOWN && wp == VK_F4) return CallOrig(hwnd, msg, wp, lp);  // Alt+F4 still works
      return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
      SetCursor(nullptr);  // ImGui draws its own cursor
      return TRUE;
    }
  }
  return CallOrig(hwnd, msg, wp, lp);
}

bool IsGameForeground(HWND hwnd) {
  HWND fg = GetForegroundWindow();
  return fg == hwnd || GetAncestor(fg, GA_ROOT) == GetAncestor(hwnd, GA_ROOT);
}

bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

void InstallGlobalHooks() {
  if (g_cfg.hookCursor) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    void* scp = (void*)GetProcAddress(u32, "SetCursorPos");
    void* cc = (void*)GetProcAddress(u32, "ClipCursor");
    if (hooks::Create(scp, (void*)&hkSetCursorPos, (void**)&oSetCursorPos, "SetCursorPos"))
      hooks::Enable(scp, "SetCursorPos");
    if (hooks::Create(cc, (void*)&hkClipCursor, (void**)&oClipCursor, "ClipCursor"))
      hooks::Enable(cc, "ClipCursor");
  }
  if (g_cfg.hookXInput) {
    HMODULE xi = GetModuleHandleW(L"xinput1_3.dll");
    void* gs = xi ? (void*)GetProcAddress(xi, "XInputGetState") : nullptr;
    if (!gs) LOG("xinput: xinput1_3.dll not loaded, gamepad not muted in menu");
    else if (hooks::Create(gs, (void*)&hkXInputGetState, (void**)&oXInputGetState, "XInputGetState"))
      hooks::Enable(gs, "XInputGetState");
  }
}

void AttachWindow(HWND hwnd) {
  if (!g_cfg.hookWndProc) {
    LOG("input: WndProc hook disabled in config (menu will not get mouse/keyboard)");
    return;
  }
  g_unicodeWnd = IsWindowUnicode(hwnd) != FALSE;
  g_origWndProc = (WNDPROC)(g_unicodeWnd
                                ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc)
                                : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc));
  LOG("input: WndProc hooked (unicode=%d, orig=%p)", g_unicodeWnd, g_origWndProc);
}

void OnFrame(HWND hwnd) {
  bool fg = IsGameForeground(hwnd);

  if (!hotkeys::Capturing()) {
    static bool prev = false;
    bool down = KeyDown(g_cfg.menuKey);
    if (down && !prev && fg) {
      state::menuOpen = !state::menuOpen;
      LOG("menu %s", state::menuOpen ? "opened" : "closed");
      if (state::menuOpen && oClipCursor) oClipCursor(nullptr);
    }
    prev = down;
  }
  hotkeys::Poll(fg);

  ImGuiIO& io = ImGui::GetIO();
  io.MouseDrawCursor = state::menuOpen && fg;
  if (state::menuOpen)
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
  else
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
}

void SetBackBufferSize(int w, int h) {
  g_bbW = w;
  g_bbH = h;
}

bool RawPadState(XINPUT_STATE* st) {
  XInputGetState_t fn = oXInputGetState;
  if (!fn) {
    if (!g_ownXInput) {
      static bool tried = false;
      if (tried) return false;
      tried = true;
      for (const wchar_t* dll : {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
        HMODULE m = GetModuleHandleW(dll);
        if (!m) m = LoadLibraryW(dll);
        if (m && (g_ownXInput = (XInputGetState_t)GetProcAddress(m, "XInputGetState"))) break;
      }
    }
    fn = g_ownXInput;
  }
  return fn && fn(0, st) == ERROR_SUCCESS;
}

const char* KeyName(int vk) {
  static char buf[64];
  if (vk <= 0) return "none";
  UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  switch (vk) {  // extended keys need the extended bit for GetKeyNameText
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
      sc |= 0x100;
      break;
  }
  switch (vk) {
    case VK_MBUTTON: return "Mouse3";
    case VK_XBUTTON1: return "Mouse4";
    case VK_XBUTTON2: return "Mouse5";
    // punctuation keys by their US symbol: the layout name ("Х" on a Russian layout) is not what most guides say
    case VK_OEM_4: return "[";
    case VK_OEM_6: return "]";
    case VK_OEM_1: return ";";
    case VK_OEM_7: return "'";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_5: return "\\";
    case VK_OEM_3: return "`";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
  }
  wchar_t wbuf[64];
  if (GetKeyNameTextW((LONG)(sc << 16), wbuf, 64) > 0) {  // UTF-8 for the menu font (ANSI names showed "?")
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
  }
  snprintf(buf, sizeof buf, "VK 0x%02X", vk);
  return buf;
}

}  // namespace input
