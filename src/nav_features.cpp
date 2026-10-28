// Navigation actions (Practice tab). They work on what is picked in the menu (point, mission, difficulty, slot);
// binds and presets use that same pick. The work itself is in nav.cpp.
#include "features.h"
#include "nav.h"
#include "realcp.h"

namespace features {
namespace {

class NavAction : public Feature {
 protected:
  nav::Action act_;

 public:
  explicit NavAction(nav::Action a) : act_(a) {
    action = true;
    group = Group::Navigation;
  }
  std::string Blocked() const override { return nav::Blocked(act_, nav::GetSel().slot); }
};

class RestartMission : public NavAction {
 public:
  RestartMission() : NavAction(nav::Action::Restart) {
    id = "nav_restart";
    name = "Restart mission";
    help = "Starts the current mission again from the very beginning, on the same difficulty - like Restart in "
           "the pause menu.";
  }
  void Run(const Ctx& c) override { nav::RestartMission(c.w); }
};

class LoadCheckpoint : public NavAction {
 public:
  LoadCheckpoint() : NavAction(nav::Action::LoadCheckpoint) {
    id = "nav_checkpoint";
    name = "Load checkpoint";
    help = "Loads the mission at the checkpoint picked above, the way Continue does: enemies, arenas, doors and "
           "scripts are set up for that point. Takes a loading screen.";
  }
  void Run(const Ctx& c) override { nav::LoadPoint(c.w, nav::PointFor(nav::GetSel(), c.w.map)); }
};

class Teleport : public NavAction {
 public:
  Teleport() : NavAction(nav::Action::Teleport) {
    id = "nav_teleport";
    name = "Teleport to point";
    noBind = true;
    help = "Moves only Dante to the point picked above, instantly. The level is NOT reset: enemies, arenas and "
           "doors stay as they are. Use Load checkpoint for a clean start.";
  }
  void Run(const Ctx& c) override { nav::Teleport(c.w, nav::PointFor(nav::GetSel(), c.w.map)); }
};

class SavePosition : public NavAction {
 public:
  SavePosition() : NavAction(nav::Action::SavePos) {
    group = Group::Positions;
    id = "nav_save_pos";
    name = "Save position";
    help = "Remembers where Dante stands and where he looks, in the slot picked above. Slots are kept until you "
           "leave this map.";
  }
  void Run(const Ctx& c) override { nav::SavePos(c.w, nav::GetSel().slot); }
};

class LoadPosition : public NavAction {
 public:
  LoadPosition() : NavAction(nav::Action::LoadPos) {
    group = Group::Positions;
    id = "nav_load_pos";
    name = "Load position";
    help = "Puts Dante back where you saved him (slot picked above). Only Dante moves, the level is not reset.";
  }
  void Run(const Ctx& c) override { nav::LoadPos(c.w, nav::GetSel().slot); }
};

class KeepCheckpoint : public NavAction {
 public:
  KeepCheckpoint() : NavAction(nav::Action::Keep) {
    id = "nav_keep";
    name = "Create checkpoint";
    noBind = true;
    help = "Play to a checkpoint normally, then press this: the tool saves a copy of the checkpoint the game just "
           "reached (what Continue would load). From then on Load checkpoint loads that checkpoint from your copy, "
           "exactly like Continue: moving floors, doors and scripts as they were.";
  }
  void Run(const Ctx&) override { realcp::KeepLast(); }
};

class MissionSelect : public NavAction {
 public:
  MissionSelect() : NavAction(nav::Action::MissionSelect) {
    group = Group::Missions;
    id = "nav_mission";
    name = "Mission select";
    frontEndOk = true;
    help = "Loads the mission, difficulty and checkpoint picked above, from inside a mission or from the main "
           "menu. Nothing new is unlocked in your save by loading. Secret missions always use their own "
           "difficulty.";
  }
  void Run(const Ctx& c) override {
    nav::Selection s = nav::GetSel();
    nav::MissionSelect(c.w, s.selMission, s.selDifficulty, s.selPoint);
  }
};

}  // namespace

// Menu order on the Practice tab.
std::vector<Feature*> CreateNavFeatures() {
  return {new MissionSelect, new LoadCheckpoint, new Teleport,     new RestartMission,
          new KeepCheckpoint, new SavePosition,   new LoadPosition};
}

}  // namespace features
