#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game.h"
#include "json.hpp"

// Visuals tab: wireframes of what the game really uses for collisions, drawn over the picture. Research:
// research/visuals.md.
//
// - Enemy / Player collision: the bodies hits are tested against - the capsules, spheres and boxes of the
//   character's physics asset, on its animated bones (+ optionally the upright capsule the character moves with).
// - Attack hitboxes: NTCombat_ProcessStrike processes while they run (the game starts / stops them through
//   ProcessEvent), each with its strike volume (arc, capsule, cone, two arcs) at the process's location; projectiles
//   with their capsule. They stay on screen a little longer and fade.
// - Hit markers: the point where a hit was counted (Contact.ContactPointOnContactee).
// - Triggers: Kismet Touch / Used events with a volume or trigger actor as their originator, sorted by what the
//   event leads to in the level script (an encounter = arena, a cinematic / matinee with a director = cutscene).
// - Invisible walls: blocking volumes of the level that stop Dante now (BlockingVolume, DynamicBlockingVolume while
//   it blocks, NTCollisionVolume), not camera volumes and not walls meant only for other classes (enemies).
//
// Everything is read-only: nothing in the game is changed.
namespace visuals {

enum class Cat { Enemy, Player, Hitbox, HitMarker, Cutscene, Arena, OtherTrigger, Walls, Count };
constexpr int kCats = (int)Cat::Count;

// Colours of the categories (legend on the tab, lines). Dante's hitboxes and the enemies' ones differ.
uint32_t Color(Cat c);
uint32_t EnemyHitboxColor();
uint32_t HitOnDanteColor();

// ---- what is switched on (features set it, any thread)
void SetOn(Cat c, bool on);
bool On(Cat c);
void SetMoveCapsule(bool enemies, bool on);  // option: also the upright movement capsule
void SetLinger(float seconds);               // hitboxes stay this long after their last active frame
void SetWallGrid(bool on);                   // option: a 1 m grid on the faces of near invisible walls

// ---- options shared by all categories (saved in DmCPracticeTool.json "visuals")
struct Options {
  bool labels = true;
  float maxDistance = 40.0f;  // metres from the camera (triggers are drawn at any distance)
  float thickness = 1.5f;     // line width in pixels at 1080p
  bool occlude = true;        // triggers and walls: hidden behind the level's geometry (needs the scene depth)
};
Options GetOptions();
void SetOptions(const Options& o);
void LoadJson(const nlohmann::json& j);
nlohmann::json SaveJson();

// ---- game thread
void OnFrame(const game::World& w);
// ProcessEvent observer. Before the call: strike processes starting / stopping, hits for the markers; after it:
// a strike process or projectile did its hit check (ProcessTick / Tick), its shape is taken then.
void OnEvent(uintptr_t self, uintptr_t fn, void* parms, bool after);
bool WantsEvents();  // any category that needs the observer is on

// ---- render thread: draws the last collected frame under the menu
void Draw();

// Lines hidden behind walls (triggers, invisible walls). The game clears its scene depth again after drawing the
// scene, so at Present there is nothing left to test against. The D3D9 hook draws these lines into a layer of its
// own just before that clear, with the view-projection the scene was drawn with and a depth test against the scene,
// and lays the layer over the frame at Present. Draw() hands them over in world space; the hook draws them in the
// next frame. Lines of one trigger share a group: an occlusion query tells whether any of them is visible, and the
// trigger's label is shown only then. Without the scene depth they go through ImGui like every other line.
struct OccLine {
  float a[3], b[3];  // world, already pulled a little toward the camera (lines lying on a floor or in a wall)
  uint32_t argb;
  uint32_t group;    // 0 = none
};
struct Occluded {
  std::vector<OccLine> lines;
  float cam[3] = {0, 0, 0};  // camera the lines were collected with (fallback for a camera-relative projection)
  float thickness = 1.5f;    // pixels at 1080p
  bool planeOn = false;      // Dev: a screen-wide plane through this point, depth tested
  float plane[3] = {0, 0, 0};
};
Occluded& OccludedLines();                     // render thread: filled by Draw(), taken by the D3D9 hook
void SetOcclusionLive(bool ok);                // render thread, every frame: scene depth and projection found
bool OcclusionLive();                          // any thread (menu)
void SetGroupVisible(uint32_t group, bool v);  // render thread: result of a group's occlusion query
bool LastCamera(game::Pov& out);               // render thread: camera of the last drawn frame
void SetDepthTestPlane(float metres);          // Dev: a screen-wide plane this far ahead, depth tested (0 = off)
void SetOccludeAll(bool on);                   // Dev: every category hidden behind walls (bodies show the depth test)

// Counts for the tab ("12 shapes, 3 triggers (1 arena, 2 cutscene)") and Dev.
struct Stats {
  int lines = 0, labels = 0;
  int bodies = 0, strikes = 0, markers = 0;
  int triggers[3] = {0, 0, 0};  // cutscene, arena, other (in range or not)
  int walls = 0;                // blocking Dante now, in range or not
  int labelsHidden = 0;         // labels of triggers hidden behind walls (last drawn frame)
  float collectMs = 0, drawMs = 0;
};
Stats GetStats();

// Dev / autorun: details of active strikes and triggers (shape parameters, classification path).
std::vector<std::string> DebugLines(const std::string& what);

}  // namespace visuals
