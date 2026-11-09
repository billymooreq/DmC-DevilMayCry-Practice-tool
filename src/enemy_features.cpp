// Practice tab, Enemies and Damage log blocks. The work is in enemies.cpp and damagelog.cpp.
#include <map>

#include "damagelog.h"
#include "enemies.h"
#include "features.h"
#include "log.h"
#include "nav.h"

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
    help = "Spawns the enemies picked above, in front of Dante or at an enemy position. An enemy this level does "
           "not have is loaded first (a second or two).";
  }
  void Run(const Ctx& c) override { enemies::Spawn(c.w); }
};

class KillAllEnemies : public EnemyAction {
 public:
  KillAllEnemies() : EnemyAction(enemies::Action::KillAll) {
    id = "enemy_kill_all";
    name = "Kill all enemies";
    help = "Click to kill every enemy on the map right now, also with Infinite enemy HP on. Allies (like Kat) "
           "are not hurt.";
  }
  void Run(const Ctx& c) override { enemies::KillAll(c.w); }
};

class DespawnEnemies : public EnemyAction {
 public:
  DespawnEnemies() : EnemyAction(enemies::Action::Despawn) {
    id = "enemy_despawn";
    name = "Despawn spawned";
    help = "Removes only the enemies you spawned (no kill, no orbs). The level's own enemies stay.";
  }
  void Run(const Ctx& c) override { enemies::DespawnSpawned(c.w); }
};

// "Save here as N": where Dante stands now becomes enemy position N (4..6) - the one picked on the Enemies tab
// (Where = a position), else 4. Menu button, key and preset.
class SaveEnemyPosition : public Feature {
 public:
  SaveEnemyPosition() {
    id = "enemy_save_pos";
    name = "Save enemy position (Save here as N)";
    action = true;
    group = Group::EnemyTools;
    help = "Saves where Dante stands now as an enemy position: the one picked on the Enemies tab (else 4). Does not "
           "touch Dante's slots 1-3.";
  }
  std::string Blocked() const override { return nav::Blocked(nav::Action::SavePos); }
  void Run(const Ctx& c) override {
    enemies::Selection s = enemies::GetSel();
    nav::SavePos(c.w, s.where == 1 ? s.posSlot : nav::kPosSlots);
  }
};

class ClearDamageLog : public Feature {
 public:
  ClearDamageLog() {
    id = "damage_log_clear";
    name = "Clear damage log";
    action = true;
    anywhere = true;
    frontEndOk = true;
    noOverlay = true;  // empties the tool's own list, the game is not touched
    group = Group::DamageLog;
    help = "Empties the damage log.";
  }
  void Run(const Ctx&) override { damagelog::Clear(); }
};

class InfiniteEnemyHp : public Feature {
  std::map<uintptr_t, bool> ctrls_;  // enemy controller -> its own bGodMode

 public:
  InfiniteEnemyHp() {
    id = "enemy_inf_hp";
    name = "Infinite enemy HP";
    group = Group::Enemies;
    help = "Enemies never die. Good for long combo practice. Does not work on bosses.\n"
           "Has priority over One-hit kills.";
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
    help = "Health above each enemy's head. Infinite enemy HP is in Cheats.";
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
    name = "Show damage log";
    group = Group::DamageLog;
    help = "Opens the damage log window: every hit, its move, damage and the victim's health. Move it with the "
           "mouse while the menu is open.";
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
  return {new InfiniteEnemyHp, new SpawnEnemy,  new KillAllEnemies, new DespawnEnemies, new SaveEnemyPosition,
          new ShowEnemyHp,     new DamageLog,   new ClearDamageLog};
}

}  // namespace features
