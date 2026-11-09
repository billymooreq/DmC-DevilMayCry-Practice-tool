// The v0.11 look: fonts built into the DLL, the menu scale and the row widgets. Rules: ui_guidelines.md.
#include "ui.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "d3d9_hook.h"
#include "imgui_internal.h"
#include "log.h"

namespace ui {
namespace {

// ------------------------------------------------------------------ fonts
// RCDATA ids (resources.rc)
constexpr int kResMono = 102, kResMonoBold = 103, kResTitle = 104, kResHeavy = 105, kResRazium = 106;

struct Blob {
  void* data = nullptr;
  int size = 0;
};

Blob Resource(int id) {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&Resource), &self);
  HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!r) return {};
  HGLOBAL h = LoadResource(self, r);
  void* p = h ? LockResource(h) : nullptr;
  return p ? Blob{p, (int)SizeofResource(self, r)} : Blob{};
}

struct Loaded {
  F f;
  int px;
  ImFont* font;
};
std::vector<Loaded> g_fonts;
std::vector<std::pair<F, int>> g_built;  // what g_fonts was built for
ImFont* g_overlay = nullptr;
float g_screen = 1, g_menu = 1;
constexpr float kOverlayFontPx = 64.0f;  // rasterised large, drawn smaller: crisp up to 4K

void AddWant(std::vector<std::pair<F, int>>& v, F f, float px) {
  int p = std::max(6, (int)std::lround(px));
  for (const auto& w : v)
    if (w.first == f && w.second == p) return;
  v.push_back({f, p});
}

// Every size the menu (menu scale), the damage log and the health numbers (screen scale) draw with.
std::vector<std::pair<F, int>> Desired() {
  std::vector<std::pair<F, int>> v;
  for (float css : {13.0f, 12.0f, 11.0f, 10.0f}) AddWant(v, F::Mono, css * g_menu);
  for (float css : {13.0f, 11.0f}) AddWant(v, F::MonoBold, css * g_menu);
  AddWant(v, F::Title, 13 * g_menu);
  for (float css : {13.0f, 11.0f}) AddWant(v, F::Mono, css * g_screen * kBoost);   // damage log
  for (float css : {13.0f, 11.0f}) AddWant(v, F::MonoBold, css * g_screen * kBoost);
  AddWant(v, F::Title, 13 * g_screen * kBoost);
  AddWant(v, F::Heavy, 22 * g_screen);                                     // enemy health
  AddWant(v, F::Heavy, 15 * g_screen);
  std::sort(v.begin(), v.end());
  return v;
}

ImFont* TryWindowsFont(const char* file, float px) {
  char dir[MAX_PATH];
  GetWindowsDirectoryA(dir, MAX_PATH);
  std::string path = std::string(dir) + "\\Fonts\\" + file;
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0};
  return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges);
}

void Build() {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  g_fonts.clear();
  g_built = Desired();
  // Latin-1 (× · ‹ ›), Cyrillic (save names and notes), punctuation (– — … ‹ ›), arrows
  static const ImWchar kMonoRanges[] = {0x0020, 0x00FF, 0x0400, 0x04FF, 0x2010, 0x205E, 0x2190, 0x2193, 0};
  static const ImWchar kTitleRanges[] = {0x0020, 0x00FF, 0x2010, 0x205E, 0};
  for (const auto& [f, px] : g_built) {
    int res = f == F::Mono ? kResMono : f == F::MonoBold ? kResMonoBold : f == F::Title ? kResTitle : kResHeavy;
    Blob b = Resource(res);
    if (!b.data) continue;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // the DLL's own resource
    cfg.PixelSnapH = f == F::Mono || f == F::MonoBold;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(b.data, b.size, (float)px, &cfg,
                                                  f == F::Mono || f == F::MonoBold ? kMonoRanges : kTitleRanges);
    if (font) g_fonts.push_back({f, px, font});
  }
  if (g_fonts.empty()) g_fonts.push_back({F::Mono, 13, io.Fonts->AddFontDefault()});
  g_overlay = TryWindowsFont("segoeuib.ttf", kOverlayFontPx);
  io.FontDefault = Get(F::Mono, 13 * g_menu);
}

// ------------------------------------------------------------------ picture
ImTextureID g_razium = 0;
float g_raziumW = 0, g_raziumH = 0;
bool g_raziumTried = false;

bool DecodePng(const Blob& b, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  IWICImagingFactory* fac = nullptr;
  IWICStream* st = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  IWICFormatConverter* cv = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
      SUCCEEDED(fac->CreateStream(&st)) &&
      SUCCEEDED(st->InitializeFromMemory(static_cast<BYTE*>(b.data), (DWORD)b.size)) &&
      SUCCEEDED(fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(cv->GetSize(&w, &h)) && w && h && w < 4096 && h < 4096) {
    bgra.resize((size_t)w * h * 4);
    ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
  }
  if (cv) cv->Release();
  if (fr) fr->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  if (fac) fac->Release();
  if (co == S_OK || co == S_FALSE) CoUninitialize();
  return ok;
}

// ------------------------------------------------------------------ hint
std::string g_hint;

// ------------------------------------------------------------------ editing a number in its row
ImGuiID g_editId = 0, g_dragId = 0;
char g_editBuf[32];
int g_editFrames = 0;

struct Row {
  ImVec2 p;
  float w = 0, h = 0;
  bool hov = false, clicked = false;
  ImVec2 clickPos;
};

Row BeginRow(const char* id, float css, bool overlap = false) {
  Row r;
  r.p = ImGui::GetCursorScreenPos();
  r.w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  r.h = P(css);
  if (overlap) ImGui::SetNextItemAllowOverlap();
  r.clicked = ImGui::InvisibleButton(id, ImVec2(r.w, r.h));
  r.hov = ImGui::IsItemHovered();
  r.clickPos = ImGui::GetIO().MouseClickedPos[0];
  return r;
}

bool In(const ImVec2& m, const ImVec2& a, const ImVec2& b) { return m.x >= a.x && m.y >= a.y && m.x < b.x && m.y < b.y; }

float TextY(const Row& r, float px) { return std::floor(r.p.y + (r.h - px) * 0.5f); }

ImDrawList* DL() { return ImGui::GetWindowDrawList(); }

void DottedLine(ImDrawList* dl, float x0, float x1, float y, ImU32 col) {
  float d = std::max(1.0f, std::round(P(1)));
  for (float x = x0; x < x1; x += d * 2) dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x1, x + d), y + d), col);
}

float Round2(float v) { return std::round(v * 100.0f) / 100.0f; }

}  // namespace

// ------------------------------------------------------------------ fonts API
void InitFonts() { Build(); }

void SetScales(float screen, float menu) {
  g_screen = screen;
  g_menu = menu;
}

bool FontsWanted() {
  if (ImGui::GetCurrentContext() && (ImGui::IsMouseDown(ImGuiMouseButton_Left))) return false;  // resizing
  return Desired() != g_built;
}

void RebuildFonts() {
  Build();
  LOG("ui: fonts rebuilt for screen x%.2f, menu x%.2f (%zu sizes)", g_screen, g_menu, g_fonts.size());
}

ImFont* Get(F f, float px) {
  ImFont* best = nullptr;
  float bd = 1e9f;
  for (const Loaded& l : g_fonts) {
    float d = std::fabs(l.px - px) + (l.f == f ? 0 : 1000);
    if (d < bd) bd = d, best = l.font;
  }
  return best ? best : ImGui::GetFont();
}

ImFont* Overlay() { return g_overlay ? g_overlay : Get(F::MonoBold, 64); }

ImTextureID Razium(float* w, float* h) {
  if (!g_raziumTried) {
    g_raziumTried = true;
    Blob b = Resource(kResRazium);
    std::vector<uint8_t> px;
    UINT iw = 0, ih = 0;
    if (b.data && DecodePng(b, px, iw, ih)) {
      g_razium = (ImTextureID)(uintptr_t)d3d9hook::CreateTextureBGRA(px.data(), (int)iw, (int)ih);
      g_raziumW = (float)iw;
      g_raziumH = (float)ih;
    }
    LOG("ui: easter egg picture %s (%ux%u)", g_razium ? "ready" : "NOT available", iw, ih);
  }
  if (w) *w = g_raziumW;
  if (h) *h = g_raziumH;
  return g_razium;
}

float Screen() { return g_screen; }
float S() { return g_menu; }

// ------------------------------------------------------------------ text
ImVec2 Measure(F f, float px, const char* s, float spacing, float wrap) {
  ImFont* font = Get(f, px);
  if (spacing == 0) return font->CalcTextSizeA(px, FLT_MAX, wrap, s);
  float x = 0;
  for (const char* c = s; *c; ++c) x += font->GetCharAdvance((ImWchar)(unsigned char)*c) * px / font->FontSize + spacing;
  return ImVec2(std::max(0.0f, x - spacing), px);
}

void Text(ImDrawList* dl, F f, float px, ImVec2 pos, ImU32 col, const char* s, float spacing, float wrap) {
  ImFont* font = Get(f, px);
  pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
  if (spacing == 0) {
    dl->AddText(font, px, pos, col, s, nullptr, wrap);
    return;
  }
  for (const char* c = s; *c; ++c) {  // letter spacing: ASCII titles only
    dl->AddText(font, px, pos, col, c, c + 1);
    pos.x += font->GetCharAdvance((ImWchar)(unsigned char)*c) * px / font->FontSize + spacing;
  }
}

std::string Ellipsize(F f, float px, const std::string& s, float maxW) {
  if (maxW <= 0) return "";
  if (Measure(f, px, s.c_str()).x <= maxW) return s;
  const char* dots = "\xE2\x80\xA6";
  size_t lo = 0, hi = s.size();
  while (lo < hi) {  // longest prefix that fits with the dots
    size_t mid = (lo + hi + 1) / 2;
    std::string t = s.substr(0, mid) + dots;
    if (Measure(f, px, t.c_str()).x <= maxW) lo = mid; else hi = mid - 1;
  }
  while (lo > 0 && (s[lo] & 0xC0) == 0x80) --lo;  // never cut a UTF-8 character
  return s.substr(0, lo) + dots;
}

std::string Upper(std::string s) {
  for (char& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

// ------------------------------------------------------------------ hint
void SetHint(const std::string& s) { g_hint = s; }
const std::string& Hint() { return g_hint; }
void ClearHint() { g_hint.clear(); }

// ------------------------------------------------------------------ rows
void Group(const char* label) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float px = P(11);
  Text(DL(), F::MonoBold, px, ImVec2(p.x + P(10), p.y + P(7)), kAccent, Upper(label).c_str(), px * 0.1f);
  ImGui::Dummy(ImVec2(1, P(7) + px * 1.25f + P(2)));
}

bool Plain(const char* id, const char* label, const std::string& right, ImU32 rightCol, const RowOpt& o) {
  Row r = BeginRow(id, 22);
  bool hov = r.hov && !o.locked;
  ImDrawList* dl = DL();
  if (hov)
    dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  else if (o.picked)
    dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kSelBg);
  float px = P(13), padL = P(o.sub ? 34 : 18), padR = P(10);
  float y = TextY(r, px);
  F rf = o.picked ? F::MonoBold : F::Mono;
  float rw = right.empty() ? 0 : Measure(rf, px, right.c_str()).x;
  std::string l = (o.picked ? "\xE2\x80\xBA " : "") + std::string(label);
  ImU32 lc = o.locked ? kLocked : hov || o.picked ? kWhite : o.sub ? IM_COL32(0xBD, 0xBD, 0xBD, 255) : kText;
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL - (o.picked ? Measure(F::Mono, px, "\xE2\x80\xBA ").x : 0), y), lc,
       Ellipsize(F::Mono, px, l, r.w - padL - padR - rw - P(12)).c_str());
  if (!right.empty())
    Text(dl, rf, px, ImVec2(r.p.x + r.w - padR - rw, y), hov ? kWhite : rightCol, right.c_str());
  if (r.hov && !o.desc.empty()) SetHint(o.desc);
  return r.clicked && !o.locked;
}

bool Toggle(const char* id, const char* label, bool on, const RowOpt& o) {
  Row r = BeginRow(id, 22);
  bool hov = r.hov && !o.locked;
  ImDrawList* dl = DL();
  if (hov) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  float px = P(13), padL = P(o.sub ? 34 : 18), padR = P(10);
  float y = TextY(r, px);
  std::string rt = o.locked ? o.lockText : on ? "[ON]" : "[off]";
  F rf = !o.locked && on ? F::MonoBold : F::Mono;
  ImU32 rc = o.locked ? kErr : on || hov ? kWhite : kOff;
  float rw = Measure(rf, px, rt.c_str()).x;
  ImU32 lc = o.locked ? kLocked : hov ? kWhite : o.sub ? IM_COL32(0xBD, 0xBD, 0xBD, 255) : kText;
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), lc,
       Ellipsize(F::Mono, px, label, r.w - padL - padR - rw - P(12)).c_str());
  Text(dl, rf, px, ImVec2(r.p.x + r.w - padR - rw, y), rc, rt.c_str());
  if (r.hov && !o.desc.empty()) SetHint(o.desc);
  return r.clicked && !o.locked;
}

std::string NumText(float v, const NumSpec& n) {
  char b[48];
  if (n.integer) {
    snprintf(b, sizeof b, "%d", (int)std::lround(v));
  } else {
    snprintf(b, sizeof b, "%.2f", Round2(v));
    char* e = b + strlen(b) - 1;
    while (e > b && *e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
  }
  return b + n.unit;
}

bool Number(const char* id, const char* label, float* v, const NumSpec& n, bool hot, const RowOpt& o) {
  ImGui::PushID(id);
  ImGuiID me = ImGui::GetID("row");
  bool editing = g_editId == me;
  Row r = BeginRow("row", 22, true);
  bool active = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();
  bool hov = r.hov && !o.locked;
  ImDrawList* dl = DL();
  if (hov || (editing && !o.locked)) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  float px = P(13), padL = P(o.sub ? 34 : 18), padR = P(10);
  float y = TextY(r, px);
  float x1 = r.p.x + r.w - padR;
  ImU32 lc = o.locked ? kLocked : hov ? kWhite : o.sub ? IM_COL32(0xBD, 0xBD, 0xBD, 255) : kText;
  bool changed = false;
  if (o.locked) {
    float rw = Measure(F::Mono, px, o.lockText.c_str()).x;
    Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), lc, Ellipsize(F::Mono, px, label, r.w - padL - padR - rw - P(12)).c_str());
    Text(dl, F::Mono, px, ImVec2(x1 - rw, y), kErr, o.lockText.c_str());
    if (r.hov && !o.desc.empty()) SetHint(o.desc);
    ImGui::PopID();
    return false;
  }
  std::string txt = NumText(*v, n);
  F nf = hot ? F::MonoBold : F::Mono;
  float charW = Measure(F::Mono, px, "<").x;
  float arrowW = charW + P(10);
  float numW = std::max(P(60), Measure(nf, px, txt.c_str()).x + P(8));
  float valW = arrowW * 2 + numW;
  float labelW = std::max(P(o.sub ? 134 : 150), Measure(F::Mono, px, label).x);
  float rtX0 = r.p.x + padL + std::min(labelW, r.w * 0.45f) + P(12);
  float sliderW = std::min(P(230), x1 - valW - P(14) - rtX0);
  ImVec2 aL(x1 - valW, r.p.y), aR(x1 - arrowW, r.p.y), nb(x1 - valW + arrowW, r.p.y);
  ImVec2 sl0(x1 - valW - P(14) - sliderW, r.p.y), sl1(x1 - valW - P(14), r.p.y + r.h);
  bool hasSlider = sliderW >= P(30);
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), lc,
       Ellipsize(F::Mono, px, label, (hasSlider ? sl0.x : aL.x) - (r.p.x + padL) - P(8)).c_str());
  auto setv = [&](float x) {
    x = std::min(n.hardMax, std::max(n.min, x));
    x = n.integer ? std::round(x) : Round2(x);
    if (x != *v) *v = x, changed = true;
  };
  // slider
  if (hasSlider) {
    float cy = r.p.y + r.h * 0.5f, t = std::max(1.0f, std::round(P(3)));
    float pct = n.max > n.min ? std::min(1.0f, std::max(0.0f, (*v - n.min) / (n.max - n.min))) : 0;
    dl->AddRectFilled(ImVec2(sl0.x, cy - t * 0.5f), ImVec2(sl1.x, cy + t * 0.5f), hov ? IM_COL32(0, 0, 0, 90) : kFieldBorder);
    dl->AddRectFilled(ImVec2(sl0.x, cy - t * 0.5f), ImVec2(sl0.x + sliderW * pct, cy + t * 0.5f), hov ? kWhite : kAccent);
    float tx = sl0.x + sliderW * pct;
    dl->AddRectFilled(ImVec2(tx - P(2.5f), cy - P(6.5f)), ImVec2(tx + P(2.5f), cy + P(6.5f)), kText);
    if (ImGui::IsItemActivated() && In(io.MouseClickedPos[0], sl0, sl1)) g_dragId = me;
    if (g_dragId == me) {
      if (active && io.MouseDown[0]) {
        float p = std::min(1.0f, std::max(0.0f, (io.MousePos.x - sl0.x) / sliderW));
        float x = n.min + p * (n.max - n.min);
        setv(std::round(x / n.step) * n.step);
      } else {
        g_dragId = 0;
      }
    }
  }
  // < number >
  ImVec2 m = io.MousePos;
  bool onL = hov && In(m, aL, ImVec2(aL.x + arrowW, r.p.y + r.h)), onR = hov && In(m, aR, ImVec2(x1, r.p.y + r.h));
  if (onL) dl->AddRectFilled(ImVec2(aL.x, r.p.y + P(1)), ImVec2(aL.x + arrowW, r.p.y + r.h - P(1)), IM_COL32(0, 0, 0, 90));
  if (onR) dl->AddRectFilled(ImVec2(aR.x, r.p.y + P(1)), ImVec2(x1, r.p.y + r.h - P(1)), IM_COL32(0, 0, 0, 90));
  ImU32 ac = hov ? kWhite : IM_COL32(0x9A, 0x9A, 0x9A, 255);
  Text(dl, F::Mono, px, ImVec2(aL.x + P(5), y), ac, "<");
  Text(dl, F::Mono, px, ImVec2(aR.x + P(5), y), ac, ">");
  if (r.clicked && g_dragId == 0) {
    ImVec2 c = r.clickPos;
    if (In(c, aL, ImVec2(aL.x + arrowW, r.p.y + r.h))) setv(*v - n.step);
    else if (In(c, aR, ImVec2(x1, r.p.y + r.h))) setv(*v + n.step);
    else if (In(c, nb, ImVec2(nb.x + numW, r.p.y + r.h)) && !editing) {
      g_editId = me;
      editing = true;
      g_editFrames = 0;
      snprintf(g_editBuf, sizeof g_editBuf, "%s", NumText(*v, NumSpec{n.min, n.max, n.step, n.def, n.hardMax, "", n.integer}).c_str());
    }
  }
  if (editing) {
    float bh = P(18);
    ImGui::SetCursorScreenPos(ImVec2(nb.x, r.p.y + (r.h - bh) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(4), std::max(0.0f, (bh - ImGui::GetFontSize()) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, std::round(P(1))));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(10, 10, 10, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, kWhite);
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    ImGui::SetNextItemWidth(numW);
    if (g_editFrames == 0) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##edit", g_editBuf, sizeof g_editBuf,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                      ImGuiInputTextFlags_CharsScientific);
    bool live = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    if (enter) {
      for (char* c = g_editBuf; *c; ++c)
        if (*c == ',') *c = '.';
      char* end = nullptr;
      float x = strtof(g_editBuf, &end);
      if (end != g_editBuf) setv(x);
      g_editId = 0;
    } else if (g_editFrames > 1 && !live) {
      g_editId = 0;  // Esc or a click somewhere else: nothing changes
    }
    ++g_editFrames;
    ImGui::SetCursorScreenPos(ImVec2(r.p.x, r.p.y + r.h));
    ImGui::Dummy(ImVec2(0, 0));
  } else {
    ImU32 nc = hot || hov ? kWhite : kText;
    float tw = Measure(nf, px, txt.c_str()).x;
    Text(dl, nf, px, ImVec2(nb.x + (numW - tw) * 0.5f, y), nc, txt.c_str());
    DottedLine(dl, nb.x + P(2), nb.x + numW - P(2), y + px + P(1), hov ? kWhite : kText2);
  }
  if (r.hov && !o.desc.empty()) SetHint(o.desc);
  ImGui::PopID();
  return changed;
}

int Drop(const char* id, const char* label, const std::vector<DropItem>& items, int cur, const RowOpt& o) {
  ImGui::PushID(id);
  Row r = BeginRow("row", 22);
  ImGuiID pop = ImGui::GetID("##drop");
  bool open = ImGui::IsPopupOpen(pop, 0);
  bool hov = r.hov && !o.locked;
  ImDrawList* dl = DL();
  if (hov) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  float px = P(13), padL = P(o.sub ? 34 : 18), padR = P(10);
  float y = TextY(r, px);
  float x1 = r.p.x + r.w - padR;
  float labelW = std::max(P(o.sub ? 134 : 150), Measure(F::Mono, px, label).x);
  float fw = std::min(P(300), std::max(P(110), x1 - (r.p.x + padL + std::min(labelW, r.w * 0.4f) + P(12))));
  ImVec2 f0(x1 - fw, r.p.y + (r.h - P(18)) * 0.5f), f1(x1, f0.y + P(18));
  ImU32 lc = o.locked ? kLocked : hov ? kWhite : o.sub ? IM_COL32(0xBD, 0xBD, 0xBD, 255) : kText;
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), lc, Ellipsize(F::Mono, px, label, f0.x - r.p.x - padL - P(8)).c_str());
  dl->AddRectFilled(f0, f1, kFieldBg);
  dl->AddRect(f0, f1, hov || open ? kWhite : kFieldBorder, 0, 0, std::max(1.0f, std::round(P(1))));
  std::string val = cur >= 0 && cur < (int)items.size() ? items[cur].text : "";
  ImU32 vc = o.locked ? kLocked : cur >= 0 && cur < (int)items.size() && items[cur].col ? items[cur].col : kText;
  Text(dl, F::Mono, px, ImVec2(f0.x + P(6), y), vc, Ellipsize(F::Mono, px, val, fw - P(26)).c_str());
  // caret
  float cx = f1.x - P(10), cy = (f0.y + f1.y) * 0.5f, cs = P(3.5f);
  dl->AddTriangleFilled(ImVec2(cx - cs, cy - cs * 0.6f), ImVec2(cx + cs, cy - cs * 0.6f), ImVec2(cx, cy + cs * 0.8f),
                        hov ? kWhite : kAccent);
  if (r.hov && !o.desc.empty()) SetHint(o.desc);
  if (r.clicked && !o.locked && !items.empty()) ImGui::OpenPopup(pop);
  int picked = -1;
  float ih = P(20), w = std::max(fw, P(300));
  float hgt = std::min(P(230), ih * items.size() + 2);
  ImVec2 ds = ImGui::GetIO().DisplaySize;
  float px0 = std::max(0.0f, x1 - w), py0 = r.p.y + P(21);
  if (py0 + hgt > ds.y) py0 = std::max(0.0f, r.p.y - hgt + P(1));
  ImGui::SetNextWindowPos(ImVec2(px0, py0));
  ImGui::SetNextWindowSize(ImVec2(w, hgt));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, std::max(1.0f, std::round(P(1))));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::max(3.0f, P(6)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0x0D, 0x0D, 0x0F, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
  if (ImGui::BeginPopup("##drop", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
    ImDrawList* pd = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)items.size(); ++i) {
      ImGui::PushID(i);
      ImVec2 p = ImGui::GetCursorScreenPos();
      float iw = ImGui::GetContentRegionAvail().x;
      if (ImGui::InvisibleButton("##o", ImVec2(std::max(1.0f, iw), ih))) {
        picked = i;
        ImGui::CloseCurrentPopup();
      }
      bool h = ImGui::IsItemHovered();
      if (i == cur)
        pd->AddRectFilled(p, ImVec2(p.x + iw, p.y + ih), kAccent);
      else if (h)
        pd->AddRectFilled(p, ImVec2(p.x + iw, p.y + ih), kSelBg);
      ImU32 c = i == cur || h ? kWhite : items[i].col ? items[i].col : kSoft;
      Text(pd, F::Mono, px, ImVec2(p.x + P(8), std::floor(p.y + (ih - px) * 0.5f)), c,
           Ellipsize(F::Mono, px, items[i].text, iw - P(16)).c_str());
      if (i == cur && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(5);
  ImGui::PopID();
  return picked;
}

int Modes(const char* id, const char* label, const std::vector<const char*>& modes, int cur, bool on,
          const RowOpt& o, bool* toggled) {
  *toggled = false;
  Row r = BeginRow(id, 22);
  bool hov = r.hov && !o.locked;
  ImDrawList* dl = DL();
  if (hov) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  float px = P(13), padL = P(18), padR = P(10);
  float y = TextY(r, px);
  std::string rt = o.locked ? o.lockText : on ? "[ON]" : "[off]";
  F rf = !o.locked && on ? F::MonoBold : F::Mono;
  float rw = Measure(rf, px, rt.c_str()).x;
  float x = r.p.x + r.w - padR - rw - P(12);
  std::vector<std::pair<float, float>> rects(modes.size());
  for (int i = (int)modes.size() - 1; i >= 0; --i) {
    float w = P(9) + P(6) + Measure(F::Mono, px, modes[i]).x;
    rects[i] = {x - w, x};
    x -= w + P(12);
  }
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), o.locked ? kLocked : hov ? kWhite : kText,
       Ellipsize(F::Mono, px, label, x + P(12) - r.p.x - padL - P(8)).c_str());
  for (int i = 0; i < (int)modes.size(); ++i) {
    float bx = rects[i].first, by = r.p.y + (r.h - P(9)) * 0.5f;
    bool sel = i == cur;
    ImU32 c = o.locked ? kLocked : hov || sel ? kWhite : IM_COL32(0x9A, 0x9A, 0x9A, 255);
    if (sel) {
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + P(9), by + P(9)), o.locked ? kLocked : hov ? kWhite : kAccent);
      dl->AddRect(ImVec2(bx + P(1), by + P(1)), ImVec2(bx + P(8), by + P(8)), IM_COL32(10, 10, 10, 255), 0, 0,
                  std::max(1.0f, P(1.5f)));
    } else {
      dl->AddRect(ImVec2(bx, by), ImVec2(bx + P(9), by + P(9)), c, 0, 0, std::max(1.0f, std::round(P(1))));
    }
    Text(dl, F::Mono, px, ImVec2(bx + P(15), y), c, modes[i]);
  }
  Text(dl, rf, px, ImVec2(r.p.x + r.w - padR - rw, y), o.locked ? kErr : on || hov ? kWhite : kOff, rt.c_str());
  if (r.hov && !o.desc.empty()) SetHint(o.desc);
  if (!r.clicked || o.locked) return -1;
  for (int i = 0; i < (int)modes.size(); ++i)
    if (r.clickPos.x >= rects[i].first - P(3) && r.clickPos.x < rects[i].second + P(3)) return i == cur ? -1 : i;
  *toggled = true;
  return -1;
}

int Buttons(const char* id, const std::vector<Btn>& b, const Slots* slots, int* slot) {
  if (slot) *slot = -1;
  ImGui::PushID(id);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  float x0 = p.x + P(12), x1 = p.x + w - P(10), gap = P(4);
  float y = p.y + P(1), lineH = P(22);
  for (const Btn& k : b)
    if (k.kind == BtnKind::Big) lineH = std::max(lineH, P(26));
  float x = x0;
  ImDrawList* dl = DL();
  int clicked = -1;
  float px = P(13);
  if (slots) {
    float lw = Measure(F::Mono, px, slots->label).x;
    Text(dl, F::Mono, px, ImVec2(x + P(6), std::floor(y + (lineH - px) * 0.5f)), kText, slots->label);
    x += P(6) + lw + P(8);
    for (int i = 0; i < slots->count; ++i) {
      ImVec2 a(x, y + (lineH - P(18)) * 0.5f), c(x + P(24), a.y + P(18));
      ImGui::SetCursorScreenPos(a);
      ImGui::PushID(1000 + i);
      bool hit = ImGui::InvisibleButton("##slot", ImVec2(P(24), P(18)));
      bool h = ImGui::IsItemHovered();
      ImGui::PopID();
      bool sel = i == slots->picked, saved = (slots->saved >> i) & 1;
      if (sel) dl->AddRectFilled(a, c, kSelBg);
      dl->AddRect(a, c, sel ? kAccent : h ? kWhite : kFieldBorder, 0, 0, std::max(1.0f, std::round(P(1))));
      char num[8];
      snprintf(num, sizeof num, "%d", slots->first + i);
      F nf = sel ? F::MonoBold : F::Mono;
      ImVec2 ts = Measure(nf, px, num);
      Text(dl, nf, px, ImVec2(a.x + (P(24) - ts.x) * 0.5f, std::floor(a.y + (P(18) - px) * 0.5f)),
           sel ? kWhite : saved ? kText : kOff, num);
      if (h && !slots->desc.empty()) SetHint(slots->desc);
      if (hit && slot) *slot = i;
      x = c.x + P(2);
    }
    x += P(6);
  }
  // sizes
  std::vector<float> bw(b.size());
  float total = 0;
  for (size_t i = 0; i < b.size(); ++i) {
    F f = b[i].kind == BtnKind::Big ? F::MonoBold : F::Mono;
    float fpx = b[i].kind == BtnKind::Small ? P(11) : px;
    float pad = b[i].kind == BtnKind::Big ? P(16) : b[i].kind == BtnKind::Small ? P(6) : P(10);
    bw[i] = std::min(x1 - x0, Measure(f, fpx, b[i].label.c_str()).x + pad * 2);
    total += bw[i] + (i ? gap : 0);
  }
  if (slots && x + total <= x1) x = x1 - total;  // with slots: the buttons sit at the right end
  for (size_t i = 0; i < b.size(); ++i) {
    if (x + bw[i] > x1 + 0.5f && x > x0) {  // wrap
      x = x0;
      y += lineH + gap;
    }
    const Btn& k = b[i];
    float bh = k.kind == BtnKind::Big ? P(26) : k.kind == BtnKind::Small ? P(18) : P(20);
    ImVec2 a(x, y + (lineH - bh) * 0.5f), c(x + bw[i], a.y + bh);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID((int)i);
    bool hit = ImGui::InvisibleButton("##b", ImVec2(std::max(1.0f, bw[i]), bh));
    bool h = ImGui::IsItemHovered();
    ImGui::PopID();
    bool dis = k.kind == BtnKind::Disabled;
    ImU32 bg = kBtnBg, br = kBtnBorder, tc = kText;
    if (dis) bg = IM_COL32(0x14, 0x14, 0x14, 255), br = IM_COL32(0x22, 0x22, 0x22, 255), tc = kLocked;
    else if (k.kind == BtnKind::Big) bg = br = kAccent, tc = kWhite;
    else if (k.kind == BtnKind::Primary) tc = kWhite;  // user (#16): no red frame on buttons
    if (h && !dis) bg = br = k.kind == BtnKind::Big ? IM_COL32(0xEE, 0x22, 0x28, 255) : kAccent, tc = kWhite;
    dl->AddRectFilled(a, c, bg);
    dl->AddRect(a, c, br, 0, 0, std::max(1.0f, std::round(P(1))));
    F f = k.kind == BtnKind::Big ? F::MonoBold : F::Mono;
    float fpx = k.kind == BtnKind::Small ? P(11) : px;
    float pad = k.kind == BtnKind::Big ? P(16) : k.kind == BtnKind::Small ? P(6) : P(10);
    Text(dl, f, fpx, ImVec2(a.x + pad, std::floor(a.y + (bh - fpx) * 0.5f)), tc,
         Ellipsize(f, fpx, k.label, bw[i] - pad * 2 + 1).c_str());
    if (h) {
      std::string d = k.desc;
      if (dis && !k.why.empty()) d += (d.empty() ? "" : "\n") + std::string("Not now: ") + k.why + ".";
      if (!d.empty()) SetHint(d);
    }
    if (hit) clicked = (int)i;
    x = c.x + gap;
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, y + lineH + P(1)));
  ImGui::Dummy(ImVec2(w, 0));
  ImGui::PopID();
  return clicked;
}

void Info(const std::string& text, ImU32 col) {
  if (text.empty()) return;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  float px = P(12), wrap = w - P(28);
  ImVec2 sz = Measure(F::Mono, px, text.c_str(), 0, wrap);
  Text(DL(), F::Mono, px, ImVec2(p.x + P(18), p.y + P(2)), col, text.c_str(), 0, wrap);
  ImGui::Dummy(ImVec2(w, sz.y + P(4)));
}

int ListRow(const char* id, const std::string& label, const std::string& right, ImU32 rightCol,
            const std::vector<Btn>& btns, const RowOpt& o) {
  ImGui::PushID(id);
  Row r = BeginRow("row", 22, true);
  ImDrawList* dl = DL();
  float px = P(13), sp = P(11), padL = P(o.sub ? 34 : 18), padR = P(10), bh = P(18);
  // buttons from the right end
  std::vector<float> bx(btns.size()), bw(btns.size());
  float x = r.p.x + r.w - padR;
  for (int i = (int)btns.size() - 1; i >= 0; --i) {
    bw[i] = Measure(F::Mono, sp, btns[i].label.c_str()).x + P(12);
    x -= bw[i];
    bx[i] = x;
    x -= P(3);
  }
  int res = -1;
  bool anyBtnHov = false;
  std::vector<bool> bh_(btns.size());
  for (size_t i = 0; i < btns.size(); ++i) {
    ImGui::SetCursorScreenPos(ImVec2(bx[i], r.p.y + (r.h - bh) * 0.5f));
    ImGui::PushID((int)i);
    bool hit = ImGui::InvisibleButton("##b", ImVec2(bw[i], bh));
    bh_[i] = ImGui::IsItemHovered();
    ImGui::PopID();
    anyBtnHov |= bh_[i];
    if (hit) res = (int)i;
  }
  bool hov = (r.hov || anyBtnHov) && !o.locked;
  if (hov) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  else if (o.picked) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kSelBg);
  float y = TextY(r, px);
  float rw = right.empty() ? 0 : Measure(F::Mono, px, right.c_str()).x;
  float rx = (btns.empty() ? r.p.x + r.w - padR : bx[0] - P(10)) - rw;
  std::string l = (o.picked ? "› " : "") + label;
  ImU32 lc = o.locked ? kLocked : hov || o.picked ? kWhite : o.sub ? IM_COL32(0xBD, 0xBD, 0xBD, 255) : kText;
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), lc, Ellipsize(F::Mono, px, l, rx - r.p.x - padL - P(10)).c_str());
  if (!right.empty()) Text(dl, F::Mono, px, ImVec2(rx, y), hov ? kWhite : rightCol, right.c_str());
  for (size_t i = 0; i < btns.size(); ++i) {
    bool dis = btns[i].kind == BtnKind::Disabled;
    ImVec2 a(bx[i], r.p.y + (r.h - bh) * 0.5f), c(bx[i] + bw[i], a.y + bh);
    ImU32 bg = dis ? IM_COL32(0x14, 0x14, 0x14, 255) : bh_[i] ? kWhite : kBtnBg;
    ImU32 tc = dis ? kLocked : bh_[i] ? kAccent : kText;
    dl->AddRectFilled(a, c, bg);
    dl->AddRect(a, c, dis ? IM_COL32(0x22, 0x22, 0x22, 255) : bh_[i] ? kWhite : kBtnBorder, 0, 0, std::max(1.0f, std::round(P(1))));
    Text(dl, F::Mono, sp, ImVec2(a.x + P(6), std::floor(a.y + (bh - sp) * 0.5f)), tc, btns[i].label.c_str());
    if (bh_[i]) {
      std::string d = btns[i].desc;
      if (dis && !btns[i].why.empty()) d += (d.empty() ? "" : "\n") + std::string("Not now: ") + btns[i].why + ".";
      if (!d.empty()) SetHint(d);
    }
  }
  if (r.hov && !anyBtnHov && !o.desc.empty()) SetHint(o.desc);
  if (res >= 0 && btns[res].kind == BtnKind::Disabled) res = -1;
  if (res < 0 && r.clicked && !o.locked) res = -2;
  ImGui::SetCursorScreenPos(ImVec2(r.p.x, r.p.y + r.h));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
  return res;
}

bool TextField(const char* id, const char* label, char* buf, size_t size, const char* placeholder,
               const std::string& desc, bool multiline) {
  ImGui::PushID(id);
  float lines = multiline ? 3.0f : 1.0f;
  Row r = BeginRow("row", multiline ? 22 + 16 * (lines - 1) : 22, true);
  ImDrawList* dl = DL();
  if (r.hov) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), kAccent);
  float px = P(13), padL = P(18), padR = P(10);
  float x1 = r.p.x + r.w - padR;
  float labelW = std::max(P(150), Measure(F::Mono, px, label).x);
  float fw = std::min(P(300), std::max(P(110), x1 - (r.p.x + padL + std::min(labelW, r.w * 0.4f) + P(12))));
  float fh = multiline ? r.h - P(4) : P(18);
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, multiline ? r.p.y + P(4) : TextY(r, px)), r.hov ? kWhite : kText,
       Ellipsize(F::Mono, px, label, x1 - fw - r.p.x - padL - P(8)).c_str());
  ImGui::SetCursorScreenPos(ImVec2(x1 - fw, r.p.y + (r.h - fh) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(6), std::max(0.0f, (P(18) - ImGui::GetFontSize()) * 0.5f)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, std::round(P(1))));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kFieldBg);
  ImGui::PushStyleColor(ImGuiCol_Border, kFieldBorder);
  ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
  bool ch;
  if (multiline) {
    ch = ImGui::InputTextMultiline("##t", buf, size, ImVec2(fw, fh));
  } else {
    ImGui::SetNextItemWidth(fw);
    ch = ImGui::InputTextWithHint("##t", placeholder ? placeholder : "", buf, size);
  }
  if (ImGui::IsItemActive())
    dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kAccent, 0, 0, std::max(1.0f, std::round(P(1))));
  if (ImGui::IsItemHovered() && !desc.empty()) SetHint(desc);
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
  ImGui::SetCursorScreenPos(ImVec2(r.p.x, r.p.y + r.h));
  ImGui::Dummy(ImVec2(0, 0));
  if (r.hov && !desc.empty()) SetHint(desc);
  ImGui::PopID();
  return ch;
}

void Legend(const std::vector<LegendItem>& items) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  float px = P(12), colW = (w - P(28) - P(12)) * 0.5f, lh = px * 1.3f + P(2);
  ImDrawList* dl = DL();
  for (size_t i = 0; i < items.size(); ++i) {
    float x = p.x + P(18) + (i % 2) * (colW + P(12)), y = p.y + P(2) + (i / 2) * lh;
    float sy = y + (px - P(10)) * 0.5f + P(1);
    dl->AddRectFilled(ImVec2(x, sy), ImVec2(x + P(10), sy + P(10)), items[i].col);
    Text(dl, F::Mono, px, ImVec2(x + P(18), y), kSoft, Ellipsize(F::Mono, px, items[i].text, colW - P(18)).c_str());
  }
  ImGui::Dummy(ImVec2(w, P(2) + ((items.size() + 1) / 2) * lh + P(4)));
}

int KeyRow(const char* id, const char* label, const char* group, const std::string& key, KeyState st, bool canClear,
           const std::string& desc) {
  ImGui::PushID(id);
  Row r = BeginRow("row", 22, true);
  ImDrawList* dl = DL();
  if (r.hov) dl->AddRectFilled(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h), IM_COL32(0x1F, 0x1F, 0x1F, 255));
  float px = P(13), sp = P(11), padL = P(18), padR = P(10);
  float y = TextY(r, px);
  float x1 = r.p.x + r.w - padR;
  float cBtn = x1 - P(70), cKey = cBtn - P(110), cGrp = cKey - P(80);
  Text(dl, F::Mono, px, ImVec2(r.p.x + padL, y), kText, Ellipsize(F::Mono, px, label, cGrp - r.p.x - padL - P(6)).c_str());
  if (group && *group) Text(dl, F::Mono, sp, ImVec2(cGrp, TextY(r, sp)), kOff, Ellipsize(F::Mono, sp, group, P(76)).c_str());
  // key box
  std::string kt = st == KeyState::Waiting ? "press\xE2\x80\xA6" : st == KeyState::None ? "no key" : key;
  if (st == KeyState::Conflict) kt += " (used twice)";
  F kf = st == KeyState::None ? F::Mono : F::MonoBold;
  std::string shown = Ellipsize(kf, sp, kt, P(110) - P(16));
  ImVec2 ks = Measure(kf, sp, shown.c_str());
  float ky = TextY(r, sp);
  if (st != KeyState::None) {
    ImU32 bc = st == KeyState::Waiting ? kAccent : st == KeyState::Conflict ? kErr : kText;
    dl->AddRect(ImVec2(cKey, ky - P(2)), ImVec2(cKey + ks.x + P(12), ky + sp + P(2)), bc, 0, 0, std::max(1.0f, std::round(P(1))));
  }
  ImU32 kc = st == KeyState::Waiting ? kToast : st == KeyState::Conflict ? kErr : st == KeyState::None ? kLocked : kWhite;
  Text(dl, kf, sp, ImVec2(cKey + (st == KeyState::None ? 0 : P(6)), ky), kc, shown.c_str());
  if (r.hov && !desc.empty()) SetHint(desc);
  // buttons (right)
  int res = 0;
  float bh = P(18);
  float xb = x1;
  auto smallBtn = [&](const char* t, int code) {
    float bw = Measure(F::Mono, sp, t).x + P(12);
    xb -= bw;
    ImVec2 a(xb, r.p.y + (r.h - bh) * 0.5f), c(xb + bw, a.y + bh);
    ImGui::SetCursorScreenPos(a);
    bool hit = ImGui::InvisibleButton(t, ImVec2(bw, bh));
    bool h = ImGui::IsItemHovered();
    dl->AddRectFilled(a, c, h ? kAccent : kBtnBg);
    dl->AddRect(a, c, h ? kAccent : kBtnBorder, 0, 0, std::max(1.0f, std::round(P(1))));
    Text(dl, F::Mono, sp, ImVec2(a.x + P(6), std::floor(a.y + (bh - sp) * 0.5f)), h ? kWhite : kText, t);
    if (h && !desc.empty()) SetHint(desc);
    if (hit) res = code;
    xb -= P(3);
  };
  if (canClear && st != KeyState::None && st != KeyState::Waiting) smallBtn("\xC3\x97", 2);
  smallBtn(st == KeyState::Waiting ? "Cancel" : "Bind", 1);
  ImGui::SetCursorScreenPos(ImVec2(r.p.x, r.p.y + r.h));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
  return res;
}

bool Picture(const char* id, ImTextureID tex, float iw, float ih, const char* caption) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  float pw = std::min(P(260), w - P(20)), ph = iw > 0 ? pw * ih / iw : 0;
  float px = P(12);
  float h = P(4) + ph + P(2) + px * 1.3f + P(8);
  bool hit = ImGui::InvisibleButton(id, ImVec2(w, h));
  ImDrawList* dl = DL();
  float x = p.x + (w - pw) * 0.5f;
  if (tex) dl->AddImage(tex, ImVec2(x, p.y + P(4)), ImVec2(x + pw, p.y + P(4) + ph));
  ImVec2 cs = Measure(F::Mono, px, caption);
  Text(dl, F::Mono, px, ImVec2(p.x + (w - cs.x) * 0.5f, p.y + P(4) + ph + P(2)), kSoft, caption);
  return hit;
}

void Space(float css) { ImGui::Dummy(ImVec2(1, P(css))); }

bool AnyPopupOpen() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

void ApplyImGuiStyle(float s) {
  ImGuiStyle& st = ImGui::GetStyle();
  st = ImGuiStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.ScrollbarRounding =
      st.GrabRounding = st.TabRounding = 0;
  st.WindowBorderSize = 1;
  st.PopupBorderSize = 1;
  st.FrameBorderSize = 1;
  st.WindowPadding = ImVec2(10, 8);
  st.FramePadding = ImVec2(8, 3);
  st.ItemSpacing = ImVec2(6, 4);
  st.ScrollbarSize = 6;
  st.WindowMinSize = ImVec2(10, 10);
  st.ScaleAllSizes(s);
  auto c = [](ImU32 v) { return ImGui::ColorConvertU32ToFloat4(v); };
  ImVec4* k = st.Colors;
  k[ImGuiCol_Text] = c(kText);
  k[ImGuiCol_TextDisabled] = c(kText2);
  k[ImGuiCol_WindowBg] = c(kWindowBg);
  k[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  k[ImGuiCol_PopupBg] = c(IM_COL32(0x0D, 0x0D, 0x0F, 250));
  k[ImGuiCol_Border] = c(kFieldBorder);
  k[ImGuiCol_FrameBg] = c(kFieldBg);
  k[ImGuiCol_FrameBgHovered] = c(IM_COL32(0x24, 0x24, 0x24, 255));
  k[ImGuiCol_FrameBgActive] = c(IM_COL32(0x2A, 0x2A, 0x2A, 255));
  k[ImGuiCol_TitleBg] = k[ImGuiCol_TitleBgActive] = k[ImGuiCol_TitleBgCollapsed] = c(IM_COL32(8, 8, 10, 240));
  k[ImGuiCol_ScrollbarBg] = c(IM_COL32(0x11, 0x11, 0x11, 200));
  k[ImGuiCol_ScrollbarGrab] = k[ImGuiCol_ScrollbarGrabHovered] = k[ImGuiCol_ScrollbarGrabActive] = c(kAccent);
  k[ImGuiCol_CheckMark] = c(kAccent);
  k[ImGuiCol_SliderGrab] = k[ImGuiCol_SliderGrabActive] = c(kAccent);
  k[ImGuiCol_Button] = c(kBtnBg);
  k[ImGuiCol_ButtonHovered] = c(kAccent);
  k[ImGuiCol_ButtonActive] = c(IM_COL32(0xA8, 0x10, 0x14, 255));
  k[ImGuiCol_Header] = c(kSelBg);
  k[ImGuiCol_HeaderHovered] = c(kAccent);
  k[ImGuiCol_HeaderActive] = c(IM_COL32(0xA8, 0x10, 0x14, 255));
  k[ImGuiCol_Separator] = k[ImGuiCol_SeparatorHovered] = k[ImGuiCol_SeparatorActive] = c(IM_COL32(0x2A, 0x2A, 0x2A, 255));
  k[ImGuiCol_ResizeGrip] = c(IM_COL32(0xD7, 0x14, 0x1A, 60));
  k[ImGuiCol_ResizeGripHovered] = c(IM_COL32(0xD7, 0x14, 0x1A, 170));
  k[ImGuiCol_ResizeGripActive] = c(kAccent);
  k[ImGuiCol_TextSelectedBg] = c(IM_COL32(0xD7, 0x14, 0x1A, 110));
  k[ImGuiCol_ModalWindowDimBg] = c(IM_COL32(0, 0, 0, 120));
  k[ImGuiCol_NavCursor] = c(kAccent);
}

}  // namespace ui
