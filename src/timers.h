#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game.h"
#include "json.hpp"

// Timers tab: on-screen timers (research: docs/timers.md).
//
// "LiveSplit / board rules" timers follow the community autosplitter DMCDevilMayCry.asl exactly:
//  - loading = the int the .asl reads at [exe+0x27521C0]+0x5B4 (v1.3; 1.0: exe+0x273F0F8). It is a C++ object of
//    the game (not a UObject), the busy state of its system UI (loading screens, the save icon). The tool finds the
//    same global and offset by the code that reads it (one signature for both versions), not by fixed addresses;
//  - map = DVGameEngine.TransitionDescription (FString), the .asl's [exe+0x272C328]+0x560 (read by reflection);
//  - Start: loading on a map of the .asl list (when not running); Split: the first load of each new map of the
//    list; Reset: loading "mainmenu_startup_vergil". Game Time = real time while not loading.
// The sampler thread reads both values every ~1 ms (they change while the game thread is stuck in a load).
// Other timers (not for the leaderboard): the game's own mission time (DVGameInfo.m_fCurrentMissionTime), the run
// with loads (RTA), the save's play time (NTGameStatsManager.AppTimer_Get(DVStats.GENERAL_TOTALGAMETIME)) and a
// practice segment: one mission from its start to its results screen.
namespace features {
class Feature;
}

namespace timers {

std::vector<features::Feature*> CreateFeatures();  // registered by feature_list.cpp
void Start();                                      // init thread: the sampler thread (idles until a timer is on)

void OnFrame(const game::World& w);  // game thread, every frame
// Game thread, every ProcessEvent: the end of the run. M20 (ch20_main): DVPawn_Vergil.SetDead of the boss - the
// moment the final cutscene (Dante runs Vergil through) starts; runners stop LiveSplit there (user, #16).
void OnEvent(uintptr_t self, uintptr_t fn);

// Runs > Speedrun. ResetRun: the run timers (LiveSplit rules, RTA, mission, this run's splits) back to zero, the
// next mission load starts a new run; bests are kept. why: for the log.
void ResetRun(const char* why);
struct RunInfo {
  bool sampling = false;  // the sampler is reading the game now
  bool running = false;   // the LiveSplit-rules run has started
  bool finished = false;  // ... and ended (time frozen, blue)
  bool newPb = false;     // ... with a new personal best
  bool ticking = false;   // ... and its time runs now (not loading)
  bool loading = false;
  std::string map;        // the map name the autosplitter sees now
  std::string startMap;
  double game = 0;        // ms
  std::string loadNote;   // where the loading flag was found (or why not)
};
RunInfo Run();
bool Active();  // a timer of the tool is on (only then the sampler reads the game)

// Runs > Best times. Personal best: the best full run (ch1_main .. the end of ch20_main), the sum of its 20
// segments (a faster finished full run replaces them; each can be edited). Sum of best: the best LiveSplit-rules segment of each of the 20 story missions added up (secret
// missions not included); ms = -1 while none has a best yet.
struct SumOfBest {
  double ms = -1;
  int have = 0, total = 0;
  std::vector<std::string> missing;  // story maps without a best segment yet
};
SumOfBest SoB();
SumOfBest PersonalBest();  // the sum of the personal-best segments (like SoB; set by a faster full run or by hand)
void DebugFinish();        // autorun "timers_end": end the run now, as if Vergil was defeated (tests)
// Editors on the Runs tab (map = "ch4_main", one of StoryMaps(); ms < 0 = clear). PB segments: the personal-best
// run's time of each mission (its split times follow; with all 20 set their sum becomes the personal best). Best
// splits: the best LiveSplit-timed segment of each mission (Sum of best, gold on screen).
std::vector<std::string> StoryMaps();
double PbSegment(const std::string& map);
void SetPbSegment(const std::string& map, double ms);
double BestSplit(const std::string& map);
void SetBestSplit(const std::string& map, double ms);

// Time typed by the player.
bool ParseTime(const std::string& text, double* ms);  // "1:23.456", "83.456", "1:02:03.4" -> ms

void Draw(bool menuOpen);  // render thread: the timers window
void ResetPosition();      // back above the health bar
float Background();        // 0..1: the dark background under the timers (Timers > Look)
void SetBackground(float a);

void LoadJson(const nlohmann::json& j);  // its place (DmCPracticeTool.json "timers")
nlohmann::json SaveJson();

std::vector<std::string> DebugLines();  // autorun "timers"
std::string Format(double ms, bool withMs = true);  // "1:02:03.456" / "02:03.456"

}  // namespace timers
