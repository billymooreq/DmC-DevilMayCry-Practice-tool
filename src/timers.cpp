#include "timers.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "config.h"
#include "features.h"
#include "imgui.h"
#include "log.h"
#include "nav.h"
#include "ui.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"

namespace timers {
namespace {

// ---- the autosplitter's map list (DMCDevilMayCry.asl, all settings on by default). Case matters, as in LiveSplit
// (its settings are a Dictionary<string, ...>): the .asl lists "DLC1_Ch1_main" and "DLC1_ch1_main" separately.
const std::set<std::string> kRunMaps = {
    "ch1_main", "ch2_main", "ch4_main", "ch5_main", "ch6_main", "ch7_main", "ch8_main", "ch9A_main", "ch9B_main",
    "ch10_main", "ch11_main", "ch12_main", "ch13_main", "ch14_main", "ch15_main", "ch16_main", "ch17_main",
    "ch18_main", "ch19_main", "ch20_main", "DLC1_Ch1_main", "DLC1_ch1_main", "DLC1_ch2_main", "DLC1_ch3_main",
    "DLC1_ch4_main", "DLC1_ch5_main", "DLC1_ch6_main", "SecretMission01_Main", "SecretMission02_Main",
    "SecretMission03_Main", "SecretMission04_Main", "SecretMission05_Main", "SecretMission06_Main",
    "SecretMission07_Main", "SecretMission08_Main", "SecretMission09_Main", "SecretMission10_Main",
    "SecretMission11_Main", "SecretMission12_Main", "SecretMission13_Main", "SecretMission14_Main",
    "SecretMission15_Main", "SecretMission16_Main", "SecretMission17_Main", "SecretMission18_Main",
    "SecretMission19_Main", "SecretMission20_Main", "SecretMission21_Main"};
constexpr const char* kResetMap = "mainmenu_startup_vergil";
// The full game (the .asl's "DMC" splits, LiveSplit's 20 segments Found .. The End): its first and last map. A run
// from ch1_main through all of them to the end of ch20_main is a full run (Personal best); the sum of the best
// segment of each of them is the Sum of best. Secret missions and Vergil's Downfall are not part of it.
const char* const kStoryMaps[] = {"ch1_main",  "ch2_main",  "ch4_main",  "ch5_main",  "ch6_main",
                                  "ch7_main",  "ch8_main",  "ch9A_main", "ch9B_main", "ch10_main",
                                  "ch11_main", "ch12_main", "ch13_main", "ch14_main", "ch15_main",
                                  "ch16_main", "ch17_main", "ch18_main", "ch19_main", "ch20_main"};
constexpr int kNumStory = sizeof kStoryMaps / sizeof kStoryMaps[0];
constexpr const char* kFirstMap = "ch1_main";
constexpr const char* kLastMap = "ch20_main";

// The function of the game that reads the loading int (v1.3 RVA 0xAA4080, v1.0 0xA9C5D0):
// mov eax,[global] / test / je / push esi / push edi / lea esi,[eax+0C] / ... / cmp dword [edi+off],0 / je / test byte [edi+24],80
constexpr const char* kSigLoading =
    "A1 ?? ?? ?? ?? 85 C0 74 ?? 56 57 8D 70 0C 56 8B F8 FF 15 ?? ?? ?? ?? 85 C0 75 ?? 56 FF 15 ?? ?? ?? ?? 83 BF ?? ?? ?? "
    "?? 00 74 ?? F6 47 24 80";

// ---- feature switches (polled by the sampler and the game thread)
const char* const kIds[] = {"timer_mission_igt", "timer_mission_ls", "timer_run_ls", "timer_run_rta", "timer_total_igt"};
enum { kMissionIgt, kMissionLs, kRunLs, kRunRta, kTotalIgt, kCount };
std::atomic<bool> g_show[kCount];
std::atomic<bool> g_any{false};

void PollSwitches() {
  bool any = false;
  for (int i = 0; i < kCount; ++i) {
    bool on = features::Get(kIds[i]).on;
    g_show[i] = on;
    any |= on;
  }
  g_any = any;
}

double NowMs() {
  static LARGE_INTEGER fq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)fq.QuadPart;
}

struct Split {
  std::string map;  // the mission that ended (LiveSplit: its segment)
  double at = 0;    // game time of the split
  double seg = 0;   // game time of that mission's segment
  double best = -1; // best segment before this one (-1 = none)
  double pbSeg = -1; // the personal-best run's segment of this mission (-1 = none / not a full run)
  double pbAt = -1;  // the personal-best run's time at this split
};

struct State {
  // where the two values are (sampler thread)
  uintptr_t loadingGlobal = 0;  // address of the global pointer
  int loadingOff = -1;
  uintptr_t engine = 0;
  int mapOff = -1;
  std::string resolveNote;
  // last sample
  bool loading = false, prevLoading = false;
  int flag = 0;
  std::string map;
  double last = 0;
  // LiveSplit run
  bool running = false;
  bool finished = false;    // the run ended (LiveSplit "Ended"): time frozen, no new start until a reset
  bool newPb = false;       // ... and it was a new personal best
  std::string startMap;     // where the run started
  double game = 0;          // Game Time (ms)
  double rtaStart = 0;      // real time of the start
  double rtaFinal = 0;      // after a reset
  double loadExcluded = 0;  // loading time not counted since the start
  int loads = 0;
  std::set<std::string> done;
  std::vector<Split> splits;
  std::string cur;          // the mission being run (its map name as the .asl sees it)
  double segStart = 0;      // game time when this mission's segment began
  double lastLoadStart = 0; // game time when the last load began
  double sinceLoad = 0;     // real time not loading since the last load began (any run state)
  bool missionStopped = false;
  double missionFinal = 0;
  // game thread values
  float igt = -1;
  std::string igtMap;
  bool igtResults = false;
  double total = -1;  // seconds
};
std::mutex g_mu;
State g_s;

// ---- best splits / segments: Binaries\Win32\DmCPT_timers.json
std::map<std::string, double> g_bestSplit;              // mission map -> best LiveSplit segment (ms)
// The personal-best run's splits (map -> its segment, its time at that split): recorded when a finished full run
// is faster, editable on the Runs tab. The personal best is their sum (all 20 story missions).
std::map<std::string, double> g_pbSeg, g_pbAt;
bool g_bestLoaded = false;

std::wstring BestPath() { return util::InExeDir(L"DmCPT_timers.json"); }

void LoadBest() {  // under g_mu
  if (g_bestLoaded) return;
  g_bestLoaded = true;
  std::ifstream f(BestPath());
  if (!f) return;
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    // named copies: iterating items() of a temporary would read a destroyed object
    nlohmann::json splits = j.value("best_splits", nlohmann::json::object());
    for (auto& [k, v] : splits.items())
      if (v.is_number()) g_bestSplit[k] = v.get<double>();
    nlohmann::json pbs = j.value("pb_splits", nlohmann::json::object());
    for (auto& [k, v] : pbs.items())
      if (v.is_object()) {
        g_pbSeg[k] = v.value("segment_ms", -1.0);
        g_pbAt[k] = v.value("at_ms", -1.0);
      }
    LOG("timers: best times loaded: %zu best segment(s), %zu personal-best segment(s)", g_bestSplit.size(),
        g_pbSeg.size());
  } catch (...) {
    LOG("timers: %s unreadable - best times start empty", util::WideToUtf8(BestPath()).c_str());
  }
}

void SaveBest() {  // under g_mu
  nlohmann::json j;
  j["note"] = "DmC Practice Tool: best and personal-best LiveSplit-timed mission segments, milliseconds";
  if (!g_pbAt.empty()) {
    j["pb_splits"] = nlohmann::json::object();
    for (auto& [k, at] : g_pbAt)
      j["pb_splits"][k] = {{"segment_ms", std::round(g_pbSeg[k] * 1000) / 1000}, {"at_ms", std::round(at * 1000) / 1000}};
  }
  j["best_splits"] = nlohmann::json::object();
  for (auto& [k, v] : g_bestSplit) j["best_splits"][k] = std::round(v * 1000) / 1000;
  std::ofstream f(BestPath(), std::ios::binary | std::ios::trunc);
  f << j.dump(2);
}

// The personal best: the sum of its 20 segments (-1 while one is missing). Under g_mu.
double PbTotal() {
  double sum = 0;
  for (const char* m : kStoryMaps) {
    auto it = g_pbSeg.find(m);
    if (it == g_pbSeg.end() || it->second < 0) return -1;
    sum += it->second;
  }
  return sum;
}

void FillPb(const State& s, Split& sp) {  // under g_mu
  if (s.startMap != kFirstMap) return;
  if (auto it = g_pbSeg.find(sp.map); it != g_pbSeg.end()) sp.pbSeg = it->second;
  if (auto it = g_pbAt.find(sp.map); it != g_pbAt.end()) sp.pbAt = it->second;
}

// The end of the run (under g_mu): the last segment is split, the time stops (LiveSplit's final split). A full run
// (ch1_main .. ch20_main, every story map) faster than the personal best becomes the new one.
void Finish(State& s, double now, const char* why) {
  if (!s.running) return;
  if (!s.prevLoading && s.last > 0) s.game += now - s.last;  // up to this very moment
  s.last = now;
  Split sp;
  sp.map = s.cur;
  sp.at = s.game;
  sp.seg = s.game - (s.splits.empty() ? 0.0 : s.splits.back().at);
  auto it = g_bestSplit.find(sp.map);
  sp.best = it == g_bestSplit.end() ? -1 : it->second;
  FillPb(s, sp);
  if (sp.best < 0 || sp.seg < sp.best) g_bestSplit[sp.map] = sp.seg;
  s.splits.push_back(sp);
  s.running = false;
  s.finished = true;
  s.rtaFinal = now - s.rtaStart;
  if (!s.missionStopped) {
    s.missionStopped = true;
    s.missionFinal = s.game - s.segStart;
  }
  bool full = s.startMap == kFirstMap && s.cur == kLastMap;
  for (const char* m : kStoryMaps) full = full && s.done.count(m);
  double pb = PbTotal();
  s.newPb = full && (pb < 0 || s.game < pb);
  if (s.newPb) {  // its splits become the personal best
    g_pbSeg.clear();
    g_pbAt.clear();
    for (const Split& x : s.splits) g_pbSeg[x.map] = x.seg, g_pbAt[x.map] = x.at;
  }
  SaveBest();
  LOG("timers: run END (%s) at %s - last segment '%s' %s; %s", why, Format(s.game).c_str(), sp.map.c_str(),
      Format(sp.seg).c_str(), !full ? "not a full run (no personal best)" : s.newPb ? "NEW PERSONAL BEST" : "no PB");
}

// ---- the .asl's start / split / reset, applied to one sample (under g_mu)
void Start(State& s, double now) {
  s.running = true;
  s.finished = s.newPb = false;
  s.startMap = s.map;
  s.game = 0;
  s.rtaStart = now;
  s.loadExcluded = 0;
  s.loads = 0;
  s.done = {s.map};
  s.splits.clear();
  s.cur = s.map;
  s.segStart = 0;
  s.lastLoadStart = 0;
  s.missionStopped = false;
  LOG("timers: run START on '%s' (LiveSplit rules)", s.map.c_str());
}

void DoSplit(State& s) {
  Split sp;
  sp.map = s.cur;
  sp.at = s.game;
  sp.seg = s.game - (s.splits.empty() ? 0.0 : s.splits.back().at);
  auto it = g_bestSplit.find(sp.map);
  sp.best = it == g_bestSplit.end() ? -1 : it->second;
  FillPb(s, sp);
  if (sp.best < 0 || sp.seg < sp.best) {
    g_bestSplit[sp.map] = sp.seg;
    SaveBest();
  }
  s.splits.push_back(sp);
  s.done.insert(s.map);
  LOG("timers: SPLIT '%s' -> '%s' at %s (segment %s, best %s)", sp.map.c_str(), s.map.c_str(), Format(sp.at).c_str(),
      Format(sp.seg).c_str(), sp.best < 0 ? "-" : Format(sp.best).c_str());
  s.cur = s.map;
  s.segStart = s.game;
  s.missionStopped = false;
}

void Reset(State& s, double now, const char* why) {
  if (s.running) LOG("timers: run RESET (%s) at %s", why, Format(s.game).c_str());
  if (s.finished) LOG("timers: finished run cleared (%s)", why);
  s.running = false;
  s.finished = s.newPb = false;
  s.rtaFinal = s.rtaStart > 0 ? now - s.rtaStart : 0;
  s.game = 0;
  s.splits.clear();
  s.done.clear();
  s.cur.clear();
  s.missionStopped = false;
}

void Sample(double now, bool loading, int flag, const std::string& map) {
  std::lock_guard<std::mutex> lk(g_mu);
  State& s = g_s;
  double dt = s.last > 0 ? now - s.last : 0;
  s.last = now;
  // time since the previous sample counts unless the game was loading then (LiveSplit pauses Game Time from the
  // update that saw "loading" until the update that sees it end)
  if (s.running) {
    if (!s.prevLoading) s.game += dt;
    else s.loadExcluded += dt;
  }
  if (!s.prevLoading) s.sinceLoad += dt;
  s.loading = loading;
  s.flag = flag;
  s.map = map;
  bool inList = kRunMaps.count(map) != 0;
  if (loading && !s.prevLoading) {
    s.lastLoadStart = s.game;
    s.sinceLoad = 0;
    if (s.running) ++s.loads;
  }
  if (!s.running) {
    if (s.finished) {  // like LiveSplit after the last split: only a reset starts over
      if (loading && map == kResetMap) Reset(s, now, "main menu of Vergil's Downfall");
    } else if (inList && loading) {
      Start(s, now);
    }
  } else if (loading && map == kResetMap) {
    Reset(s, now, "main menu of Vergil's Downfall");
  } else if (inList && loading && !s.done.count(map)) {
    DoSplit(s);
  }
  s.prevLoading = loading;
}

bool Resolve(State& r) {
  if (!ue3::Ready()) return false;
  if (!r.loadingGlobal) {
    uintptr_t hit = ue3::SigScan(kSigLoading);
    if (!hit) {
      r.resolveNote = "loading flag: code not found";
      return false;
    }
    r.loadingGlobal = util::ReadOr<uintptr_t>(hit + 1);
    r.loadingOff = (int)util::ReadOr<uint32_t>(hit + 36);
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    char b[200];
    snprintf(b, sizeof b, "loading flag: [exe+0x%X]+0x%X (code at exe+0x%X)", (unsigned)(r.loadingGlobal - base),
             (unsigned)r.loadingOff, (unsigned)(hit - base));
    r.resolveNote = b;
    LOG("timers: %s", b);
  }
  if (!r.engine) {
    for (int i = 0, n = ue3::NumObjects(); i < n && !r.engine; ++i) {
      uintptr_t o = ue3::Object(i);
      if (o && ue3::ClassNameOf(o) == "DVGameEngine" && ue3::ObjName(o).find("Default__") != 0) r.engine = o;
    }
    if (!r.engine) return false;
    r.mapOff = game::FieldOf(r.engine, "TransitionDescription").off;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    LOG("timers: map name: %s.TransitionDescription = engine 0x%08X +0x%X", ue3::ObjName(r.engine).c_str(),
        (unsigned)r.engine, (unsigned)r.mapOff);
    // The .asl's own addresses (v1.3 "Current"): must be the same things.
    if (GetGameVersion().version == GameVersion::Latest2025) {
      uintptr_t aslEngine = util::ReadOr<uintptr_t>(base + 0x272C328);
      LOG("timers: check against DMCDevilMayCry.asl (Current): loading global %s (exe+0x27521C0 +0x5B4), map %s "
          "([exe+0x272C328]=0x%08X +0x560)",
          r.loadingGlobal == base + 0x27521C0 && r.loadingOff == 0x5B4 ? "SAME" : "DIFFERENT",
          aslEngine == r.engine && r.mapOff == 0x560 ? "SAME" : "DIFFERENT", (unsigned)aslEngine);
    }
  }
  return r.engine && r.mapOff >= 0 && r.loadingGlobal && r.loadingOff >= 0;
}

void SamplerLoop() {
  State r;  // addresses, resolved once
  bool fine = false;
  int poll = 0;
  for (;;) {
    if (++poll >= (fine ? 50 : 1)) {  // switches: every ~50 ms while sampling
      poll = 0;
      PollSwitches();
    }
    if (!g_any) {
      if (fine) {
        timeEndPeriod(1);
        fine = false;
      }
      Sleep(200);
      continue;
    }
    if (!r.engine && !Resolve(r)) {
      Sleep(500);
      continue;
    }
    if (!fine) {
      timeBeginPeriod(1);
      fine = true;
      std::lock_guard<std::mutex> lk(g_mu);
      LoadBest();
      g_s.resolveNote = r.resolveNote;
    }
    uintptr_t g = util::ReadOr<uintptr_t>(r.loadingGlobal);
    int flag = g ? util::ReadOr<int>(g + r.loadingOff) : 0;
    ue3::TArrayRaw a{};
    std::string map;
    if (util::Read(r.engine + r.mapOff, a) && a.data && a.num > 1 && a.num < 128)
      map = util::WideToUtf8(util::ReadWString(a.data, (size_t)a.num - 1));
    Sample(NowMs(), flag != 0, flag, map);
    Sleep(1);
  }
}

// ---- game thread: the game's own mission time, results screen, play time
uintptr_t g_lastGameInfo = 0;
bool g_checkFresh = false;
uint64_t g_lastTotalPoll = 0;

struct Cached {
  const char* cls;
  uintptr_t obj = 0;
  uint64_t lastScan = 0;
  uintptr_t Get() {
    if (obj && game::Alive(obj) && ue3::ClassNameOf(obj) == cls) return obj;
    obj = 0;
    uint64_t now = GetTickCount64();
    if (now - lastScan < 2000) return 0;
    lastScan = now;
    for (int i = 0, n = ue3::NumObjects(); i < n; ++i) {
      uintptr_t o = ue3::Object(i);
      if (o && ue3::ClassNameOf(o) == cls && ue3::ObjName(o).find("Default__") != 0) {
        obj = o;
        break;
      }
    }
    return obj;
  }
};
Cached g_ingameUI{"DVGFxMovieHandler_IngameUI"};

bool ResultsOpen() {
  uintptr_t ui = g_ingameUI.Get();
  if (!ui) return false;
  game::Field f = game::FieldOf(ui, "m_LoadedComponents");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(ui + f.off, a) || a.num <= 0 || a.num > 64) return false;
  for (int i = 0; i < a.num; ++i) {
    uintptr_t c = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (ue3::IsObject(c) && ue3::ClassNameOf(c) == "DVGFxContainer_MissionRankScreen") return true;
  }
  return false;
}

double TotalPlayTime(const game::World& w) {
  static uintptr_t fn = 0, stats = 0;
  static int statOff = -1;
  if (!fn) fn = ue3::FindObject("Function", "AppTimer_Get", "NTGameStatsManager");
  if (!stats) {
    uintptr_t cls = ue3::FindObject("Class", "DVStats");
    stats = cls ? ue3::ClassDefaultObject(cls) : 0;
    if (stats) statOff = game::FieldOf(stats, "GENERAL_TOTALGAMETIME").off;
  }
  uintptr_t ctx = w.pc ? w.pc : w.engine;
  if (!fn || !stats || statOff < 0 || !ctx) return -1;
  engine::Call c(ctx, fn);  // static native: a live object as context
  std::vector<engine::Param> ps = engine::Params(fn);
  if (ps.empty()) return -1;
  uint8_t id[8];
  if (!util::SafeRead(stats + statOff, id, sizeof id)) return -1;
  c.Raw(ps[0].name.c_str(), id, sizeof id);
  return c.Invoke() ? c.RetFloat() : -1;
}

std::string Lower(std::string s) {
  for (char& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// ---- features: the switches and the buttons
class TimerToggle : public features::Feature {
 public:
  TimerToggle(const char* i, const char* n, const char* h) {
    id = i;
    name = n;
    help = h;
    group = features::Group::Timers;
    anywhere = true;
    noOverlay = true;
  }
};

class TimerAction : public features::Feature {
 public:
  TimerAction(const char* i, const char* n, const char* h, void (*run)()) : run_(run) {
    id = i;
    name = n;
    help = h;
    group = features::Group::Timers;
    action = true;
    anywhere = true;
    frontEndOk = true;
    noOverlay = true;
  }
  void Run(const features::Ctx&) override { run_(); }

 private:
  void (*run_)();
};

#define LS_HINT                                                                                                 \
  "Counted like the leaderboard's LiveSplit: loading screens do not count. Green while it runs, blue when the run " \
  "is over (Vergil is beaten in Mission 20)."
#define NOT_BOARD "\nNot the leaderboard time."

// ---- drawing
float g_x = -1, g_y = -1;  // the player's place (screen fractions), -1 = above the health bar
bool g_replace = false;     // Reset position: place the window again
float g_bg = 0;             // background darkness 0..1 (Timers > Look)

}  // namespace

std::string Format(double ms, bool withMs) {
  if (ms < 0) return "--:--";
  long long t = (long long)std::floor(ms + 0.0001);
  long long h = t / 3600000, m = t / 60000 % 60, s = t / 1000 % 60, f = t % 1000;
  char b[48];
  if (!withMs) snprintf(b, sizeof b, "%lld:%02lld:%02lld", h, m, s);
  else if (h > 0) snprintf(b, sizeof b, "%lld:%02lld:%02lld.%03lld", h, m, s, f);
  else snprintf(b, sizeof b, "%02lld:%02lld.%03lld", m, s, f);
  return b;
}

std::vector<features::Feature*> CreateFeatures() {
  return {
      new TimerToggle("timer_run_ls", "Run time (LiveSplit)",
                      LS_HINT "\nThe whole run: starts when the first mission loads. Next to it: how far ahead (-) "
                              "or behind (+) your personal best you are. On screen: IGT."),
      new TimerToggle("timer_mission_ls", "Mission time (LiveSplit)",
                      LS_HINT "\nThis mission only. Stops on the results screen. Next to it: the last finished "
                              "mission and its time - green, yellow when it is your best, red when it is slower "
                              "than your personal best. On screen: IL."),
      new TimerToggle("timer_mission_igt", "Mission time (game)",
                      "The game's own mission timer, the one on the results screen. On screen: GAME." NOT_BOARD),
      new TimerToggle("timer_run_rta", "Run time with loads",
                      "The same run time, but loading screens count too. On screen: RTA." NOT_BOARD),
      new TimerToggle("timer_total_igt", "Total play time",
                      "Play time of your save, like on the game's slot screen. On screen: TOTAL." NOT_BOARD),
      new TimerAction("timer_reset_position", "Reset timers position",
                      "Puts the timers back above the health bar. Drag them anywhere while the menu is open.",
                      [] { ResetPosition(); }),
  };
}

void Start() {
  static bool started = false;
  if (started) return;
  started = true;
  std::thread(SamplerLoop).detach();
}

void OnFrame(const game::World& w) {
  if (!g_any) return;
  // the game's mission time
  if (w.gameInfo && ue3::IsA(w.gameInfo, "DVGameInfo")) {
    float igt = game::GetFloat(w.gameInfo, "m_fCurrentMissionTime", -1);
    if (w.gameInfo != g_lastGameInfo) {
      g_lastGameInfo = w.gameInfo;
      g_checkFresh = true;
    }
    bool fresh = false;
    if (g_checkFresh && w.mission) {  // the first frame with Dante in a new world
      g_checkFresh = false;
      fresh = igt >= 0 && igt < 2.0f;  // the mission timer was reset: a new attempt, not a checkpoint
    }
    bool results = ResultsOpen();
    std::lock_guard<std::mutex> lk(g_mu);
    State& s = g_s;
    s.igt = igt;
    s.igtMap = w.map;
    if (fresh && s.running && Lower(s.cur) == Lower(w.map)) {
      // the same mission started again without a split (restart / Mission select of the same mission): its
      // LiveSplit-rules mission time starts again from that load (loads never count)
      if (s.segStart != s.lastLoadStart) LOG("timers: mission '%s' started again - mission time restarts", w.map.c_str());
      s.segStart = s.lastLoadStart;
      s.missionStopped = false;
    }
    if (results && !s.igtResults && s.running && !s.missionStopped) {
      s.missionStopped = true;
      s.missionFinal = s.game - s.segStart;
      LOG("timers: results screen - mission %s (LiveSplit rules %s, game's mission time %.3f s)", w.map.c_str(),
          Format(s.missionFinal).c_str(), igt);
    }
    s.igtResults = results;
  }
  // play time of the save (every 0.5 s)
  uint64_t now = GetTickCount64();
  if (g_show[kTotalIgt] && now - g_lastTotalPoll > 500) {
    g_lastTotalPoll = now;
    double t = TotalPlayTime(w);
    std::lock_guard<std::mutex> lk(g_mu);
    g_s.total = t;
  }
}

void ResetRun(const char* why) {
  std::lock_guard<std::mutex> lk(g_mu);
  State& s = g_s;
  Reset(s, NowMs(), why);
  s.rtaFinal = 0;
  s.rtaStart = 0;
  s.missionFinal = 0;
}

SumOfBest PersonalBest() {
  SumOfBest r;
  std::lock_guard<std::mutex> lk(g_mu);
  LoadBest();
  r.total = kNumStory;
  double sum = 0;
  for (const char* m : kStoryMaps)
    if (auto it = g_pbSeg.find(m); it != g_pbSeg.end() && it->second >= 0) sum += it->second, ++r.have;
    else r.missing.push_back(m);
  r.ms = r.have > 0 ? sum : -1;
  return r;
}

std::vector<std::string> StoryMaps() { return std::vector<std::string>(kStoryMaps, kStoryMaps + kNumStory); }

double PbSegment(const std::string& map) {
  std::lock_guard<std::mutex> lk(g_mu);
  LoadBest();
  auto it = g_pbSeg.find(map);
  return it == g_pbSeg.end() ? -1 : it->second;
}

void SetPbSegment(const std::string& map, double ms) {
  std::lock_guard<std::mutex> lk(g_mu);
  LoadBest();
  if (ms < 0) g_pbSeg.erase(map);
  else g_pbSeg[map] = ms;
  // the PB's split times follow its segments in mission order (up to the first mission without one)
  g_pbAt.clear();
  double cum = 0;
  int have = 0;
  for (const char* m : kStoryMaps) {
    auto it = g_pbSeg.find(m);
    if (it == g_pbSeg.end() || it->second < 0) break;
    cum += it->second;
    g_pbAt[m] = cum;
    ++have;
  }
  SaveBest();
  LOG("timers: PB segment '%s' set by hand: %s (%d of %d in a row, PB %s)", map.c_str(), Format(ms).c_str(), have,
      kNumStory, Format(PbTotal()).c_str());
}

double BestSplit(const std::string& map) {
  std::lock_guard<std::mutex> lk(g_mu);
  LoadBest();
  auto it = g_bestSplit.find(map);
  return it == g_bestSplit.end() ? -1 : it->second;
}

void SetBestSplit(const std::string& map, double ms) {
  std::lock_guard<std::mutex> lk(g_mu);
  LoadBest();
  if (ms < 0) g_bestSplit.erase(map);
  else g_bestSplit[map] = ms;
  SaveBest();
  LOG("timers: best LiveSplit segment '%s' set by hand: %s", map.c_str(), Format(ms).c_str());
}

SumOfBest SoB() {
  SumOfBest r;
  std::lock_guard<std::mutex> lk(g_mu);
  LoadBest();
  r.total = kNumStory;
  double sum = 0;
  for (const char* m : kStoryMaps)
    if (auto it = g_bestSplit.find(m); it != g_bestSplit.end() && it->second >= 0) sum += it->second, ++r.have;
    else r.missing.push_back(m);
  r.ms = r.have > 0 ? sum : -1;
  return r;
}

bool Active() { return g_any; }

RunInfo Run() {
  RunInfo r;
  std::lock_guard<std::mutex> lk(g_mu);
  const State& s = g_s;
  r.finished = s.finished;
  r.newPb = s.newPb;
  r.sampling = g_any && s.last > 0 && NowMs() - s.last < 500;
  r.running = s.running;
  r.ticking = s.running && !s.loading && !s.prevLoading;
  r.loading = s.loading;
  r.map = s.map;
  r.startMap = s.splits.empty() ? s.cur : s.splits.front().map;
  r.game = s.game;
  r.loadNote = s.resolveNote;
  return r;
}

void OnEvent(uintptr_t self, uintptr_t fn) {
  static uintptr_t setDead = 0;
  static bool looked = false;
  if (!g_any || !fn) return;
  if (!looked) {
    looked = true;
    setDead = ue3::FindObject("Function", "SetDead", "DVPawn");
    LOG("timers: run end hook %s (DVPawn.SetDead)", setDead ? "ready" : "NOT FOUND");
  }
  if (fn != setDead || ue3::ClassNameOf(self) != "DVPawn_Vergil") return;
  const game::World& w = game::W();
  if (self == w.pawn || Lower(w.map) != Lower(kLastMap)) return;  // Vergil's Downfall: Vergil is the player
  std::lock_guard<std::mutex> lk(g_mu);
  Finish(g_s, NowMs(), "Vergil defeated");
}

void DebugFinish() {
  std::lock_guard<std::mutex> lk(g_mu);
  Finish(g_s, NowMs(), "test command");
}

bool ParseTime(const std::string& text, double* ms) {
  // [h:]m:ss[.fff] or seconds[.fff]; a comma works as the point
  std::string t;
  for (char c : text)
    if (c != ' ') t += c == ',' ? '.' : c;
  if (t.empty()) return false;
  double parts[3] = {0, 0, 0};
  int n = 0;
  size_t i = 0;
  for (;;) {
    size_t j = t.find(':', i);
    bool last = j == std::string::npos;
    std::string p = t.substr(i, last ? std::string::npos : j - i);
    if (p.empty() || n >= 3) return false;
    int dots = 0;
    for (char c : p) {
      if (c == '.') ++dots;
      else if (!isdigit((unsigned char)c)) return false;
    }
    if (dots > 1 || (dots && !last)) return false;  // a fraction only in the seconds
    parts[n++] = atof(p.c_str());
    if (last) break;
    i = j + 1;
  }
  double sec = 0;
  for (int k = 0; k < n; ++k) {
    if (k > 0 && parts[k] >= 60) return false;
    sec = sec * 60 + parts[k];
  }
  if (sec <= 0 || sec > 100 * 3600) return false;
  *ms = std::round(sec * 1000.0);
  return true;
}

void LoadJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  // "hx"/"hy": the player's place (screen fractions); none = above the health bar. The first version's box
  // ("x", "y", "w", "h") is not used any more.
  g_x = j.value("hx", -1.0f);
  g_y = j.value("hy", -1.0f);
  g_bg = std::clamp(j.value("bg", 0.0f), 0.0f, 1.0f);
}

nlohmann::json SaveJson() {
  nlohmann::json j = nlohmann::json::object();
  if (g_x >= 0) j["hx"] = g_x, j["hy"] = g_y;
  if (g_bg > 0) j["bg"] = g_bg;
  return j;
}

float Background() { return g_bg; }

void SetBackground(float a) {
  g_bg = std::clamp(a, 0.0f, 1.0f);
  MarkConfigDirty();
}

void ResetPosition() {
  g_x = g_y = -1;
  g_replace = true;
  MarkConfigDirty();
}

namespace {
struct Line {
  std::string label;      // "IGT" (on screen, user #16: LiveSplit run = IGT, LiveSplit mission = IL)
  std::string value;      // "12:34.567"
  bool ticking = false;   // board rules: the time is running now (green digits)
  bool done = false;      // the run ended: blue digits (LiveSplit's personal-best blue)
  std::string extra;      // after the value, smaller: last split, segment's mission and best
  std::vector<std::pair<std::string, ImU32>> parts = {};  // after extra, smaller, each in its own colour
  ImU32 valueCol = 0;     // 0 = by ticking / done
};

constexpr ImU32 kGreen = IM_COL32(0x4C, 0xE6, 0x7C, 255), kGold = IM_COL32(0xFF, 0xD2, 0x3C, 255),
                kRed = IM_COL32(0xFF, 0x4B, 0x4B, 255), kSoft = IM_COL32(0xD8, 0xD8, 0xD8, 235);

std::string ShortMission(const std::string& map) {  // "ch4_main" -> "M3" (the catalog's id without the zero)
  int i = nav::FindMission(map);
  if (i < 0) return map;
  std::string id = nav::Missions()[i].id;  // "M03", "SM05"
  size_t d = id.find_first_of("0123456789");
  if (d == std::string::npos) return id;
  size_t z = d;
  while (z + 1 < id.size() && id[z] == '0') ++z;
  return id.substr(0, d) + id.substr(z);
}

std::string FormatDelta(double ms) {  // "+1.234", "-12.345", "+1:02.345"
  double a = std::fabs(ms) / 1000.0;
  char b[32];
  if (a >= 60) snprintf(b, sizeof b, "%c%d:%06.3f", ms < 0 ? '-' : '+', (int)(a / 60), std::fmod(a, 60.0));
  else snprintf(b, sizeof b, "%c%.3f", ms < 0 ? '-' : '+', a);
  return b;
}

std::vector<Line> Lines() {
  std::vector<Line> out;
  std::lock_guard<std::mutex> lk(g_mu);
  const State& s = g_s;
  double now = NowMs();
  double live = s.running && !s.prevLoading && s.last > 0 ? now - s.last : 0;  // since the last sample
  double game = s.running ? s.game + live : s.finished ? s.game : 0;
  bool runTicking = s.running && !s.loading && !s.prevLoading;
  if (g_show[kRunLs]) {
    Line l{"IGT", Format(game), runTicking};
    if (!s.finished && !s.running) l.extra = "waits for a mission load";
    l.done = s.finished;
    // How far ahead (-) or behind (+) the personal best the run is (user, #16: only this one, no per-mission +/-):
    // at the last split, or live once this mission already took longer than in the personal best. Behind: red.
    bool known = false;
    double delta = 0;
    if (s.startMap == kFirstMap && !g_pbAt.empty()) {
      auto it = g_pbAt.find(s.cur);
      if (s.running && it != g_pbAt.end() && game > it->second) {
        known = true;
        delta = game - it->second;
      } else if (!s.splits.empty() && s.splits.back().pbAt >= 0) {
        known = true;
        delta = s.splits.back().at - s.splits.back().pbAt;
      }
    }
    if (known) {
      l.parts.push_back({FormatDelta(delta), delta > 0 ? kRed : kGreen});
      if (delta > 0 && runTicking) l.valueCol = kRed;
    }
    out.push_back(l);
  }
  if (g_show[kMissionLs]) {
    double v = !s.running && !s.finished ? 0 : s.missionStopped ? s.missionFinal : game - s.segStart;
    Line l{"IL", Format(v), runTicking && !s.missionStopped};
    l.done = s.finished;
    // the last finished mission, LiveSplit's colours (user, #16): green; gold = a best segment; red = slower than
    // in the personal best
    if (!s.splits.empty()) {
      const Split& sp = s.splits.back();
      ImU32 c = sp.best >= 0 && sp.seg < sp.best ? kGold : sp.pbSeg >= 0 && sp.seg > sp.pbSeg ? kRed : kGreen;
      l.parts.push_back({ShortMission(sp.map), kSoft});
      l.parts.push_back({Format(sp.seg), c});
    }
    out.push_back(l);
  }
  if (g_show[kMissionIgt]) out.push_back({"GAME", s.igt >= 0 ? Format(s.igt * 1000.0) : "--:--"});
  if (g_show[kRunRta]) out.push_back({"RTA", Format(s.running ? now - s.rtaStart : s.rtaFinal)});
  if (g_show[kTotalIgt]) out.push_back({"TOTAL", s.total >= 0 ? Format(s.total * 1000.0, false) : "--:--"});
  return out;
}

void Shadowed(ImDrawList* dl, ui::F f, float px, ImVec2 p, ImU32 col, const char* t, float off) {
  ui::Text(dl, f, px, ImVec2(p.x + off, p.y + off), IM_COL32(0, 0, 0, 200), t);
  ui::Text(dl, f, px, p, col, t);
}
}  // namespace

// A strip right above the game's health bar, next to Dante's portrait: the timers flow left to right and wrap
// into more rows upwards (the strip's bottom stays on the bar). No box, HUD-like white text with a shadow;
// board-rules digits are green while that time runs. While the menu is open it can be dragged anywhere
// ("Reset position" puts it back).
void Draw(bool menuOpen) {
  if (!g_any) return;
  std::vector<Line> lines = Lines();
  if (lines.empty()) return;
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  if (ds.x <= 0 || ds.y <= 0) return;
  float sc = ui::Screen() * ui::kBoost;
  auto P = [sc](float css) { return css * sc; };
  const float px = P(13), epx = P(11), rowH = P(16), padX = P(5), padY = P(2), gap = P(16);
  const float off = std::max(1.0f, std::round(P(1)));
  const ImU32 blue = IM_COL32(0x16, 0xA6, 0xFF, 255);  // LiveSplit's personal-best blue
  const ImU32 white = IM_COL32(255, 255, 255, 255), green = IM_COL32(0x4C, 0xE6, 0x7C, 255),
              soft = IM_COL32(0xD8, 0xD8, 0xD8, 235);
  // the HUD is laid out for the screen height (1080p: portrait x 100..195, health bar x 182.., top at y 108)
  float hud = ds.y / 1080.0f;
  const float maxW = 760 * hud;  // about the longest health bar
  struct Item {
    float x, y, labelW, valueW;
  };
  std::vector<Item> items(lines.size());
  float x = 0, rowsW = 0;
  int row = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const Line& l = lines[i];
    float lw = ui::Measure(ui::F::Mono, px, l.label.c_str()).x;
    float vw = ui::Measure(ui::F::MonoBold, px, l.value.c_str()).x;
    float w = lw + P(5) + vw + (l.extra.empty() ? 0 : P(5) + ui::Measure(ui::F::Mono, epx, l.extra.c_str()).x);
    for (const auto& pt : l.parts) w += P(5) + ui::Measure(ui::F::Mono, epx, pt.first.c_str()).x;
    if (x > 0 && x + w > maxW) x = 0, ++row;
    items[i] = {x, (float)row, lw, vw};
    x += w + gap;
    rowsW = std::max(rowsW, x - gap);
  }
  int rows = row + 1;
  ImVec2 size(rowsW + padX * 2, rows * rowH + padY * 2);
  ImVec2 anchor(212 * hud - padX, 104 * hud - size.y);  // bottom on the bar's top, right of the portrait
  static bool held = false;  // the player drags it now
  if (!held || g_replace) {
    g_replace = false;
    ImVec2 pos = g_x >= 0 ? ImVec2(g_x * ds.x, g_y * ds.y) : ImVec2(std::max(0.0f, anchor.x), std::max(0.0f, anchor.y));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  }
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground;
  if (!menuOpen) fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  bool open = ImGui::Begin("##Timers", nullptr, fl);
  ImGui::PopStyleVar(3);
  if (open) {
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 np(std::max(0.0f, std::min(p.x, ds.x - sz.x)), std::max(0.0f, std::min(p.y, ds.y - sz.y)));
    if (np.x != p.x || np.y != p.y) ImGui::SetWindowPos(np);
    if (menuOpen && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) held = true;
    if (held && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // dropped: that is the player's place now
      held = false;
      g_x = np.x / ds.x, g_y = np.y / ds.y;
      MarkConfigDirty();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bg = g_bg;  // the player's background; with the menu open at least a bit (the place to grab)
    if (menuOpen) bg = std::max(bg, ImGui::IsWindowHovered() ? 0.67f : 0.43f);
    if (bg > 0) dl->AddRectFilled(np, ImVec2(np.x + sz.x, np.y + sz.y), IM_COL32(8, 8, 10, (int)(bg * 255 + 0.5f)), P(2));
    if (menuOpen) dl->AddRect(np, ImVec2(np.x + sz.x, np.y + sz.y), IM_COL32(255, 255, 255, 60), P(2));
    for (size_t i = 0; i < lines.size(); ++i) {
      const Line& l = lines[i];
      const Item& it = items[i];
      float ix = np.x + padX + it.x, iy = np.y + padY + it.y * rowH;
      float ty = iy + (rowH - px) * 0.5f;
      Shadowed(dl, ui::F::Mono, px, ImVec2(ix, ty), soft, l.label.c_str(), off);
      float vx = ix + it.labelW + P(5);
      Shadowed(dl, ui::F::MonoBold, px, ImVec2(vx, ty), l.done ? blue : l.valueCol ? l.valueCol : l.ticking ? green : white,
               l.value.c_str(), off);
      float ex = vx + it.valueW, ey = iy + (rowH - epx) * 0.5f + P(1);
      if (!l.extra.empty()) {
        Shadowed(dl, ui::F::Mono, epx, ImVec2(ex + P(5), ey), soft, l.extra.c_str(), off);
        ex += P(5) + ui::Measure(ui::F::Mono, epx, l.extra.c_str()).x;
      }
      for (const auto& pt : l.parts) {
        Shadowed(dl, ui::F::Mono, epx, ImVec2(ex + P(5), ey), pt.second, pt.first.c_str(), off);
        ex += P(5) + ui::Measure(ui::F::Mono, epx, pt.first.c_str()).x;
      }
    }
    if (menuOpen && ImGui::IsWindowHovered())
      ui::SetHint("Timers: drag to move (only while the menu is open). Reset position on the Runs tab puts them "
                  "back above the health bar. Green digits: LiveSplit / board-rules time, running now.");
  }
  ImGui::End();
}

std::vector<std::string> DebugLines() {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lk(g_mu);
  const State& s = g_s;
  char b[400];
  snprintf(b, sizeof b,
           "%s | loading %d (flag %d) map '%s' | run %s game %s rta %s loads %d excluded %s | mission '%s' %s%s | "
           "igt %.3f (%s) results %d | total %.1f s",
           s.resolveNote.c_str(), s.loading, s.flag, s.map.c_str(), s.running ? "RUNNING" : s.finished ? (s.newPb ? "FINISHED-PB" : "FINISHED") : "stopped",
           Format(s.game).c_str(), Format(s.running ? NowMs() - s.rtaStart : s.rtaFinal).c_str(), s.loads,
           Format(s.loadExcluded).c_str(), s.cur.c_str(),
           Format(s.missionStopped ? s.missionFinal : s.game - s.segStart).c_str(), s.missionStopped ? " (stopped)" : "",
           s.igt, s.igtMap.c_str(), s.igtResults, s.total);
  out.push_back(b);
  snprintf(b, sizeof b, "pb %s (%zu segments) | best segments %zu | start '%s'", Format(PbTotal()).c_str(),
           g_pbSeg.size(), g_bestSplit.size(), s.startMap.c_str());
  out.push_back(b);
  for (const Split& sp : s.splits) {
    snprintf(b, sizeof b, "split %s at %s segment %s best-before %s", sp.map.c_str(), Format(sp.at).c_str(),
             Format(sp.seg).c_str(), Format(sp.best).c_str());
    out.push_back(b);
  }
  return out;
}

}  // namespace timers
