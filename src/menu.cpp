// The tool's menu (v0.11 look: a list like a debug menu), the "Practice Tool Active" label and the enemy health
// numbers. The widgets are in ui.cpp; rules: ui_guidelines.md.
#include "menu.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "compat.h"
#include "config.h"
#include "cpsets.h"
#include "damagelog.h"
#include "devtools.h"
#include "enemies.h"
#include "features.h"
#include "hotkeys.h"
#include "imgui.h"
#include "input.h"
#include "log.h"
#include "nav.h"
#include "realcp.h"
#include "saveslots.h"
#include "state.h"
#include "steam_guard.h"
#include "ue3/dumper.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "ui.h"
#include "unlock.h"
#include "util.h"
#include "version.h"
#include "speedrun.h"
#include "timers.h"
#include "visuals.h"
#include "window_mode.h"

namespace menu {
namespace {

using ui::Btn;
using ui::BtnKind;
using ui::P;
using ui::RowOpt;

// "Practice Tool Active": fixed by the user's choice, not configurable. Label centre in fractions of the
// centred 16:9 area of the screen (on 21:9 it stays next to the weapon HUD instead of drifting to the edge),
// size relative to that area's height (base 30 px at 1080 lines x 0.83).
constexpr float kOverlayX = 0.148f, kOverlayY = 0.910f, kOverlayScale = 0.83f, kOverlayBasePx = 30.0f;
constexpr float kBaseW = 560.0f, kBaseH = 600.0f;  // menu window at the default zoom, css px (x kBoost at 1080p)
constexpr const char* kGitHub = "https://github.com/billymooreq/DmC-DevilMayCry-Practice-tool";
const char* const kTabs[] = {"Cheats", "Practice", "Enemies", "Visuals", "Runs", "Unlocks", "Saves", "Hotkeys", "Settings", "Dev"};

std::atomic<bool> g_forceDetails{false};  // autorun: open Details / Edit points / Share for screenshots
std::mutex g_mu;                          // g_tab, g_toast, g_rows
std::string g_tab = "Cheats";
std::string g_toast;
ULONGLONG g_toastAt = 0;
float g_winW = 0;      // menu window width of the last frame (its zoom)
bool g_candy = false;  // the Easter egg in About
std::atomic<float> g_winSet[4] = {-1, -1, -1, -1};  // autorun ui_win: x y w h as fractions of the screen

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
  ImFont* font = ui::Overlay();
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

void Say(const std::string& s) { Toast(s); }

// ------------------------------------------------------------------ binds
// Every bind that is used twice (features, presets, menu key) is shown in red.
std::set<const hotkeys::Bind*> g_conflicts;

void ComputeConflicts() {
  g_conflicts.clear();
  auto lk = features::Lock();
  features::Data& d = features::D();
  std::vector<const hotkeys::Bind*> all;
  for (size_t i = 0; i < d.binds.size() && i < features::All().size(); ++i)
    if (!features::All()[i]->noBind) all.push_back(&d.binds[i]);
  for (const auto& p : d.presets) all.push_back(&p.bind);
  all.push_back(&g_cfg.menuKey);
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i]->Empty()) continue;
    for (size_t k = i + 1; k < all.size(); ++k)
      if (hotkeys::Overlaps(*all[i], *all[k])) {
        g_conflicts.insert(all[i]);
        g_conflicts.insert(all[k]);
      }
  }
}

ui::KeyState KeyStateOf(const hotkeys::Bind* b) {
  if (hotkeys::CaptureTarget() == b) return ui::KeyState::Waiting;
  if (b->Empty()) return ui::KeyState::None;
  return g_conflicts.count(b) ? ui::KeyState::Conflict : ui::KeyState::Set;
}

// A key row: Bind starts capturing into *b (any key with modifiers, or a gamepad combination; Esc cancels).
void BindRow(const char* id, const char* label, const char* group, hotkeys::Bind* b, bool canClear,
             const std::string& desc) {
  int r = ui::KeyRow(id, label, group, hotkeys::Text(*b), KeyStateOf(b), canClear, desc);
  if (r == 1) {
    if (hotkeys::CaptureTarget() == b)
      hotkeys::CancelCapture();
    else
      hotkeys::BeginCapture(b);
  } else if (r == 2) {
    *b = {};
    features::MarkDirty();
  }
}

// ------------------------------------------------------------------ feature rows
const char* kNotInMission = "in a mission only";

std::string Unit(const char* fmt) {
  if (!fmt) return "";
  if (strchr(fmt, 'x')) return "\xC3\x97";
  if (strstr(fmt, " m")) return " m";
  if (strstr(fmt, " s")) return " s";
  return "";
}

ui::NumSpec SpecOf(const features::ParamDef& d) {
  ui::NumSpec n;
  n.min = d.min;
  n.max = d.max;
  n.step = d.step;
  n.def = d.def;
  n.hardMax = d.hardMax;
  n.unit = Unit(d.fmt);
  n.integer = d.fmt && strstr(d.fmt, "%.0f");
  return n;
}

// Hint text of a feature: its help, why it is not working right now, the preset that keeps it on.
std::string FeatDesc(const features::Feature* f, const features::View& v, const char* extra = nullptr) {
  std::string d = extra ? extra : f->help;
  if (features::Feature* w = features::LockedBy(f->id))
    d += std::string("\n") + w->name + " has priority: turn it off to use this.";
  else if (v.on && !v.applied && !v.why.empty())
    d += "\nOn, " + v.why + ".";
  if (v.viaPreset) d += "\nOn because a preset is on (Hotkeys tab).";
  return d;
}

constexpr const char* kSpeedrunLock = "[Speedrun mode]";
const char* kSpeedrunWhy = "off in Speedrun mode (Runs tab: turn Speedrun mode off)";

RowOpt FeatOpt(const features::Feature* f, const features::View& v, bool sub = false, const char* desc = nullptr) {
  RowOpt o;
  o.sub = sub;
  o.desc = FeatDesc(f, v, desc);
  if (features::SpeedrunMode() && features::IsCheat(f)) {
    o.locked = true;
    o.lockText = kSpeedrunLock;
    o.desc += "\nOff in Speedrun mode: turn it off on the Runs tab to use this.";
  } else if (features::Feature* w = features::LockedBy(f->id)) {
    o.locked = true;
    o.lockText = std::string("[") + w->name + " has priority]";
  }
  return o;
}

void FeatToggle(const char* id, const char* label = nullptr) {
  features::Feature* f = features::Find(id);
  if (!f) return;
  features::View v = features::Get(id);
  if (ui::Toggle(id, label ? label : f->name, v.on, FeatOpt(f, v))) features::ToggleManual(id);
}

// An option of a feature (a 0/1 parameter): changes how it works, does not turn it on.
void FeatOption(const char* id, int k, const char* label = nullptr) {
  features::Feature* f = features::Find(id);
  if (!f || k >= (int)f->params.size()) return;
  const features::ParamDef& d = f->params[k];
  features::View v = features::Get(id);
  RowOpt o = FeatOpt(f, v, true, d.help ? d.help : "");
  if (o.locked && o.lockText != kSpeedrunLock) o.lockText = "[locked]";
  bool on = v.manualP[k] != 0;
  if (ui::Toggle((std::string(id) + "#" + std::to_string(k)).c_str(), label ? label : d.label, on, o))
    features::SetParam(id, k, on ? 0.0f : 1.0f);
}

// A number: the feature's own value (multipliers: the value is the switch) or a nested setting.
void FeatNumber(const char* id, int k, const char* label = nullptr, bool sub = false) {
  features::Feature* f = features::Find(id);
  if (!f || k >= (int)f->params.size()) return;
  const features::ParamDef& d = f->params[k];
  features::View v = features::Get(id);
  RowOpt o = FeatOpt(f, v, sub, sub ? (d.help ? d.help : "") : nullptr);
  float val = v.manualP[k];
  bool hot = f->autoOn ? v.on : std::fabs(val - d.def) > 1e-4f;
  if (f->autoOn && v.viaPreset) val = v.p[k];
  if (ui::Number((std::string(id) + "#" + std::to_string(k)).c_str(), label ? label : f->name, &val, SpecOf(d), hot, o))
    features::SetParam(id, k, val);
}

void FeatModes(const char* id) {
  features::Feature* f = features::Find(id);
  if (!f || f->params.empty()) return;
  features::View v = features::Get(id);
  bool toggled = false;
  int cur = (int)(v.manualP[0] + 0.5f);
  int m = ui::Modes(id, f->name, f->params[0].choices, cur, v.on, FeatOpt(f, v), &toggled);
  if (m >= 0) features::SetParam(id, 0, (float)m);
  if (toggled) features::ToggleManual(id);
}

// Why an action button is grey ("" = it can run).
std::string ActionWhy(const char* id, const std::string& extra = {}) {
  features::Feature* f = features::Find(id);
  if (!f) return "not available";
  if (features::SpeedrunMode() && features::IsCheat(f)) return kSpeedrunWhy;
  std::string w = f->Blocked();
  return w.empty() ? extra : w;
}

Btn ActionBtn(const char* id, const char* label, BtnKind kind = BtnKind::Normal, const std::string& extraWhy = {},
              const char* desc = nullptr) {
  features::Feature* f = features::Find(id);
  Btn b;
  b.label = label;
  b.desc = desc ? desc : f ? f->help : "";
  b.why = ActionWhy(id, extraWhy);
  b.kind = b.why.empty() ? kind : BtnKind::Disabled;
  return b;
}

// A clicked action button: run it, or say in the status line why it cannot run now.
void RunAction(const char* id, const Btn& b) {
  if (b.kind == BtnKind::Disabled) {
    std::string w = b.why;
    if (!w.empty()) w[0] = (char)toupper((unsigned char)w[0]);
    Say(b.label + ": " + w);
    return;
  }
  features::Trigger(id);
}

// ------------------------------------------------------------------ Cheats tab
void TabCheats() {
  ui::Group("Player");
  FeatToggle("inf_hp");
  FeatToggle("ohko_player");
  FeatOption("ohko_player", 0);
  FeatToggle("inf_dt");
  FeatModes("inf_glide");
  FeatToggle("noclip");
  FeatNumber("noclip", 0, "Fly speed", true);

  ui::Group("Enemies");
  FeatToggle("ohk_enemies");
  FeatToggle("enemy_inf_hp");
  {  // an action as a plain row: click = run
    features::Feature* f = features::Find("enemy_kill_all");
    std::string why = ActionWhy("enemy_kill_all");
    RowOpt o;
    o.desc = f ? f->help : "";
    if (!why.empty()) o.desc += "\nNot now: " + why + ".";
    bool sr = features::SpeedrunMode();
    if (sr) o.locked = true, o.lockText = kSpeedrunLock;
    if (ui::Plain("killall", "Kill all enemies", why.empty() ? "" : sr ? kSpeedrunLock : "(" + why + ")", ui::kLocked, o)) {
      if (why.empty())
        features::Trigger("enemy_kill_all");
      else
        Say("Kill all enemies: " + why);
    }
  }

  ui::Group("Damage");
  FeatNumber("dmg_dealt", 0);
  FeatNumber("dmg_taken", 0);

  ui::Group("Speed");
  FeatNumber("game_speed", 0);
  FeatNumber("player_speed", 0);
  FeatNumber("enemy_speed", 0);

  ui::Group("Camera");
  FeatToggle("unlock_camera");
}

// ------------------------------------------------------------------ Practice tab
std::string PointLabel(const nav::Mission& m, int i) {
  const nav::Point& p = m.points[i];
  const char* tag = p.kind == nav::Kind::Arena ? "  [before arena]" : p.kind == nav::Kind::Cutscene ? "  [before cutscene]" : "";
  bool kept = !p.cp.empty() && realcp::Find(m.map, p.cp);
  return std::to_string(i + 1) + ". " + p.name + tag + (kept ? "  [created]" : "");
}

std::string ShortMission(const std::string& name) {  // "Mission 12 - Under Siege" -> "M12 – Under Siege"
  std::string s = name;
  auto cut = [&](const char* a, const char* b) {
    if (s.rfind(a, 0) == 0) s = b + s.substr(strlen(a));
  };
  cut("Secret Mission ", "SM");
  cut("Mission ", "M");
  if (size_t d = s.find(" - "); d != std::string::npos) s.replace(d, 3, " \xE2\x80\x93 ");
  return s;
}

std::wstring g_pendingImport;   // run at the start of the next frame (it replaces the mission list)
bool g_pendingDefault = false;  // same, back to the built-in list
std::string g_shareMsg;
bool g_editOpen = false, g_shareOpen = false;

// Edit checkpoints: order, rename, delete the points of the mission you are in (saved to DmCPT_missions.json).
void EditPanel(int mi, nav::Selection& sel) {
  const nav::Mission& m = nav::Missions()[mi];
  ui::Info("Change the order, rename or delete checkpoints of " + ShortMission(m.name) +
           (nav::Edited(mi) ? " (edited)" : "") + ". Saved at once.");
  int pt = nav::PointFor(sel, m.map);
  int n = (int)m.points.size();
  int moveFrom = -1, moveDir = 0, doDelete = -1;
  static char nameBuf[128];
  static int popFor = -1;
  bool openRen = false, openDel = false;
  for (int i = 0; i < n; ++i) {
    bool fixed = i == 0;  // the mission start stays first
    std::vector<Btn> b(4);
    b[0].label = "\xE2\x86\x91";
    b[0].desc = "Move up";
    b[0].kind = fixed || i == 1 ? BtnKind::Disabled : BtnKind::Small;
    b[1].label = "\xE2\x86\x93";
    b[1].desc = "Move down";
    b[1].kind = fixed || i == n - 1 ? BtnKind::Disabled : BtnKind::Small;
    b[2].label = "Rename";
    b[2].desc = "Rename this checkpoint.";
    b[3].label = "Delete";
    b[3].desc = "Remove it from the list (Reset this mission brings it back).";
    b[3].kind = fixed ? BtnKind::Disabled : BtnKind::Small;
    if (fixed) b[3].why = b[0].why = b[1].why = "the mission start stays first";
    RowOpt o;
    o.sub = true;
    o.picked = i == pt;
    o.desc = "Click to pick this checkpoint.";
    int r = ui::ListRow(("ep" + std::to_string(i)).c_str(), PointLabel(m, i), "", ui::kText2, b, o);
    if (r == -2) {
      sel.point = i;
      sel.pointMap = m.map;
      nav::SetSel(sel);
    } else if (r == 0) moveFrom = i, moveDir = -1;
    else if (r == 1) moveFrom = i, moveDir = 1;
    else if (r == 2) {
      snprintf(nameBuf, sizeof nameBuf, "%s", m.points[i].name.c_str());
      popFor = i;
      openRen = true;
    } else if (r == 3) {
      popFor = i;
      openDel = true;
    }
  }
  if (openRen) ImGui::OpenPopup("Rename checkpoint");
  if (openDel) ImGui::OpenPopup("Delete checkpoint");
  if (ImGui::BeginPopup("Rename checkpoint")) {
    if (popFor >= 0 && popFor < n) {
      ImGui::Text("New name for \"%s\":", m.points[popFor].name.c_str());
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(P(260));
      bool enter = ImGui::InputText("##name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
      if ((ImGui::Button("OK") || enter) && nameBuf[0]) {
        nav::RenamePoint(mi, popFor, nameBuf);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("Delete checkpoint")) {
    if (popFor > 0 && popFor < n) {
      ImGui::Text("Delete \"%s\" from the list?", m.points[popFor].name.c_str());
      if (ImGui::Button("Delete")) {
        doDelete = popFor;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
  std::vector<Btn> b(2);
  b[0].label = "Rename mission";
  b[0].desc = "Rename this mission in the lists.";
  b[1].label = "Reset this mission";
  b[1].desc = "Back to the built-in name, list and order of this mission (deleted points come back).";
  if (!nav::Edited(mi)) b[1].kind = BtnKind::Disabled, b[1].why = "nothing changed";
  int c = ui::Buttons("editbtns", b);
  if (c == 0) {
    snprintf(nameBuf, sizeof nameBuf, "%s", m.name.c_str());
    ImGui::OpenPopup("Rename mission");
  } else if (c == 1) {
    nav::ResetMission(mi);
    sel.point = 0;
    sel.pointMap = m.map;
    nav::SetSel(sel);
    Say("Mission list reset");
  }
  if (ImGui::BeginPopup("Rename mission")) {
    ImGui::Text("New name for \"%s\":", m.name.c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(P(260));
    bool enter = ImGui::InputText("##mname", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    if ((ImGui::Button("OK") || enter) && nameBuf[0]) {
      nav::RenameMission(mi, nameBuf);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

// Share checkpoints: export the player's list + created checkpoints as one file, use a friend's file.
void SharePanel() {
  static std::vector<cpsets::SetInfo> sets;
  static bool scanned = false;
  static char name[64] = "My checkpoints";
  static int useAt = -2;  // -1 = the built-in list
  if (!scanned) {
    sets = cpsets::List();
    scanned = true;
  }
  ui::Info("Give your checkpoint list (order, names, created checkpoints) to a friend as one file, or use a file "
           "you got. Files are in the DmCPT_sets folder next to the game.");
  ui::TextField("setname", "List name", name, sizeof name, "My checkpoints", "The name of the file you save.");
  std::vector<Btn> b(3);
  b[0].label = "Save my list to a file";
  b[0].kind = BtnKind::Primary;
  b[0].desc = "Writes <name>.dmcptset into the DmCPT_sets folder.";
  b[1].label = "Open folder";
  b[1].desc = "Shows the DmCPT_sets folder.";
  b[2].label = "Refresh";
  b[2].desc = "Read the folder again.";
  int c = ui::Buttons("sharebtns", b);
  if (c == 0) {
    cpsets::Export(name, &g_shareMsg);
    scanned = false;
  } else if (c == 1) {
    CreateDirectoryW(cpsets::Dir().c_str(), nullptr);
    slots::OpenFolder(cpsets::Dir());
  } else if (c == 2) {
    scanned = false;
  }
  if (!g_shareMsg.empty()) ui::Info(g_shareMsg, g_shareMsg.rfind("Can't", 0) == 0 ? ui::kErr : ui::kOk);
  ui::Info("In use: " + nav::ListName());
  bool isDefault = nav::ListName() == nav::kDefaultList && nav::CatalogSource() == "built in";
  std::vector<Btn> use(1);
  use[0].label = "Use this list";
  use[0].desc = "Your list in use is replaced by this one.";
  if (isDefault) use[0].kind = BtnKind::Disabled, use[0].why = "in use now";
  RowOpt o;
  o.sub = true;
  o.desc = "The checkpoint list that comes with the tool. Always here to go back to.";
  if (ui::ListRow("setdef", std::string(nav::kDefaultList) + " (built into the tool)", "", ui::kText2, use, o) == 0)
    useAt = -1, ImGui::OpenPopup("Use checkpoint list");
  if (sets.empty()) ui::Info("No player lists yet. Put a .dmcptset file you got into the DmCPT_sets folder, then Refresh.");
  for (size_t i = 0; i < sets.size(); ++i) {
    const cpsets::SetInfo& st = sets[i];
    if (!st.ok) {
      ui::Info(st.file + ": can't use this file: " + st.error, ui::kErr);
      continue;
    }
    std::vector<Btn> u(1);
    u[0].label = "Use this list";
    u[0].desc = "Replaces your checkpoint list with this one; its created checkpoints are added to yours.";
    RowOpt so;
    so.sub = true;
    so.desc = st.name + ": " + std::to_string(st.points) + " checkpoints, " + std::to_string(st.kept) + " created" +
              (st.created.empty() ? "" : ", " + st.created);
    if (ui::ListRow(("set" + std::to_string(i)).c_str(), st.name,
                    std::to_string(st.points) + " cp \xC2\xB7 " + std::to_string(st.kept) + " created", ui::kOff, u, so) == 0)
      useAt = (int)i, ImGui::OpenPopup("Use checkpoint list");
  }
  if (ImGui::BeginPopupModal("Use checkpoint list", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (useAt == -1) {
      ImGui::Text("Go back to the default checkpoint list?");
      ImGui::TextDisabled("Your edits to the list in use are lost unless you saved it to a file. Created checkpoints stay.");
    } else if (useAt >= 0 && useAt < (int)sets.size()) {
      ImGui::Text("Use \"%s\"?", sets[useAt].name.c_str());
      ImGui::TextDisabled("Your checkpoint list is replaced by this one (save yours to a file first if you want to "
                          "keep it). Its created checkpoints are added to yours.");
    }
    if (ImGui::Button("Use it")) {
      if (useAt == -1) g_pendingDefault = true;
      else if (useAt >= 0 && useAt < (int)sets.size()) g_pendingImport = sets[useAt].path;
      scanned = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
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

  // ---- cutscenes
  ui::Group("Cutscenes");
  FeatToggle("skip_cutscenes");
  FeatToggle("skip_popups");

  // ---- mission select
  ui::Group("Mission select");
  if (!ms.empty()) {
    const auto& rooms = nav::DevRooms();
    const int nDev = (int)rooms.size();
    if (sel.selMission < -nDev || sel.selMission >= (int)ms.size()) sel.selMission = 0;
    const bool dev = sel.selMission < 0;
    std::vector<ui::DropItem> mi;
    for (const nav::DevRoom& d : rooms) mi.push_back({d.name});  // the developers' test maps come first
    for (const nav::Mission& m : ms) {
      std::string n = m.name;
      if (size_t d = n.find(" - "); d != std::string::npos) n.replace(d, 3, " – ");
      mi.push_back({n});
    }
    RowOpt o;
    o.desc = "Pick any mission or secret mission. Works in a mission and in the main menu.\n"
             "Dev room: the developers' test arena. Flat open ground, no story, no cutscenes. Spawn any enemy from "
             "the Enemies tab and practice; Cheats, positions and Visuals work there too. Dante keeps the abilities "
             "of your save. Not on game v1.0: that version crashes there.";
    int r = ui::Drop("msmission", "Mission", mi, dev ? -sel.selMission - 1 : nDev + sel.selMission, o);
    if (r >= 0) {
      sel.selMission = r < nDev ? -(r + 1) : r - nDev;
      sel.selPoint = 0;
      nav::SetSel(sel);
    }
  }
  if (!ms.empty() && sel.selMission < 0) {
    // ---- a dev room: one start point, any difficulty
    std::vector<ui::DropItem> di;
    for (int d = 0; d < nav::kDifficulties; ++d) di.push_back({nav::DifficultyName(d)});
    RowOpt od;
    od.desc = "Difficulty in the Dev room: enemies you spawn there are as tough as on this difficulty.";
    int r = ui::Drop("msdiff", "Difficulty", di, sel.selDifficulty, od);
    if (r >= 0) {
      sel.selDifficulty = r;
      nav::SetSel(sel);
    }
    RowOpt op;
    op.desc = "The Dev room has one start point.";
    ui::Drop("mspoint", "Checkpoint", {{"Start of the map"}}, 0, op);
    std::vector<Btn> b = {ActionBtn("nav_mission", "Start mission", BtnKind::Primary, {},
                                    "Loads the Dev room on the picked difficulty. Your save keeps its abilities and "
                                    "progress; like any mission start, Continue then starts in the Dev room.")};
    if (ui::Buttons("msgo", b) == 0) RunAction("nav_mission", b[0]);
  } else if (!ms.empty()) {
    const nav::Mission& m = ms[sel.selMission];
    std::vector<ui::DropItem> di;
    for (int d = 0; d < nav::kDifficulties; ++d) di.push_back({nav::DifficultyName(d)});
    RowOpt od;
    od.desc = "Difficulty for Start mission: any of the 7, also ones you have not unlocked. Nothing is unlocked in "
              "your save.";
    if (m.secret) od.locked = true, od.lockText = "[set by the secret mission]";
    int r = ui::Drop("msdiff", "Difficulty", di, sel.selDifficulty, od);
    if (r >= 0) {
      sel.selDifficulty = r;
      nav::SetSel(sel);
    }
    std::vector<ui::DropItem> pi;
    std::vector<int> idx;
    for (int i = 0; i < (int)m.points.size(); ++i)
      if (m.points[i].Loadable()) pi.push_back({PointLabel(m, i)}), idx.push_back(i);
    int cur = 0;
    for (size_t i = 0; i < idx.size(); ++i)
      if (idx[i] == sel.selPoint) cur = (int)i;
    RowOpt op;
    op.desc = "Where in the mission to start.";
    r = ui::Drop("mspoint", "Checkpoint", pi, cur, op);
    if (r >= 0 && r < (int)idx.size()) {
      sel.selPoint = idx[r];
      nav::SetSel(sel);
    }
    std::vector<Btn> b = {ActionBtn("nav_mission", "Start mission", BtnKind::Primary)};
    if (ui::Buttons("msgo", b) == 0) RunAction("nav_mission", b[0]);
  }

  // ---- checkpoints of the current mission
  ui::Group("Checkpoint");
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
    if (realcp::TakeCreated(made) && _stricmp(made.map.c_str(), cur->map.c_str()) == 0 &&
        nav::FindPoint(s.missionIndex, made.name) < 0)
      addPoint(made.name, &made);
    int pt = nav::PointFor(sel, cur->map);
    if (pt >= (int)cur->points.size()) pt = 0;
    std::vector<ui::DropItem> items;
    for (int i = 0; i < (int)cur->points.size(); ++i) items.push_back({PointLabel(*cur, i)});
    RowOpt o;
    o.desc = "Checkpoints of the mission you are in. [created] = one you made with Create: it loads exactly where "
             "you made it.";
    int r = ui::Drop("curpoint", "Checkpoint", items, pt, o);
    if (r >= 0) {
      pt = r;
      sel.point = pt;
      sel.pointMap = cur->map;
      nav::SetSel(sel);
    }
    const nav::Point& p = cur->points[pt];
    std::vector<Btn> b = {
        ActionBtn("nav_checkpoint", "Load", BtnKind::Primary, p.Loadable() ? "" : "this point is for Teleport only"),
        ActionBtn("nav_teleport", "Teleport", BtnKind::Normal, p.hasPos ? "" : "no position known for this point"),
        ActionBtn("nav_keep", "Create"), ActionBtn("nav_restart", "Restart mission")};
    const char* ids[] = {"nav_checkpoint", "nav_teleport", "nav_keep", "nav_restart"};
    int c = ui::Buttons("cpbtns", b);
    if (c >= 0) RunAction(ids[c], b[c]);
    realcp::Entry ke;
    if (!p.cp.empty() && realcp::Find(cur->map, p.cp, &ke)) {
      ui::Info("Your checkpoint: loads exactly where you made it (like Continue).", ui::kOk);
      std::vector<Btn> d(1);
      d[0].label = "Delete my checkpoint";
      d[0].desc = std::string("Deletes the checkpoint you made (") + nav::DifficultyName(ke.difficulty) + ", " +
                  ke.saved + "). This point then loads the normal way again.";
      if (ui::Buttons("delmine", d) == 0) {
        realcp::Forget(cur->map, p.cp);
        Say("Your checkpoint deleted: this point loads the normal way again");
      }
    }
    // created earlier but not in the list (deleted from it, or from a friend's set)
    for (const realcp::Entry& e : realcp::List()) {
      if (_stricmp(e.map.c_str(), cur->map.c_str()) != 0 || nav::FindPoint(s.missionIndex, e.name) >= 0) continue;
      std::vector<Btn> ab(2);
      ab[0].label = "Add";
      ab[0].desc = "Put it into the list after the picked checkpoint.";
      ab[1].label = "Delete";
      ab[1].desc = "Delete this created checkpoint.";
      RowOpt ro;
      ro.sub = true;
      ro.desc = "A checkpoint you created that is not in the list.";
      int rr = ui::ListRow(("nl" + e.name).c_str(), "Your created checkpoint \"" + nav::NiceName(e.name) + "\" is not in the list",
                           "", ui::kText2, ab, ro);
      if (rr == 0) addPoint(e.name, nullptr);
      if (rr == 1) realcp::Forget(e.map, e.name);
    }
  } else {
    RowOpt o;
    o.desc = "Checkpoints of the mission you are in.";
    ui::Plain("curpoint", "Checkpoint",
              s.devRoom >= 0 ? "(no checkpoints in the Dev room)" : s.frontEnd ? "(in a mission only)" : "(loading)",
              ui::kLocked, o);
    std::vector<Btn> b = {ActionBtn("nav_checkpoint", "Load", BtnKind::Primary), ActionBtn("nav_teleport", "Teleport"),
                          ActionBtn("nav_keep", "Create"), ActionBtn("nav_restart", "Restart mission")};
    const char* ids[] = {"nav_checkpoint", "nav_teleport", "nav_keep", "nav_restart"};
    int c = ui::Buttons("cpbtns", b);
    if (c >= 0) RunAction(ids[c], b[c]);
  }

  // ---- Dante's positions
  ui::Group("Dante position");
  {
    ui::Slots sl;
    sl.label = "Slot";
    sl.first = 1;
    sl.count = nav::kPosSlots;
    sl.picked = sel.slot;
    for (int i = 0; i < nav::kPosSlots; ++i)
      if (s.slot[i]) sl.saved |= 1u << i;
    sl.desc = "Slots 1-3 are Dante's. Enemy positions 4-6 are in the Enemies tab.";
    std::vector<Btn> b = {ActionBtn("nav_save_pos", "Save"), ActionBtn("nav_load_pos", "Load")};
    if (!s.slot[sel.slot] && b[1].kind != BtnKind::Disabled)
      b[1].kind = BtnKind::Disabled, b[1].why = "slot " + std::to_string(sel.slot + 1) + " is empty";
    int slot = -1;
    int c = ui::Buttons("pos", b, &sl, &slot);
    if (slot >= 0) {
      sel.slot = slot;
      nav::SetSel(sel);
    }
    if (c == 0) RunAction("nav_save_pos", b[0]);
    if (c == 1) RunAction("nav_load_pos", b[1]);
    ui::Info("Positions are kept until you leave the map.");
  }

  // ---- more
  ui::Group("More");
  {
    if (g_forceDetails) g_editOpen = g_shareOpen = true;
    std::vector<Btn> b(2);
    b[0].label = "Edit checkpoints";
    b[0].kind = g_editOpen ? BtnKind::Primary : BtnKind::Normal;
    b[0].desc = "Change the order, rename or delete checkpoints of this mission.";
    if (!cur) b[0].kind = BtnKind::Disabled, b[0].why = kNotInMission;
    b[1].label = "Share checkpoints";
    b[1].kind = g_shareOpen ? BtnKind::Primary : BtnKind::Normal;
    b[1].desc = "Save your checkpoint list to a file or use a friend's list.";
    int c = ui::Buttons("more", b);
    if (c == 0) {
      if (b[0].kind == BtnKind::Disabled) Say("Edit checkpoints: in a mission only");
      else g_editOpen = !g_editOpen;
    }
    if (c == 1) g_shareOpen = !g_shareOpen;
    if (g_editOpen && cur) {
      ui::Group("Edit checkpoints");
      EditPanel(s.missionIndex, sel);
    }
    if (g_shareOpen) {
      ui::Group("Share checkpoints");
      SharePanel();
    }
  }
}

// ------------------------------------------------------------------ Enemies tab
void TabEnemies() {
  enemies::Snapshot es = enemies::Now();
  enemies::Selection sel = enemies::GetSel();
  nav::Snapshot ns = nav::Now();
  bool changed = false;

  ui::Group("Spawn");
  {
    std::vector<ui::DropItem> items;
    int cur = 0;
    for (size_t i = 0; i < es.kinds.size(); ++i) {
      enemies::Avail a = i < es.avail.size() ? es.avail[i] : enemies::Avail::Unknown;
      const char* t = enemies::AvailText(a);
      items.push_back({es.kinds[i].name + (*t ? std::string("  (") + t + ")" : ""), a == enemies::Avail::Missing ? ui::kLocked : 0u});
      if (es.kinds[i].id == sel.kind) cur = (int)i;
    }
    RowOpt o;
    o.desc = "Every regular enemy of the game, by its in-game name (bosses live in their own levels). \"loads first\" "
             "= it takes a second or two before it appears.";
    int r = ui::Drop("enemy", "Enemy", items, cur, o);
    if (r >= 0 && r < (int)es.kinds.size()) sel.kind = es.kinds[r].id, changed = true;
  }
  {
    float v = (float)sel.count;
    ui::NumSpec n;
    n.min = 1, n.max = 10, n.step = 1, n.def = 1, n.hardMax = (float)enemies::kMaxCount, n.integer = true;
    RowOpt o;
    o.desc = "How many enemies to spawn (type up to " + std::to_string(enemies::kMaxCount) + ").";
    if (ui::Number("count", "How many", &v, n, sel.count != 1, o)) sel.count = (int)v, changed = true;
  }
  {
    std::vector<ui::DropItem> w = {{"In front of Dante"}, {"Position 4"}, {"Position 5"}, {"Position 6"}};
    int cur = sel.where == 0 ? 0 : sel.posSlot - nav::kPosSlots + 1;
    RowOpt o;
    o.desc = "In front of Dante, or at one of your enemy positions 4-6.";
    int r = ui::Drop("where", "Where", w, cur, o);
    if (r == 0) sel.where = 0, changed = true;
    if (r > 0) sel.where = 1, sel.posSlot = nav::kPosSlots + r - 1, changed = true;
  }
  if (sel.where == 0) {
    float v = sel.distance;
    ui::NumSpec n;
    n.min = 1, n.max = 20, n.step = 1, n.def = 4, n.hardMax = 100, n.unit = " m";
    RowOpt o;
    o.desc = "How far in front of Dante they appear (where the camera looks).";
    if (ui::Number("dist", "Distance", &v, n, std::fabs(v - 4) > 1e-3f, o)) sel.distance = v, changed = true;
  }
  int epos = sel.where == 1 ? sel.posSlot : nav::kPosSlots;  // the enemy position "Save here" writes
  {
    ui::Slots sl;
    sl.label = "Positions";
    sl.first = nav::kPosSlots + 1;
    sl.count = nav::kAllPosSlots - nav::kPosSlots;
    sl.picked = sel.where == 1 ? sel.posSlot - nav::kPosSlots : -1;
    for (int i = 0; i < sl.count; ++i)
      if (ns.slot[nav::kPosSlots + i]) sl.saved |= 1u << i;
    sl.desc = "Enemy positions 4-6: spawn there. They are separate from Dante's slots 1-3 and kept until you leave "
              "the map.";
    int slot = -1;
    ui::Buttons("epos", {}, &sl, &slot);
    if (slot >= 0) sel.where = 1, sel.posSlot = nav::kPosSlots + slot, epos = sel.posSlot, changed = true;
  }
  {
    std::vector<Btn> b = {ActionBtn("enemy_save_pos", ("Save here as " + std::to_string(epos + 1)).c_str())};
    if (ui::Buttons("esave", b) == 0) {
      if (b[0].kind == BtnKind::Disabled) RunAction("enemy_save_pos", b[0]);
      else features::Trigger("enemy_save_pos");
    }
  }
  if (changed) {
    enemies::SetSel(sel);
    MarkConfigDirty();
  }
  {
    std::vector<Btn> b = {ActionBtn("enemy_spawn", "Spawn", BtnKind::Primary), ActionBtn("enemy_kill_all", "Kill all enemies"),
                          ActionBtn("enemy_despawn", "Despawn spawned")};
    const char* ids[] = {"enemy_spawn", "enemy_kill_all", "enemy_despawn"};
    int c = ui::Buttons("spawn", b);
    if (c == 0 && b[0].kind != BtnKind::Disabled && sel.where == 1 && !ns.slot[sel.posSlot])
      Say("Position " + std::to_string(sel.posSlot + 1) + " is empty: save it first");
    else if (c >= 0)
      RunAction(ids[c], b[c]);
  }
  if (es.mission)
    ui::Info("Enemies alive: " + std::to_string(es.active) + "  (spawned by you: " + std::to_string(es.spawned) + ")");

  ui::Group("Enemy health");
  FeatToggle("enemy_show_hp");

  ui::Group("Damage log");
  FeatToggle("damage_log");
  {
    RowOpt o;
    o.desc = "The log is visible only while this menu is open, but it keeps counting hits.";
    if (ui::Toggle("hidelog", "Hide log in-game", damagelog::HideInGame(), o))
      damagelog::SetHideInGame(!damagelog::HideInGame());
    std::vector<ui::DropItem> f = {{"All hits"}, {"Dante's hits"}, {"Hits on Dante"}};
    o.desc = "Which hits the log lists.";
    int r = ui::Drop("logfilter", "Show", f, damagelog::Filter(), o);
    if (r >= 0) damagelog::SetFilter(r), MarkConfigDirty();
    std::vector<ui::DropItem> v = {{"Hits"}, {"Moves"}};
    o.desc = "Hits: every hit. Moves: totals per move.";
    r = ui::Drop("logview", "View", v, damagelog::View(), o);
    if (r >= 0) damagelog::SetView(r);
    std::vector<ui::DropItem> w = {{"Add to the hit"}, {"Separate line"}};
    o.desc = "Some bosses take extra damage on a weak spot. Add to the hit: it is added to the hit that caused it "
             "(shown in red). Separate line: a line of its own.";
    r = ui::Drop("logweak", "Weak spot", w, damagelog::MergeWeak() ? 0 : 1, o);
    if (r >= 0) damagelog::SetMergeWeak(r == 0), MarkConfigDirty();
    std::vector<Btn> b(2);
    b[0].label = "Clear log";
    b[0].desc = "Empties the log.";
    b[1].label = "Export CSV";
    b[1].desc = "Saves the hits shown (and the totals per move) as a table file in DmCPT_logs next to the game.";
    int c = ui::Buttons("logbtns", b);
    if (c == 0) {
      damagelog::Clear();
      Say("Log cleared");
    } else if (c == 1) {
      std::string path;
      bool ok = damagelog::ExportCsv(&path);
      size_t s = path.find("DmCPT_logs");
      Say(ok ? "Saved " + (s != std::string::npos ? path.substr(s) : path) : "Could not save the file");
    }
  }
}

// ------------------------------------------------------------------ Visuals tab
void TabVisuals() {
  ui::Group("Collisions");
  FeatToggle("vis_enemy_collision");
  FeatOption("vis_enemy_collision", 0);
  FeatToggle("vis_player_collision");
  FeatOption("vis_player_collision", 0);
  ui::Group("Attacks");
  FeatToggle("vis_hitboxes");
  FeatNumber("vis_hitboxes", 0, "Keep on screen", true);
  FeatToggle("vis_area_damage");
  FeatNumber("vis_area_damage", 0, "Keep on screen", true);
  FeatToggle("vis_hit_markers");
  FeatToggle("vis_level_hazards");
  ui::Group("Triggers");
  FeatToggle("vis_cutscene_triggers");
  FeatToggle("vis_arena_triggers");
  FeatToggle("vis_other_triggers");
  if (features::Get("vis_cutscene_triggers").applied || features::Get("vis_arena_triggers").applied ||
      features::Get("vis_other_triggers").applied) {
    visuals::Stats st = visuals::GetStats();
    ui::Info("In this part of the level: " + std::to_string(st.triggers[0]) + " cutscene, " +
             std::to_string(st.triggers[1]) + " arena, " + std::to_string(st.triggers[2]) + " other");
  }
  ui::Group("Walls");
  FeatToggle("vis_invisible_walls");
  FeatOption("vis_invisible_walls", 0);

  ui::Group("Options");
  visuals::Options o = visuals::GetOptions();
  bool ch = false;
  RowOpt ro;
  ro.desc = "Names next to the shapes: what a trigger starts (Arena, Cutscene) and the name of an attack.";
  if (ui::Toggle("vlabels", "Show labels", o.labels, ro)) o.labels = !o.labels, ch = true;
  ro.desc = "Triggers and invisible walls behind the level's walls and floors are not drawn. Off: they show "
            "through everything.";
  bool na = o.occlude && !visuals::OcclusionLive() && features::Status().mission;
  if (na) ro.desc += "\nNot available right now: the game's depth picture was not found this frame.";
  if (ui::Toggle("vocc", na ? "Hide behind walls (not available now)" : "Hide behind walls", o.occlude, ro))
    o.occlude = !o.occlude, ch = true;
  ui::NumSpec md;
  md.min = 5, md.max = 200, md.step = 5, md.def = 40, md.hardMax = 2000, md.unit = " m", md.integer = true;
  ro.desc = "Shapes farther than this are not drawn (triggers are always drawn).";
  ch |= ui::Number("vdist", "Max distance", &o.maxDistance, md, std::fabs(o.maxDistance - 40) > 1e-3f, ro);
  ui::NumSpec lw;
  lw.min = 1, lw.max = 5, lw.step = 0.5f, lw.def = 1.5f, lw.hardMax = 20;
  ro.desc = "Line thickness of all shapes.";
  ch |= ui::Number("vwidth", "Line width", &o.thickness, lw, std::fabs(o.thickness - 1.5f) > 1e-3f, ro);
  if (ch) {
    visuals::SetOptions(o);
    MarkConfigDirty();
  }

  ui::Group("Colours");
  using visuals::Cat;
  ui::Legend({{visuals::Color(Cat::Enemy), "Enemy body"},
              {visuals::Color(Cat::Player), "Dante body"},
              {visuals::Color(Cat::Hitbox), "Dante attacks"},
              {visuals::EnemyHitboxColor(), "Enemy attacks"},
              {visuals::Color(Cat::Area), "Explosions, zones"},
              {visuals::Color(Cat::Hazard), "Level hazards"},
              {visuals::Color(Cat::HitMarker), "Dante hits"},
              {visuals::HitOnDanteColor(), "Hits on Dante"},
              {visuals::Color(Cat::Arena), "Arena"},
              {visuals::Color(Cat::Cutscene), "Cutscene"},
              {visuals::Color(Cat::OtherTrigger), "Other triggers"},
              {visuals::Color(Cat::Walls), "Invisible walls"}});
}

// ------------------------------------------------------------------ Hotkeys tab
void PresetEditor(features::Preset& ps, int pi, int& removeAt, int& toggleAt) {
  const auto& all = features::All();
  char name[64];
  snprintf(name, sizeof name, "%s", ps.name.c_str());
  if (ui::TextField(("pname" + std::to_string(pi)).c_str(), "Name", name, sizeof name, "Preset", "The preset's name.") && name[0]) {
    ps.name = name;
    features::MarkDirty();
  }
  BindRow(("pkey" + std::to_string(pi)).c_str(), "Key", "", &ps.bind, true,
          "The key or gamepad combination that turns this preset on and off.");
  std::vector<Btn> b(2);
  b[0].label = ps.active ? "Turn off" : "Turn on";
  b[0].desc = "Same as pressing the preset's key.";
  b[1].label = "Delete preset";
  b[1].desc = "Removes this preset.";
  int c = ui::Buttons(("pbtn" + std::to_string(pi)).c_str(), b);
  if (c == 0) toggleAt = pi;
  if (c == 1) removeAt = pi;
  ui::Info("Features in this preset (click to add or remove):");
  for (size_t i = 0; i < all.size(); ++i) {
    features::Feature* f = all[i];
    if (f->noBind) continue;
    ImGui::PushID(f->id);
    auto it = std::find_if(ps.items.begin(), ps.items.end(), [&](const features::PresetItem& x) { return x.id == f->id; });
    bool in = it != ps.items.end();
    RowOpt o;
    o.sub = true;
    o.desc = f->help;
    std::string label = std::string(f->name) + (f->action ? " (runs once when the preset turns on)" : "");
    if (ui::Toggle("in", label.c_str(), in, o)) {
      if (!in) {
        features::PresetItem item;
        item.id = f->id;
        for (size_t k = 0; k < f->params.size() && k < (size_t)features::kMaxParams; ++k) item.p[k] = f->params[k].def;
        ps.items.push_back(item);
      } else {
        ps.items.erase(it);
      }
      features::MarkDirty();
      it = std::find_if(ps.items.begin(), ps.items.end(), [&](const features::PresetItem& x) { return x.id == f->id; });
      in = it != ps.items.end();
    }
    if (in && it != ps.items.end())
      for (size_t k = 0; k < f->params.size() && k < (size_t)features::kMaxParams; ++k) {
        const features::ParamDef& d = f->params[k];
        ImGui::PushID((int)k);
        RowOpt po;
        po.sub = true;
        po.desc = d.help ? d.help : f->help;
        std::string pl = std::string("   ") + d.label;
        if (d.kind == features::ParamKind::Toggle) {
          if (ui::Toggle("p", pl.c_str(), it->p[k] != 0, po)) it->p[k] = it->p[k] != 0 ? 0.0f : 1.0f, features::MarkDirty();
        } else if (d.kind == features::ParamKind::Choice) {
          std::vector<ui::DropItem> ch;
          for (const char* c2 : d.choices) ch.push_back({c2});
          int r = ui::Drop("p", pl.c_str(), ch, (int)(it->p[k] + 0.5f), po);
          if (r >= 0) it->p[k] = (float)r, features::MarkDirty();
        } else {
          if (ui::Number("p", pl.c_str(), &it->p[k], SpecOf(d), std::fabs(it->p[k] - d.def) > 1e-4f, po)) features::MarkDirty();
        }
        ImGui::PopID();
      }
    ImGui::PopID();
  }
  ui::Space(4);
}

// ------------------------------------------------------------------ Runs tab
// Order (user, #16): Speedrun · Timers (LiveSplit ones first) · Best times (personal best, sum of best, best
// segments) · Look. Texts for players, short and plain.

// 1. Speedrun: Speedrun mode, the run to start (slot, difficulty, mission), Start run / Restart run / Reset time.
void SpeedrunGroup() {
  static bool first = true;
  if (first) {  // the slot list (the Saves tab reads it the same way)
    slots::Refresh();
    first = false;
  }
  const bool mode = features::SpeedrunMode();
  const bool busy = speedrun::Busy();
  ui::Group("Speedrun");
  {
    RowOpt o;
    o.desc = "Turn it on before you record a run for the leaderboard. All cheats turn off and stay locked, and "
             "\"Practice Tool Active\" goes away. Turn it off to use cheats again. Timers always work.";
    if (ui::Toggle("srmode", "Speedrun mode", mode, o)) {
      if (mode) speedrun::Leave();
      else speedrun::GoLegit();
    }
  }
  speedrun::Pick pk = speedrun::GetPick();
  bool changed = false;
  {
    std::vector<slots::SlotInfo> sl = slots::Slots();
    int n = std::max(5, slots::NumSlots());
    std::vector<ui::DropItem> items;
    for (int i = 0; i < n; ++i) {
      const slots::SlotInfo* si = nullptr;
      for (const auto& x : sl)
        if (x.index == i) si = &x;
      std::string t = "Slot " + std::to_string(i + 1) + " \xE2\x80\x94 ";
      if (!si || !si->used) t += "empty";
      else if (!si->error.empty()) t += "unreadable";
      else {  // like the game's slot screen: time played, highest mission unlocked, last played
        const savefile::InfoData& in = si->sum.info;
        t += si->sum.TimePlayed();
        if (in.valid && in.highestMission >= 0)
          t += std::string(" \xC2\xB7 Mission ") + std::to_string(in.highestMission) + " (" +
               savefile::DifficultyShort(in.highestDifficulty) + ")";
        if (in.valid && in.year > 0) {
          char d[40];
          snprintf(d, sizeof d, " \xC2\xB7 %02d:%02d %02d/%02d/%04d", in.hour, in.minute, in.day, in.month, in.year);
          t += d;
        }
      }
      items.push_back({t, si && si->used && si->error.empty() ? 0u : ui::kText2});
    }
    RowOpt o;
    o.desc = "The save slot you play the run on.";
    int r = ui::Drop("srslot", "Slot", items, std::clamp(pk.slot, 0, n - 1), o);
    if (r >= 0) pk.slot = r, changed = true;
  }
  const auto& ms = nav::Missions();
  int mi = 0;
  for (size_t i = 0; i < ms.size(); ++i)
    if (ms[i].id == pk.mission) mi = (int)i;
  {
    std::vector<ui::DropItem> di;
    for (int d = 0; d < nav::kDifficulties; ++d) di.push_back({nav::DifficultyName(d)});
    RowOpt o;
    o.desc = "The difficulty of the run.";
    if (!ms.empty() && ms[mi].secret) o.locked = true, o.lockText = "[set by the secret mission]";
    int r = ui::Drop("srdiff", "Difficulty", di, pk.difficulty, o);
    if (r >= 0) pk.difficulty = r, changed = true;
  }
  if (!ms.empty()) {
    std::vector<ui::DropItem> items;
    for (const nav::Mission& m : ms) {
      std::string n = m.name;
      if (size_t d = n.find(" - "); d != std::string::npos) n.replace(d, 3, " \xE2\x80\x93 ");
      items.push_back({n});
    }
    RowOpt o;
    o.desc = "Mission 1 for a full run. Any other mission to run just that one.";
    int r = ui::Drop("srmission", "Mission", items, mi, o);
    if (r >= 0) pk.mission = ms[r].id, changed = true;
  }
  if (changed) speedrun::SetPick(pk);
  {
    const char* save = "\nThe game saves when the mission starts, so Continue in this slot will start there too.";
    std::vector<Btn> b(3);
    b[0].label = "Start run";
    b[0].kind = BtnKind::Primary;
    b[0].desc = std::string("Loads the slot and starts the mission. The time starts "
                            "from zero.") + save;
    b[1].label = "Restart run";
    b[1].desc = std::string("Starts the same run again from zero. Your best times stay. Reset your LiveSplit too "
                            "(its Reset key).") + save;
    if (busy)
      for (int i = 0; i < 2; ++i) b[i].kind = BtnKind::Disabled, b[i].why = "a run is being started";
    b[2] = ActionBtn("timer_reset", "Reset time");
    int c = ui::Buttons("srrun", b);
    if (c == 2) {
      RunAction("timer_reset", b[2]);
    } else if (c >= 0) {
      if (b[c].kind == BtnKind::Disabled) Say(b[c].label + ": a run is being started");
      else speedrun::StartRun(c == 1);
    }
  }
  speedrun::Status st = speedrun::LastStatus();
  if (!st.text.empty()) ui::Info(st.text, st.kind == 1 ? ui::kOk : st.kind == 2 ? ui::kErr : ui::kWarn);
}

// 2. Timers: the two LiveSplit (leaderboard) ones first, then the others.
void TimersGroup() {
  ui::Group("Timers");
  FeatToggle("timer_run_ls");
  FeatToggle("timer_mission_ls");
  FeatToggle("timer_mission_igt");
  FeatToggle("timer_run_rta");
  FeatToggle("timer_total_igt");
}

// 3. Best times, two groups (user, #16: PB and SoB apart, both added up the same way): Personal best (total + each
// mission's PB segment), Sum of best (total + each mission's best segment). Menu only, never on the game screen.

// A mission picker with a time column + a time field + Save / Clear. maps: what is picked (map names or mission ids);
// get/set read and write the time (ms, -1 = none).
void TimeEditor(const char* id, const std::vector<std::string>& keys, const std::vector<std::string>& labels,
                const char* field, const std::string& pickHint, const std::string& fieldHint,
                const std::function<double(const std::string&)>& get,
                const std::function<void(const std::string&, double)>& set) {
  if (keys.empty()) return;
  static std::map<std::string, int> picks, filled;
  static std::map<std::string, std::string> bufs;
  int& pick = picks[id];
  int& filledFor = filled.try_emplace(id, -1).first->second;
  if (pick < 0 || pick >= (int)keys.size()) pick = 0;
  std::vector<ui::DropItem> items;
  size_t wid = 0;  // the times in one column (mono font)
  for (const std::string& l : labels) wid = std::max(wid, l.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    double t = get(keys[i]);
    std::string l = labels[i];
    l.append(wid - l.size() + 3, ' ');
    items.push_back({l + (t >= 0 ? timers::Format(t) : "--:--"), t >= 0 ? 0u : ui::kText2});
  }
  RowOpt o;
  o.desc = pickHint;
  int r = ui::Drop((std::string(id) + "m").c_str(), "Mission", items, pick, o);
  if (r >= 0 && r != pick) pick = r;
  char buf[32];
  if (filledFor != pick) {
    double t = get(keys[pick]);
    bufs[id] = t >= 0 ? timers::Format(t) : "";
    filledFor = pick;
  }
  snprintf(buf, sizeof buf, "%s", bufs[id].c_str());
  if (ui::TextField((std::string(id) + "t").c_str(), field, buf, sizeof buf, "1:23.456", fieldHint)) bufs[id] = buf;
  std::vector<Btn> b(2);
  b[0].label = "Save";
  b[0].desc = "Saves the typed time.";
  b[1].label = "Clear";
  b[1].desc = "Removes this mission's time.";
  int c = ui::Buttons((std::string(id) + "b").c_str(), b);
  std::string name = labels[pick].substr(0, labels[pick].find(' '));
  if (c == 0) {
    double v = 0;
    if (timers::ParseTime(bufs[id], &v)) {
      set(keys[pick], v);
      bufs[id] = timers::Format(v);
      Say(name + ": " + field + " " + timers::Format(v));
    } else {
      Say("Type the time like 1:23.456");
    }
  } else if (c == 1) {
    set(keys[pick], -1);
    bufs[id].clear();
    Say(name + ": " + field + " cleared");
  }
}

std::string MissionLabel(const std::string& mapOrId) {  // "M05 · Under Watch"
  int i = nav::FindMission(mapOrId);
  const auto& ms = nav::Missions();
  if (i < 0)
    for (size_t k = 0; k < ms.size(); ++k)
      if (ms[k].id == mapOrId) i = (int)k;
  if (i < 0) return mapOrId;
  const nav::Mission& m = ms[i];
  size_t d = m.name.find(" - ");
  return m.id + " \xC2\xB7 " + (d == std::string::npos ? m.name : m.name.substr(d + 3));
}

// A total that adds up 20 mission times (personal best, sum of best): the time, "(N of 20 missions)" while some
// are missing (the hint lists them).
void TotalRow(const char* id, const char* label, const timers::SumOfBest& t, std::string desc) {
  RowOpt o;
  std::string right = t.ms >= 0 ? timers::Format(t.ms) : "--:--";
  if (t.have < t.total) {
    right += "  (" + std::to_string(t.have) + " of " + std::to_string(t.total) + " missions)";
    std::string miss;
    for (const std::string& m : t.missing) miss += (miss.empty() ? "" : ", ") + MissionLabel(m).substr(0, 3);
    desc += "\nNo time yet: " + miss + ".";
  }
  o.desc = desc;
  ui::Plain(id, label, right, t.have == t.total ? ui::kWhite : ui::kText2, o);
}

void BestTimesGroups() {
  std::vector<std::string> story = timers::StoryMaps(), storyLabels;
  for (const std::string& m : story) storyLabels.push_back(MissionLabel(m));

  ui::Group("Personal best");
  timers::RunInfo ri = timers::Run();
  if (ri.finished)
    ui::Info("Run finished: " + timers::Format(ri.game) + (ri.newPb ? "  -  new personal best!" : ""),
             ri.newPb ? IM_COL32(0x16, 0xA6, 0xFF, 255) : ui::kText2);
  TotalRow("pb", "Personal best", timers::PersonalBest(),
           "Your best full run, from Mission 1 to the end of Vergil in Mission 20: its 20 mission times added up. "
           "A faster full run replaces them by itself.");
  TimeEditor("pbseg", story, storyLabels, "PB segment",
             "Each mission's time in your personal best. The game screen compares your run with them: red when a "
             "mission is slower, the + / - next to IGT.",
             "Type the mission's time like 5:51.923, then press Save.",
             [](const std::string& k) { return timers::PbSegment(k); },
             [](const std::string& k, double v) { timers::SetPbSegment(k, v); });

  ui::Group("Sum of best");
  TotalRow("sob", "Sum of best", timers::SoB(),
           "Your best time of each story mission, added up: the best run you could do. Secret missions do not "
           "count. Fills in by itself as you play.");
  TimeEditor("bestsplit", story, storyLabels, "Best segment",
             "Your best time of each mission. Sum of best adds them up; on the game screen a mission faster than "
             "this is yellow.",
             "Type the mission's best time like 5:51.923, then press Save.",
             [](const std::string& k) { return timers::BestSplit(k); },
             [](const std::string& k, double v) { timers::SetBestSplit(k, v); });
}

// 4. Look: background, place.
void LookGroup() {
  ui::Group("Look");
  float v = std::round(timers::Background() * 100.0f);
  ui::NumSpec n;
  n.min = 0, n.max = 100, n.step = 10, n.def = 0, n.hardMax = 100, n.integer = true, n.unit = "%";
  RowOpt o;
  o.desc = "A dark box behind the timers: 0% = none, 100% = black.";
  if (ui::Number("tbg", "Background", &v, n, v > 0.5f, o)) timers::SetBackground(v / 100.0f);
  std::vector<Btn> bp = {ActionBtn("timer_reset_position", "Reset position")};
  if (ui::Buttons("timerpos", bp) == 0) timers::ResetPosition();  // at once (render thread anyway)
}

void TabRuns() {
  SpeedrunGroup();
  TimersGroup();
  BestTimesGroups();
  LookGroup();
}

void TabHotkeys() {
  ui::Info("The only place to set keys. Press once = on, again = off. Keys do nothing while the menu is open.");
  if (features::SpeedrunMode())
    ui::Info("Speedrun mode: keys of cheats and presets do nothing now. Speedrun and Runs keys work.", ui::kOk);
  ui::Group("Features");
  const auto& all = features::All();
  {
    auto lk = features::Lock();
    features::Data& d = features::D();
    using features::Group;
    for (Group g : {Group::Speedrun, Group::Player, Group::Enemies, Group::Damage, Group::Speed, Group::Camera, Group::Cutscenes,
                    Group::Missions, Group::Navigation, Group::Positions, Group::EnemyTools, Group::DamageLog,
                    Group::Visuals, Group::Timers})
      for (size_t i = 0; i < all.size(); ++i) {
        if (all[i]->group != g || all[i]->noBind) continue;
        BindRow(all[i]->id, all[i]->name, features::GroupName(g), &d.binds[i], true,
                std::string("Bind: then press a key or a gamepad button combination (Esc cancels).\n") + all[i]->help);
      }
  }

  ui::Group("Presets");
  ui::Info("A preset turns several features on with one key, with its own values. Buttons from Practice and "
           "Enemies run once when the preset turns on.");
  static std::set<std::string> expanded;
  auto lk = features::Lock();
  features::Data& d = features::D();
  int removeAt = -1, toggleAt = -1;
  for (size_t pi = 0; pi < d.presets.size(); ++pi) {
    features::Preset& ps = d.presets[pi];
    ImGui::PushID((int)pi);
    std::string key = hotkeys::Text(ps.bind);
    std::string list;
    for (const features::PresetItem& it : ps.items)
      if (features::Feature* f = features::Find(it.id)) {
        std::string one = f->name;
        if (!f->params.empty() && f->params[0].kind == features::ParamKind::Slider) one += " " + f->params[0].Text(it.p[0]);
        list += (list.empty() ? "" : " + ") + one;
      }
    bool open = expanded.count(ps.name) > 0;
    std::vector<Btn> b(1);
    b[0].label = open ? "Close" : "Edit";
    b[0].desc = "Change the name, key and features of this preset.";
    RowOpt o;
    o.desc = "Click to turn the preset on or off.\n" + ps.name + " = " + (list.empty() ? "nothing yet" : list) + ".";
    std::string label = ps.name + (key.empty() ? "" : "   [" + key + "]");
    bool srLock = features::SpeedrunMode() && !ps.active;
    if (srLock) o.desc += "\nOff in Speedrun mode.";
    int r = ui::ListRow("preset", label, ps.active ? "[ON]" : srLock ? kSpeedrunLock : "[off]",
                        ps.active ? ui::kWhite : srLock ? ui::kLocked : ui::kOff, b, o);
    if (r == -2) toggleAt = (int)pi;
    if (r == 0) {
      if (open) expanded.erase(ps.name); else expanded.insert(ps.name);
      open = !open;
    }
    if (open || g_forceDetails) PresetEditor(ps, (int)pi, removeAt, toggleAt);
    ImGui::PopID();
  }
  if (toggleAt >= 0) features::TogglePreset(toggleAt);
  if (removeAt >= 0) {
    if (hotkeys::CaptureTarget() == &d.presets[removeAt].bind) hotkeys::CancelCapture();
    expanded.erase(d.presets[removeAt].name);
    d.presets.erase(d.presets.begin() + removeAt);
    features::MarkDirty();
  }
  std::vector<Btn> nb(1);
  nb[0].label = "+ New preset";
  nb[0].desc = "Adds an empty preset.";
  if (ui::Buttons("newpreset", nb) == 0) {
    if (hotkeys::Capturing()) hotkeys::CancelCapture();  // the vector may move
    features::Preset p;
    p.name = "Preset " + std::to_string(d.presets.size() + 1);
    d.presets.push_back(p);
    expanded.insert(p.name);
    features::MarkDirty();
  }
}

// ------------------------------------------------------------------ Settings tab
std::string GameVersionText(ImU32* col) {
  if (GameVersionKnown()) {
    if (col) *col = ui::kOk;
    return std::string("Game ") + GetGameVersion().label + " detected";
  }
  if (col) *col = ui::kWarn;
  return "Game version undetected (still can use tool)";
}

void TabSettings() {
  ui::Group("Menu");
  BindRow("menukey", "Open / close menu", "", &g_cfg.menuKey, false,
          "The key that opens and closes this menu. Click Bind, then press any key or gamepad button combination.");
  RowOpt o;
  o.desc = "The game freezes when you open the tool and goes on when you close it.";
  {
    RowOpt po = o;
    if (features::SpeedrunMode())
      po.locked = true, po.lockText = kSpeedrunLock, po.desc += "\nOff in Speedrun mode: a pause would not be fair in a run.";
    if (ui::Toggle("pause", "Pause the game while the menu is open", g_cfg.pauseInMenu && !features::SpeedrunMode(), po))
      g_cfg.pauseInMenu = !g_cfg.pauseInMenu, MarkConfigDirty();
  }
  o.desc = "Hides the box at the bottom of the menu that explains each line.";
  if (ui::Toggle("nohints", "Disable hints", g_cfg.hideHints, o)) g_cfg.hideHints = !g_cfg.hideHints, MarkConfigDirty();
  {
    std::vector<Btn> b(1);
    b[0].label = "Reset window size and position";
    b[0].desc = "Puts the menu window back to its default place and size.";
    if (ui::Buttons("resetwin", b) == 0) {
      g_cfg.winW = 0;
      MarkConfigDirty();
      Say("Window reset");
    }
  }

  ui::Group("Game window");
  {
    // three switches, one of them on (user, #16); picking another restarts the game in it at once
    struct M {
      const char* id;
      const char* label;
      const char* desc;
    };
    const M modes[] = {
        {"wm_full", "Fullscreen", "The game's normal fullscreen."},
        {"wm_border", "Borderless",
         "Fullscreen in a window without borders. Alt+Tab is instant and other windows (LiveSplit, "
         "OBS) can stay on top of the game."},
        {"wm_window", "Window",
         "A normal window with a frame: move it and drag its edges to make it bigger or smaller (the picture "
         "stretches with it)."},
    };
    static bool restarting = false;
    for (int i = 0; i < 3; ++i) {
      RowOpt o;
      o.desc = std::string(modes[i].desc) +
               "\nPicking it restarts the game at once (a few seconds). Progress since the last checkpoint is lost.";
      if (restarting) o.locked = true, o.lockText = g_cfg.windowMode == i ? "[restarting...]" : "[off]";
      if (ui::Toggle(modes[i].id, modes[i].label, g_cfg.windowMode == i, o) && g_cfg.windowMode != i) {
        int was = g_cfg.windowMode;
        g_cfg.windowMode = i;
        if (winmode::RestartGame()) {
          restarting = true;
          Say(std::string("Restarting the game: ") + modes[i].label);
        } else {
          g_cfg.windowMode = was;
          SaveConfig();
          Say("Could not restart the game - nothing changed");
        }
      }
    }
  }

  ui::Group("Other");
  {
    RowOpt dm;
    dm.desc = "Optional. Lets you open the game's hidden DEBUG MENU, left in the game by its developers: Jump Menu "
              "and Console Commands (stat FPS, god mode, slow motion, kill all enemies, set difficulty...). Some "
              "of its buttons (Upgrades, Unlock All Missions, Give Gold Orbs...) change your save for good. "
              "Steam achievements stop for this session once it is opened.\n"
              "Open it in a mission: hold Angel + Demon and press Pause.\n"
              "Keyboard: hold Q + E, press Esc (your Angel / Demon / Pause keys).\n"
              "Gamepad: hold LT + RT, press Start.\n"
              "In the debug menu: arrows / left stick move, Enter / A picks, Esc / B goes back, "
              "Page Up / Page Down (LB / RB) switch the command page.";
    int av = devtools::DebugMenuAvailable();
    if (av == 0) dm.locked = true, dm.lockText = "[not in this game version]",
                 dm.desc = "This game version has no debug menu left in it.";
    else if (features::SpeedrunMode())
      dm.locked = true, dm.lockText = kSpeedrunLock, dm.desc += "\nOff in Speedrun mode.";
    if (ui::Toggle("debugmenu", "Unlock the game's debug menu", devtools::DebugMenuOn(), dm))
      devtools::SetDebugMenu(!devtools::DebugMenuOn()), MarkConfigDirty();
  }
  o.desc = "Shows the Dev tab (dumps, logs). For testing.";
  if (ui::Toggle("devtab", "Show Dev tab", g_cfg.showDevTab, o)) g_cfg.showDevTab = !g_cfg.showDevTab, MarkConfigDirty();

  ui::Group("Config");
  ui::Info("Keys, values and presets are saved by themselves to DmCPracticeTool.json.");
  {
    std::vector<Btn> b(2);
    b[0].label = "Save config";
    b[0].desc = "Writes DmCPracticeTool.json now.";
    b[1].label = "Load config";
    b[1].desc = "Reads DmCPracticeTool.json again.";
    int c = ui::Buttons("cfg", b);
    if (c == 0) Say(SaveConfig() ? "Config saved" : "Could not write the config");
    if (c == 1) Say(LoadConfig() ? "Config loaded" : "Could not read the config");
  }

  ui::Group("Compatibility report");
  ui::Info(GameVersionKnown() ? "Something does not work on your game build? Create a report and send it to us."
                              : "The tool does not know your game version. Create a report and send it to us, so we "
                                "can add it.",
           GameVersionKnown() ? ui::kText2 : ui::kWarn);
  {
    std::vector<Btn> b(2);
    b[0].label = "Create report";
    b[0].desc = "Writes DmCPT_compat_report.txt next to the game: which game version you have and what the tool "
                "finds in it. Only reads, changes nothing.";
    if (compat::Busy()) b[0].kind = BtnKind::Disabled, b[0].why = "working";
    b[1].label = "Open report folder";
    b[1].desc = "Shows the report file.";
    int c = ui::Buttons("compat", b);
    if (c == 0 && b[0].kind != BtnKind::Disabled) compat::CreateReportAsync();
    if (c == 1) slots::OpenFolder(util::ExeDir());
    std::string res = compat::LastResult();
    if (!res.empty()) ui::Info(res, compat::Busy() ? ui::kWarn : ui::kOk);
  }

  ui::Group("About");
  {
    RowOpt a;
    a.desc = std::string("github.com/billymooreq/DmC-DevilMayCry-Practice-tool");
    std::string line = std::string("DmC Practice Tool v") + kToolVersion + " \xC2\xB7 " + GameVersionText(nullptr);
    if (ui::Plain("about", line.c_str(), "", ui::kText2, a)) g_candy = !g_candy;
    if (g_candy) {
      float w = 0, h = 0;
      ImTextureID t = ui::Razium(&w, &h);
      if (ui::Picture("candy", t, w, h, "you found mekarazium")) g_candy = false;
    }
    std::vector<Btn> b(1);
    b[0].label = "GitHub";
    b[0].desc = "Opens the tool's page: github.com/billymooreq/DmC-DevilMayCry-Practice-tool";
    if (ui::Buttons("github", b) == 0) {
      ShellExecuteA(nullptr, "open", kGitHub, nullptr, nullptr, SW_SHOWNORMAL);
      Say("Opened the GitHub page in your browser");
    }
  }
}

// ------------------------------------------------------------------ Unlocks tab
void UnknownBuildWarning() {
  if (!GameVersionKnown())
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::kWarn),
                       "Your game version is not known to the tool: this may not work and could change your save\n"
                       "in a way the game does not expect. The backup is your way back.");
}

// "Save Unlocks": game functions on the game thread, main menu only, backup + confirmation first.
void TabUnlocks() {
  unlock::Status st = unlock::CurrentStatus();
  bool busy = unlock::Busy();
  bool canRun = st.state == unlock::State::Ready && !busy;
  std::string why;
  if (busy) {
    ui::Info("Working: an unlock is running.", ui::kWarn);
    why = "an unlock is running";
  } else if (st.state == unlock::State::Ready) {
    ui::Info("READY \xC2\xB7 main menu, save loaded. A backup of your save is made before any change.", ui::kOk);
  } else if (st.state == unlock::State::InMission) {
    why = "works only in the main menu";
    ui::Info("Works only in the main menu (now in " + st.map + "). Quit to the main menu.", ui::kWarn);
  } else if (st.state == unlock::State::NoSave) {
    why = "choose your save slot first";
    ui::Info("Choose your save slot in the main menu first (" + st.detail + ").", ui::kWarn);
  } else {
    why = unlock::StateText(st.state);
    ui::Info(std::string("Not ready: ") + unlock::StateText(st.state) + " (" + st.detail + ").", ui::kWarn);
  }
  static std::vector<unlock::Step> pending;
  static std::string pendingWhat;
  bool open = false, openSSS = false;
  auto mk = [&](const std::string& label, const std::string& desc, BtnKind k = BtnKind::Normal) {
    Btn b;
    b.label = label;
    b.desc = desc;
    b.kind = canRun ? k : BtnKind::Disabled;
    b.why = why;
    return b;
  };
  auto blockedSay = [&](const Btn& b) { Say(b.label + ": " + why); };
  {
    std::vector<Btn> b = {mk("Unlock all \xE2\x80\x94 100% save",
                             "Missions on all difficulties, story weapons, secret missions, keys, Lost Souls, skills, "
                             "max HP/DT, costumes, concept art. Ranks and play time are not changed.",
                             BtnKind::Big)};
    if (ui::Buttons("all", b) == 0) {
      if (!canRun) blockedSay(b[0]);
      else {
        pending.clear();
        for (int s = 0; s < unlock::StepCount; ++s) pending.push_back((unlock::Step)s);
        pendingWhat = "everything, like a 100% save: all missions on all difficulties, story weapons & abilities,\n"
                      "secret missions (completed), keys, lost souls, skills, max HP/DT, costumes, concept art.\n"
                      "Mission ranks and scores are not changed.";
        open = true;
      }
    }
  }
  ui::Group("Unlock one part");
  {
    std::vector<Btn> b;
    for (int s = 0; s < unlock::StepCount; ++s)
      b.push_back(mk(unlock::StepLabel((unlock::Step)s), std::string("Unlocks only: ") + unlock::StepLabel((unlock::Step)s) + "."));
    int c = ui::Buttons("parts", b);
    if (c >= 0) {
      if (!canRun) blockedSay(b[c]);
      else {
        pending = {(unlock::Step)c};
        pendingWhat = unlock::StepLabel((unlock::Step)c);
        open = true;
      }
    }
    std::vector<Btn> sv = {mk("Save now", "Saves the current state without unlocking anything.")};
    if (ui::Buttons("savenow", sv) == 0) {
      if (!canRun) blockedSay(sv[0]);
      else {
        pending.clear();
        pendingWhat = "nothing new - only save the current state";
        open = true;
      }
    }
  }
  ui::Group("Ranks");
  {
    std::vector<Btn> b = {mk("Set all ranks SSS\xE2\x80\xA6", "Sets every mission rank to SSS. This replaces your own ranks.")};
    if (ui::Buttons("sss", b) == 0) {
      if (!canRun) blockedSay(b[0]);
      else openSSS = true;
    }
    ui::Info("Replaces your mission ranks.");
  }
  if (open) ImGui::OpenPopup("Confirm unlock");
  if (openSSS) ImGui::OpenPopup("Confirm ranks SSS");
  float em = ImGui::GetFontSize();
  if (ImGui::BeginPopupModal("Confirm ranks SSS", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Set SSS on every completed mission (all difficulties) and every secret mission?");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::kWarn), "Your own ranks are overwritten. Scores and times stay as they are.");
    ImGui::Text("A backup copy of your save is made first (DmCPT_backups).");
    UnknownBuildWarning();
    if (ImGui::Button("Yes, set SSS and save", ImVec2(em * 14, 0))) {
      unlock::Run({unlock::RanksSSS}, true);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 8, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("Confirm unlock", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Unlock: %s", pendingWhat.c_str());
    ImGui::Text("This changes your save. A backup copy is made first (DmCPT_backups).");
    ImGui::Text("Steam achievements are blocked while it runs.");
    UnknownBuildWarning();
    if (ImGui::Button("Yes, unlock and save", ImVec2(em * 14, 0))) {
      unlock::Run(pending, true);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 8, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  for (const std::string& l : unlock::Report()) ui::Info(l, l.rfind("FAIL", 0) == 0 ? ui::kErr : ui::kText2);
}

// ------------------------------------------------------------------ Saves tab
std::string SlotLabel(const slots::SlotInfo& s) {
  if (!s.used) return "Slot " + std::to_string(s.index + 1) + " \xE2\x80\x94 empty";
  if (!s.error.empty()) return "Slot " + std::to_string(s.index + 1) + " \xE2\x80\x94 unreadable";
  return "Slot " + std::to_string(s.index + 1) + " \xE2\x80\x94 " + s.sum.OneLine();
}

std::atomic<int> g_popupRename{-1}, g_popupImport{-1};
std::atomic<bool> g_popupClose{false};

ImU32 ReportCol(const std::string& l) {
  if (l.rfind("FAIL", 0) == 0) return ui::kErr;
  if (l.rfind("OK", 0) == 0) return ui::kOk;
  if (l.find("version") != std::string::npos || l.rfind("note", 0) == 0) return ui::kWarn;
  return ui::kText2;
}

void TabSave() {
  static bool first = true;
  if (first) {
    slots::Refresh();
    first = false;
  }
  static int lastOp = 0;  // where the result of the last job is shown: 0 under Import, 1 under Export
  unlock::Status st = unlock::CurrentStatus();
  bool busy = slots::Busy() || unlock::Busy();
  int active = slots::ActiveSlot();
  std::vector<slots::SlotInfo> sl = slots::Slots();
  float em = ImGui::GetFontSize();
  auto results = [&](int op) {
    if (lastOp != op) return;
    if (slots::Busy()) ui::Info("working\xE2\x80\xA6", ui::kWarn);
    for (const std::string& l : slots::Report()) ui::Info(l, ReportCol(l));
  };

  // ------------------------------------------------------------------ import
  ui::Group("Import");
  std::vector<slots::ImportEntry> im = slots::Imports();
  bool inMenu = st.state == unlock::State::Ready;
  if (!inMenu)
    ui::Info(std::string("Loading a save works in the main menu after you pick a save slot (now: ") +
             unlock::StateText(st.state) + ").", ui::kWarn);
  static int importAt = -1, renameAt = -1;
  static char newName[96] = {};
  static int sel = 0;
  static bool details = false;
  if (g_popupImport >= 0 && g_popupImport < (int)im.size()) sel = g_popupImport;  // autorun: select it first
  if (g_popupRename >= 0 && g_popupRename < (int)im.size()) sel = g_popupRename;
  if (sel >= (int)im.size()) sel = 0;
  if (im.empty()) ui::Info("Put .dmcpt files into the import folder, then Refresh. Files from friends usually come as .dmcpt.");
  for (int j = 0; j < (int)im.size(); ++j) {
    const slots::ImportEntry& r = im[j];
    RowOpt o;
    o.picked = j == sel;
    o.desc = r.ok ? "A save someone sent you. Click to pick it." + (r.notes.empty() ? "" : "\n" + r.notes)
                  : "Can't load this file: " + r.error;
    if (ui::Plain(("imp" + std::to_string(j)).c_str(), r.name.c_str(), r.ok ? r.sum.Brief() : "can't load",
                  r.ok ? (j == sel ? ui::kWhite : ui::kOff) : ui::kErr, o))
      sel = j;
  }
  const slots::ImportEntry* e = sel < (int)im.size() ? &im[sel] : nullptr;
  {
    std::vector<Btn> b(5);
    b[0].label = "Import\xE2\x80\xA6";
    b[0].kind = BtnKind::Primary;
    b[0].desc = "Asks which slot to overwrite (with a backup), then imports.";
    if (!e) b[0].why = "no save picked";
    else if (!e->ok) b[0].why = "this file can't be loaded";
    else if (busy) b[0].why = "working";
    else if (!inMenu) b[0].why = "main menu with a save slot picked only";
    if (!b[0].why.empty()) b[0].kind = BtnKind::Disabled;
    b[1].label = "Rename";
    b[1].desc = "Rename the save file.";
    if (!e || !e->ok || busy) b[1].kind = BtnKind::Disabled, b[1].why = !e ? "no save picked" : busy ? "working" : "this file can't be loaded";
    b[2].label = details ? "Hide details" : "Details";
    b[2].kind = details ? BtnKind::Primary : BtnKind::Normal;
    b[2].desc = "Time, skills, HP/DT, orbs, keys, versions.";
    b[3].label = "Open import folder";
    b[3].desc = "Put .dmcpt files here.";
    b[4].label = "Refresh";
    b[4].desc = "Read the folder again.";
    if (busy) b[4].kind = BtnKind::Disabled, b[4].why = "working";
    int c = ui::Buttons("impbtns", b);
    bool forceImport = e && g_popupImport == sel && g_popupImport.exchange(-1) == sel;
    bool forceRename = e && g_popupRename == sel && g_popupRename.exchange(-1) == sel;
    if ((c == 0 && b[0].kind != BtnKind::Disabled) || forceImport) {
      importAt = sel;
      lastOp = 0;
      ImGui::OpenPopup("Import save");
    } else if (c == 0) {
      Say("Import: " + b[0].why);
    }
    if ((c == 1 && b[1].kind != BtnKind::Disabled) || forceRename) {
      renameAt = sel;
      lastOp = 0;
      snprintf(newName, sizeof newName, "%s", e->name.c_str());
      ImGui::OpenPopup("Rename save");
    }
    if (c == 2) details = !details;
    if (c == 3) slots::OpenFolder(slots::ImportDir());
    if (c == 4 && b[4].kind != BtnKind::Disabled) slots::Refresh();
  }
  // the result of the last import / rename, right under the buttons
  if (e && e->ok && !e->error.empty()) ui::Info(e->error, ui::kWarn);
  results(0);
  if (e && (details || g_forceDetails)) {
    if (!e->notes.empty()) ui::Info(e->notes, ui::kSoft);
    if (e->ok) {
      ui::Info(e->sum.OneLine(), ui::kSoft);
      for (const std::string& l : savefile::SummaryLines(e->sum)) ui::Info(l);
    }
    ui::Info("File: " + e->display);
    if (!e->exportedAt.empty())
      ui::Info("Exported " + e->exportedAt + " with tool " + e->toolVersion + ", game " + VersionLabel(e->gameVersion) +
               " (" + e->gameVersion + ")");
    if (e->rawSlot >= 0)
      ui::Info("Plain game save files (not a .dmcpt package), Steam account " + std::to_string(e->rawAccount) +
               ". Rename packs them into a .dmcpt file.");
    if (e->ok && e->infoRebuilt) ui::Info("The text on the save slot screen is rebuilt for your game version.");
  }
  if (ImGui::BeginPopupModal("Import save", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_popupClose.exchange(false)) ImGui::CloseCurrentPopup();
    static int target = -1;
    int n = slots::NumSlots();
    if (target < 0 || target >= n) target = n > 0 ? n - 1 : 0;
    auto targetLabel = [&](int t) {
      return t < (int)sl.size() ? SlotLabel(sl[t]) + (t == active ? "  (in use now)" : "")
                                : "Slot " + std::to_string(t + 1) + " \xE2\x80\x94 empty";
    };
    std::string nm = importAt >= 0 && importAt < (int)im.size() ? im[importAt].name : "";
    ImGui::Text("Load \"%s\"", nm.c_str());
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
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::kWarn), "Slot %d is not empty. What is in it now will be replaced.", target + 1);
    ImGui::TextDisabled("A backup copy of all your saves is made first.");
    UnknownBuildWarning();
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
    if (renameAt >= 0 && renameAt < (int)im.size() && im[renameAt].rawSlot >= 0)
      ImGui::TextDisabled("These are plain save files: they will be packed into a .dmcpt file.");
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

  // ------------------------------------------------------------------ export
  ui::Group("Export");
  static int expSlot = -1, nameFor = -2;
  static char name[96] = {};
  static char notes[512] = {};
  static bool saveFirst = true;
  if (expSlot < 0 || expSlot >= (int)sl.size() || !sl[expSlot].used) {
    expSlot = -1;
    for (const auto& s : sl)
      if (s.used && (expSlot < 0 || s.index == active)) expSlot = s.index;
  }
  {
    std::vector<ui::DropItem> items;
    std::vector<int> idx;
    int cur = -1;
    for (const auto& s : sl)
      if (s.used) {
        if (s.index == expSlot) cur = (int)items.size();
        items.push_back({SlotLabel(s) + (s.index == active ? "  (in use now)" : "")});
        idx.push_back(s.index);
      }
    if (items.empty()) items.push_back({"no saves found", ui::kLocked});
    RowOpt o;
    o.desc = "Which of your saves to share.";
    int r = ui::Drop("expslot", "Slot", items, cur < 0 ? 0 : cur, o);
    if (r >= 0 && r < (int)idx.size()) expSlot = idx[r];
  }
  // The name follows the chosen slot until the player types their own.
  if (expSlot >= 0 && expSlot != nameFor) {
    snprintf(name, sizeof name, "%s", sl[expSlot].sum.DefaultName().c_str());
    nameFor = expSlot;
  }
  ui::TextField("expname", "Name", name, sizeof name, "", "File name for the shared save.");
  ui::TextField("expnotes", "Notes", notes, sizeof notes, "Optional",
                "A note for the person you send it to, for example: \"M12 boss, DMD, no Kablooey upgrades\".");
  {
    RowOpt o;
    o.desc = "Saves the slot in use before exporting (main menu), so the file matches what the game shows.";
    if (ui::Toggle("savefirst", "Save the game first", saveFirst, o)) saveFirst = !saveFirst;
  }
  if (expSlot >= 0 && !sl[expSlot].error.empty())
    ui::Info("This slot can't be read: " + sl[expSlot].error, ui::kErr);
  if (st.state == unlock::State::InMission && expSlot == active)
    ui::Info("You are in a mission: the last checkpoint is exported.", ui::kWarn);
  {
    std::vector<Btn> b(2);
    b[0].label = "Export";
    b[0].kind = BtnKind::Primary;
    b[0].desc = "Writes the .dmcpt file to the export folder.";
    if (busy) b[0].why = "working";
    else if (expSlot < 0) b[0].why = "no save to export";
    else if (!name[0]) b[0].why = "type a name first";
    if (!b[0].why.empty()) b[0].kind = BtnKind::Disabled;
    b[1].label = "Open export folder";
    b[1].desc = "Shows the exported files.";
    int c = ui::Buttons("expbtns", b);
    if (c == 0 && b[0].kind != BtnKind::Disabled) {
      lastOp = 1;
      slots::Export(expSlot, name, notes, saveFirst);
    } else if (c == 0) {
      Say("Export: " + b[0].why);
    }
    if (c == 1) slots::OpenFolder(slots::ExportDir());
  }
  results(1);
  g_popupClose = false;  // only for a window that is open now
}

// ------------------------------------------------------------------ Dev tab (developers: plain ImGui widgets)
void TabLog() {
  ImGui::BeginChild("log", ImVec2(0, ImGui::GetFontSize() * 18), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  for (const std::string& l : logx::Tail(200)) ImGui::TextUnformatted(l.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

void TabDev() {
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(P(6), P(4)));
  ImGui::Indent(P(10));
  const VersionInfo& gv = GetGameVersion();
  ImGui::Text("Tool v%s. Game build: %s = %s (TimeDateStamp 0x%08X, SizeOfImage 0x%08X)", kToolVersion, gv.name,
              gv.label, gv.timeDateStamp, gv.sizeOfImage);
  static bool pretend = false;
  if (ImGui::Checkbox("Pretend the game version is unknown (test)", &pretend)) SetPretendUnknown(pretend);
  ImGui::TextWrapped("UE3: %s", ue3::StatusText().c_str());
  ImGui::TextWrapped("Visuals: %s", visuals::DebugLines("").front().c_str());
  {
    static std::string navMsg;
    size_t pts = 0;
    for (const nav::Mission& m : nav::Missions()) pts += m.points.size();
    ImGui::Text("Mission catalog: %s (%zu missions, %zu points)", nav::CatalogSource().c_str(), nav::Missions().size(), pts);
    ImGui::SameLine();
    if (ImGui::SmallButton("Export for editing")) nav::ExportCatalog(&navMsg);
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
  ImGui::SetNextItemWidth(P(200));
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
  ImGui::SetNextItemWidth(-P(10));
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
  ImGui::Unindent(P(10));
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ enemy health
// "450 / 600" above each enemy (Barlow Condensed ExtraBold, white with a black outline) and a small bar,
// projected with the camera of the last game frame (UE3 view: X forward, Y right, Z up; FOV is horizontal).
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
  float s = ui::Screen();
  float big = 22 * s, smallPx = 15 * s, o = std::max(1.0f, std::round(2 * s));
  ImFont* fb = ui::Get(ui::F::Heavy, big);
  ImFont* fs = ui::Get(ui::F::Heavy, smallPx);
  for (const enemies::HpLabel& l : v.labels) {
    float dx = l.x - v.camX, dy = l.y - v.camY, dz = l.z - v.camZ;
    float depth = dx * fx + dy * fy + dz * fz;
    if (depth < 50) continue;  // behind the camera
    float sx = cx + (dx * rx + dy * ry) * focal / depth, sy = cy - (dx * ux + dy * uy + dz * uz) * focal / depth;
    if (sx < -100 || sx > ds.x + 100 || sy < -50 || sy > ds.y + 50) continue;
    char a[24], b[24];
    snprintf(a, sizeof a, "%.0f", l.hp);
    snprintf(b, sizeof b, " / %.0f", l.maxHp);
    ImVec2 sa = fb->CalcTextSizeA(big, FLT_MAX, 0, a), sb = fs->CalcTextSizeA(smallPx, FLT_MAX, 0, b);
    float barW = 80 * s, barH = std::max(3.0f, std::round(5 * s)), gap = 3 * s;
    float total = sa.x + sb.x;
    float bottom = std::floor(sy - 4 * s);
    ImVec2 bar0(std::floor(sx - barW * 0.5f), bottom - barH), bar1(bar0.x + barW, bottom);
    ImVec2 ta(std::floor(sx - total * 0.5f), bar0.y - gap - sa.y);
    ImVec2 tb(ta.x + sa.x, ta.y + sa.y - sb.y - big * 0.06f);
    for (int ox = -1; ox <= 1; ++ox)
      for (int oy = -1; oy <= 1; ++oy)
        if (ox || oy) {
          dl->AddText(fb, big, ImVec2(ta.x + ox * o, ta.y + oy * o), IM_COL32(0, 0, 0, 255), a);
          dl->AddText(fs, smallPx, ImVec2(tb.x + ox * o, tb.y + oy * o), IM_COL32(0, 0, 0, 255), b);
        }
    dl->AddText(fb, big, ta, IM_COL32(255, 255, 255, 255), a);
    dl->AddText(fs, smallPx, tb, IM_COL32(0xD6, 0xD3, 0xCF, 255), b);
    float f = l.maxHp > 0 ? std::min(1.0f, std::max(0.0f, l.hp / l.maxHp)) : 0;
    dl->AddRectFilled(ImVec2(bar0.x - 1, bar0.y - 1), ImVec2(bar1.x + 1, bar1.y + 1), IM_COL32(0, 0, 0, 255));
    dl->AddRectFilled(bar0, bar1, IM_COL32(0, 0, 0, 205));
    if (f > 0)
      dl->AddRectFilled(bar0, ImVec2(bar0.x + barW * f, bar1.y), f < 0.25f ? ui::kAccent : IM_COL32(0xF2, 0xF0, 0xED, 255));
  }
}

// ------------------------------------------------------------------ the window
float g_styleFor = -1;

void Header(const ImVec2& wp, float W, std::string& tab) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float h = P(24);
  float tpx = P(13);
  ui::Text(dl, ui::F::Title, tpx, ImVec2(wp.x + P(10), wp.y + (h - tpx) * 0.5f), ui::kText, "PRACTICE TOOL", tpx * 0.14f);
  float tw = ui::Measure(ui::F::Title, tpx, "PRACTICE TOOL", tpx * 0.14f).x;
  float vpx = P(10);
  std::string ver = std::string("v") + kToolVersion;
  ui::Text(dl, ui::F::Mono, vpx, ImVec2(wp.x + P(10) + tw + P(8), wp.y + (h - vpx) * 0.5f + P(1)), ui::kOff, ver.c_str());
  if (features::SpeedrunMode()) {  // a green SPEEDRUN tag instead of "Practice Tool Active"
    float sx = wp.x + P(10) + tw + P(8) + ui::Measure(ui::F::Mono, vpx, ver.c_str()).x + P(10);
    float spx = P(11);
    ImVec2 ts = ui::Measure(ui::F::MonoBold, spx, "SPEEDRUN");
    ImVec2 a(sx, wp.y + (h - ts.y) * 0.5f - P(2)), b(sx + ts.x + P(10), wp.y + (h + ts.y) * 0.5f + P(2));
    dl->AddRectFilled(a, b, IM_COL32(0x2F, 0xB8, 0x5F, 255));
    ui::Text(dl, ui::F::MonoBold, spx, ImVec2(sx + P(5), a.y + P(2)), IM_COL32(0x06, 0x16, 0x0C, 255), "SPEEDRUN");
    if (ImGui::IsMouseHoveringRect(a, b))
      ui::SetHint("Speedrun mode: every cheat is off and locked, no \"Practice Tool Active\". Runs tab: turn it "
                  "off.");
  }
  // close (x) at the right end, the game version before it
  ImVec2 x0(wp.x + W - P(4) - P(22), wp.y + P(1));
  ImGui::SetCursorScreenPos(x0);
  bool close = ImGui::InvisibleButton("##close", ImVec2(P(22), P(22)));
  bool xh = ImGui::IsItemHovered();
  if (xh) dl->AddRectFilled(x0, ImVec2(x0.x + P(22), x0.y + P(22)), ui::kAccent);
  float xpx = P(14);
  ImVec2 xs = ui::Measure(ui::F::Mono, xpx, "\xC3\x97");
  ui::Text(dl, ui::F::Mono, xpx, ImVec2(x0.x + (P(22) - xs.x) * 0.5f, x0.y + (P(22) - xpx) * 0.5f),
           xh ? ui::kWhite : IM_COL32(0x9A, 0x9A, 0x9A, 255), "\xC3\x97");
  if (xh) ui::SetHint("Closes the menu (same as the menu key).");
  if (close) {
    state::menuOpen = false;
    LOG("menu closed (x)");
  }
  ImU32 gc;
  std::string gv = GameVersionText(&gc);
  ImVec2 gs = ui::Measure(ui::F::Mono, vpx, gv.c_str());
  float gx = x0.x - P(6) - gs.x;
  float gy = wp.y + (h - vpx) * 0.5f + P(1);
  ui::Text(dl, ui::F::Mono, vpx, ImVec2(gx, gy), gc, gv.c_str());
  dl->AddRectFilled(ImVec2(gx - P(12), gy + vpx * 0.5f - P(3)), ImVec2(gx - P(6), gy + vpx * 0.5f + P(3)), gc);
  if (ImGui::IsMouseHoveringRect(ImVec2(gx - P(12), wp.y), ImVec2(gx + gs.x, wp.y + h)))
    ui::SetHint(GameVersionKnown() ? "The tool knows your game version: everything is made for it."
                                   : "The tool does not know your game version. Nothing is locked, but some things may "
                                     "not work: Settings > Compatibility report helps us add it.");

  // tabs
  float ty = wp.y + h + P(4), tabH = P(4) * 2 + tpx * 1.2f;
  float x = wp.x + P(4);
  int n = g_cfg.showDevTab ? 10 : 9;
  for (int i = 0; i < n; ++i) {
    std::string t = ui::Upper(kTabs[i]);
    float sp = tpx * 0.06f;
    float w = ui::Measure(ui::F::Title, tpx, t.c_str(), sp).x + P(16);
    ImVec2 a(x, ty);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(i);
    bool hit = ImGui::InvisibleButton("##tab", ImVec2(w, tabH));
    bool hv = ImGui::IsItemHovered();
    ImGui::PopID();
    bool on = tab == kTabs[i];
    if (on) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + tabH), ui::kAccent);
    ui::Text(dl, ui::F::Title, tpx, ImVec2(a.x + P(8), a.y + P(4)), on || hv ? ui::kWhite : ui::kTabIdle, t.c_str(), sp);
    if (hit && !on) {
      tab = kTabs[i];
      ui::ClearHint();
      g_editOpen = g_shareOpen = false;
      if (hotkeys::Capturing() && hotkeys::CaptureTarget() != &g_cfg.menuKey) hotkeys::CancelCapture();
    }
    x += w;
  }
  float ly = ty + tabH;
  dl->AddRectFilled(ImVec2(wp.x, ly), ImVec2(wp.x + W, ly + std::max(1.0f, std::round(P(2)))), ui::kAccent);

  // status: where the player is | the last message (fades after 5 s)
  float sy = ly + P(2), spx = P(11);
  nav::Snapshot ns = nav::Now();
  std::string left;
  if (ns.mission && ns.missionIndex >= 0) {
    left = "In mission: " + ShortMission(nav::Missions()[ns.missionIndex].name);
    if (ns.difficulty >= 0 && !nav::Missions()[ns.missionIndex].secret) left += std::string(" \xC2\xB7 ") + nav::DifficultyName(ns.difficulty);
  } else if (ns.mission && ns.devRoom >= 0) {
    left = std::string("In: ") + nav::DevRooms()[ns.devRoom].name;
    if (ns.difficulty >= 0) left += std::string(" \xC2\xB7 ") + nav::DifficultyName(ns.difficulty);
  } else if (ns.mission) {
    left = "In a level: " + ns.map;
  } else if (ns.frontEnd) {
    left = "Main menu";
  } else {
    left = ns.ready ? "Loading\xE2\x80\xA6" : "Starting up\xE2\x80\xA6";
  }
  {
    auto lk = features::Lock();
    std::string act;
    for (const auto& p : features::D().presets)
      if (p.active) act += (act.empty() ? "" : ", ") + p.name;
    if (!act.empty()) left += " \xC2\xB7 preset: " + act;
  }
  std::string toast;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_toast.empty() && GetTickCount64() - g_toastAt < 5000) toast = g_toast;
  }
  float sh = P(3) * 2 + spx * 1.3f;
  float tw2 = toast.empty() ? 0 : ui::Measure(ui::F::Mono, spx, toast.c_str()).x;
  float maxToast = W * 0.62f;
  std::string ts = ui::Ellipsize(ui::F::Mono, spx, toast, maxToast);
  tw2 = std::min(tw2, maxToast);
  ui::Text(dl, ui::F::Mono, spx, ImVec2(wp.x + P(10), sy + P(3)), IM_COL32(0x9A, 0x9A, 0x9A, 255),
           ui::Ellipsize(ui::F::Mono, spx, left, W - P(30) - tw2).c_str());
  if (!ts.empty()) ui::Text(dl, ui::F::Mono, spx, ImVec2(wp.x + W - P(10) - tw2, sy + P(3)), ui::kToast, ts.c_str());
  dl->AddRectFilled(ImVec2(wp.x, sy + sh), ImVec2(wp.x + W, sy + sh + std::max(1.0f, std::round(P(1)))),
                    IM_COL32(0x22, 0x22, 0x22, 255));
  ImGui::SetCursorScreenPos(ImVec2(wp.x, sy + sh + std::max(1.0f, std::round(P(1)))));
}

// The hint box at the bottom: lines of the description, wrapped, 1.55 line height. Always kHintLines high, so
// the list above never changes size when the mouse moves (user: the last row jumped away from under the cursor);
// a longer text is cut, its last line ends with "...". Returns its height.
constexpr int kHintLines = 4;
float HintHeight(const std::string& text, float W, std::vector<std::string>* lines) {
  float px = P(12), lh = px * 1.55f, wrap = W - P(20);
  ImFont* f = ui::Get(ui::F::Mono, px);
  float sc = px / f->FontSize;
  int n = 0;
  size_t s = 0;
  while (s <= text.size()) {
    size_t e = text.find('\n', s);
    if (e == std::string::npos) e = text.size();
    const char* b = text.c_str() + s;
    const char* end = text.c_str() + e;
    if (b == end) {
      ++n;
      if (lines) lines->push_back("");
    }
    while (b < end) {
      const char* w = f->CalcWordWrapPositionA(sc, b, end, wrap);
      if (w == b) w = b + 1;
      if (lines) lines->push_back(std::string(b, w));
      ++n;
      b = w;
      while (b < end && *b == ' ') ++b;
    }
    s = e + 1;
  }
  if (lines && (int)lines->size() > kHintLines) {
    lines->resize(kHintLines);
    std::string& last = lines->back();
    while (!last.empty() && (last.back() == ' ' || last.back() == '.')) last.pop_back();
    std::string dotted = last + "\xE2\x80\xA6";
    last = ui::Ellipsize(ui::F::Mono, px, dotted, wrap);  // fits: dotted itself, else cut with the dots
  }
  (void)n;
  return P(7) * 2 + kHintLines * lh;
}

void Window() {
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  float ss = ui::Screen() * ui::kBoost;  // menu units at the default zoom
  // Size/position: saved as fractions of the screen; the default fits 1080p and grows with the resolution.
  static bool placed = false;
  static float placedW = -1;
  static ImVec2 placedFor;
  bool set = false;
  ImVec2 size, pos;
  if (g_winSet[2] > 0) {  // autorun ui_win
    size = ImVec2(g_winSet[2] * ds.x, g_winSet[3] * ds.y);
    pos = ImVec2(g_winSet[0] * ds.x, g_winSet[1] * ds.y);
    g_winSet[2] = -1;
    set = true;
  } else if (!placed || g_cfg.winW != placedW || placedFor.x != ds.x || placedFor.y != ds.y) {  // + resolution change
    size = g_cfg.winW > 0 ? ImVec2(g_cfg.winW * ds.x, g_cfg.winH * ds.y) : ImVec2(kBaseW * ss, kBaseH * ss);
    pos = g_cfg.winW > 0 ? ImVec2(g_cfg.winX * ds.x, g_cfg.winY * ds.y) : ImVec2(60 * ss, 60 * ss);
    set = true;
  }
  if (set) {
    placedFor = ds;
    placed = true;
    placedW = g_cfg.winW;
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    g_winW = size.x;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(kBaseW * 0.6f * ss, ds.x), std::min(220 * ss, ds.y)), ds);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::kWindowBg);
  bool vis = ImGui::Begin("##DmCPracticeTool", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor();
  if (vis) {
    ImFont* base = ImGui::GetFont();
    ImGui::SetWindowFontScale(P(13) / base->FontSize);
    // keep the whole window on screen (resolution change, saved position from a bigger screen)
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 np(std::max(0.0f, std::min(p.x, ds.x - sz.x)), std::max(0.0f, std::min(p.y, ds.y - sz.y)));
    if (np.x != p.x || np.y != p.y) ImGui::SetWindowPos(np);
    g_winW = sz.x;
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
    // soft shadow under the window
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    for (int i = 1; i <= 8; ++i) {
      float e = P(4.0f * i);
      bg->AddRectFilled(ImVec2(np.x - e * 0.5f, np.y - e * 0.25f + P(10)), ImVec2(np.x + sz.x + e * 0.5f, np.y + sz.y + e * 0.5f + P(10)),
                        IM_COL32(0, 0, 0, 14));
    }
    ComputeConflicts();
    std::string tab;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      tab = g_tab;
    }
    if (tab == "Dev" && !g_cfg.showDevTab) tab = "Settings";
    static std::string hintTab;  // the hint belongs to the tab it was set on (also when a script switches tabs)
    if (hintTab != tab) {
      ui::ClearHint();
      hintTab = tab;
    }
    Header(np, sz.x, tab);
    std::vector<std::string> lines;
    const std::string& hint = ui::Hint();
    std::string shown = hint.empty() ? "Point at a line to see what it does." : hint;
    float hintH = g_cfg.hideHints ? 0 : HintHeight(shown, sz.x, &lines);
    float listH = std::max(P(40), np.y + sz.y - ImGui::GetCursorScreenPos().y - hintH);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::max(3.0f, P(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, P(2)));
    ImGui::BeginChild("##list", ImVec2(sz.x, listH), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleVar(2);
    if (tab == "Cheats") TabCheats();
    else if (tab == "Practice") TabPractice();
    else if (tab == "Enemies") TabEnemies();
    else if (tab == "Visuals") TabVisuals();
    else if (tab == "Runs") TabRuns();
    else if (tab == "Unlocks") TabUnlocks();
    else if (tab == "Saves") TabSave();
    else if (tab == "Hotkeys") TabHotkeys();
    else if (tab == "Settings") TabSettings();
    else if (tab == "Dev") TabDev();
    ui::Space(6);
    ImGui::EndChild();
    if (!g_cfg.hideHints) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      float y = np.y + sz.y - hintH;
      dl->AddRectFilled(ImVec2(np.x, y), ImVec2(np.x + sz.x, y + std::max(1.0f, std::round(P(1)))), IM_COL32(0x2A, 0x2A, 0x2A, 255));
      float px = P(12), lh = px * 1.55f;
      for (size_t i = 0; i < lines.size(); ++i)
        ui::Text(dl, ui::F::Mono, px, ImVec2(np.x + P(10), y + P(7) + i * lh + (lh - px) * 0.5f),
                 hint.empty() ? ui::kOff : ui::kSoft, lines[i].c_str());
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_tab = tab;
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

}  // namespace

void Toast(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_toast = s;
  g_toastAt = GetTickCount64();
}

void ForceDetails(bool open) { g_forceDetails = open; }
void OpenSavePopup(const std::string& which, int index) {
  if (which == "rename") g_popupRename = index;
  if (which == "import") g_popupImport = index;
  if (which == "close") g_popupClose = true;
}

void SelectTab(const std::string& name) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (const char* t : kTabs)
    if (_stricmp(t, name.c_str()) == 0 || (_stricmp(name.c_str(), "save") == 0 && !strcmp(t, "Saves")) ||
        (_stricmp(name.c_str(), "timers") == 0 && !strcmp(t, "Runs")))  // the tab's name up to #16
      g_tab = t;
}

void SetWindow(float x, float y, float w, float h) {
  g_winSet[0] = x;
  g_winSet[1] = y;
  g_winSet[3] = h;
  g_winSet[2] = w;
}

void SetupStyle() { ui::ApplyImGuiStyle(1.0f); }

void Draw() {
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  float ss = std::min(3.0f, std::max(0.6f, ds.y / 1080.0f));
  float base = ss * ui::kBoost;  // the default zoom (user: the prototype sizes were too small to read)
  float w = g_winW > 0 ? g_winW : (g_cfg.winW > 0 ? g_cfg.winW * ds.x : kBaseW * base);
  float zoom = std::min(2.0f, std::max(0.6f, w / (kBaseW * base)));
  ui::SetScales(ss, base * zoom);
  if (std::fabs(g_styleFor - ui::S()) > 0.01f) {
    g_styleFor = ui::S();
    ui::ApplyImGuiStyle(ui::S());
  }
  DrawOverlay();
  ConfigAutosaveTick();
  ImFont* font = ui::Get(ui::F::Mono, 13 * ui::S());
  ImGui::PushFont(font);
  visuals::Draw();  // wireframes first: the health numbers stay readable on top
  DrawEnemyHp();
  damagelog::Draw(state::menuOpen);
  timers::Draw(state::menuOpen);
  if (state::menuOpen) Window();
  ImGui::PopFont();
}

}  // namespace menu
