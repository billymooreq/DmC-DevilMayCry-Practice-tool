// Offline test without the game: a D3D9 window that loads our dinput8.dll, checks the DirectInput
// forwarding, presents frames, does a Reset (resize) and saves a screenshot of the back buffer.
// Usage: test_host.exe [path\to\dinput8.dll]   (results in test_result.txt + test_shot.bmp)
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d9.h>
#include <dinput.h>

#include <cstdio>
#include <string>

static FILE* g_out = nullptr;
static int g_fail = 0;

static void Check(bool ok, const char* what) {
  fprintf(g_out, "[%s] %s\n", ok ? " OK " : "FAIL", what);
  fflush(g_out);
  if (!ok) ++g_fail;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProcW(h, m, w, l);
}

static bool SaveBackBuffer(IDirect3DDevice9* dev, const wchar_t* path) {
  IDirect3DSurface9* bb = nullptr;
  IDirect3DSurface9* sys = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  bool ok = false;
  if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
      SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
      FILE* f = _wfopen(path, L"wb");
      if (f) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih;
        ih.biWidth = d.Width;
        ih.biHeight = -(LONG)d.Height;  // top-down
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        DWORD img = d.Width * d.Height * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + img;
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        for (UINT y = 0; y < d.Height; ++y) fwrite((BYTE*)lr.pBits + y * lr.Pitch, d.Width * 4, 1, f);
        fclose(f);
        ok = true;
      }
      sys->UnlockRect();
    }
  }
  if (sys) sys->Release();
  bb->Release();
  return ok;
}

int wmain(int argc, wchar_t** argv) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
  g_out = _wfopen((dir + L"test_result.txt").c_str(), L"w");
  std::wstring dll = argc > 1 ? argv[1] : dir + L"dinput8.dll";

  // Config for the test: overlay forced on and menu open, so the screenshot shows both.
  FILE* cfg = _wfopen((dir + L"DmCPracticeTool.json").c_str(), L"w");
  fputs("{\"debug\":{\"force_overlay\":true,\"menu_open_at_start\":true}}\n", cfg);
  fclose(cfg);

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DmCPT_TestHost";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DmCPT test host", WS_OVERLAPPEDWINDOW, 50, 50, 1280, 720,
                              nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOW);

  SetEnvironmentVariableW(L"DMCPT_TEST_SCAN", L"1");  // scanner must fail gracefully here
  HMODULE mod = LoadLibraryW(dll.c_str());
  Check(mod != nullptr, "LoadLibrary(our dinput8.dll)");
  if (!mod) return 1;
  Sleep(2000);  // init thread: config, hooks, dummy device

  // DirectInput forwarding
  using DI8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  auto di8 = (DI8Create)GetProcAddress(mod, "DirectInput8Create");
  Check(di8 != nullptr, "export DirectInput8Create");
  Check(GetProcAddress(mod, "GetdfDIJoystick") != nullptr, "export GetdfDIJoystick");
  IDirectInput8W* di = nullptr;
  HRESULT hr = di8 ? di8(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr)
                   : E_FAIL;
  Check(SUCCEEDED(hr) && di, "DirectInput8Create forwarded to system dinput8");
  if (di) {
    IDirectInputDevice8W* kb = nullptr;
    hr = di->CreateDevice(GUID_SysKeyboard, &kb, nullptr);
    Check(SUCCEEDED(hr) && kb, "CreateDevice(keyboard) through hooked vtable");
    if (kb) {
      // c_dfDIKeyboard lives in dinput8.lib, which we cannot link (name clash) - build it by hand.
      static DIOBJECTDATAFORMAT objs[256];
      for (int i = 0; i < 256; ++i)
        objs[i] = {&GUID_Key, (DWORD)i, DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | 0x80000000 /* optional */, 0};
      DIDATAFORMAT fmt{sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS, 256, 256, objs};
      kb->SetDataFormat(&fmt);
      kb->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
      kb->Acquire();
      BYTE keys[256];
      hr = kb->GetDeviceState(256, keys);
      Check(SUCCEEDED(hr), "GetDeviceState(keyboard) through hook");
      kb->Release();
    }
    di->Release();
  }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_COPY;  // back buffer keeps the overlay after Present -> screenshot
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 1264;
  pp.BackBufferHeight = 681;
  pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9* dev = nullptr;
  hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  Check(SUCCEEDED(hr) && dev, "game-like device created");
  if (!dev) return 1;

  bool shot = false;
  for (int frame = 0; frame < 240; ++frame) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (frame == 90) {  // like a resolution change / Alt+Tab recovery; DMCPT_TEST_RES=WxH picks the size
      pp.BackBufferWidth = 1024;
      pp.BackBufferHeight = 576;
      char res[32] = {};
      if (GetEnvironmentVariableA("DMCPT_TEST_RES", res, sizeof res)) {
        unsigned w = 0, h = 0;
        if (sscanf_s(res, "%ux%u", &w, &h) == 2 && w && h) {
          pp.BackBufferWidth = w;
          pp.BackBufferHeight = h;
        }
      }
      hr = dev->Reset(&pp);
      Check(SUCCEEDED(hr), "Reset with ImGui alive");
    }
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(30, 60, 90), 1.0f, 0);
    dev->BeginScene();
    dev->EndScene();
    hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
    if (frame == 0) Check(SUCCEEDED(hr), "Present through hook");
    if (frame == 200) {
      wchar_t name[128] = L"test_shot.bmp";
      GetEnvironmentVariableW(L"DMCPT_TEST_SHOT", name, 128);
      shot = SaveBackBuffer(dev, (dir + name).c_str());
    }
  }
  Check(shot, "screenshot saved (test_shot.bmp)");

  dev->Release();
  d3d->Release();
  DestroyWindow(hwnd);
  fprintf(g_out, "%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  fclose(g_out);
  ExitProcess(g_fail ? 1 : 0);  // skip DLL unload ordering issues of a test process
}
