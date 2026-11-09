#include "features.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <deque>

#include "config.h"
#include "damagelog.h"
#include "devtools.h"
#include "enemies.h"
#include "log.h"
#include "nav.h"
#include "state.h"
#include "steam_guard.h"
#include "timers.h"
#include "ue3/engine.h"
#include "version.h"
#include "visuals.h"

namespace features {

// feature_list.cpp
std::vector<Feature*> CreateAll();
void OnGameEvent(void* self, void* fn, void* parms);
void OnGameEventAfter(void* self, void* fn, void* parms);

namespace {

std::vector<Feature*> g_all;
std::recursive_mutex g_mu;
Data g_data;
std::vector<bool> g_manual;      // parallel to g_all
std::vector<Params> g_manualP;
uint64_t g_seq = 0;
std::atomic<bool> g_sticky{false};
std::atomic<bool> g_speedrun{false};  // Timers > Speedrun "Go legit": no cheat can turn on

// Game-thread bookkeeping.
struct Runtime {
  bool applied = false;
  unsigned gen = 0;
};
std::vector<Runtime> g_rt;
std::vector<std::atomic<bool>>* g_appliedFlags = nullptr;  // for View (any thread)
std::deque<std::pair<std::string, Params>> g_actions;        // pending action features (under g_mu)

int IndexOf(const std::string& id) {
  for (size_t i = 0; i < g_all.size(); ++i)
    if (id == g_all[i]->id) return (int)i;
  return -1;
}

// Effective state of feature i (under g_mu): on, via preset, parameters.
void Effective(int i, bool& on, bool& viaPreset, Params& p) {
  // Parameters: an active preset containing the feature wins over the menu sliders (the most recently activated
  // one if several do); without one, the sliders.
  p = g_manualP[i];
  bool inPreset = false;
  uint64_t best = 0;
  for (const Preset& ps : g_data.presets) {
    if (!ps.active) continue;
    for (const PresetItem& it : ps.items) {
      if (it.id != g_all[i]->id) continue;
      if (!inPreset || ps.activatedAt >= best) {
        best = ps.activatedAt;
        p = it.p;
      }
      inPreset = true;
    }
  }
  bool manual = g_manual[i];
  // a multiplier is on while its value differs from normal (1x)
  if (manual && g_all[i]->autoOn && !g_all[i]->params.empty() &&
      std::fabs(g_manualP[i][0] - g_all[i]->params[0].def) < 1e-4f)
    manual = false;
  on = manual || inPreset;
  viaPreset = inPreset && !manual;
  if (g_all[i]->action) on = viaPreset = false;  // one-shot: runs when the preset turns on, never "on"
}

// Under g_mu. On and not switched off by a feature with priority over it.
int Winner(int i);
bool Active(int i) {
  bool on, via;
  Params p;
  Effective(i, on, via, p);
  return on && Winner(i) < 0;
}
// The feature with priority over feature i that is active now, -1 = none.
int Winner(int i) {
  for (const char* w : g_all[i]->losesTo) {
    int k = IndexOf(w);
    if (k >= 0 && k != i && Active(k)) return k;
  }
  return -1;
}

// Under g_mu. A feature with priority turned on (by hand or by a preset): the one it wins over is switched off
// (a multiplier goes back to normal), so it stays off when the winner is turned off again.
void EnforcePriority() {
  for (size_t i = 0; i < g_all.size(); ++i) {
    if (g_all[i]->losesTo.empty() || !g_manual[i]) continue;
    int w = Winner((int)i);
    if (w < 0) continue;
    g_manual[i] = false;
    if (g_all[i]->autoOn && !g_all[i]->params.empty()) g_manualP[i][0] = g_all[i]->params[0].def;
    LOG("features: %s off - %s has priority", g_all[i]->id, g_all[w]->id);
  }
}

// Under g_mu. The label "Practice Tool Active" stays from the first cheat until the game exits; from that moment
// Steam achievements and stats are also blocked for the rest of the session.
void UpdateSticky() {
  if (g_sticky || g_speedrun || !FeaturesAllowed()) return;  // unknown build: nothing can turn on, so no label either
  for (size_t i = 0; i < g_all.size(); ++i) {
    bool on, via;
    Params p;
    Effective((int)i, on, via, p);
    if (on && !g_all[i]->noOverlay) {
      g_sticky = true;
      LOG("features: first cheat this session (%s) - overlay on until exit, achievements blocked", g_all[i]->id);
      steam::BlockForSession();
      return;
    }
  }
}

// ------------------------------------------------------------------ pause while the menu is open
// Settings > "Pause the game while the menu is open": WorldInfo.Pauser, the engine's own pause (what the game's
// pause menu sets through SetPause): the world, physics, cutscenes and game sounds stand still; the menu, the
// camera and our frame callback keep running. No text is shown. TimeDilation (Game speed) is not touched, so
// everything goes on from the same moment when the menu closes. Only set when nothing else paused the game.
struct MenuPause {
  bool ours = false;
  uintptr_t world = 0, pauser = 0;
} g_pause;

void PauseForMenu(const game::World& w, const game::Status& st) {
  bool want = g_cfg.pauseInMenu && !g_speedrun && state::menuOpen && st.ready && w.mission && w.world && w.pc;
  if (g_pause.ours && (!want || w.world != g_pause.world)) {
    if (game::Alive(g_pause.world)) {
      game::Field f = game::FieldOf(g_pause.world, "Pauser");
      auto* p = f.ok() ? reinterpret_cast<uintptr_t*>(g_pause.world + f.off) : nullptr;
      if (p && *p == g_pause.pauser) *p = 0;  // still our pause (the game did not change it)
    }
    LOG("features: menu pause off");
    g_pause = {};
  }
  if (!want || g_pause.ours) return;
  game::Field f = game::FieldOf(w.world, "Pauser");
  uintptr_t pri = game::GetObj(w.pc, "PlayerReplicationInfo");
  if (!f.ok() || !pri) return;
  auto* p = reinterpret_cast<uintptr_t*>(w.world + f.off);
  if (*p) return;  // the game is paused already (its own pause menu)
  *p = pri;
  g_pause = {true, w.world, pri};
  LOG("features: menu pause on");
}

// ------------------------------------------------------------------ frame (game thread)
void OnFrame() {
  game::OnFrame();
  if (!FeaturesAllowed()) return;  // unknown build: no per-frame reads of game objects at all
  const game::World& w = game::W();
  game::Status st = game::CurrentStatus();
  bool allowed = st.ready && w.mission;
  PauseForMenu(w, st);
  nav::OnFrame(w);
  enemies::OnFrame(w);
  damagelog::OnFrame();
  timers::OnFrame(w);
  visuals::OnFrame(w);

  struct Want {
    bool on;
    Params p;
  };
  std::vector<Want> want(g_all.size());
  std::vector<std::pair<std::string, Params>> actions;
  {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    EnforcePriority();
    for (size_t i = 0; i < g_all.size(); ++i) {
      bool on, via;
      Effective((int)i, on, via, want[i].p);
      if (on && Winner((int)i) >= 0) on = false;
      if (g_speedrun && IsCheat(g_all[i])) on = false;  // Speedrun mode: nothing that changes the game
      want[i].on = on && (g_all[i]->anywhere ? st.ready : allowed) && !g_all[i]->action;
    }
    // a press that cannot run now is dropped, never kept for later (a load must not start by itself after a
    // loading screen)
    actions.assign(g_actions.begin(), g_actions.end());
    g_actions.clear();
  }
  for (size_t i = 0; i < g_all.size(); ++i) {
    Feature* f = g_all[i];
    Runtime& rt = g_rt[i];
    Ctx ctx{w, want[i].p};
    if (rt.applied && (!want[i].on || rt.gen != w.gen)) {
      f->Disable();
      rt.applied = false;
      LOG("features: %s off%s", f->id, want[i].on ? " (world changed)" : "");
    }
    if (!rt.applied && want[i].on && f->Enable(ctx)) {
      rt.applied = true;
      rt.gen = w.gen;
      LOG("features: %s on", f->id);
    }
    if (rt.applied) f->Tick(ctx);
    (*g_appliedFlags)[i] = rt.applied;
  }
  for (const auto& [id, p] : actions) {
    int i = IndexOf(id);
    if (i < 0) continue;
    Feature* f = g_all[i];
    if (g_speedrun && IsCheat(f)) {
      LOG("features: %s ignored (Speedrun mode)", id.c_str());
      continue;
    }
    if (!allowed && !(f->frontEndOk && st.ready && w.frontEnd) && !(f->anywhere && st.ready)) {
      LOG("features: %s ignored (%s)", id.c_str(), st.reason.empty() ? "not now" : st.reason.c_str());
      continue;
    }
    LOG("features: run %s", id.c_str());
    f->Run(Ctx{w, p});
  }
}

}  // namespace

std::string ParamDef::Text(float v) const {
  if (kind == ParamKind::Toggle) return v != 0 ? "on" : "off";
  if (kind == ParamKind::Choice) {
    int i = (int)(v + 0.5f);
    return i >= 0 && i < (int)choices.size() ? choices[i] : "?";
  }
  char b[32];
  snprintf(b, sizeof b, fmt, v);
  return b;
}

const char* GroupName(Group g) {
  switch (g) {
    case Group::Player: return "Player";
    case Group::Enemies: return "Enemies";
    case Group::Damage: return "Damage";
    case Group::Speed: return "Speed";
    case Group::Visuals: return "Visuals";
    case Group::Navigation: return "Practice";
    case Group::Positions: return "Practice";
    case Group::Missions: return "Practice";
    case Group::EnemyTools: return "Enemies";
    case Group::DamageLog: return "Enemies";
    case Group::Cutscenes: return "Practice";
    case Group::Camera: return "Camera";
    case Group::Timers: return "Runs";
    case Group::Speedrun: return "Speedrun";
  }
  return "?";
}

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  nav::Init();
  g_all = CreateAll();
  g_manual.assign(g_all.size(), false);
  g_manualP.resize(g_all.size());
  for (size_t i = 0; i < g_all.size(); ++i)
    for (size_t k = 0; k < g_all[i]->params.size() && k < kMaxParams; ++k) g_manualP[i][k] = g_all[i]->params[k].def;
  g_data.binds.resize(g_all.size());
  g_rt.resize(g_all.size());
  g_appliedFlags = new std::vector<std::atomic<bool>>(g_all.size());
  engine::SetFrameCallback(&OnFrame);
  engine::SetEventHook(&OnGameEvent);
  engine::SetEventHookAfter(&OnGameEventAfter);
  devtools::Init();
  timers::Start();
  LOG("features: %zu registered", g_all.size());
}

const std::vector<Feature*>& All() { return g_all; }

Feature* Find(const std::string& id) {
  int i = IndexOf(id);
  return i < 0 ? nullptr : g_all[i];
}

bool AnyInGroup(Group g) {
  for (Feature* f : g_all)
    if (f->group == g) return true;
  return false;
}

View Get(const std::string& id) {
  View v;
  int i = IndexOf(id);
  if (i < 0) return v;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  v.manual = g_manual[i];
  v.manualP = g_manualP[i];
  Effective(i, v.on, v.viaPreset, v.p);
  v.applied = (*g_appliedFlags)[i];
  if (v.on && !v.applied) {
    Feature* f = g_all[i];
    game::Status st = game::CurrentStatus();
    if (int w = Winner(i); w >= 0) {
      v.why = std::string(g_all[w]->name) + " has priority";
    } else if (!FeaturesAllowed()) {
      v.why = "unknown game version";
    } else if (!st.mission && !f->anywhere) {
      v.why = "starts in a mission";
    }
  }
  return v;
}

void SetManual(const std::string& id, bool on) {
  int i = IndexOf(id);
  if (i < 0) return;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (g_all[i]->action) return;
  if (on && Winner(i) >= 0) return;  // locked: a feature with priority is on
  if (on && g_speedrun && IsCheat(g_all[i])) {
    LOG("features: %s not turned on (Speedrun mode)", id.c_str());
    return;
  }
  if (g_manual[i] != on) LOG("features: %s %s by the player", id.c_str(), on ? "ticked" : "unticked");
  g_manual[i] = on;
  EnforcePriority();
  UpdateSticky();
}

void ToggleManual(const std::string& id) {
  int i = IndexOf(id);
  if (i < 0) return;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  SetManual(id, !g_manual[i]);
}

void SetParam(const std::string& id, int k, float v) {
  int i = IndexOf(id);
  if (i < 0 || k < 0 || k >= (int)g_all[i]->params.size()) return;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  const ParamDef& d = g_all[i]->params[k];
  Feature* f = g_all[i];
  if (f->autoOn && k == 0 && Winner(i) >= 0) return;  // locked multiplier stays at normal
  if (g_speedrun && IsCheat(f)) return;               // Speedrun mode: cheats stay as they are (off)
  g_manualP[i][k] = std::min(d.hardMax, std::max(d.min, v));
  if (f->autoOn && k == 0) {  // the value is the switch
    bool on = std::fabs(g_manualP[i][k] - d.def) >= 1e-4f;
    if (on != g_manual[i]) LOG("features: %s %s (%s)", f->id, on ? "on" : "off", d.Text(g_manualP[i][k]).c_str());
    g_manual[i] = on;
    EnforcePriority();
    UpdateSticky();
  }
  MarkDirty();
}

Feature* LockedBy(const std::string& id) {
  int i = IndexOf(id);
  if (i < 0) return nullptr;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  int w = Winner(i);
  return w >= 0 ? g_all[w] : nullptr;
}

void Trigger(const std::string& id) {
  int i = IndexOf(id);
  if (i < 0) return;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  Trigger(id, g_manualP[i]);
}

void Trigger(const std::string& id, const Params& p) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  int i = IndexOf(id);
  if (g_speedrun && i >= 0 && IsCheat(g_all[i])) {
    LOG("features: %s ignored (Speedrun mode)", id.c_str());
    return;
  }
  if (g_actions.size() >= 8) return;  // a stuck queue (game thread busy) never grows
  g_actions.push_back({id, p});
  if (!g_sticky && FeaturesAllowed() && !(i >= 0 && g_all[i]->noOverlay)) {
    g_sticky = true;
    steam::BlockForSession();
    LOG("features: first cheat this session (%s) - overlay on until exit", id.c_str());
  }
}

std::unique_lock<std::recursive_mutex> Lock() { return std::unique_lock<std::recursive_mutex>(g_mu); }
Data& D() { return g_data; }

void TogglePreset(int index) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (index < 0 || index >= (int)g_data.presets.size()) return;
  Preset& p = g_data.presets[index];
  if (!p.active && g_speedrun) {
    LOG("features: preset '%s' not turned on (Speedrun mode)", p.name.c_str());
    return;
  }
  p.active = !p.active;
  p.activatedAt = p.active ? ++g_seq : 0;
  LOG("features: preset '%s' %s (%zu features)", p.name.c_str(), p.active ? "ON" : "off", p.items.size());
  if (p.active)  // one-shot actions of the preset run once, when it turns on
    for (const PresetItem& it : p.items)
      if (Feature* f = Find(it.id); f && f->action && !f->noBind) Trigger(it.id, it.p);
  EnforcePriority();
  UpdateSticky();
}

void MarkDirty() { MarkConfigDirty(); }

bool OverlaySticky() { return g_sticky && !g_speedrun; }
void ForceOverlay() { g_sticky = true; }

bool IsCheat(const Feature* f) { return f && !f->noOverlay; }
bool SpeedrunMode() { return g_speedrun; }

std::vector<std::string> GoLegit() {
  std::vector<std::string> was;
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  for (size_t i = 0; i < g_all.size(); ++i) {
    Feature* f = g_all[i];
    if (!IsCheat(f)) continue;
    bool on, via;
    Params p;
    Effective((int)i, on, via, p);
    if (on) was.push_back(f->name);
    g_manual[i] = false;
    if (f->autoOn && !f->params.empty()) g_manualP[i][0] = f->params[0].def;  // multipliers back to normal
  }
  for (Preset& ps : g_data.presets)
    if (ps.active) {
      was.push_back("preset " + ps.name);
      ps.active = false;
      ps.activatedAt = 0;
    }
  // presses still waiting for the game thread
  for (auto it = g_actions.begin(); it != g_actions.end();) {
    int k = IndexOf(it->first);
    it = k >= 0 && IsCheat(g_all[k]) ? g_actions.erase(it) : it + 1;
  }
  // settings that are cheats in a run: the menu pause (a free pause) and the game's debug menu (god mode...)
  if (g_cfg.pauseInMenu) was.push_back("Pause the game while the menu is open");
  if (g_cfg.debugMenu) was.push_back("Unlock the game's debug menu");
  g_cfg.pauseInMenu = false;
  g_cfg.debugMenu = false;
  MarkConfigDirty();
  g_sticky = false;
  g_speedrun = true;
  std::string list;
  for (const std::string& w : was) list += (list.empty() ? "" : ", ") + w;
  LOG("features: GO LEGIT - Speedrun mode on, label off; turned off: %s", list.empty() ? "nothing was on" : list.c_str());
  return was;
}

void LeaveSpeedrun() {
  if (!g_speedrun) return;
  g_speedrun = false;
  LOG("features: Speedrun mode off (nothing turned back on)");
}

game::Status Status() { return game::CurrentStatus(); }

// ------------------------------------------------------------------ config
void LoadJson(const nlohmann::json& feats, const nlohmann::json& presets) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  hotkeys::CancelCapture();  // may point into the presets being replaced
  for (size_t i = 0; i < g_all.size(); ++i) {
    Feature* f = g_all[i];
    g_data.binds[i] = {};
    for (size_t k = 0; k < f->params.size() && k < kMaxParams; ++k) g_manualP[i][k] = f->params[k].def;
    if (!feats.is_object() || !feats.contains(f->id)) continue;
    if (f->noBind) continue;  // an old bind / setting of a button that has none any more
    const nlohmann::json& j = feats[f->id];
    if (j.contains("bind")) g_data.binds[i] = hotkeys::FromJson(j["bind"]);
    if (j.contains("params") && j["params"].is_object())
      for (size_t k = 0; k < f->params.size() && k < kMaxParams; ++k) {
        const ParamDef& d = f->params[k];
        // a multiplier's value is its switch: every game start begins at normal (1x), like every feature off
        if (f->autoOn && k == 0) continue;
        float v = j["params"].value(d.id, d.def);
        g_manualP[i][k] = std::min(d.hardMax, std::max(d.min, v));
      }
  }
  std::vector<Preset> old = g_data.presets;
  g_data.presets.clear();
  if (presets.is_array())
    for (const nlohmann::json& pj : presets) {
      if (!pj.is_object()) continue;
      Preset p;
      p.name = pj.value("name", std::string("Preset"));
      if (pj.contains("bind")) p.bind = hotkeys::FromJson(pj["bind"]);
      if (pj.contains("features") && pj["features"].is_array())
        for (const nlohmann::json& ij : pj["features"]) {
          PresetItem it;
          it.id = ij.value("id", std::string());
          Feature* f = Find(it.id);
          if (!f) continue;
          for (size_t k = 0; k < f->params.size() && k < kMaxParams; ++k) {
            const ParamDef& d = f->params[k];
            float v = ij.contains("params") && ij["params"].is_object() ? ij["params"].value(d.id, d.def) : d.def;
            it.p[k] = std::min(d.hardMax, std::max(d.min, v));
          }
          p.items.push_back(it);
        }
      for (const Preset& o : old)  // a reload keeps presets that are running
        if (o.name == p.name && o.active) {
          p.active = true;
          p.activatedAt = o.activatedAt;
        }
      g_data.presets.push_back(p);
    }
}

void SaveJson(nlohmann::json& feats, nlohmann::json& presets) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  feats = nlohmann::json::object();
  for (size_t i = 0; i < g_all.size(); ++i) {
    Feature* f = g_all[i];
    nlohmann::json j = nlohmann::json::object();
    if (!g_data.binds[i].Empty()) j["bind"] = hotkeys::ToJson(g_data.binds[i]);
    if (!f->params.empty()) {
      nlohmann::json pj = nlohmann::json::object();
      for (size_t k = 0; k < f->params.size() && k < kMaxParams; ++k) pj[f->params[k].id] = g_manualP[i][k];
      j["params"] = pj;
    }
    if (!j.empty()) feats[f->id] = j;
  }
  presets = nlohmann::json::array();
  for (const Preset& p : g_data.presets) {
    nlohmann::json pj = {{"name", p.name}, {"bind", hotkeys::ToJson(p.bind)}};
    nlohmann::json items = nlohmann::json::array();
    for (const PresetItem& it : p.items) {
      Feature* f = Find(it.id);
      if (!f) continue;
      nlohmann::json ij = {{"id", it.id}};
      if (!f->params.empty()) {
        nlohmann::json par = nlohmann::json::object();
        for (size_t k = 0; k < f->params.size() && k < kMaxParams; ++k) par[f->params[k].id] = it.p[k];
        ij["params"] = par;
      }
      items.push_back(ij);
    }
    pj["features"] = items;
    presets.push_back(pj);
  }
}

std::string DebugText() {
  std::string s;
  for (Feature* f : g_all) {
    View v = Get(f->id);
    char b[256];
    snprintf(b, sizeof b, "%s: manual=%d on=%d preset=%d applied=%d p=%.2f/%.2f%s%s\n", f->id, v.manual, v.on,
             v.viaPreset, v.applied, v.p[0], v.p[1], v.why.empty() ? "" : " why=", v.why.c_str());
    s += b;
  }
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  for (const Preset& p : g_data.presets) s += "preset '" + p.name + "' " + (p.active ? "ACTIVE" : "off") + "\n";
  s += std::string("overlay sticky: ") + (g_sticky ? "yes" : "no") + ", label shown: " + (OverlaySticky() ? "yes" : "no") +
       ", speedrun mode: " + (g_speedrun ? "yes" : "no") + ", menu pause: " + (g_cfg.pauseInMenu ? "on" : "off");
  return s;
}

}  // namespace features
