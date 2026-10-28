// Practice tab, Enemies and Damage log blocks. The work is in enemies.cpp and damagelog.cpp.
#include <map>

#include "damagelog.h"
#include "enemies.h"
#include "features.h"
#include "log.h"

namespace features {
namespace {

using game::Alive;
using game::GetBool;
using game::GetObj;
using game::SetBool;

class EnemyAction : public Feature {
  enemies::Action act_;

 public:
  explicit EnemyAction(enemies::Action a) : act_(a) {
    action = true;
    group = Group::EnemyTools;
  }
  std::string Blocked() const override { return enemies::Blocked(act_); }
};

class SpawnEnemy : public EnemyAction {
 public:
  SpawnEnemy() : EnemyAction(enemies::Action::Spawn) {
    id = "enemy_spawn";
    name = "Spawn";
    help = "Spawns the enemy picked above, as many as set, in front of Dante or at a saved position. An enemy this "
           "level does not have is loaded first (a second or two). Despawn spawned removes them again.";
  }
  void Run(const Ctx& c) override { enemies::Spawn(c.w); }
};

class KillAllEnemies : public EnemyAction {
 public:
  KillAllEnemies() : EnemyAction(enemies::Action::KillAll) {
    id = "enemy_kill_all";
    name = "Kill all enemies";
    help = "Every enemy that is alive now dies at once, also with Infinite enemy HP on. Allies (like Kat) are "
           "not hurt.";
  }
  void Run(const Ctx& c) override { enemies::KillAll(c.w); }
};

class DespawnEnemies : public EnemyAction {
 public:
  DespawnEnemies() : EnemyAction(enemies::Action::Despawn) {
    id = "enemy_despawn";
    name = "Despawn spawned";
    help = "Removes only the enemies you spawned with this tool (no kill, no orbs). The level's own enemies stay.";
  }
  void Run(const Ctx& c) override { enemies::DespawnSpawned(c.w); }
};

class InfiniteEnemyHp : public Feature {
  std::map<uintptr_t, bool> ctrls_;  // enemy controller -> its own bGodMode

 public:
  InfiniteEnemyHp() {
    id = "enemy_inf_hp";
    name = "Infinite enemy HP";
    group = Group::EnemyTools;
    help = "Enemies never lose health and cannot die: a training dummy for combos. They still react to hits, and "
           "the Damage log still shows every hit. Wins over \"One-hit kills\".";
  }
  void Tick(const Ctx& c) override {
    // The game's own god mode on the enemy's controller (what the dev command AIGod sets): TakeDamage skips the
    // health change. New enemies get it as they appear.
    for (uintptr_t e : enemies::ActiveEnemies(c.w)) {
      uintptr_t ctrl = GetObj(e, "m_Controller");
      if (!ctrl || !game::FieldOf(ctrl, "bGodMode").ok()) continue;
      if (!ctrls_.count(ctrl)) ctrls_[ctrl] = GetBool(ctrl, "bGodMode");
      if (!GetBool(ctrl, "bGodMode")) SetBool(ctrl, "bGodMode", true);
    }
    for (auto it = ctrls_.begin(); it != ctrls_.end();) it = Alive(it->first) ? std::next(it) : ctrls_.erase(it);
  }
  void Disable() override {
    for (auto& [ctrl, orig] : ctrls_)
      if (Alive(ctrl)) SetBool(ctrl, "bGodMode", orig);
    ctrls_.clear();
  }
};

class ShowEnemyHp : public Feature {
 public:
  ShowEnemyHp() {
    id = "enemy_show_hp";
    name = "Show enemy HP";
    group = Group::EnemyTools;
    help = "Shows each enemy's health as a number above its head.";
  }
  bool Enable(const Ctx&) override {
    enemies::SetShowHp(true);
    return true;
  }
  void Disable() override { enemies::SetShowHp(false); }
};

class DamageLog : public Feature {
 public:
  DamageLog() {
    id = "damage_log";
    name = "Damage log";
    group = Group::DamageLog;
    help = "Opens a small window with every hit: who hit whom, the move, the damage. It stays on screen with the "
           "menu closed. Open the menu to move it with the mouse, filter, clear, see totals per move (Moves tab) or "
           "save it as a table file (Export CSV).";
  }
  bool Enable(const Ctx&) override {
    damagelog::SetOn(true);
    return true;
  }
  void Disable() override { damagelog::SetOn(false); }
};

}  // namespace

// Menu order on the Practice tab.
std::vector<Feature*> CreateEnemyFeatures() {
  return {new SpawnEnemy, new KillAllEnemies, new DespawnEnemies, new InfiniteEnemyHp, new ShowEnemyHp, new DamageLog};
}

}  // namespace features
