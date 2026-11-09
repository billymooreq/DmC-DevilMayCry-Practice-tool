#include "speedrun.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "config.h"
#include "features.h"
#include "log.h"
#include "menu.h"
#include "nav.h"
#include "saveslots.h"
#include "timers.h"
#include "ue3/engine.h"

namespace speedrun {
namespace {

std::mutex g_mu;  // g_pick, g_status
Pick g_pick;
Status g_status;
std::atomic<bool> g_busy{false};

// How long Start run waits (ms): for a load to finish before it starts, for the main menu, for the timer.
constexpr int kWaitReady = 60000, kWaitMenu = 60000, kWaitSlot = 20000, kWaitRun = 90000;

void Set(const std::string& text, int kind, bool toast = true) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = {text, kind};
  }
  LOG("speedrun: %s%s", kind == 2 ? "FAILED: " : "", text.c_str());
  if (toast) menu::Toast(text);
}

int MissionIndex(const std::string& id) {
  const auto& ms = nav::Missions();
  for (size_t i = 0; i < ms.size(); ++i)
    if (ms[i].id == id) return (int)i;
  return -1;
}

std::string Short(const nav::Mission& m) {  // "M01 Found"
  size_t d = m.name.find(" - ");
  return m.id + " " + (d == std::string::npos ? m.name : m.name.substr(d + 3));
}

// Polls cond every 100 ms up to ms; true when it held.
template <class F>
bool WaitFor(F cond, int ms) {
  for (int t = 0; t < ms; t += 100) {
    if (cond()) return true;
    Sleep(100);
  }
  return cond();
}

bool Settled(const nav::Snapshot& s) { return s.ready && !s.busy && (s.frontEnd || s.mission); }

void Job(bool reset, Pick p) {
  const char* what = reset ? "Restart run" : "Start run";
  int mi = MissionIndex(p.mission);
  if (mi < 0) {
    Set(std::string(what) + ": mission " + p.mission + " is not in the list", 2);
    return;
  }
  nav::Mission m = nav::Missions()[mi];
  std::string target = Short(m) + (m.secret ? "" : std::string(" \xC2\xB7 ") + nav::DifficultyName(p.difficulty)) +
                       " \xC2\xB7 slot " + std::to_string(p.slot + 1);
  LOG("speedrun: %s - %s", what, target.c_str());
  // Nothing else is switched on (user, #16): Speedrun mode and the timers stay as the player set them - one may run
  // with his own LiveSplit only, or practise with cheats on time.
  timers::ResetRun(what);
  Set(std::string(what) + ": getting ready\xE2\x80\xA6", 0, false);

  // 1. a load in progress ends first
  if (!WaitFor([] { return Settled(nav::Now()); }, kWaitReady)) {
    Set(std::string(what) + ": the game is still loading - try again", 2);
    return;
  }
  nav::Snapshot ns = nav::Now();
  int active = slots::ActiveSlotNow();  // -1 on the title screen
  LOG("speedrun: now %s (map %s), active slot %d, slot status %d", ns.mission ? "in a mission" : "in the main menu",
      ns.map.c_str(), active + 1, ns.slotStatus);

  // 2. another slot from a mission: through the main menu (a slot is loaded only there)
  if (ns.mission && active != p.slot) {
    Set(std::string(what) + ": going to the main menu to load slot " + std::to_string(p.slot + 1) + "\xE2\x80\xA6", 0);
    bool sent = engine::OnGame<bool>([] { return nav::OpenMainMenu(game::W()); }, 5000);
    if (!sent || !WaitFor([] {
          nav::Snapshot s = nav::Now();
          return s.ready && s.frontEnd;
        }, kWaitMenu)) {
      Set(std::string(what) + ": could not get to the main menu", 2);
      return;
    }
    Sleep(1500);  // the menu settles (its own startup script)
    ns = nav::Now();
  }

  // 3. in the main menu: the picked slot, read from disk the way the slot screen does
  if (ns.frontEnd) {
    std::string err;
    if (ns.slotStatus != 3 && ns.slotStatus != 4) {  // the title screen: Start first, like the player
      Set(std::string(what) + ": pressing Start on the title screen\xE2\x80\xA6", 0, false);
      if (!slots::SignInNow(&err)) {
        Set(std::string(what) + ": the title screen did not let go (" + err + ")", 2);
        return;
      }
    }
    Set(std::string(what) + ": loading slot " + std::to_string(p.slot + 1) + "\xE2\x80\xA6", 0, false);
    if (!slots::ReloadSlotNow(p.slot, &err)) {
      Set(std::string(what) + ": slot " + std::to_string(p.slot + 1) + " did not load (" + err + ")", 2);
      return;
    }
    if (!WaitFor([] {
          nav::Snapshot s = nav::Now();
          return Settled(s) && s.frontEnd && (s.slotStatus == 3 || s.slotStatus == 4);
        }, kWaitSlot)) {
      Set(std::string(what) + ": slot " + std::to_string(p.slot + 1) + " loaded, but the main menu did not come back", 2);
      return;
    }
  }

  // 4. the mission, like the tool's Mission select (the game's map name: LiveSplit sees the load)
  Set(std::string(what) + ": starting " + target + "\xE2\x80\xA6", 0);
  bool sent = engine::OnGame<bool>(
      [mi, p] {
        const game::World& w = game::W();
        if (!(w.mission || w.frontEnd)) return false;
        nav::MissionSelect(w, mi, p.difficulty, 0, false);
        return true;
      },
      5000);
  if (!sent) {
    Set(std::string(what) + ": the mission could not be started now", 2);
    return;
  }

  // 5. the check: the LiveSplit-rules run started and its time runs (green digits). Only while a timer of the tool
  // is on - with all of them off the tool does not read the game's loading at all.
  if (!timers::Active()) {
    WaitFor([] { return nav::Now().busy; }, 3000);  // the menu's snapshot sees the load a frame later
    if (!WaitFor([] {
          nav::Snapshot n = nav::Now();
          return !n.busy && n.mission;
        }, kWaitRun)) {
      Set(std::string(what) + ": the mission did not load in " + std::to_string(kWaitRun / 1000) + " s", 2);
      return;
    }
    Set("Mission started: " + target + ".", 1);
    LOG("speedrun: no timer of the tool is on - the run start is not checked");
    return;
  }
  timers::RunInfo r;
  bool ok = WaitFor([&r] {
    r = timers::Run();
    return r.running && r.ticking;
  }, kWaitRun);
  if (ok) {
    Set("Run started: " + target + ". Good luck!", 1);
    LOG("speedrun: run timer running on '%s' (game time %.3f s)", r.map.c_str(), r.game / 1000.0);
    return;
  }
  std::string why;
  if (!r.sampling) why = "the timer cannot read the game (" + (r.loadNote.empty() ? std::string("not ready") : r.loadNote) + ")";
  else if (!r.running) why = "no load of a mission the autosplitter knows was seen (map now: '" + r.map + "')";
  else why = "the game is still loading";
  Set(std::string(what) + ": the run timer did not start in " + std::to_string(kWaitRun / 1000) + " s - " + why, 2);
}

class SpeedrunAction : public features::Feature {
 public:
  SpeedrunAction(const char* i, const char* n, const char* h, void (*run)()) : run_(run) {
    id = i;
    name = n;
    help = h;
    group = features::Group::Speedrun;
    action = true;
    anywhere = true;
    frontEndOk = true;
    noOverlay = true;  // not a cheat: works in Speedrun mode
  }
  void Run(const features::Ctx&) override { run_(); }
  std::string Blocked() const override { return g_busy ? "a run is being started" : ""; }

 private:
  void (*run_)();
};

}  // namespace

Pick GetPick() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_pick;
}

void SetPick(const Pick& p) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pick = p;
  }
  MarkConfigDirty();
}

void LoadJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_pick.slot = std::clamp(j.value("slot", 0), 0, 9);
  g_pick.difficulty = std::clamp(j.value("difficulty", 0), 0, nav::kDifficulties - 1);
  g_pick.mission = j.value("mission", std::string("M01"));
}

nlohmann::json SaveJson() {
  std::lock_guard<std::mutex> lk(g_mu);
  return {{"slot", g_pick.slot}, {"difficulty", g_pick.difficulty}, {"mission", g_pick.mission}};
}

void GoLegit() {
  std::vector<std::string> was = features::GoLegit();
  Set(was.empty() ? "Speedrun mode on: nothing was on" : "Speedrun mode on: " + std::to_string(was.size()) + " turned off", 1);
}

void Leave() {
  if (!features::SpeedrunMode()) return;
  features::LeaveSpeedrun();
  Set("Speedrun mode off: cheats can be turned on again", 1);
}

void StartRun(bool reset) {
  if (g_busy.exchange(true)) {
    menu::Toast("A run is being started already");
    return;
  }
  Pick p = GetPick();
  std::thread([reset, p] {
    Job(reset, p);
    g_busy = false;
  }).detach();
}

bool Busy() { return g_busy; }

Status LastStatus() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_status;
}

std::vector<features::Feature*> CreateFeatures() {
  return {
      new SpeedrunAction("speedrun_mode", "Speedrun mode (on / off)",
                         "On: all cheats off and locked, no \"Practice Tool Active\" - ready to record a run for the "
                         "leaderboard. Off: cheats can be used again.",
                         [] {
                           if (features::SpeedrunMode()) Leave();
                           else GoLegit();
                         }),
      new SpeedrunAction("speedrun_start", "Start run",
                         "Loads the slot and starts the mission picked on the Runs tab. The time starts from zero.",
                         [] { StartRun(false); }),
      new SpeedrunAction("speedrun_reset", "Restart run",
                         "Starts the same run again from zero. Your best times stay.", [] { StartRun(true); }),
      // id from #15 ("Reset timers"), so its key stays; next to Start / Restart run like on the Runs tab
      new SpeedrunAction("timer_reset", "Reset time",
                         "Puts the run time back to zero. The next mission you load starts a new run.",
                         [] { timers::ResetRun("Reset time"); }),
  };
}

std::vector<std::string> DebugLines() {
  Pick p = GetPick();
  Status s = LastStatus();
  timers::RunInfo r = timers::Run();
  char b[512];
  snprintf(b, sizeof b,
           "mode %s busy %d pick slot %d difficulty %d mission %s | run running %d ticking %d loading %d map '%s' "
           "game %.3f s | label %s | status [%d] %s",
           features::SpeedrunMode() ? "SPEEDRUN" : "normal", (int)g_busy, p.slot + 1, p.difficulty, p.mission.c_str(),
           r.running, r.ticking, r.loading, r.map.c_str(), r.game / 1000.0, features::OverlaySticky() ? "shown" : "hidden",
           s.kind, s.text.c_str());
  return {b};
}

}  // namespace speedrun
