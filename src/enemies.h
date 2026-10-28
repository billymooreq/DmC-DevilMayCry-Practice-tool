#pragma once
#include <string>
#include <vector>

#include "game.h"
#include "json.hpp"

// Practice tab, Enemies block: spawn any normal enemy of the game, kill all, remove the ones we spawned, enemy
// health numbers. Research: research/enemies.md.
//
// The enemy list is the game's own (Params.EnemyEncounterParams.m_EnemyDataParams: name, spawn level, ...); the
// names players know come from the game's localisation (Stygian, Ravager, ...). An enemy can be spawned when its
// character template is in memory (a DVSeqVar_EnemyWaveData of a loaded level points to it); otherwise its small
// spawn level ("SpawnLevel_Rage") is streamed in first, the same way the game's dev command LoadSingleSpawnLevel
// does, and the spawn happens when the template appears.
namespace enemies {

struct Kind {
  std::string id;     // the game's EnemyName ("WarriorDrone")
  std::string name;   // what players call it ("Stygian")
  std::string level;  // its spawn level ("SpawnLevel_Warrior")
};

enum class Avail { Unknown, Ready, Loadable, Missing };

// ---- state for the menu (any thread)
struct Snapshot {
  bool mission = false;
  std::vector<Kind> kinds;     // menu order
  std::vector<Avail> avail;    // parallel to kinds
  int active = 0;              // enemies alive now
  int spawned = 0;             // of them spawned by us
  bool loading = false;        // a spawn waits for its enemy to load
  std::string message;         // last result, player-facing
};
Snapshot Now();
const char* AvailText(Avail a);  // "", "loads first", "not on this map"

// ---- what the player picked (any thread; saved in DmCPracticeTool.json "enemies")
struct Selection {
  std::string kind = "WarriorDrone";
  int count = 1;          // 1..10
  int where = 0;          // 0 = in front of Dante, 1 = saved position
  float distance = 4.0f;  // metres in front of Dante (1 m = 100 game units)
  int posSlot = 0;        // saved position slot (nav)
};
Selection GetSel();
void SetSel(const Selection& s);
void LoadJson(const nlohmann::json& j);
nlohmann::json SaveJson();

enum class Action { Spawn, KillAll, Despawn };
std::string Blocked(Action a);  // "" = can run now

// ---- game thread
void OnFrame(const game::World& w);  // availability scan, pending spawns, health labels
void Spawn(const game::World& w);    // with the menu selection
void KillAll(const game::World& w);
void DespawnSpawned(const game::World& w);
// Player-facing name of an actor ("Dante", "Stygian", ...), for the damage log. Game thread.
std::string ActorName(const game::World& w, uintptr_t actor);
std::vector<uintptr_t> ActiveEnemies(const game::World& w);
// Health of a character as the game shows it. False = not a character with health. Game thread.
// Bosses: what their health bar at the bottom of the screen shows. The bar is driven from two places: the boss's
// own phases (DVAIController_Boss.m_HealthBarComp: fraction x max of the current phase) and the level's script
// (DVSeqAct_BossHealthBar: CurrentHealth / MaximumHealth, e.g. a second body part with its own count). The one that
// changed last is what the player sees.
bool Health(uintptr_t actor, float* hp, float* maxHp, bool* boss = nullptr);
// ProcessEvent observer, after the call: the level script updated a boss health bar. Game thread.
void OnEventAfter(uintptr_t self, uintptr_t fn);

// Show enemy HP: health above each enemy's head. Collected on the game thread, drawn by the menu.
void SetShowHp(bool on);
struct HpLabel {
  float x, y, z;  // world position above the head
  float hp, maxHp;
};
struct HpView {
  bool valid = false;
  float camX, camY, camZ;
  int pitch, yaw, roll;
  float fov;
  std::vector<HpLabel> labels;
};
HpView HpLabels();  // any thread

// Dev / autorun: one line per kind with availability, template and spawn level.
std::vector<std::string> DebugLines();

}  // namespace enemies
