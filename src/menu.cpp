#include "menu.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "features.h"
#include "hotkeys.h"
#include "imgui.h"
#include "input.h"
#include "log.h"
#include "nav.h"
#include "cpsets.h"
#include "damagelog.h"
#include "enemies.h"
#include "realcp.h"
#include "saveslots.h"
#include "state.h"
#include "steam_guard.h"
#include "ue3/dumper.h"
#include "ue3/engine.h"
#include "compat.h"
#include "unlock.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"
#include "visuals.h"

namespace menu {
namespace {

// UI fonts at a few sizes (the closest one is scaled to 17 px per 1080 lines of screen height).
constexpr float kUiBasePx = 17.0f;
const float kUiSizes[] = {15.0f, 17.0f, 21.0f, 25.0f, 30.0f, 36.0f};
ImFont* g_uiFonts[6] = {};
ImFont* g_overlayFont = nullptr;
constexpr float kOverlayFontPx = 64.0f;  // rasterised large, drawn smaller: crisp up to 4K
ImGuiStyle g_baseStyle;
float g_uiScale = 0;

// "Practice Tool Active": fixed by the user's choice, not configurable. Label centre in fractions of the
// centred 16:9 area of the screen (on 21:9 it stays next to the weapon HUD instead of drifting to the edge),
// size relative to that area's height (base 30 px at 1080 lines x 0.83).
constexpr float kOverlayX = 0.148f, kOverlayY = 0.910f, kOverlayScale = 0.83f, kOverlayBasePx = 30.0f;

const ImVec4 kGreen(0.40f, 0.95f, 0.45f, 1.0f);
const ImVec4 kYellow(1.0f, 0.80f, 0.20f, 1.0f);
const ImVec4 kRed(1.0f, 0.40f, 0.40f, 1.0f);
std::atomic<bool> g_forceDetails{false};  // autorun: open Details / Edit points / Share for screenshots

ImFont* TryFont(const char* file, float px) {
  char dir[MAX_PATH];
  GetWindowsDirectoryA(dir, MAX_PATH);
  std::string path = std::string(dir) + "\\Fonts\\" + file;
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
  ImGuiIO& io = ImGui::GetIO();
  // Latin-1, Cyrillic, general punctuation (dashes, quotes, ellipsis in save names and notes)
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0};
  return io.Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges);
}

// The 16:9 rectangle centred on the screen (letterboxed on 16:10, pillarboxed on 21:9).
void Rect169(const ImVec2& d, ImVec2& origin, ImVec2& size) {
  if (d.x / d.y > 16.0f / 9.0f) {
    size = ImVec2(d.y * 16.0f / 9.0f, d.y);
    origin = ImVec2((d.x - size.x) * 0.5f, 0);
  } else {
    size = ImVec2(d.x, d.x * 9.0f / 16.0f);
    origin = ImVec2(0, (d.y - size.y) * 0.5f);
  }
}

void DrawOverlay() {
  if (!features::OverlaySticky()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = g_overlayFont ? g_overlayFont : ImGui::GetFont();
  ImVec2 o, r;
  Rect169(io.DisplaySize, o, r);
  float px = kOverlayBasePx * kOverlayScale * r.y / 1080.0f;
  const char* text = "Practice Tool Active";
  ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
  ImVec2 pos(std::floor(o.x + r.x * kOverlayX - size.x * 0.5f), std::floor(o.y + r.y * kOverlayY - size.y * 0.5f));
  // Black outline (8 directions) + soft drop shadow: readable on bright snow, red demons and dark caves alike.
  float t = std::max(1.0f, std::round(px / 14.0f));
  dl->AddText(font, px, ImVec2(pos.x + t * 1.5f, pos.y + t * 1.5f), IM_COL32(0, 0, 0, 140), text);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx || dy) dl->AddText(font, px, ImVec2(pos.x + dx * t, pos.y + dy * t), IM_COL32(0, 0, 0, 230), text);
  dl->AddText(font, px, pos, IM_COL32(235, 40, 40, 255), text);
}

void Help(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Game version in the title bar, right after "DmC Practice Tool": green when the build is known, red
// "unknown build" with a tooltip otherwise. Call right after Begin() of the main window.
void TitleVersion(const char* title) {
  const VersionInfo& v = GetGameVersion();
  const ImGuiStyle& st = ImGui::GetStyle();
  ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  float barH = ImGui::GetFontSize() + st.FramePadding.y * 2;
  ImVec2 pos(wp.x + st.FramePadding.x + ImGui::CalcTextSize(title).x + ImGui::GetFontSize() * 0.6f, wp.y + st.FramePadding.y);
  ImVec2 size = ImGui::CalcTextSize(v.label);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + barH), false);
  dl->AddText(pos, ImGui::GetColorU32(FeaturesAllowed() ? kGreen : kRed), v.label);
  dl->PopClipRect();
  if (!FeaturesAllowed() && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted("This version of the game is not supported yet. All features are locked; the menu "
                           "and your settings still work.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// ------------------------------------------------------------------ binds
// Every bind that is used twice (features, presets, menu key) is shown in red.
std::set<const hotkeys::Bind*> g_conflicts;

void ComputeConflicts() {
  g_conflicts.clear();
  auto lk = features::Lock();
  features::Data& d = features::D();
  std::vector<const hotkeys::Bind*> all;
  for (const auto& b : d.binds) all.push_back(&b);
  for (const auto& p : d.presets) all.push_back(&p.bind);
  hotkeys::Bind menuKey;
  menuKey.vk = g_cfg.menuKey;
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i]->Empty()) continue;
    if (hotkeys::Overlaps(*all[i], menuKey)) g_conflicts.insert(all[i]);
    for (size_t k = i + 1; k < all.size(); ++k)
      if (hotkeys::Overlaps(*all[i], *all[k])) {
        g_conflicts.insert(all[i]);
        g_conflicts.insert(all[k]);
      }
  }
}

// "[F5] [Bind] [x]" for any bind. Returns true while capturing into it.
void BindWidget(hotkeys::Bind* b, const char* id) {
  ImGui::PushID(id);
  bool capturing = hotkeys::CaptureTarget() == b;
  if (capturing) {
    ImGui::TextColored(kYellow, "press a key or pad buttons (Esc = cancel)");
  } else {
    std::string t = hotkeys::Text(*b);
    if (t.empty())
      ImGui::TextDisabled("no key");
    else if (g_conflicts.count(b))
      ImGui::TextColored(kRed, "[%s] used twice", t.c_str());
    else
      ImGui::Text("[%s]", t.c_str());
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(capturing ? "Cancel" : "Bind")) {
    if (capturing)
      hotkeys::CancelCapture();
    else
      hotkeys::BeginCapture(b);
  }
  if (!b->Empty() && !capturing) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      *b = {};
      features::MarkDirty();
    }
    ImGui::SetItemTooltip("Remove this key");
  }
  ImGui::PopID();
}

// ------------------------------------------------------------------ feature rows
void StatusLine() {
  game::Status st = features::Status();
  if (!FeaturesAllowed())
    ImGui::TextColored(kRed, "Unknown game version: all features are locked.");
  else if (st.mission)
    ImGui::TextColored(kGreen, "In a mission (%s)", st.map.c_str());
  else if (st.frontEnd)
    ImGui::TextDisabled("Main menu: you can tick features now, they start working in a mission.");
  else
    ImGui::TextDisabled("Loading... features start when the mission is on screen.");
  std::string act;
  {
    auto lk = features::Lock();
    for (const auto& p : features::D().presets)
      if (p.active) act += (act.empty() ? "" : ", ") + p.name;
  }
  if (!act.empty()) ImGui::TextColored(kGreen, "Active presets: %s", act.c_str());
}

float Round01(float v) { return std::round(v * 10.0f) / 10.0f; }

// One feature parameter: slider, checkbox or a choice of a few options. Returns true when changed.
bool ParamWidget(const features::ParamDef& d, float* v, float sliderWidth) {
  using features::ParamKind;
  if (d.kind == ParamKind::Toggle) {
    bool b = *v != 0;
    bool ch = ImGui::Checkbox(d.label, &b);
    if (d.help) Help(d.help);
    if (ch) *v = b ? 1.0f : 0.0f;
    return ch;
  }
  if (d.kind == ParamKind::Choice) {
    int cur = (int)(*v + 0.5f), was = cur;
    for (int i = 0; i < (int)d.choices.size(); ++i) {
      if (i) ImGui::SameLine();
      ImGui::RadioButton(d.choices[i], &cur, i);
    }
    if (d.help) Help(d.help);
    if (cur != was) *v = (float)cur;
    return cur != was;
  }
  ImGui::SetNextItemWidth(sliderWidth);
  if (!ImGui::SliderFloat(d.label, v, d.min, d.max, d.fmt, ImGuiSliderFlags_AlwaysClamp)) return false;
  *v = Round01(*v) == 0.0f && d.min > 0 ? d.min : std::max(d.min, Round01(*v));
  return true;
}

// extraWhy: action features - a reason from the menu selection ("pick a checkpoint") on top of f->Blocked().
void FeatureRow(features::Feature* f, size_t index, const std::string& extraWhy = {}) {
  features::View v = features::Get(f->id);
  ImGui::PushID(f->id);
  bool locked = !FeaturesAllowed();
  std::string actWhy;
  if (f->action) {
    actWhy = f->Blocked();
    if (actWhy.empty()) actWhy = extraWhy;
  }
  ImGui::BeginDisabled(locked);
  // marker: filled green = working, hollow = off, yellow = on but waiting (why is shown below)
  float h = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImU32 col = v.applied ? ImGui::GetColorU32(kGreen) : v.on ? ImGui::GetColorU32(kYellow)
                                                             : ImGui::GetColorU32(ImGuiCol_TextDisabled);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (v.on)
    dl->AddCircleFilled(ImVec2(p.x + h * 0.3f, p.y + h * 0.5f), h * 0.18f, col);
  else
    dl->AddCircle(ImVec2(p.x + h * 0.3f, p.y + h * 0.5f), h * 0.18f, col, 0, 1.5f);
  ImGui::Dummy(ImVec2(h * 0.6f, h));
  ImGui::SameLine(0, 0);
  if (f->action) {
    ImGui::BeginDisabled(!actWhy.empty());
    if (ImGui::Button(f->name)) features::Trigger(f->id);
    ImGui::EndDisabled();
  } else {
    bool m = v.manual;
    if (v.on) ImGui::PushStyleColor(ImGuiCol_Text, v.applied ? kGreen : kYellow);
    if (ImGui::Checkbox(f->name, &m)) features::SetManual(f->id, m);
    if (v.on) ImGui::PopStyleColor();
  }
  Help(f->help);
  if (!actWhy.empty() && !locked) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", actWhy.c_str());
  }
  if (v.viaPreset) {
    ImGui::SameLine();
    ImGui::TextColored(kGreen, "(preset)");
  }
  if (!f->noBind) {
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 13.0f);
    auto lk = features::Lock();
    BindWidget(&features::D().binds[index], "bind");
  }
  for (size_t k = 0; k < f->params.size() && k < (size_t)features::kMaxParams; ++k) {
    const features::ParamDef& d = f->params[k];
    ImGui::PushID((int)k);
    ImGui::Indent(h * 0.6f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    float val = v.manualP[k];
    if (ParamWidget(d, &val, ImGui::GetFontSize() * 14.0f)) {
      features::SetParam(f->id, (int)k, val);
      // moving a slider turns the feature on; options (checkbox / choice) only change how it works
      if (!v.manual && d.kind == features::ParamKind::Slider) features::SetManual(f->id, true);
    }
    if (d.kind == features::ParamKind::Slider) {
      ImGui::SetItemTooltip("Drag, or Ctrl+click to type a number.");
      ImGui::SameLine();
      // multipliers reset to "1x"; other sliders (seconds...) to their default
      bool mult = d.fmt && strchr(d.fmt, 'x');
      if (ImGui::SmallButton(mult ? "1x" : "Reset")) features::SetParam(f->id, (int)k, d.def);
      ImGui::SetItemTooltip("Back to normal (%s)", d.Text(d.def).c_str());
    }
    if (v.viaPreset || (v.on && v.p[k] != v.manualP[k])) {
      ImGui::SameLine();
      ImGui::TextDisabled("preset uses %s", d.Text(v.p[k]).c_str());
    }
    ImGui::Unindent(h * 0.6f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PopID();
  }
  if (v.on && !v.applied && !v.why.empty()) {
    ImGui::Indent(h * 0.6f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextColored(v.why.find("wins") != std::string::npos ? kYellow : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled],
                       "(%s)", v.why.c_str());
    ImGui::Unindent(h * 0.6f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

// Cheats tab: every in-mission feature, grouped (Player, Enemies, Game). Visuals have their own tab.
void TabCheats() {
  StatusLine();
  const auto& all = features::All();
  for (features::Group g : {features::Group::Player, features::Group::Enemies, features::Group::Game}) {
    if (!features::AnyInGroup(g)) continue;  // a group appears when it has something in it
    ImGui::SeparatorText(features::GroupName(g));
    for (size_t i = 0; i < all.size(); ++i)
      if (all[i]->group == g) FeatureRow(all[i], i);
  }
}

// ------------------------------------------------------------------ Practice tab
int FeatureIndex(const char* id) {
  const auto& all = features::All();
  for (size_t i = 0; i < all.size(); ++i)
    if (!strcmp(all[i]->id, id)) return (int)i;
  return -1;
}

void NavRow(const char* id, const std::string& extraWhy = {}) {
  int i = FeatureIndex(id);
  if (i >= 0) FeatureRow(features::All()[i], (size_t)i, extraWhy);
}

std::string PointLabel(const nav::Mission& m, int i) {
  const nav::Point& p = m.points[i];
  const char* tag = p.kind == nav::Kind::Arena ? "  [before arena]" : p.kind == nav::Kind::Cutscene ? "  [before cutscene]" : "";
  bool kept = !p.cp.empty() && realcp::Find(m.map, p.cp);
  return std::to_string(i + 1) + ". " + p.name + tag + (kept ? "  [created]" : "");
}

// A combo over a mission's points; returns true when changed.
bool PointCombo(const char* label, const nav::Mission& m, int* cur, bool loadableOnly, float width) {
  int n = (int)m.points.size();
  if (*cur < 0 || *cur >= n) *cur = 0;
  bool changed = false;
  ImGui::SetNextItemWidth(width);
  if (ImGui::BeginCombo(label, PointLabel(m, *cur).c_str(), ImGuiComboFlags_HeightLarge)) {
    for (int i = 0; i < n; ++i) {
      if (loadableOnly && !m.points[i].Loadable()) continue;
      bool sel = i == *cur;
      if (ImGui::Selectable(PointLabel(m, i).c_str(), sel)) {
        *cur = i;
        changed = true;
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// The player's own point list for the current mission: order, delete, rename (the picked point follows a moved
// row). Saved to DmCPT_missions.json next to the game exe at once.
void PointOrderEditor(int mi, nav::Selection& sel) {
  const nav::Mission& m = nav::Missions()[mi];
  std::string head = std::string("Edit points") + (nav::Edited(mi) ? "  - edited" : "") + "###order";
  if (g_forceDetails) ImGui::SetNextItemOpen(true);  // autorun screenshots
  if (!ImGui::CollapsingHeader(head.c_str())) return;
  ImGui::TextDisabled("Change the order, remove points you do not need, rename them. Saved at once.");
  int pt = nav::PointFor(sel, m.map);
  int n = (int)m.points.size();
  int moveFrom = -1, moveDir = 0, doDelete = -1;
  static char nameBuf[128];
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    bool fixed = i == 0;  // the mission start stays first
    ImGui::BeginDisabled(fixed || i == 1);
    if (ImGui::ArrowButton("up", ImGuiDir_Up)) moveFrom = i, moveDir = -1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(fixed || i == n - 1);
    if (ImGui::ArrowButton("down", ImGuiDir_Down)) moveFrom = i, moveDir = 1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(fixed);
    if (ImGui::SmallButton("Delete")) ImGui::OpenPopup("del");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Rename")) {
      snprintf(nameBuf, sizeof nameBuf, "%s", m.points[i].name.c_str());
      ImGui::OpenPopup("ren");
    }
    ImGui::SameLine();
    if (ImGui::Selectable(PointLabel(m, i).c_str(), i == pt)) {
      sel.point = i;
      sel.pointMap = m.map;
      nav::SetSel(sel);
    }
    if (ImGui::BeginPopup("del")) {
      ImGui::Text("Delete \"%s\" from the list?", m.points[i].name.c_str());
      if (ImGui::Button("Delete")) {
        doDelete = i;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("ren")) {
      ImGui::Text("New name for \"%s\" (%s):", m.points[i].name.c_str(), m.points[i].id.c_str());
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
      bool enter = ImGui::InputText("##name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
      if ((ImGui::Button("OK") || enter) && nameBuf[0]) {
        nav::RenamePoint(mi, i, nameBuf);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (doDelete > 0 && nav::DeletePoint(mi, doDelete)) {  // after the loop: the list gets shorter
    if (pt == doDelete) pt = 0;
    else if (pt > doDelete) --pt;
    sel.point = pt;
    sel.pointMap = m.map;
    nav::SetSel(sel);
  }
  if (moveFrom >= 0 && nav::MovePoint(mi, moveFrom, moveDir)) {
    if (pt == moveFrom) pt += moveDir;  // the picked point moves with its row
    else if (pt == moveFrom + moveDir) pt -= moveDir;
    sel.point = pt;
    sel.pointMap = m.map;
    nav::SetSel(sel);
  }
  if (ImGui::SmallButton("Rename mission")) {
    snprintf(nameBuf, sizeof nameBuf, "%s", m.name.c_str());
    ImGui::OpenPopup("renm");
  }
  if (ImGui::BeginPopup("renm")) {
    ImGui::Text("New name for \"%s\":", m.name.c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    bool enter = ImGui::InputText("##mname", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    if ((ImGui::Button("OK") || enter) && nameBuf[0]) {
      nav::RenameMission(mi, nameBuf);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!nav::Edited(mi));
  if (ImGui::SmallButton("Reset this mission")) {
    nav::ResetMission(mi);
    sel.point = 0;
    sel.pointMap = m.map;
    nav::SetSel(sel);
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Back to the built-in name, list and order of this mission (deleted points come back).");
}

// A group of the Practice tab: heading + one plain sentence of what it is for.
void GroupHead(const char* title, const char* hint) {
  ImGui::SeparatorText(title);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", hint);
  ImGui::PopTextWrapPos();
}

// Share checkpoints: export the player's list + created checkpoints as one file, use a friend's file.
std::wstring g_pendingImport;  // run at the start of the next frame (it replaces the mission list)
bool g_pendingDefault = false;  // same, back to the built-in list
std::string g_shareMsg;

void ShareBlock() {
  static std::vector<cpsets::SetInfo> sets;
  static bool scanned = false;
  static char name[64] = "My checkpoints";
  if (g_forceDetails) ImGui::SetNextItemOpen(true);  // autorun screenshots
  if (!ImGui::CollapsingHeader("Share checkpoints")) {
    scanned = false;
    return;
  }
  if (!scanned) {
    sets = cpsets::List();
    scanned = true;
  }
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("Give your checkpoint list (order, names, created checkpoints) to a friend as one file, or use "
                      "a file you got. Files are in the DmCPT_sets folder next to the game.");
  ImGui::PopTextWrapPos();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  ImGui::InputText("##setname", name, sizeof name);
  ImGui::SameLine();
  if (ImGui::Button("Save my list to a file")) {
    cpsets::Export(name, &g_shareMsg);
    scanned = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Open folder")) {
    CreateDirectoryW(cpsets::Dir().c_str(), nullptr);
    slots::OpenFolder(cpsets::Dir());
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh")) scanned = false;
  if (!g_shareMsg.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(g_shareMsg.rfind("Can't", 0) == 0 ? kRed : kGreen, "%s", g_shareMsg.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Text("In use:");
  ImGui::SameLine();
  ImGui::TextColored(kGreen, "%s", nav::ListName().c_str());
  // the built-in list: always there, to go back to
  bool isDefault = nav::ListName() == nav::kDefaultList && nav::CatalogSource() == "built in";
  ImGui::BeginDisabled(isDefault);
  if (ImGui::SmallButton("Use this list##default")) ImGui::OpenPopup("usedefault");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Text("%s", nav::kDefaultList);
  ImGui::SameLine();
  ImGui::TextDisabled("(built into the tool)");
  if (ImGui::BeginPopup("usedefault")) {
    ImGui::Text("Go back to the default checkpoint list?");
    ImGui::TextDisabled("Your edits to the list in use are lost unless you saved it to a file. Created checkpoints stay.");
    if (ImGui::Button("Use it")) {
      g_pendingDefault = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (sets.empty()) {
    ImGui::TextDisabled("No player lists yet. Put a .dmcptset file you got into the DmCPT_sets folder, then Refresh.");
    return;
  }
  for (size_t i = 0; i < sets.size(); ++i) {
    const cpsets::SetInfo& st = sets[i];
    ImGui::PushID((int)i);
    if (!st.ok) {
      ImGui::TextColored(kRed, "%s - can't use this file: %s", st.file.c_str(), st.error.c_str());
      ImGui::PopID();
      continue;
    }
    if (ImGui::SmallButton("Use this list")) ImGui::OpenPopup("use");
    ImGui::SameLine();
    ImGui::Text("%s", st.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d checkpoints, %d created%s%s", st.points, st.kept, st.created.empty() ? "" : ", ",
                        st.created.c_str());
    if (ImGui::BeginPopup("use")) {
      ImGui::Text("Use \"%s\"?", st.name.c_str());
      ImGui::TextDisabled("Your checkpoint list is replaced by this one (save yours to a file first if you want "
                          "to keep it). Its created checkpoints are added to yours.");
      if (ImGui::Button("Use it")) {
        g_pendingImport = st.path;
        scanned = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
}

// Practice tab: Mission select, Checkpoints, Saved positions, Share checkpoints.
void EnemiesBlock(float w) {
  GroupHead("Spawn", "Spawn any normal enemy to practise on, then clear them.");
  enemies::Snapshot es = enemies::Now();
  enemies::Selection sel = enemies::GetSel();
  bool changed = false;
  int cur = -1;
  for (size_t i = 0; i < es.kinds.size(); ++i)
    if (es.kinds[i].id == sel.kind) cur = (int)i;
  auto availOf = [&](int i) { return i >= 0 && i < (int)es.avail.size() ? es.avail[i] : enemies::Avail::Unknown; };
  std::string preview = cur >= 0 ? es.kinds[cur].name : sel.kind;
  ImGui::SetNextItemWidth(w);
  if (ImGui::BeginCombo("Enemy", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
    for (int i = 0; i < (int)es.kinds.size(); ++i) {
      enemies::Avail a = availOf(i);
      ImGui::PushID(i);
      if (a == enemies::Avail::Missing) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      if (ImGui::Selectable(es.kinds[i].name.c_str(), i == cur)) {
        sel.kind = es.kinds[i].id;
        changed = true;
      }
      if (a == enemies::Avail::Missing) ImGui::PopStyleColor();
      const char* t = enemies::AvailText(a);
      if (*t) {
        ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
        ImGui::TextDisabled("(%s)", t);
      }
      if (i == cur) ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  Help("All normal enemies of the game (bosses are not here: they live in their own levels). Grey ones cannot be "
       "loaded on this map. \"loads first\" = it takes a second or two before it appears.");
  ImGui::SetNextItemWidth(w);
  if (ImGui::SliderInt("How many", &sel.count, 1, 10)) changed = true;
  ImGui::AlignTextToFramePadding();
  ImGui::Text("Where:");
  ImGui::SameLine();
  if (ImGui::RadioButton("In front of Dante", sel.where == 0)) sel.where = 0, changed = true;
  ImGui::SameLine();
  if (ImGui::RadioButton("Saved position", sel.where == 1)) sel.where = 1, changed = true;
  Help("In front of Dante: where the camera looks, at the distance below.\nSaved position: at a position from "
       "Saved positions above (pick its slot).");
  ImGui::Indent();
  if (sel.where == 0) {
    ImGui::SetNextItemWidth(w * 0.8f);
    if (ImGui::SliderFloat("Distance", &sel.distance, 1.0f, 20.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp)) changed = true;
  } else {
    nav::Snapshot ns = nav::Now();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Slot:");
    for (int i = 0; i < nav::kPosSlots; ++i) {
      ImGui::SameLine();
      char b[32];
      snprintf(b, sizeof b, "%d%s##es", i + 1, ns.slot[i] ? " (saved)" : "");
      if (ImGui::RadioButton(b, sel.posSlot == i)) sel.posSlot = i, changed = true;
    }
  }
  ImGui::Unindent();
  if (changed) {
    enemies::SetSel(sel);
    MarkConfigDirty();
  }
  NavRow("enemy_spawn");
  if (!es.message.empty()) {
    ImGui::Indent();
    ImGui::TextDisabled("%s", es.message.c_str());
    ImGui::Unindent();
  }
  NavRow("enemy_kill_all");
  NavRow("enemy_despawn");
  if (es.mission) {
    ImGui::Indent();
    ImGui::TextDisabled("Enemies alive: %d (spawned by you: %d)", es.active, es.spawned);
    ImGui::Unindent();
  }
  GroupHead("Enemy health", "Make enemies unkillable training dummies, see their health.");
  NavRow("enemy_inf_hp");
  NavRow("enemy_show_hp");
}

// Enemies tab: spawn / kill / despawn, enemy health options, damage log.
void TabEnemies() {
  if (!FeaturesAllowed())
    ImGui::TextColored(kRed, "Unknown game version: all features are locked.");
  else if (enemies::Now().mission)
    ImGui::TextColored(kGreen, "In a mission (%s)", features::Status().map.c_str());
  else
    ImGui::TextDisabled("Spawning and the damage log work in a mission.");
  EnemiesBlock(ImGui::GetFontSize() * 20.0f);

  GroupHead("Damage log", "See every hit, its damage and the victim's health, also with the menu closed.");
  NavRow("damage_log");
}

// Visuals tab: collision wireframes (visuals.cpp) by category, then the options for all of them and the colours.
void LegendSwatch(uint32_t col, const char* text) {
  float h = ImGui::GetTextLineHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + h * 0.6f, p.y + h * 0.8f), col, 2.0f);
  ImGui::Dummy(ImVec2(h * 0.8f, h));
  ImGui::SameLine(0, 0);
  ImGui::TextUnformatted(text);
}

void TabVisuals() {
  StatusLine();
  GroupHead("Collisions", "What attacks are checked against: shapes on the characters' bodies.");
  NavRow("vis_enemy_collision");
  NavRow("vis_player_collision");
  GroupHead("Attacks", "Where attacks reach while they can hit, and where the game counted a hit.");
  NavRow("vis_hitboxes");
  NavRow("vis_hit_markers");
  GroupHead("Triggers", "Invisible areas of the level that start something when Dante walks in.");
  NavRow("vis_cutscene_triggers");
  NavRow("vis_arena_triggers");
  NavRow("vis_other_triggers");
  if (features::Get("vis_cutscene_triggers").applied || features::Get("vis_arena_triggers").applied ||
      features::Get("vis_other_triggers").applied) {
    visuals::Stats st = visuals::GetStats();
    ImGui::TextDisabled("In this part of the level: %d cutscene, %d arena, %d other.", st.triggers[0], st.triggers[1],
                        st.triggers[2]);
  }

  GroupHead("Walls", "Where Dante cannot go, also where nothing is visible.");
  NavRow("vis_invisible_walls");

  GroupHead("Options", "For all the shapes above.");
  visuals::Options o = visuals::GetOptions();
  bool ch = ImGui::Checkbox("Show labels", &o.labels);
  Help("Names next to the shapes: what a trigger starts (Arena, Cutscene) and the name of an attack.");
  float w = ImGui::GetFontSize() * 14.0f;
  ImGui::SetNextItemWidth(w);
  ch |= ImGui::SliderFloat("Max distance", &o.maxDistance, 5.0f, 200.0f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
  Help("Shapes farther from the camera are not drawn (triggers are always drawn). Lower it when the screen gets too "
       "busy. Ctrl+click to type a number.");
  ImGui::SetNextItemWidth(w);
  ch |= ImGui::SliderFloat("Line width", &o.thickness, 1.0f, 5.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
  Help("How thick the lines are.");
  ch |= ImGui::Checkbox("Hide behind walls", &o.occlude);
  Help("Triggers and invisible walls are not drawn where the level's walls, floors and objects hide them, so you "
       "see only those in front of you. Off: they show through everything.");
  if (o.occlude && !visuals::OcclusionLive() && features::Status().mission) {
    ImGui::SameLine();
    ImGui::TextDisabled("(not available right now)");
  }
  if (ch) {
    visuals::SetOptions(o);
    MarkConfigDirty();
  }

  ImGui::SeparatorText("Colours");
  using visuals::Cat;
  if (ImGui::BeginTable("legend", 2)) {
    struct Item {
      uint32_t col;
      const char* text;
    };
    const Item items[] = {{visuals::Color(Cat::Enemy), "Enemy body"},       {visuals::Color(Cat::Player), "Dante's body"},
                          {visuals::Color(Cat::Hitbox), "Dante's attacks"},  {visuals::EnemyHitboxColor(), "Enemy attacks"},
                          {visuals::Color(Cat::HitMarker), "Hit by Dante"},  {visuals::HitOnDanteColor(), "Hit on Dante"},
                          {visuals::Color(Cat::Arena), "Arena trigger"},     {visuals::Color(Cat::Cutscene), "Cutscene trigger"},
                          {visuals::Color(Cat::OtherTrigger), "Other trigger"},
                          {visuals::Color(Cat::Walls), "Invisible wall"}};
    for (const Item& it : items) {
      ImGui::TableNextColumn();
      LegendSwatch(it.col, it.text);
    }
    ImGui::EndTable();
  }
}

void TabPractice() {
  if (!g_pendingImport.empty()) {  // before any reference into the mission list is taken this frame
    cpsets::Import(g_pendingImport, &g_shareMsg);
    g_pendingImport.clear();
  }
  if (g_pendingDefault) {
    nav::UseBuiltin();
    g_shareMsg = std::string("Now using \"") + nav::kDefaultList + "\".";
    g_pendingDefault = false;
  }
  nav::Snapshot s = nav::Now();
  nav::Selection sel = nav::GetSel();
  const auto& ms = nav::Missions();
  float w = ImGui::GetFontSize() * 20.0f;
  if (!FeaturesAllowed())
    ImGui::TextColored(kRed, "Unknown game version: all features are locked.");
  else if (s.mission && s.missionIndex >= 0)
    ImGui::TextColored(kGreen, "%s  -  %s", ms[s.missionIndex].name.c_str(), nav::DifficultyName(s.difficulty));
  else if (s.mission)
    ImGui::TextColored(kGreen, "In a level that is not a mission (%s)", s.map.c_str());
  else if (s.frontEnd)
    ImGui::TextDisabled("Main menu: Mission select works here, the rest in a mission.");
  else
    ImGui::TextDisabled("Loading...");

  // ---- mission select
  GroupHead("Mission select", "Start any mission on any difficulty, from a mission or from the main menu.");
  if (!ms.empty()) {
    if (sel.selMission < 0 || sel.selMission >= (int)ms.size()) sel.selMission = 0;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Mission", ms[sel.selMission].name.c_str(), ImGuiComboFlags_HeightLarge)) {
      for (int i = 0; i < (int)ms.size(); ++i) {
        if (i > 0 && ms[i].secret && !ms[i - 1].secret) ImGui::Separator();
        if (ImGui::Selectable(ms[i].name.c_str(), i == sel.selMission)) {
          sel.selMission = i;
          sel.selPoint = 0;
          nav::SetSel(sel);
        }
        if (i == sel.selMission) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    const nav::Mission& m = ms[sel.selMission];
    ImGui::BeginDisabled(m.secret);
    ImGui::SetNextItemWidth(w);
    int d = sel.selDifficulty;
    if (ImGui::Combo("Difficulty", &d, [](void*, int i) { return nav::DifficultyName(i); }, nullptr,
                     nav::kDifficulties)) {
      sel.selDifficulty = d;
      nav::SetSel(sel);
    }
    ImGui::EndDisabled();
    if (m.secret) {
      ImGui::SameLine();
      ImGui::TextDisabled("(set by the secret mission)");
    } else {
      Help("Any of the 7 difficulties, also ones you have not unlocked yet. Loading does not unlock anything in "
           "your save.");
    }
    int mp = sel.selPoint;
    if (PointCombo("Checkpoint##ms", m, &mp, true, w)) {
      sel.selPoint = mp;
      nav::SetSel(sel);
    }
    NavRow("nav_mission");
  }

  // ---- checkpoints of the current mission
  GroupHead("Checkpoints", "Jump to any checkpoint of the mission you are in.");
  const nav::Mission* cur = s.missionIndex >= 0 ? &ms[s.missionIndex] : nullptr;
  if (cur) {
    // a checkpoint just created that is not in the list goes in after the picked one (and gets picked)
    realcp::Created made;
    auto addPoint = [&](const std::string& cpName, const realcp::Created* at) {
      nav::Point np;
      np.id = np.cp = cpName;
      np.name = nav::NiceName(cpName);
      np.kind = nav::Kind::Checkpoint;
      if (at && at->hasPos) {
        np.hasPos = true;
        np.pos = {at->x, at->y, at->z};
        np.yaw = at->yaw;
      }
      int idx = nav::AddPoint(s.missionIndex, np, nav::PointFor(sel, cur->map));
      if (idx >= 0) {
        sel.point = idx;
        sel.pointMap = cur->map;
        nav::SetSel(sel);
      }
    };
    if (realcp::TakeCreated(made) && _stricmp(made.map.c_str(), cur->map.c_str()) == 0 && nav::FindPoint(s.missionIndex, made.name) < 0)
      addPoint(made.name, &made);
    int pt = nav::PointFor(sel, cur->map);
    if (PointCombo("Checkpoint##cur", *cur, &pt, false, w)) {
      sel.point = pt;
      sel.pointMap = cur->map;
      nav::SetSel(sel);
    }
    Help("Checkpoints of this mission in playing order. Load checkpoint and Teleport to point use the one picked "
         "here (also from a key or a preset).");
    if (pt >= (int)cur->points.size()) pt = 0;
    const nav::Point& p = cur->points[pt];
    NavRow("nav_checkpoint", p.Loadable() ? "" : "pick a checkpoint, this point is for Teleport");
    realcp::Entry ke;
    if (!p.cp.empty() && realcp::Find(cur->map, p.cp, &ke)) {
      ImGui::Indent();
      ImGui::TextColored(kGreen, "Your created checkpoint (%s, %s): loads exactly like Continue.",
                         nav::DifficultyName(ke.difficulty), ke.saved.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) realcp::Forget(cur->map, p.cp);
      ImGui::SetItemTooltip("Delete your created checkpoint: this checkpoint loads the normal way again.");
      ImGui::Unindent();
    }
    NavRow("nav_teleport", p.hasPos ? "" : "no position known for this point");
    NavRow("nav_restart");
    NavRow("nav_keep");
    if (ImGui::Checkbox("Use created checkpoints", &sel.useReal)) nav::SetSel(sel);
    Help("On: checkpoints marked [created] load from your copy (like Continue, on the difficulty it was saved "
         "on). Off: every checkpoint loads the normal way.");
    std::string msg = realcp::Message();
    if (!msg.empty()) ImGui::TextDisabled("%s", msg.c_str());
    // created earlier but not in the list (deleted from it, or from a friend's set)
    for (const realcp::Entry& e : realcp::List()) {
      if (_stricmp(e.map.c_str(), cur->map.c_str()) != 0 || nav::FindPoint(s.missionIndex, e.name) >= 0) continue;
      ImGui::PushID(e.name.c_str());
      ImGui::TextDisabled("Your created checkpoint \"%s\" is not in the list.", nav::NiceName(e.name).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Add")) addPoint(e.name, nullptr);
      ImGui::SetItemTooltip("Put it into the list after the picked checkpoint.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) realcp::Forget(e.map, e.name);
      ImGui::SetItemTooltip("Delete this created checkpoint.");
      ImGui::PopID();
    }
    PointOrderEditor(s.missionIndex, sel);
  } else {
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Checkpoint##cur", "(in a mission only)")) ImGui::EndCombo();
    ImGui::EndDisabled();
    NavRow("nav_checkpoint");
    NavRow("nav_teleport");
    NavRow("nav_restart");
    NavRow("nav_keep");
  }

  // ---- saved positions
  GroupHead("Saved positions", "Remember where Dante stands and jump back there. Only Dante moves, enemies stay "
                               "as they are.");
  ImGui::AlignTextToFramePadding();
  ImGui::Text("Slot:");
  for (int i = 0; i < nav::kPosSlots; ++i) {
    ImGui::SameLine();
    char b[32];
    snprintf(b, sizeof b, "%d%s", i + 1, s.slot[i] ? " (saved)" : "");
    if (ImGui::RadioButton(b, sel.slot == i)) {
      sel.slot = i;
      nav::SetSel(sel);
    }
  }
  Help("Save position and Load position use this slot. Slots are kept until you leave the map.");
  NavRow("nav_save_pos");
  NavRow("nav_load_pos");

  // ---- share
  ImGui::Spacing();
  ShareBlock();
}

// ------------------------------------------------------------------ Hotkeys tab
void TabHotkeys() {
  ImGui::TextWrapped("Give any feature a key or a gamepad button combination. Pressing it turns the feature on, "
                     "pressing it again turns it off. Hotkeys do nothing while this menu is open.");
  ImGui::SeparatorText("Features");
  const auto& all = features::All();
  if (ImGui::BeginTable("binds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    auto lk = features::Lock();
    for (size_t i = 0; i < all.size(); ++i) {
      if (all[i]->noBind) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%s  ", all[i]->name);
      ImGui::SameLine();
      ImGui::TextDisabled("%s", features::GroupName(all[i]->group));
      ImGui::TableNextColumn();
      BindWidget(&features::D().binds[i], all[i]->id);
    }
    ImGui::EndTable();
  }

  ImGui::SeparatorText("Presets");
  ImGui::TextWrapped("A preset turns several features on with one key, with its own settings (for example F5 = "
                     "Infinite HP + Infinite Glide + Noclip). Press once = on, press again = off. Buttons from the Practice "
                     "tab (Load checkpoint, Load position...) run once when the preset turns on. A feature works "
                     "if you ticked it OR an active preset has it; turning a preset off never unticks what you "
                     "ticked yourself.");
  auto lk = features::Lock();
  features::Data& d = features::D();
  int removeAt = -1, toggleAt = -1;
  for (size_t pi = 0; pi < d.presets.size(); ++pi) {
    features::Preset& ps = d.presets[pi];
    ImGui::PushID((int)pi);
    std::string header = (ps.active ? "[ON]  " : "") + ps.name + "###preset";
    if (ps.active) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
    bool open = ImGui::CollapsingHeader(header.c_str());
    if (ps.active) ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 13.0f);
    BindWidget(&ps.bind, "pbind");
    if (open) {
      ImGui::Indent();
      char name[64];
      snprintf(name, sizeof name, "%s", ps.name.c_str());
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
      if (ImGui::InputText("Name", name, sizeof name) && name[0]) {
        ps.name = name;
        features::MarkDirty();
      }
      ImGui::SameLine();
      if (ImGui::Button(ps.active ? "Turn off" : "Turn on")) toggleAt = (int)pi;
      ImGui::SameLine();
      if (ImGui::Button("Delete preset")) removeAt = (int)pi;
      for (size_t i = 0; i < all.size(); ++i) {
        features::Feature* f = all[i];
        if (f->noBind) continue;
        ImGui::PushID(f->id);
        auto it = std::find_if(ps.items.begin(), ps.items.end(), [&](const features::PresetItem& x) { return x.id == f->id; });
        bool in = it != ps.items.end();
        if (ImGui::Checkbox(f->name, &in)) {
          if (in) {
            features::PresetItem item;
            item.id = f->id;
            for (size_t k = 0; k < f->params.size() && k < (size_t)features::kMaxParams; ++k) item.p[k] = f->params[k].def;
            ps.items.push_back(item);
          } else {
            ps.items.erase(it);
          }
          features::MarkDirty();
          it = std::find_if(ps.items.begin(), ps.items.end(), [&](const features::PresetItem& x) { return x.id == f->id; });
        }
        if (in && it != ps.items.end())
          for (size_t k = 0; k < f->params.size() && k < (size_t)features::kMaxParams; ++k) {
            const features::ParamDef& pd = f->params[k];
            ImGui::SameLine();
            ImGui::PushID((int)k);
            if (ParamWidget(pd, &it->p[k], ImGui::GetFontSize() * 9.0f)) features::MarkDirty();
            ImGui::PopID();
          }
        if (f->action) {
          ImGui::SameLine();
          ImGui::TextDisabled("(runs once when the preset turns on)");
        }
        ImGui::PopID();
      }
      ImGui::Unindent();
    }
    ImGui::PopID();
  }
  if (toggleAt >= 0) features::TogglePreset(toggleAt);
  if (removeAt >= 0) {
    if (hotkeys::CaptureTarget() == &d.presets[removeAt].bind) hotkeys::CancelCapture();
    d.presets.erase(d.presets.begin() + removeAt);
    features::MarkDirty();
  }
  if (ImGui::Button("+ New preset")) {
    if (hotkeys::Capturing()) hotkeys::CancelCapture();  // the vector may move
    features::Preset p;
    p.name = "Preset " + std::to_string(d.presets.size() + 1);
    d.presets.push_back(p);
    features::MarkDirty();
  }
}

// ------------------------------------------------------------------ Settings tab
void TabSettings() {
  ImGui::SeparatorText("Menu");
  ImGui::Text("Open/close the menu:");
  ImGui::SameLine();
  static hotkeys::Bind menuBind;
  static bool waiting = false;
  bool capturing = hotkeys::CaptureTarget() == &menuBind;
  if (waiting && !capturing) {  // capture finished or cancelled
    waiting = false;
    if (menuBind.vk && menuBind.vk != g_cfg.menuKey) {
      g_cfg.menuKey = menuBind.vk;
      MarkConfigDirty();
      LOG("menu: menu key is now %s", input::KeyName(g_cfg.menuKey));
    }
  }
  if (capturing)
    ImGui::TextColored(kYellow, "press a key (Esc = cancel)");
  else
    ImGui::Text("[%s]", input::KeyName(g_cfg.menuKey));
  ImGui::SameLine();
  if (ImGui::SmallButton(capturing ? "Cancel" : "Change")) {
    if (capturing) {
      hotkeys::CancelCapture();
    } else {
      menuBind = {};
      waiting = true;
      hotkeys::BeginCapture(&menuBind, true);
    }
  }
  if (ImGui::Button("Reset window size and position")) {
    g_cfg.winW = 0;
    MarkConfigDirty();
  }

  ImGui::SeparatorText("Config");
  ImGui::TextDisabled("Hotkeys, slider values and presets are saved automatically to DmCPracticeTool.json.");
  if (ImGui::Button("Save config")) SaveConfig();
  ImGui::SameLine();
  if (ImGui::Button("Load config")) LoadConfig();

  ImGui::SeparatorText("Other");
  if (ImGui::Checkbox("Show Dev tab", &g_cfg.showDevTab)) MarkConfigDirty();
  Help("Tools for the developers of this tool: data dumps, logs, test calls.");

  ImGui::SeparatorText("Compatibility report");
  ImGui::TextDisabled(FeaturesAllowed() ? "Only needed if the tool stops working after a game update."
                                        : "Your game version is not supported yet. Send us this report and we can add it.");
  ImGui::BeginDisabled(compat::Busy());
  if (ImGui::Button("Create report")) compat::CreateReportAsync();
  ImGui::EndDisabled();
  Help("Writes DmCPT_compat_report.txt next to the game exe: which game version you have and what the tool can "
       "find in it. Only reads, changes nothing.");
  ImGui::SameLine();
  if (ImGui::Button("Open folder")) slots::OpenFolder(util::ExeDir());
  std::string res = compat::LastResult();
  if (!res.empty()) ImGui::TextColored(compat::Busy() ? kYellow : kGreen, "%s", res.c_str());

  ImGui::SeparatorText("About");
  const VersionInfo& v = GetGameVersion();
  ImGui::Text("DmC Practice Tool v%s", kToolVersion);
  ImGui::TextDisabled("Game version: %s%s", v.label, FeaturesAllowed() ? "" : " (not supported: features locked)");
  ImGui::TextDisabled("github.com/billymooreq/DmC-DevilMayCry-Practice-tool");
}

// "Save Unlocks": game functions on the game thread, main menu only, backup + confirmation first.
void TabUnlocks() {
  ImGui::Text("Save Unlocks");
  unlock::Status st = unlock::CurrentStatus();
  bool busy = unlock::Busy();
  bool canRun = FeaturesAllowed() && st.state == unlock::State::Ready && !busy;
  ImVec4 col = st.state == unlock::State::Ready ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.8f, 0.2f, 1);
  ImGui::SameLine();
  ImGui::TextColored(col, "[%s]", unlock::StateText(st.state));
  // Why the buttons are disabled (or what happens when pressed).
  if (!FeaturesAllowed())
    ImGui::TextDisabled("Disabled: unknown game build.");
  else if (busy)
    ImGui::TextDisabled("Disabled: an unlock is running.");
  else if (st.state == unlock::State::Ready)
    ImGui::TextDisabled("Save loaded, main menu. A backup of your save is made automatically before any change.");
  else if (st.state == unlock::State::InMission)
    ImGui::TextDisabled("Disabled: works only in the main menu (now in %s). Quit to the main menu.", st.map.c_str());
  else if (st.state == unlock::State::NoSave)
    ImGui::TextDisabled("Disabled: choose your save slot in the main menu first (%s).", st.detail.c_str());
  else
    ImGui::TextDisabled("Disabled: %s (%s).", unlock::StateText(st.state), st.detail.c_str());

  static std::vector<unlock::Step> pending;
  static const char* pendingWhat = "";
  bool open = false;
  ImGui::BeginDisabled(!canRun);
  if (ImGui::Button("UNLOCK ALL")) {
    pending.clear();
    for (int s = 0; s < unlock::StepCount; ++s) pending.push_back((unlock::Step)s);
    pendingWhat =
        "everything, like a 100% save: all missions on all difficulties, story weapons & abilities,\n"
        "secret missions (completed), keys, lost souls, skills, max HP/DT, costumes, concept art.\n"
        "Mission ranks and scores are not changed.";
    open = true;
  }
  for (int s = 0; s < unlock::StepCount; ++s) {
    if (s % 4 != 0) ImGui::SameLine();
    if (ImGui::Button(unlock::StepLabel((unlock::Step)s))) {
      pending = {(unlock::Step)s};
      pendingWhat = unlock::StepLabel((unlock::Step)s);
      open = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Save now")) {
    pending.clear();
    pendingWhat = "nothing new - only save the current state";
    open = true;
  }
  bool openSSS = ImGui::Button("Set all ranks SSS...");
  ImGui::SameLine();
  ImGui::TextDisabled("(replaces your mission ranks)");
  ImGui::EndDisabled();
  if (open) ImGui::OpenPopup("Confirm unlock");
  if (openSSS) ImGui::OpenPopup("Confirm ranks SSS");
  if (ImGui::BeginPopupModal("Confirm ranks SSS", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Set SSS on every completed mission (all difficulties) and every secret mission?");
    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Your own ranks are overwritten. Scores and times stay as they are.");
    ImGui::Text("A backup copy of your save is made first (DmCPT_backups).");
    if (ImGui::Button("Yes, set SSS and save", ImVec2(200, 0))) {
      unlock::Run({unlock::RanksSSS}, true);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("Confirm unlock", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Unlock: %s", pendingWhat);
    ImGui::Text("This changes your save. A backup copy is made first (DmCPT_backups).");
    ImGui::Text("Steam achievements are blocked while it runs.");
    if (ImGui::Button("Yes, unlock and save", ImVec2(200, 0))) {
      unlock::Run(pending, true);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (unlock::Busy()) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "working...");
  for (const std::string& l : unlock::Report()) {
    bool fail = l.rfind("FAIL", 0) == 0;
    if (fail)
      ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", l.c_str());
    else
      ImGui::TextWrapped("%s", l.c_str());
  }
}

std::string SlotLabel(const slots::SlotInfo& s) {
  if (!s.used) return "Slot " + std::to_string(s.index + 1) + " - empty";
  if (!s.error.empty()) return "Slot " + std::to_string(s.index + 1) + " - unreadable";
  return "Slot " + std::to_string(s.index + 1) + " - " + s.sum.OneLine();
}

// Everything that is not needed to pick a save: time played, skills, HP/DT, orbs, versions, hashes...
// Collapsed by default.
std::atomic<int> g_popupRename{-1}, g_popupImport{-1};
std::atomic<bool> g_popupClose{false};

void SaveDetails(const char* id, const savefile::Summary& sum, const std::vector<std::string>& extra) {
  ImGui::PushID(id);
  if (g_forceDetails) ImGui::SetNextItemOpen(true);
  if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    for (const std::string& l : savefile::SummaryLines(sum)) ImGui::TextWrapped("%s", l.c_str());
    for (const std::string& l : extra) ImGui::TextWrapped("%s", l.c_str());
    ImGui::PopStyleColor();
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void ReportLines(const std::vector<std::string>& lines) {
  for (const std::string& l : lines) {
    if (l.rfind("FAIL", 0) == 0)
      ImGui::TextColored(kRed, "%s", l.c_str());
    else
      ImGui::TextWrapped("%s", l.c_str());
  }
}

// Saves tab: Import (a save someone sent you) on top, Export (share one of yours) below. The list shows only
// what is needed to choose: name, notes, mission + difficulty + checkpoint; the rest is under "Details".
void TabSave() {
  static bool first = true;
  if (first) {
    slots::Refresh();
    first = false;
  }
  unlock::Status st = unlock::CurrentStatus();
  bool busy = slots::Busy() || unlock::Busy();
  int active = slots::ActiveSlot();
  std::vector<slots::SlotInfo> sl = slots::Slots();
  float em = ImGui::GetFontSize();

  // ------------------------------------------------------------------ import
  ImGui::SeparatorText("Import: load a save someone sent you");
  std::vector<slots::ImportEntry> im = slots::Imports();
  bool inMenu = st.state == unlock::State::Ready;
  if (!inMenu)
    ImGui::TextColored(kYellow, "Loading a save works in the main menu after you pick a save slot (now: %s).",
                       unlock::StateText(st.state));
  if (im.empty()) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Put .dmcpt files here:");
    ImGui::SameLine();
  }
  if (ImGui::Button("Open import folder")) slots::OpenFolder(slots::ImportDir());
  ImGui::SameLine();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Refresh")) slots::Refresh();
  ImGui::EndDisabled();
  if (im.empty()) ImGui::TextDisabled("Then press Refresh. Files from friends usually come as .dmcpt.");
  static int importAt = -1, renameAt = -1;
  static char newName[96] = {};
  static int sel = 0;
  if (g_popupImport >= 0 && g_popupImport < (int)im.size()) sel = g_popupImport;  // autorun: select it first
  if (g_popupRename >= 0 && g_popupRename < (int)im.size()) sel = g_popupRename;
  if (sel >= (int)im.size()) sel = 0;
  if (!im.empty()) {
    // One line per save (name + mission/difficulty), so a long list stays short; the chosen one is shown below.
    // The list is as tall as the player dragged it (bottom edge), remembered in the config in text lines.
    const ImGuiStyle& sty = ImGui::GetStyle();
    float line = ImGui::GetTextLineHeightWithSpacing(), pad = sty.FramePadding.y * 2;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, line * 2 + pad), ImVec2(FLT_MAX, line * 40 + pad));
    if (ImGui::BeginChild("##imports", ImVec2(-1, line * g_cfg.savesListRows + pad),
                          ImGuiChildFlags_FrameStyle | ImGuiChildFlags_ResizeY)) {
      for (int j = 0; j < (int)im.size(); ++j) {
        const slots::ImportEntry& r = im[j];
        ImGui::PushID(j);
        if (!r.ok) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        if (ImGui::Selectable(r.name.c_str(), j == sel)) sel = j;
        if (!r.ok) ImGui::PopStyleColor();
        std::string brief = r.ok ? r.sum.Brief() : "can't load";
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(brief.c_str()).x - sty.ItemSpacing.x);
        ImGui::TextDisabled("%s", brief.c_str());
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    float rows = std::round((ImGui::GetItemRectSize().y - pad) / line * 2) / 2;  // half-line steps
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::fabs(rows - g_cfg.savesListRows) >= 0.5f) {
      g_cfg.savesListRows = std::min(40.0f, std::max(2.0f, rows));
      MarkConfigDirty();
    }
    if (ImGui::IsItemHovered() && im.size() > 3) ImGui::SetItemTooltip("Drag the bottom edge to make the list taller.");
  }
  if (sel < (int)im.size()) {
    const int i = sel;
    const slots::ImportEntry& e = im[i];
    ImGui::PushID(i);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(e.ok ? ImGui::GetStyle().Colors[ImGuiCol_Text] : kRed, "%s", e.name.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - em * 11.5f);
    ImGui::BeginDisabled(!e.ok || busy || !inMenu || !FeaturesAllowed());
    if (ImGui::Button("Import...") || (g_popupImport == i && g_popupImport.exchange(-1) == i)) {
      importAt = i;
      ImGui::OpenPopup("Import save");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!e.ok || busy);
    if (ImGui::Button("Rename") || (g_popupRename == i && g_popupRename.exchange(-1) == i)) {
      renameAt = i;
      snprintf(newName, sizeof newName, "%s", e.name.c_str());
      ImGui::OpenPopup("Rename save");
    }
    ImGui::EndDisabled();
    if (!e.notes.empty()) ImGui::TextWrapped("%s", e.notes.c_str());
    if (e.ok) ImGui::TextDisabled("%s", e.sum.OneLine().c_str());
    if (!e.ok)
      ImGui::TextColored(kRed, "Can't load this file: %s", e.error.c_str());
    else if (!e.error.empty())
      ImGui::TextColored(kYellow, "%s", e.error.c_str());
    std::vector<std::string> extra;
    extra.push_back("File: " + e.display);
    if (!e.exportedAt.empty())
      extra.push_back("Exported " + e.exportedAt + " with tool " + e.toolVersion + ", game " + VersionLabel(e.gameVersion) +
                      " (" + e.gameVersion + ")");
    if (e.rawSlot >= 0) {
      extra.push_back("Plain game save files (not a .dmcpt package), Steam account " + std::to_string(e.rawAccount) +
                      ". Rename packs them into a .dmcpt file.");
    }
    if (e.ok && e.infoRebuilt) extra.push_back("The text on the save slot screen is rebuilt for your game version.");
    if (e.ok) SaveDetails("d", e.sum, extra);

    // ---- popups (inside the row's ID scope)
    if (ImGui::BeginPopupModal("Import save", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (g_popupClose.exchange(false)) ImGui::CloseCurrentPopup();
      static int target = -1;
      int n = slots::NumSlots();
      if (target < 0 || target >= n) target = n > 0 ? n - 1 : 0;
      auto targetLabel = [&](int t) {
        return t < (int)sl.size() ? SlotLabel(sl[t]) + (t == active ? "  (in use now)" : "")
                                  : "Slot " + std::to_string(t + 1) + " - empty";
      };
      ImGui::Text("Load \"%s\"", e.name.c_str());
      ImGui::Text("into");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(em * 30);
      if (ImGui::BeginCombo("##target", targetLabel(target).c_str())) {
        for (int t = 0; t < n; ++t)
          if (ImGui::Selectable(targetLabel(t).c_str(), t == target)) target = t;
        ImGui::EndCombo();
      }
      bool used = target < (int)sl.size() && sl[target].used;
      if (used)
        ImGui::TextColored(kYellow, "Slot %d is not empty. What is in it now will be replaced.", target + 1);
      ImGui::TextDisabled("A backup copy of all your saves is made first.");
      if (ImGui::Button(used ? "Replace and load" : "Load", ImVec2(em * 11, 0))) {
        slots::Import(importAt, target);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(em * 7, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Rename save", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (g_popupClose.exchange(false)) ImGui::CloseCurrentPopup();
      ImGui::Text("New name:");
      ImGui::SetNextItemWidth(em * 24);
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      bool enter = ImGui::InputText("##newname", newName, sizeof newName, ImGuiInputTextFlags_EnterReturnsTrue);
      if (e.rawSlot >= 0) ImGui::TextDisabled("These are plain save files: they will be packed into a .dmcpt file.");
      ImGui::BeginDisabled(!newName[0]);
      if (ImGui::Button("Rename", ImVec2(em * 7, 0)) || (enter && newName[0])) {
        slots::Rename(renameAt, newName);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(em * 7, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }

  // ------------------------------------------------------------------ export
  ImGui::Spacing();
  ImGui::SeparatorText("Export: share one of your saves");
  static int expSlot = -1, nameFor = -2;
  static char name[96] = {};
  static char notes[512] = {};
  static bool saveFirst = true;
  if (expSlot < 0 || expSlot >= (int)sl.size() || !sl[expSlot].used) {
    expSlot = -1;
    for (const auto& s : sl)
      if (s.used && (expSlot < 0 || s.index == active)) expSlot = s.index;
  }
  auto slotLabel = [&](const slots::SlotInfo& s) { return SlotLabel(s) + (s.index == active ? "  (in use now)" : ""); };
  ImGui::Text("Save");
  ImGui::SameLine(em * 4.5f);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##expslot", expSlot >= 0 ? slotLabel(sl[expSlot]).c_str() : "no saves found")) {
    for (const auto& s : sl)
      if (s.used && ImGui::Selectable(slotLabel(s).c_str(), s.index == expSlot)) expSlot = s.index;
    ImGui::EndCombo();
  }
  // The name follows the chosen slot until the player types their own.
  if (expSlot >= 0 && expSlot != nameFor) {
    snprintf(name, sizeof name, "%s", sl[expSlot].sum.DefaultName().c_str());
    nameFor = expSlot;
  }
  ImGui::Text("Name");
  ImGui::SameLine(em * 4.5f);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##expname", name, sizeof name);
  ImGui::Text("Notes");
  ImGui::SameLine(em * 4.5f);
  ImGui::InputTextMultiline("##notes", notes, sizeof notes, ImVec2(-1, ImGui::GetTextLineHeight() * 3));
  if (ImGui::IsItemHovered() && !notes[0]) ImGui::SetTooltip("For example: \"M12 boss, DMD, no Kablooey upgrades\"");
  if (expSlot >= 0 && !sl[expSlot].error.empty())
    ImGui::TextColored(kRed, "This slot can't be read: %s", sl[expSlot].error.c_str());
  else if (expSlot >= 0)
    SaveDetails("exp", sl[expSlot].sum, {});
  ImGui::Checkbox("Save the game first (main menu, the slot in use)", &saveFirst);
  if (st.state == unlock::State::InMission && expSlot == active)
    ImGui::TextColored(kYellow, "You are in a mission: the last checkpoint is exported.");
  ImGui::BeginDisabled(busy || expSlot < 0 || !name[0]);
  if (ImGui::Button("Export")) slots::Export(expSlot, name, notes, saveFirst);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Open export folder")) slots::OpenFolder(slots::ExportDir());

  ImGui::Spacing();
  if (slots::Busy()) ImGui::TextColored(kYellow, "working...");
  ReportLines(slots::Report());
  g_popupClose = false;  // only for a window that is open now
}

void TabLog();

void TabDev() {
  const VersionInfo& gv = GetGameVersion();
  ImGui::Text("Tool v%s. Game build: %s = %s (TimeDateStamp 0x%08X, SizeOfImage 0x%08X)", kToolVersion, gv.name,
              gv.label, gv.timeDateStamp, gv.sizeOfImage);
  ImGui::TextWrapped("UE3: %s", ue3::StatusText().c_str());
  ImGui::TextWrapped("Visuals: %s", visuals::DebugLines("").front().c_str());
  {
    static std::string navMsg;
    size_t pts = 0;
    for (const nav::Mission& m : nav::Missions()) pts += m.points.size();
    ImGui::Text("Mission catalog: %s (%zu missions, %zu points)", nav::CatalogSource().c_str(), nav::Missions().size(), pts);
    ImGui::SameLine();
    if (ImGui::SmallButton("Export for editing")) nav::ExportCatalog(&navMsg);
    ImGui::SetItemTooltip("Writes the built-in catalog to DmCPT_missions.json next to the game exe. Edit names / "
                          "order there; it is used instead of the built-in one from the next game start.");
    if (!navMsg.empty()) ImGui::TextDisabled("%s", navMsg.c_str());
  }
  bool busy = dumper::Busy();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Dump Names")) dumper::RunAsync(dumper::Kind::Names);
  ImGui::SameLine();
  if (ImGui::Button("Dump Objects")) dumper::RunAsync(dumper::Kind::Objects);
  ImGui::SameLine();
  if (ImGui::Button("Dump Classes")) dumper::RunAsync(dumper::Kind::Classes);
  ImGui::SameLine();
  if (ImGui::Button("Dump Profile (live)")) dumper::RunAsync(dumper::Kind::Profile);
  if (ImGui::Button("Layout + signatures")) dumper::RunAsync(dumper::Kind::Layout);
  ImGui::SameLine();
  if (ImGui::Button("Rescan UE3")) {
    ue3::Reset();
    dumper::RunAsync(dumper::Kind::Layout);
  }
  ImGui::SameLine();
  if (ImGui::Button("Dump Scripts")) dumper::RunAsync(dumper::Kind::Scripts);
  ImGui::EndDisabled();
  ImGui::SliderInt("Array element limit", &g_cfg.dumpArrayLimit, 16, 4096);
  ImGui::TextWrapped("%s%s", busy ? "[working] " : "", dumper::LastResult().c_str());
  ImGui::TextDisabled("Output: Binaries\\Win32\\DmCPT_dumps\\");
  if (ue3::Ready() && ImGui::CollapsingHeader("Layout")) ImGui::TextUnformatted(ue3::LayoutReport().c_str());

  ImGui::Separator();
  ImGui::Text("ProcessEvent hook: %s   game-thread queue: %zu   Steam guard blocked: %d%s",
              engine::Hooked() ? "on" : "OFF", engine::Pending(), steam::BlockedCount(),
              steam::Blocking() ? "  [BLOCKING]" : "");
  static char filter[512] = {};
  static bool filterInit = false;
  if (!filterInit) {
    snprintf(filter, sizeof filter, "%s", g_cfg.peLogFilter.c_str());
    filterInit = true;
  }
  bool changed = ImGui::Checkbox("Log ProcessEvent calls", &g_cfg.peLog);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  changed |= ImGui::InputText("##pefilter", filter, sizeof filter);
  if (changed) {
    g_cfg.peLogFilter = filter;
    engine::SetPeLog(g_cfg.peLog, g_cfg.peLogFilter);
  }
  ImGui::BeginDisabled(unlock::Busy());
  if (ImGui::Button("Test calls (read-only)")) unlock::RunTestCalls();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Backup saves now")) {
    std::string err;
    std::wstring dst = steam::BackupSaves("manual", &err);
    LOG("menu: backup %s", dst.empty() ? ("FAILED: " + err).c_str() : util::WideToUtf8(dst).c_str());
  }
  for (const std::string& l : unlock::Report()) ImGui::TextWrapped("%s", l.c_str());

  ImGui::SeparatorText("Features");
  ImGui::TextUnformatted(features::DebugText().c_str());
  if (ImGui::CollapsingHeader("Hooks (applied after game restart)")) {
    ImGui::Checkbox("D3D9 hook (menu + overlay)", &g_cfg.hookD3D9);
    ImGui::Text("Draw in:");
    for (const char* m : {"vtable", "present", "endscene"}) {
      ImGui::SameLine();
      if (ImGui::RadioButton(m, g_cfg.renderHook == m)) g_cfg.renderHook = m;
    }
    ImGui::Checkbox("WndProc hook (menu mouse/keyboard)", &g_cfg.hookWndProc);
    ImGui::Checkbox("Mute DirectInput while menu open", &g_cfg.hookDInput);
    ImGui::Checkbox("Mute gamepad (XInput) while menu open", &g_cfg.hookXInput);
    ImGui::Checkbox("Free the cursor while menu open", &g_cfg.hookCursor);
    ImGui::Checkbox("Open menu at start", &g_cfg.debugMenuOpenAtStart);
    if (ImGui::Button("Save hooks to config")) SaveConfig();
  }
  if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) TabLog();
}

void TabLog() {
  ImGui::BeginChild("log", ImVec2(0, ImGui::GetFontSize() * 18), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  for (const std::string& l : logx::Tail(200)) ImGui::TextUnformatted(l.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

std::mutex g_tabMu;
std::string g_wantTab;

ImGuiTabItemFlags TabFlags(const char* name) {
  std::lock_guard<std::mutex> lk(g_tabMu);
  if (g_wantTab != name) return 0;
  g_wantTab.clear();
  return ImGuiTabItemFlags_SetSelected;
}

// Font + style for this screen height: 17 px UI text at 1080 lines, 23 px at 1440, 34 px at 2160.
ImFont* ApplyScale(const ImVec2& display) {
  float s = std::min(3.0f, std::max(0.75f, display.y / 1080.0f));
  if (std::fabs(s - g_uiScale) > 0.01f) {
    g_uiScale = s;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_baseStyle;
    st.ScaleAllSizes(s);
  }
  float want = kUiBasePx * s;
  int best = -1;
  for (int i = 0; i < 6; ++i)
    if (g_uiFonts[i] && (best < 0 || std::fabs(kUiSizes[i] - want) < std::fabs(kUiSizes[best] - want))) best = i;
  ImFont* f = best >= 0 ? g_uiFonts[best] : ImGui::GetFont();
  ImGui::GetIO().FontGlobalScale = best >= 0 ? want / kUiSizes[best] : 1.0f;
  return f;
}

bool BeginTab(const char* name) { return ImGui::BeginTabItem(name, nullptr, TabFlags(name)); }

// Show enemy HP: "450 / 600" above each enemy, projected with the camera of the last game frame (UE3 view:
// X forward, Y right, Z up; FOV is horizontal).
void DrawEnemyHp() {
  enemies::HpView v = enemies::HpLabels();
  if (!v.valid || v.labels.empty()) return;
  ImVec2 ds = ImGui::GetIO().DisplaySize;
  const float k = 6.2831853f / 65536.0f;
  float p = v.pitch * k, y = v.yaw * k;
  float fx = std::cos(p) * std::cos(y), fy = std::cos(p) * std::sin(y), fz = std::sin(p);
  float rx = -std::sin(y), ry = std::cos(y);
  float ux = -std::sin(p) * std::cos(y), uy = -std::sin(p) * std::sin(y), uz = std::cos(p);
  float fov = (v.fov > 5 && v.fov < 170 ? v.fov : 90.0f) * 3.14159265f / 180.0f;
  float cx = ds.x * 0.5f, cy = ds.y * 0.5f, focal = cx / std::tan(fov * 0.5f);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // under the menu and the damage log
  ImFont* font = ImGui::GetFont();
  float px = ImGui::GetFontSize() * 1.1f;
  for (const enemies::HpLabel& l : v.labels) {
    float dx = l.x - v.camX, dy = l.y - v.camY, dz = l.z - v.camZ;
    float depth = dx * fx + dy * fy + dz * fz;
    if (depth < 50) continue;  // behind the camera
    float sx = cx + (dx * rx + dy * ry) * focal / depth, sy = cy - (dx * ux + dy * uy + dz * uz) * focal / depth;
    if (sx < -100 || sx > ds.x + 100 || sy < -50 || sy > ds.y + 50) continue;
    char b[48];
    snprintf(b, sizeof b, "%.0f / %.0f", l.hp, l.maxHp);
    ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0, b);
    ImVec2 pos(std::floor(sx - ts.x * 0.5f), std::floor(sy - ts.y));
    float f = l.maxHp > 0 ? l.hp / l.maxHp : 0;
    ImU32 col = f > 0.5f ? IM_COL32(120, 240, 120, 255) : f > 0.25f ? IM_COL32(255, 210, 60, 255) : IM_COL32(255, 90, 90, 255);
    for (int ox = -1; ox <= 1; ++ox)
      for (int oy = -1; oy <= 1; ++oy)
        if (ox || oy) dl->AddText(font, px, ImVec2(pos.x + ox, pos.y + oy), IM_COL32(0, 0, 0, 220), b);
    dl->AddText(font, px, pos, col, b);
  }
}

}  // namespace

void ForceDetails(bool open) { g_forceDetails = open; }
void OpenSavePopup(const std::string& which, int index) {
  if (which == "rename") g_popupRename = index;
  if (which == "import") g_popupImport = index;
  if (which == "close") g_popupClose = true;
}

void SelectTab(const std::string& name) {
  std::lock_guard<std::mutex> lk(g_tabMu);
  g_wantTab = name;
}

void SetupFonts() {
  ImGuiIO& io = ImGui::GetIO();
  for (int i = 0; i < 6; ++i) g_uiFonts[i] = TryFont("segoeui.ttf", kUiSizes[i]);
  if (!g_uiFonts[1]) g_uiFonts[1] = io.Fonts->AddFontDefault();
  g_overlayFont = TryFont("segoeuib.ttf", kOverlayFontPx);
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 4.0f;
  st.FrameRounding = 3.0f;
  g_baseStyle = st;
}

void Draw() {
  DrawOverlay();
  ConfigAutosaveTick();
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  ImFont* font = ApplyScale(ds);
  ImGui::PushFont(font);
  visuals::Draw();  // wireframes first: the health numbers stay readable on top
  DrawEnemyHp();
  damagelog::Draw(state::menuOpen);
  if (!state::menuOpen) {
    ImGui::PopFont();
    return;
  }
  float s = g_uiScale;

  // Size/position: saved as fractions of the screen; the default fits 1080p and grows with the resolution.
  static bool placed = false;
  static float placedW = -1;
  static ImVec2 placedFor;
  if (!placed || g_cfg.winW != placedW || placedFor.x != ds.x || placedFor.y != ds.y) {  // + resolution change
    placedFor = ds;
    ImVec2 size = g_cfg.winW > 0 ? ImVec2(g_cfg.winW * ds.x, g_cfg.winH * ds.y) : ImVec2(700 * s, 640 * s);
    ImVec2 pos = g_cfg.winW > 0 ? ImVec2(g_cfg.winX * ds.x, g_cfg.winY * ds.y) : ImVec2(60 * s, 60 * s);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    placed = true;
    placedW = g_cfg.winW;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(520 * s, ds.x), std::min(360 * s, ds.y)), ds);
  if (ImGui::Begin("DmC Practice Tool", nullptr, ImGuiWindowFlags_NoCollapse)) {
    TitleVersion("DmC Practice Tool");
    // keep the whole window on screen (resolution change, saved position from a bigger screen)
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 np(std::max(0.0f, std::min(p.x, ds.x - sz.x)), std::max(0.0f, std::min(p.y, ds.y - sz.y)));
    if (np.x != p.x || np.y != p.y) ImGui::SetWindowPos(np);
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ds.x > 0 && ds.y > 0) {
      float fx = np.x / ds.x, fy = np.y / ds.y, fw = sz.x / ds.x, fh = sz.y / ds.y;
      if (std::fabs(fx - g_cfg.winX) > 0.001f || std::fabs(fy - g_cfg.winY) > 0.001f ||
          std::fabs(fw - g_cfg.winW) > 0.001f || std::fabs(fh - g_cfg.winH) > 0.001f) {
        g_cfg.winX = fx;
        g_cfg.winY = fy;
        g_cfg.winW = fw;
        g_cfg.winH = fh;
        placedW = fw;
        MarkConfigDirty();
      }
    }
    ComputeConflicts();
    if (ImGui::BeginTabBar("tabs")) {
      if (BeginTab("Cheats")) {
        ImGui::BeginChild("scroll");
        TabCheats();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Practice")) {
        ImGui::BeginChild("scroll");
        TabPractice();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Enemies")) {
        ImGui::BeginChild("scroll");
        TabEnemies();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Visuals")) {
        ImGui::BeginChild("scroll");
        TabVisuals();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Unlocks")) {
        ImGui::BeginChild("scroll");
        TabUnlocks();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Saves")) {
        ImGui::BeginChild("scroll");
        TabSave();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Hotkeys")) {
        ImGui::BeginChild("scroll");
        TabHotkeys();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (BeginTab("Settings")) {
        ImGui::BeginChild("scroll");
        TabSettings();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      // On an unknown build the read-only dumps help to add it: the Dev tab is shown without the setting.
      if ((g_cfg.showDevTab || !FeaturesAllowed()) && BeginTab("Dev")) {
        ImGui::BeginChild("scroll");
        TabDev();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace menu
