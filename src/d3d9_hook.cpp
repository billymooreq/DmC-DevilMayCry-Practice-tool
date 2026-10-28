#include "d3d9_hook.h"

#include <windows.h>
#include <d3d9.h>

#include "config.h"
#include "hooks.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "input.h"
#include "log.h"
#include "menu.h"
#include "ue3/engine.h"
#include "util.h"
#include "visuals.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace d3d9hook {
namespace {

using Present_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetDepth_t = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SetVsConst_t = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using Clear_t = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetRT_t = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DrawIdx_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

Present_t oPresent = nullptr;
EndScene_t oEndScene = nullptr;
Reset_t oReset = nullptr;
SetDepth_t oSetDepth = nullptr;
SetVsConst_t oSetVsConst = nullptr;
Clear_t oClear = nullptr;
SetRT_t oSetRT = nullptr;
DrawIdx_t oDrawIdx = nullptr;
// Dev: one frame of render-target / depth / clear / draw events in the log (autorun "d3d_trace")
std::atomic<int> g_traceReq{0};
bool g_tracing = false;
int g_traceDraws = 0;
void TraceFlushDraws() {
  if (g_tracing && g_traceDraws) LOG("d3d9 trace:   %d indexed draws", g_traceDraws);
  g_traceDraws = 0;
}
std::string SurfText(IDirect3DSurface9* s) {
  if (!s) return "null";
  D3DSURFACE_DESC d{};
  s->GetDesc(&d);
  char b[96];
  snprintf(b, sizeof b, "%p %ux%u fmt %d ms %d", (void*)s, d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);
  return b;
}

// ---- Visuals: triggers and walls hidden behind the level. The game draws the scene into its own depth buffer and
// clears that buffer again before the post-process and the HUD (d3d_trace: a z-clear after ~500 draws into it), so
// at Present it holds nothing to test against. These lines are drawn into a layer of our own right before that
// clear - with the scene's depth bound and the view-projection the scene was drawn with - and the layer is laid over
// the back buffer at Present, under the menu. The scene depth: every depth surface the game binds during a frame is
// noted; at Present the one bound most often that can sit next to the back buffer (at least its size, same
// multisampling, not a square shadow map) is taken.
struct DepthSeen {
  IDirect3DSurface9* s;
  D3DSURFACE_DESC d;
  int sets;
};
std::vector<DepthSeen> g_depthFrame;     // render thread
IDirect3DSurface9* g_sceneDepth = nullptr;  // chosen at the last Present (not AddRef'd: the game owns it)
bool g_inOwnDraw = false;                  // our own calls are not counted / traced
IDirect3DSurface9* g_curDepth = nullptr;   // bound by the game now
const int kSceneDraws = 50;                // draws into the scene depth before a clear that ends the scene
int g_sceneDraws = 0;                      // indexed draws into the scene depth since its last z-clear
bool g_layerDone = false;                  // the layer was drawn in this frame
bool g_layerShown = false;                 // ... and is waiting to be laid over the frame
D3DVIEWPORT9 g_layerVp{};                  // the game's viewport then (the scene's part of the buffers)
IDirect3DTexture9* g_layerTex = nullptr;
IDirect3DSurface9* g_layerSurf = nullptr;
UINT g_layerW = 0, g_layerH = 0;
bool g_layerFailed = false;
IDirect3DStateBlock9* g_stateSb = nullptr;  // the game's state around our layer drawing
visuals::Occluded g_occ;                   // lines of the last visuals::Draw(), drawn in the next frame
// One occlusion query per trigger (group of lines): its label is shown only while some of it is visible.
struct GroupQuery {
  IDirect3DQuery9* q;
  uint32_t group;
  bool busy;
};
std::vector<GroupQuery> g_queries;
bool g_queriesFailed = false;
// The view-projection: 4x4 blocks of vertex shader constants the game set while drawing into the scene depth (start
// registers 0..15). Once, at Present, the block that maps a point in front of the camera to the middle of the screen
// is found (register + layout); from then on each block set to that register during a frame that looks like a
// perspective (its w row is a unit direction: w = distance ahead) is kept for that frame's layer.
float g_vsBlock[16][16];
bool g_vsHave[16];
int g_vpReg = -2;   // found register (-1 / -2 = not yet)
int g_vpMode = 0;   // bit 0: positions relative to the camera, bit 1: the block holds the matrix transposed
float g_vpFrame[16];  // rows: clip_k = dot(row k, float4(p, 1)), set in this frame
bool g_vpFrameOk = false;
// Camera origin of a camera-relative view-projection: UE3 keeps it in c4 (ViewOrigin) / c5 (PreViewTranslation =
// -origin); which one holds it is checked against the game's camera, else the camera the lines came with is used.
float g_vsOrigin[2][4];
bool g_vsOriginHave[2];
int g_originReg = 0;  // 4 / 5 found, -1 none, 0 not checked yet

IDirect3DDevice9* g_device = nullptr;  // device ImGui is bound to
HWND g_hwnd = nullptr;
bool g_imgui = false;
// Where the menu/overlay is drawn (hooks.render): "vtable" (default) = in a hook on the game device's vtable
// Present slot. The game calls Present through the vtable, so this runs before every inline detour of
// d3d9!Present - OBS game capture, the Steam overlay, our own MinHook detour - whatever order they were
// installed in: OBS copies the back buffer inside its detour, our layer is already there. "present" = in our
// inline detour (older behaviour: an inline hook installed after ours, e.g. OBS hooking a running game, runs
// first and captures the frame without the overlay). "endscene" = in the first EndScene after Present.
enum class Mode { Vtable, Present, EndScene } g_mode = Mode::Vtable;
bool g_useEndScene = false;
Present_t g_vtOrig = nullptr;       // previous vtable entry (d3d9 code with its inline detours)
void** g_vtSlot = nullptr;
bool g_inVtPresent = false;         // render thread only
unsigned g_vtCalls = 0;
bool g_inOwnScene = false;
unsigned g_presentCount = 0;
unsigned g_lastDrawnPresent = ~0u;

bool InitImGui(IDirect3DDevice9* dev) {
  D3DDEVICE_CREATION_PARAMETERS cp{};
  dev->GetCreationParameters(&cp);
  HWND hwnd = cp.hFocusWindow;
  if (!hwnd) {
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
      D3DPRESENT_PARAMETERS pp{};
      sc->GetPresentParameters(&pp);
      hwnd = pp.hDeviceWindow;
      sc->Release();
    }
  }
  if (!hwnd) {
    LOG("imgui: no window for device %p, skipping", dev);
    return false;
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // window layout is not persisted (nothing written next to the game)
  io.LogFilename = nullptr;
  menu::SetupFonts();
  if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX9_Init(dev)) {
    LOG("imgui: backend init failed");
    return false;
  }
  g_device = dev;
  g_hwnd = hwnd;
  input::AttachWindow(hwnd);
  LOG("imgui: initialized on device %p, hwnd %p", dev, hwnd);
  return true;
}

// A block of the view-projection register, as rows (see g_vpMode).
void BlockRows(const float* raw, int mode, float* m) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) m[i * 4 + j] = (mode & 2) ? raw[j * 4 + i] : raw[i * 4 + j];
}

HRESULT WINAPI hkSetVsConst(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
  if (dev == g_device && !g_inOwnDraw && data && g_curDepth && g_curDepth == g_sceneDepth) {
    if (count >= 4 && start < 16) {
      memcpy(g_vsBlock[start], data, sizeof g_vsBlock[start]);
      g_vsHave[start] = true;
      if ((int)start == g_vpReg) {
        float m[16];
        BlockRows(data, g_vpMode, m);
        float l = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
        if (l > 0.9f && l < 1.1f) {
          memcpy(g_vpFrame, m, sizeof g_vpFrame);
          g_vpFrameOk = true;
        }
      }
    }
    for (UINT r = 4; r <= 5; ++r)
      if (start <= r && r < start + count) {
        memcpy(g_vsOrigin[r - 4], data + (r - start) * 4, sizeof g_vsOrigin[0]);
        g_vsOriginHave[r - 4] = true;
      }
  }
  return oSetVsConst(dev, start, data, count);
}

// Once: the block that maps a point 10 m in front of the camera to the middle of the screen at w = 10 m is the
// view-projection (absolute positions, or relative to the camera when the game uses a pre-view translation).
void PickViewProj() {
  if (g_vpReg >= 0) {
    // camera origin of a camera-relative projection, checked once against the game's camera
    game::Pov cam;
    if ((g_vpMode & 1) && g_originReg == 0 && g_vsOriginHave[0] && g_vsOriginHave[1] && visuals::LastCamera(cam)) {
      const float* c4 = g_vsOrigin[0];
      const float* c5 = g_vsOrigin[1];
      auto near3 = [&](float x, float y, float z) {
        return std::fabs(x - cam.loc.x) < 300 && std::fabs(y - cam.loc.y) < 300 && std::fabs(z - cam.loc.z) < 300;
      };
      g_originReg = near3(c4[0], c4[1], c4[2]) ? 4 : near3(-c5[0], -c5[1], -c5[2]) ? 5 : -1;
      LOG("d3d9: camera (%.0f %.0f %.0f), c4 (%.0f %.0f %.0f %.2f), c5 (%.0f %.0f %.0f %.2f) -> camera origin %s",
          cam.loc.x, cam.loc.y, cam.loc.z, c4[0], c4[1], c4[2], c4[3], c5[0], c5[1], c5[2], c5[3],
          g_originReg == 4 ? "c4" : g_originReg == 5 ? "-c5" : "not in the constants (the game camera is used)");
    }
    memset(g_vsHave, 0, sizeof g_vsHave);
    return;
  }
  game::Pov cam;
  int found = -1;
  bool rel = false;
  float rows[16] = {};
  if (visuals::LastCamera(cam)) {
    const float k = 6.2831853f / 65536.0f;
    float p = cam.rot[0] * k, y = cam.rot[1] * k;
    float fwd[3] = {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    for (int r = 0; r < 16 && found < 0; ++r) {
      if (!g_vsHave[r]) continue;
      // mode bit 0: camera-relative positions; bit 1: the block holds the matrix transposed
      for (int mode = 0; mode < 4 && found < 0; ++mode) {
        float pt[3] = {fwd[0] * 1000, fwd[1] * 1000, fwd[2] * 1000};
        if (!(mode & 1)) pt[0] += cam.loc.x, pt[1] += cam.loc.y, pt[2] += cam.loc.z;
        float m[16];
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j) m[i * 4 + j] = (mode & 2) ? g_vsBlock[r][j * 4 + i] : g_vsBlock[r][i * 4 + j];
        float c[4];
        for (int i = 0; i < 4; ++i) c[i] = pt[0] * m[i * 4] + pt[1] * m[i * 4 + 1] + pt[2] * m[i * 4 + 2] + m[i * 4 + 3];
        if (c[3] > 850 && c[3] < 1150 && std::fabs(c[0] / c[3]) < 0.15f && std::fabs(c[1] / c[3]) < 0.15f &&
            c[2] > 0 && c[2] < c[3]) {
          found = r;
          rel = (mode & 1) != 0;
          g_vpMode = mode;
          memcpy(rows, m, sizeof rows);
          LOG("d3d9: scene view-projection in vertex shader constants c%d..c%d (%s%s), depth of a point 10 m ahead "
              "%.6f -> near plane %.2f", r, r + 3, rel ? "camera-relative" : "absolute", (mode & 2) ? ", transposed" : "",
              c[2] / c[3], (1.0f - c[2] / c[3]) * c[3]);
        }
      }
    }
  }
  if (found >= 0) g_vpReg = found;
  memset(g_vsHave, 0, sizeof g_vsHave);
}

// ---- the layer
void ReleaseLayer() {
  if (g_layerSurf) g_layerSurf->Release();
  if (g_layerTex) g_layerTex->Release();
  if (g_stateSb) g_stateSb->Release();
  for (GroupQuery& q : g_queries) q.q->Release();
  g_layerSurf = nullptr;
  g_layerTex = nullptr;
  g_stateSb = nullptr;
  g_queries.clear();
  g_layerW = g_layerH = 0;
  g_layerShown = false;
}

bool EnsureLayer(IDirect3DDevice9* dev, UINT w, UINT h) {
  if (g_layerTex && g_layerW == w && g_layerH == h) return true;
  ReleaseLayer();
  if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_layerTex, nullptr)) ||
      !g_layerTex || FAILED(g_layerTex->GetSurfaceLevel(0, &g_layerSurf))) {
    LOG("d3d9: could not create the %ux%u layer for lines behind walls: they show through everything", w, h);
    g_layerFailed = true;  // until the next Reset
    ReleaseLayer();
    return false;
  }
  g_layerW = w, g_layerH = h;
  LOG("d3d9: %ux%u layer for lines behind walls created", w, h);
  return true;
}

// A free occlusion query (null: none, the label then stays shown).
IDirect3DQuery9* AcquireQuery(IDirect3DDevice9* dev, uint32_t group) {
  for (GroupQuery& q : g_queries)
    if (!q.busy) {
      q.busy = true, q.group = group;
      return q.q;
    }
  if (g_queriesFailed || g_queries.size() >= 512) return nullptr;
  IDirect3DQuery9* q = nullptr;
  if (FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q)) || !q) {
    g_queriesFailed = true;
    LOG("d3d9: no occlusion queries: labels of triggers behind walls stay shown");
    return nullptr;
  }
  g_queries.push_back({q, group, true});
  return q;
}

// Results of earlier frames' queries (whichever are ready; never waits).
void PollQueries() {
  for (GroupQuery& q : g_queries) {
    if (!q.busy) continue;
    DWORD px = 0;
    HRESULT hr = q.q->GetData(&px, sizeof px, 0);
    if (hr == S_OK) visuals::SetGroupVisible(q.group, px > 0);
    if (hr != S_FALSE) q.busy = false;
  }
}

struct LayerVtx {
  float x, y, z, rhw;
  DWORD c;
};

// The lines of g_occ into the layer, depth-tested against `depth` (the scene's, still holding the scene), projected
// with this frame's view-projection into the game's viewport `gvp`.
void DrawLayer(IDirect3DDevice9* dev, IDirect3DSurface9* depth, const D3DVIEWPORT9& gvp) {
  g_layerDone = true;
  g_layerShown = false;
  if ((g_occ.lines.empty() && !g_occ.planeOn) || !g_vpFrameOk || !depth) return;
  D3DSURFACE_DESC dd{};
  depth->GetDesc(&dd);
  if (!EnsureLayer(dev, dd.Width, dd.Height)) return;
  if (!g_stateSb && (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_stateSb)) || !g_stateSb)) {
    g_stateSb = nullptr;
    return;
  }
  // camera origin of a camera-relative projection
  float org[3] = {0, 0, 0};
  if (g_vpMode & 1) {
    if (g_originReg == 4 && g_vsOriginHave[0]) memcpy(org, g_vsOrigin[0], sizeof org);
    else if (g_originReg == 5 && g_vsOriginHave[1])
      for (int k = 0; k < 3; ++k) org[k] = -g_vsOrigin[1][k];
    else memcpy(org, g_occ.cam, sizeof org);
  }
  const float* m = g_vpFrame;
  struct Clip {
    float x, y, z, w;
  };
  auto clipOf = [&](const float* p) {
    float q[3] = {p[0] - org[0], p[1] - org[1], p[2] - org[2]};
    Clip c;
    float* o = &c.x;
    for (int k = 0; k < 4; ++k) o[k] = q[0] * m[k * 4] + q[1] * m[k * 4 + 1] + q[2] * m[k * 4 + 2] + m[k * 4 + 3];
    return c;
  };
  float vx = (float)gvp.X, vy = (float)gvp.Y, vw = (float)gvp.Width, vh = (float)gvp.Height;
  float half = g_occ.thickness * std::max(1.0f, vh / 1080.0f) * 0.5f, margin = std::max(vw, vh) * 2.0f;
  std::vector<LayerVtx> tris;
  tris.reserve(std::min<size_t>(g_occ.lines.size(), 20000) * 6);
  // one line -> a quad of the line width; false: nothing of it on screen
  auto addLine = [&](const visuals::OccLine& l) {
    Clip a = clipOf(l.a), b = clipOf(l.b);
    // D3D keeps 0 <= z <= w: cut the segment at the near plane in clip space
    const float eps = 1e-4f;
    float da = a.z - eps * a.w, db = b.z - eps * b.w;
    if (da < 0 && db < 0) return;
    auto lerp = [](const Clip& p, const Clip& q, float t) {
      return Clip{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t};
    };
    if (da < 0) a = lerp(a, b, da / (da - db));
    else if (db < 0) b = lerp(b, a, db / (db - da));
    if (a.w <= 0 || b.w <= 0) return;
    float ax = vx + (a.x / a.w * 0.5f + 0.5f) * vw, ay = vy + (0.5f - a.y / a.w * 0.5f) * vh;
    float bx = vx + (b.x / b.w * 0.5f + 0.5f) * vw, by = vy + (0.5f - b.y / b.w * 0.5f) * vh;
    float az = a.z / a.w, bz = b.z / b.w, ar = 1.0f / a.w, br = 1.0f / b.w;
    if ((ax < vx && bx < vx) || (ax > vx + vw && bx > vx + vw) || (ay < vy && by < vy) || (ay > vy + vh && by > vy + vh))
      return;
    if (std::fabs(ax - vx) > margin || std::fabs(ay - vy) > margin || std::fabs(bx - vx) > margin ||
        std::fabs(by - vy) > margin) {
      // very long after projection (an endpoint just in front of the camera): cut to the screen neighbourhood
      float dx = bx - ax, dy = by - ay, t0 = 0, t1 = 1;
      auto clip = [&](float p, float q) {  // Liang-Barsky
        if (std::fabs(p) < 1e-6f) return q >= 0;
        float r = q / p;
        if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
        else { if (r < t0) return false; if (r < t1) t1 = r; }
        return true;
      };
      if (!(clip(-dx, ax - vx + margin) && clip(dx, vx + vw + margin - ax) && clip(-dy, ay - vy + margin) &&
            clip(dy, vy + vh + margin - ay)))
        return;
      // z / w and 1 / w are linear on the screen: cut them the same way
      float nbx = ax + dx * t1, nby = ay + dy * t1, nbz = az + (bz - az) * t1, nbr = ar + (br - ar) * t1;
      ax += dx * t0, ay += dy * t0, az += (bz - az) * t0, ar += (br - ar) * t0;
      bx = nbx, by = nby, bz = nbz, br = nbr;
    }
    float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * half, ny = dx / len * half;
    LayerVtx q[4] = {{ax + nx, ay + ny, az, ar, l.argb}, {ax - nx, ay - ny, az, ar, l.argb},
                     {bx + nx, by + ny, bz, br, l.argb}, {bx - nx, by - ny, bz, br, l.argb}};
    for (int k : {0, 1, 2, 2, 1, 3}) tris.push_back(q[k]);
  };

  g_stateSb->Capture();
  IDirect3DSurface9* oldRT[4] = {};
  IDirect3DSurface9* oldDepth = nullptr;
  for (DWORD i = 0; i < 4; ++i)
    if (FAILED(dev->GetRenderTarget(i, &oldRT[i]))) oldRT[i] = nullptr;
  dev->GetDepthStencilSurface(&oldDepth);
  g_inOwnDraw = true;
  dev->SetRenderTarget(0, g_layerSurf);
  for (DWORD i = 1; i < 4; ++i)
    if (oldRT[i]) dev->SetRenderTarget(i, nullptr);
  dev->SetDepthStencilSurface(depth);
  D3DVIEWPORT9 full{0, 0, g_layerW, g_layerH, 0.0f, 1.0f};
  dev->SetViewport(&full);
  dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
  dev->SetPixelShader(nullptr);
  dev->SetVertexShader(nullptr);
  dev->SetTexture(0, nullptr);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
  // premultiplied in the layer: colour * a over it, alpha accumulated; laid over the frame with ONE, INVSRCALPHA
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
  dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
  dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  auto flush = [&] {
    const size_t kMax = 20000 * 3;  // vertices per DrawPrimitiveUP call
    for (size_t i = 0; i < tris.size(); i += kMax)
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(std::min(kMax, tris.size() - i) / 3), &tris[i], sizeof(LayerVtx));
    tris.clear();
  };
  // lines of one group come one after another (one trigger); a group gets a query around its lines
  const std::vector<visuals::OccLine>& lines = g_occ.lines;
  for (size_t i = 0; i < lines.size();) {
    size_t j = i;
    while (j < lines.size() && lines[j].group == lines[i].group) ++j;
    IDirect3DQuery9* q = lines[i].group ? AcquireQuery(dev, lines[i].group) : nullptr;
    if (q) q->Issue(D3DISSUE_BEGIN);
    for (size_t k = i; k < j; ++k) addLine(lines[k]);
    flush();
    if (q) q->Issue(D3DISSUE_END);
    i = j;
  }
  if (g_occ.planeOn) {
    // Dev: a screen-wide plane at the depth of this point - it covers only what is farther than it
    Clip p = clipOf(g_occ.plane);
    if (p.w > 1 && p.z > 0) {
      float z = p.z / p.w, r = 1.0f / p.w;
      DWORD c = 0x80FF00FFu;
      LayerVtx q[4] = {{vx, vy, z, r, c}, {vx + vw, vy, z, r, c}, {vx, vy + vh, z, r, c}, {vx + vw, vy + vh, z, r, c}};
      for (int k : {0, 1, 2, 2, 1, 3}) tris.push_back(q[k]);
      flush();
    }
  }
  for (DWORD i = 0; i < 4; ++i)
    if (i == 0 || oldRT[i]) dev->SetRenderTarget(i, oldRT[i]);
  dev->SetDepthStencilSurface(oldDepth);
  g_inOwnDraw = false;
  for (IDirect3DSurface9* s : oldRT)
    if (s) s->Release();
  if (oldDepth) oldDepth->Release();
  g_stateSb->Apply();
  g_layerVp = gvp;
  g_layerShown = true;
}

// The layer over the back buffer (bound as render target 0 now), under the menu.
void CompositeLayer(IDirect3DDevice9* dev, UINT w, UINT h) {
  if (!g_layerShown || !g_layerTex || !g_stateSb) return;
  g_layerShown = false;
  g_stateSb->Capture();
  struct V {
    float x, y, z, rhw, u, v;
  };
  float u0 = (float)g_layerVp.X / g_layerW, v0 = (float)g_layerVp.Y / g_layerH;
  float u1 = (float)(g_layerVp.X + g_layerVp.Width) / g_layerW, v1 = (float)(g_layerVp.Y + g_layerVp.Height) / g_layerH;
  float x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
  V q[4] = {{-0.5f, -0.5f, 0, 1, u0, v0}, {x1, -0.5f, 0, 1, u1, v0}, {-0.5f, y1, 0, 1, u0, v1}, {x1, y1, 0, 1, u1, v1}};
  D3DVIEWPORT9 vp{0, 0, w, h, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  dev->SetPixelShader(nullptr);
  dev->SetVertexShader(nullptr);
  dev->SetTexture(0, g_layerTex);
  bool same = g_layerVp.Width == w && g_layerVp.Height == h;
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, same ? D3DTEXF_POINT : D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, same ? D3DTEXF_POINT : D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
  g_stateSb->Apply();
}

HRESULT WINAPI hkClear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD st) {
  if (dev == g_device && !g_inOwnDraw) {
    if (g_tracing) {
      TraceFlushDraws();
      LOG("d3d9 trace: Clear flags %lu (target %d z %d stencil %d) z=%g with depth %s", flags,
          (flags & D3DCLEAR_TARGET) != 0, (flags & D3DCLEAR_ZBUFFER) != 0, (flags & D3DCLEAR_STENCIL) != 0, z,
          SurfText(g_curDepth).c_str());
    }
    // the scene depth about to be cleared after the scene: the last moment it holds the scene
    if ((flags & D3DCLEAR_ZBUFFER) && g_curDepth && g_curDepth == g_sceneDepth) {
      if (!g_layerDone && g_sceneDraws >= kSceneDraws && (!g_occ.lines.empty() || g_occ.planeOn)) {
        D3DVIEWPORT9 vp{};
        dev->GetViewport(&vp);
        DrawLayer(dev, g_curDepth, vp);
        if (g_tracing)
          LOG("d3d9 trace:   lines behind walls drawn here (%zu lines, %d draws into the scene depth, viewport %lux%lu)",
              g_occ.lines.size(), g_sceneDraws, vp.Width, vp.Height);
      }
      g_sceneDraws = 0;
    }
  }
  return oClear(dev, n, r, flags, c, z, st);
}

HRESULT WINAPI hkSetRT(IDirect3DDevice9* dev, DWORD i, IDirect3DSurface9* s) {
  if (g_tracing && dev == g_device && !g_inOwnDraw && i == 0) {
    TraceFlushDraws();
    LOG("d3d9 trace: SetRenderTarget0 %s", SurfText(s).c_str());
  }
  return oSetRT(dev, i, s);
}

HRESULT WINAPI hkDrawIdx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc) {
  if (g_tracing) ++g_traceDraws;
  if (dev == g_device && !g_inOwnDraw && g_curDepth && g_curDepth == g_sceneDepth) ++g_sceneDraws;
  return oDrawIdx(dev, t, b, mi, nv, si, pc);
}

HRESULT WINAPI hkSetDepth(IDirect3DDevice9* dev, IDirect3DSurface9* surf) {
  if (g_tracing && dev == g_device && !g_inOwnDraw && surf != g_curDepth) {
    TraceFlushDraws();
    LOG("d3d9 trace: SetDepthStencilSurface %s", SurfText(surf).c_str());
  }
  if (!g_inOwnDraw && dev == g_device) g_curDepth = surf;
  if (surf && !g_inOwnDraw && dev == g_device) {
    auto it = std::find_if(g_depthFrame.begin(), g_depthFrame.end(), [&](const DepthSeen& x) { return x.s == surf; });
    if (it != g_depthFrame.end()) ++it->sets;
    else if (g_depthFrame.size() < 16) {
      DepthSeen d{surf, {}, 1};
      surf->GetDesc(&d.d);
      g_depthFrame.push_back(d);
    }
  }
  return oSetDepth(dev, surf);
}

void PickSceneDepth(const D3DSURFACE_DESC& bb) {
  const DepthSeen* best = nullptr;
  for (const DepthSeen& d : g_depthFrame) {
    if (d.d.Width < bb.Width || d.d.Height < bb.Height || d.d.MultiSampleType != bb.MultiSampleType ||
        d.d.Width == d.d.Height)
      continue;
    if (!best || d.sets > best->sets) best = &d;
  }
  IDirect3DSurface9* was = g_sceneDepth;
  g_sceneDepth = best ? best->s : nullptr;
  if (g_sceneDepth != was) {
    if (best)
      LOG("d3d9: scene depth %p %ux%u format %d (set %d times in the frame, %zu depth surfaces seen)", (void*)best->s,
          best->d.Width, best->d.Height, (int)best->d.Format, best->sets, g_depthFrame.size());
    else
      LOG("d3d9: no scene depth this frame (%zu depth surfaces seen)", g_depthFrame.size());
  }
  g_depthFrame.clear();
}

void RenderFrame(IDirect3DDevice9* dev, bool wrapScene) {
  if (!g_imgui) {
    static bool failed = false;
    if (failed) return;
    g_imgui = InitImGui(dev);
    if (!g_imgui) {
      failed = true;
      return;
    }
  }
  if (dev != g_device) return;
  if (dev->TestCooperativeLevel() != D3D_OK) return;  // lost device: wait for Reset

  // The back buffer can differ from the client area (windowed, scaled): ImGui works in back-buffer pixels.
  IDirect3DSurface9* bbDesc = nullptr;
  D3DSURFACE_DESC desc{};
  if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bbDesc)) && bbDesc) {
    bbDesc->GetDesc(&desc);
    bbDesc->Release();
  }
  input::SetBackBufferSize((int)desc.Width, (int)desc.Height);
  PickSceneDepth(desc);
  PickViewProj();
  PollQueries();
  visuals::SetOcclusionLive(g_sceneDepth && g_vpReg >= 0 && g_vpFrameOk && !g_layerFailed);
  input::OnFrame(g_hwnd);

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  if (desc.Width && desc.Height) ImGui::GetIO().DisplaySize = ImVec2((float)desc.Width, (float)desc.Height);
  ImGui::NewFrame();
  menu::Draw();
  ImGui::EndFrame();
  ImGui::Render();
  ImDrawData* dd = ImGui::GetDrawData();
  bool imgui = dd && dd->CmdListsCount > 0;
  // No clear ended the scene in this frame (or too few draws before it): the layer is drawn now, against what the
  // scene depth holds at Present.
  bool layerLate = !g_layerDone && (!g_occ.lines.empty() || g_occ.planeOn) && g_sceneDepth;
  if (!imgui && !layerLate && !g_layerShown) {  // nothing to draw: leave the frame untouched
    std::swap(g_occ, visuals::OccludedLines());
    return;
  }

  // Draw straight into the back buffer.
  IDirect3DSurface9* oldRT = nullptr;
  IDirect3DSurface9* bb = nullptr;
  dev->GetRenderTarget(0, &oldRT);
  dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  if (bb && bb != oldRT) dev->SetRenderTarget(0, bb);
  DWORD srgb = 0;
  dev->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

  if (wrapScene) {
    g_inOwnScene = true;
    dev->BeginScene();
  }
  if (layerLate) {
    D3DVIEWPORT9 vp{0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    DrawLayer(dev, g_sceneDepth, vp);
  }
  CompositeLayer(dev, desc.Width, desc.Height);  // under the ImGui layer
  if (imgui) ImGui_ImplDX9_RenderDrawData(dd);
  if (wrapScene) {
    dev->EndScene();
    g_inOwnScene = false;
  }

  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb);
  if (bb && bb != oldRT && oldRT) dev->SetRenderTarget(0, oldRT);
  if (bb) bb->Release();
  if (oldRT) oldRT->Release();
  // the lines visuals::Draw() just collected are drawn in the next frame
  std::swap(g_occ, visuals::OccludedLines());
}

// ---- screenshots (render thread writes, requester waits on the event)
std::mutex g_shotMu;
std::wstring g_shotPath;
HANDLE g_shotDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
bool g_shotOk = false;

bool SaveBackBuffer(IDirect3DDevice9* dev, const std::wstring& path) {
  IDirect3DSurface9* bb = nullptr;
  IDirect3DSurface9* rt = nullptr;
  IDirect3DSurface9* sys = nullptr;
  bool ok = false;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
    LOG("screenshot: unsupported back buffer format %d", (int)d.Format);
    bb->Release();
    return false;
  }
  // Resolve (MSAA-safe) into a plain render target, then copy to system memory.
  if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
      SUCCEEDED(dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
      SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
      SUCCEEDED(dev->GetRenderTargetData(rt, sys))) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
      FILE* f = _wfopen(path.c_str(), L"wb");
      if (f) {
        int rowBytes = (d.Width * 3 + 3) & ~3;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih;
        ih.biWidth = d.Width;
        ih.biHeight = d.Height;  // bottom-up
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + rowBytes * d.Height;
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        std::vector<uint8_t> row(rowBytes, 0);
        for (int y = (int)d.Height - 1; y >= 0; --y) {
          const uint8_t* src = (const uint8_t*)lr.pBits + y * lr.Pitch;
          for (UINT x = 0; x < d.Width; ++x) memcpy(&row[x * 3], src + x * 4, 3);
          fwrite(row.data(), rowBytes, 1, f);
        }
        fclose(f);
        ok = true;
      }
      sys->UnlockRect();
    }
  }
  if (sys) sys->Release();
  if (rt) rt->Release();
  bb->Release();
  return ok;
}

void TakePendingScreenshot(IDirect3DDevice9* dev) {
  std::wstring path;
  {
    std::lock_guard<std::mutex> lk(g_shotMu);
    path.swap(g_shotPath);
  }
  if (path.empty()) return;
  g_shotOk = SaveBackBuffer(dev, path);
  LOG("screenshot: %s %s", util::WideToUtf8(path).c_str(), g_shotOk ? "saved" : "FAILED");
  SetEvent(g_shotDone);
}

// One frame: count it (game-thread features tick once per frame), draw, serve a pending screenshot.
void OnPresent(IDirect3DDevice9* dev, bool draw) {
  ++g_presentCount;
  if (g_tracing) {
    TraceFlushDraws();
    LOG("d3d9 trace: Present (end of the traced frame)");
    g_tracing = false;
  }
  if (g_traceReq.exchange(0)) {
    g_tracing = true;
    LOG("d3d9 trace: frame start");
  }
  engine::NotifyFrame();
  if (draw) RenderFrame(dev, true);
  if (dev == g_device) {
    TakePendingScreenshot(dev);
    // a new frame: the scene, its view-projection and the layer start over
    g_layerDone = false;
    g_layerShown = false;
    g_sceneDraws = 0;
    g_vpFrameOk = false;
    g_vsOriginHave[0] = g_vsOriginHave[1] = false;
  }
}

HRESULT WINAPI vtPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
  if (++g_vtCalls == 1) LOG("d3d9: drawing from the device vtable Present (before inline Present hooks)");
  OnPresent(dev, true);
  g_inVtPresent = true;
  HRESULT hr = g_vtOrig(dev, src, dst, wnd, dirty);
  g_inVtPresent = false;
  return hr;
}

void InstallVtableHook(IDirect3DDevice9* dev) {
  void** vt = *reinterpret_cast<void***>(dev);
  void** slot = &vt[17];
  if (*slot == (void*)&vtPresent) return;
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
    LOG("d3d9: VirtualProtect on the vtable failed (%lu), drawing stays in the inline Present hook", GetLastError());
    g_mode = Mode::Present;
    return;
  }
  g_vtOrig = reinterpret_cast<Present_t>(*slot);
  *slot = (void*)&vtPresent;
  VirtualProtect(slot, sizeof(void*), old, &old);
  g_vtSlot = slot;
  LOG("d3d9: vtable Present hook on device %p (slot %p, previous %p)", dev, slot, (void*)g_vtOrig);
}

HRESULT WINAPI hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                         const RGNDATA* dirty) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    LOG("d3d9: first Present on device %p", dev);
  }
  // Reached through the vtable hook: the frame is drawn already. Otherwise (other modes, a device with another
  // vtable, or someone replaced the vtable entry) this detour draws, as before.
  if (!g_inVtPresent) {
    OnPresent(dev, !g_useEndScene);
    if (g_mode == Mode::Vtable && g_imgui && dev == g_device) InstallVtableHook(dev);
  }
  return oPresent(dev, src, dst, wnd, dirty);
}

HRESULT WINAPI hkEndScene(IDirect3DDevice9* dev) {
  // EndScene can run several times per frame: draw once per Present.
  if (g_useEndScene && !g_inOwnScene && g_lastDrawnPresent != g_presentCount) {
    g_lastDrawnPresent = g_presentCount;
    RenderFrame(dev, false);
  }
  return oEndScene(dev);
}

HRESULT WINAPI hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  bool ours = g_imgui && dev == g_device;
  LOG("d3d9: Reset (%ux%u windowed=%d)", pp ? pp->BackBufferWidth : 0,
      pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
  if (ours) ImGui_ImplDX9_InvalidateDeviceObjects();
  g_sceneDepth = nullptr;  // the game recreates its buffers
  g_curDepth = nullptr;
  g_depthFrame.clear();
  ReleaseLayer();          // default-pool resources must go before Reset
  g_layerFailed = false;
  g_queriesFailed = false;
  visuals::SetOcclusionLive(false);
  HRESULT hr = oReset(dev, pp);
  if (ours && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
  LOG("d3d9: Reset -> 0x%08X", (unsigned)hr);
  return hr;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  return DefWindowProcW(h, m, w, l);
}

// Creates a throwaway device just to read the IDirect3DDevice9 vtable.
bool GetDeviceVtable(void** out, size_t count) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DummyWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DmCPT_Dummy";
  RegisterClassExW(&wc);
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                             nullptr, wc.hInstance, nullptr);
  if (!wnd) return false;

  bool ok = false;
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (d3d) {
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev = nullptr;
    const D3DDEVTYPE types[] = {D3DDEVTYPE_HAL, D3DDEVTYPE_NULLREF};
    for (D3DDEVTYPE t : types) {
      HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, t, wnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING |
                                         D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                     &pp, &dev);
      LOG("d3d9: dummy device type %d -> 0x%08X", (int)t, (unsigned)hr);
      if (SUCCEEDED(hr) && dev) break;
      dev = nullptr;
    }
    if (dev) {
      memcpy(out, *reinterpret_cast<void***>(dev), count * sizeof(void*));
      dev->Release();
      ok = true;
    }
    d3d->Release();
  }
  DestroyWindow(wnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return ok;
}

}  // namespace

bool Screenshot(const std::wstring& path, int timeoutMs) {
  if (!g_imgui) return false;
  ResetEvent(g_shotDone);
  {
    std::lock_guard<std::mutex> lk(g_shotMu);
    g_shotPath = path;
  }
  return WaitForSingleObject(g_shotDone, timeoutMs) == WAIT_OBJECT_0 && g_shotOk;
}

void Install() {
  void* vt[119] = {};
  bool ok = false;
  for (int attempt = 0; attempt < 120 && !ok; ++attempt) {
    ok = GetDeviceVtable(vt, 119);
    if (!ok) Sleep(500);
  }
  if (!ok) {
    LOG("d3d9: could not create dummy device, no menu");
    return;
  }
  g_mode = g_cfg.renderHook == "endscene" ? Mode::EndScene
           : g_cfg.renderHook == "present"  ? Mode::Present
                                            : Mode::Vtable;
  g_useEndScene = g_mode == Mode::EndScene;
  LOG("d3d9: vtable Reset=%p Present=%p EndScene=%p, drawing in %s", vt[16], vt[17], vt[42],
      g_mode == Mode::EndScene ? "EndScene" : g_mode == Mode::Present ? "Present (inline)" : "Present (vtable)");
  if (hooks::Create(vt[16], (void*)&hkReset, (void**)&oReset, "D3D9.Reset")) hooks::Enable(vt[16], "D3D9.Reset");
  if (hooks::Create(vt[17], (void*)&hkPresent, (void**)&oPresent, "D3D9.Present"))
    hooks::Enable(vt[17], "D3D9.Present");
  if (hooks::Create(vt[39], (void*)&hkSetDepth, (void**)&oSetDepth, "D3D9.SetDepthStencilSurface"))
    hooks::Enable(vt[39], "D3D9.SetDepthStencilSurface");
  if (hooks::Create(vt[43], (void*)&hkClear, (void**)&oClear, "D3D9.Clear")) hooks::Enable(vt[43], "D3D9.Clear");
  if (hooks::Create(vt[37], (void*)&hkSetRT, (void**)&oSetRT, "D3D9.SetRenderTarget")) hooks::Enable(vt[37], "D3D9.SetRenderTarget");
  if (hooks::Create(vt[82], (void*)&hkDrawIdx, (void**)&oDrawIdx, "D3D9.DrawIndexedPrimitive"))
    hooks::Enable(vt[82], "D3D9.DrawIndexedPrimitive");
  if (hooks::Create(vt[94], (void*)&hkSetVsConst, (void**)&oSetVsConst, "D3D9.SetVertexShaderConstantF"))
    hooks::Enable(vt[94], "D3D9.SetVertexShaderConstantF");
  if (g_useEndScene && hooks::Create(vt[42], (void*)&hkEndScene, (void**)&oEndScene, "D3D9.EndScene"))
    hooks::Enable(vt[42], "D3D9.EndScene");
}

void RequestTrace() { g_traceReq = 1; }

}  // namespace d3d9hook
