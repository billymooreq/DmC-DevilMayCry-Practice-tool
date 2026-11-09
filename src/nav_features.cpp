// Navigation actions (Practice tab). They work on what is picked in the menu (point, mission, difficulty, slot);
// binds and presets use that same pick. The work itself is in nav.cpp.
#include "features.h"
#include "nav.h"
#include "realcp.h"
#include "menu.h"

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
    help = "Starts the mission you are in from the beginning, on the same difficulty.";
  }
  void Run(const Ctx& c) override {
    nav::RestartMission(c.w);
  }
};

class LoadCheckpoint : public NavAction {
 public:
  LoadCheckpoint() : NavAction(nav::Action::LoadCheckpoint) {
    id = "nav_checkpoint";
    name = "Load checkpoint";
    help = "Loads the picked checkpoint, the way Continue does: enemies, arenas and doors are set up for it.";
  }
  void Run(const Ctx& c) override {
    int pt = nav::PointFor(nav::GetSel(), c.w.map);
    nav::LoadPoint(c.w, pt);
  }
};

class Teleport : public NavAction {
 public:
  Teleport() : NavAction(nav::Action::Teleport) {
    id = "nav_teleport";
    name = "Teleport to point";
    noBind = true;
    help = "Moves only Dante to the picked point. Enemies, arenas and doors stay as they are.";
  }
  void Run(const Ctx& c) override {
    nav::Teleport(c.w, nav::PointFor(nav::GetSel(), c.w.map));
  }
};

class SavePosition : public NavAction {
 public:
  SavePosition() : NavAction(nav::Action::SavePos) {
    group = Group::Positions;
    id = "nav_save_pos";
    name = "Save position";
    help = "Remembers where Dante stands in the picked slot.";
  }
  void Run(const Ctx& c) override { nav::SavePos(c.w, nav::GetSel().slot); }
};

class LoadPosition : public NavAction {
 public:
  LoadPosition() : NavAction(nav::Action::LoadPos) {
    group = Group::Positions;
    id = "nav_load_pos";
    name = "Load position";
    help = "Moves Dante back to the saved place. Only Dante moves.";
  }
  void Run(const Ctx& c) override {
    int slot = nav::GetSel().slot;
    nav::LoadPos(c.w, slot);
  }
};

class KeepCheckpoint : public NavAction {
 public:
  KeepCheckpoint() : NavAction(nav::Action::Keep) {
    id = "nav_keep";
    name = "Create checkpoint";
    noBind = true;
    help = "Play to a checkpoint normally, then press this: the checkpoint the game just saved becomes your own. "
           "It loads exactly like Continue.";
  }
  void Run(const Ctx&) override { realcp::KeepLast(); }
};

// "Delete my checkpoint": the checkpoint the player created for the picked point of this mission. Deletes a
// file of the tool only (the game is not touched).
class DeleteMyCheckpoint : public Feature {
 public:
  DeleteMyCheckpoint() {
    id = "nav_delete_mine";
    name = "Delete my checkpoint";
    group = Group::Navigation;
    action = true;
    noOverlay = true;  // a file of the tool, not the game
    help = "Deletes the checkpoint you made for the picked point. That point then loads the normal way again.";
  }
  static bool Mine(std::string* map, std::string* cp) {
    nav::Snapshot s = nav::Now();
    if (s.missionIndex < 0) return false;
    const nav::Mission& m = nav::Missions()[s.missionIndex];
    int pt = nav::PointFor(nav::GetSel(), m.map);
    if (pt < 0 || pt >= (int)m.points.size() || m.points[pt].cp.empty() || !realcp::Find(m.map, m.points[pt].cp))
      return false;
    *map = m.map;
    *cp = m.points[pt].cp;
    return true;
  }
  std::string Blocked() const override {
    std::string m, c;
    if (!nav::Now().mission) return "in a mission only";
    return Mine(&m, &c) ? "" : "the picked checkpoint is not one you made";
  }
  void Run(const Ctx&) override {
    std::string m, c;
    if (!Mine(&m, &c)) return;
    realcp::Forget(m, c);
    menu::Toast("Your checkpoint deleted: this point loads the normal way again");
  }
};

class MissionSelect : public NavAction {
 public:
  MissionSelect() : NavAction(nav::Action::MissionSelect) {
    group = Group::Missions;
    id = "nav_mission";
    name = "Mission select";
    frontEndOk = true;
    help = "Loads the picked mission, difficulty and checkpoint. Nothing new is unlocked in your save.";
  }
  // A dev room (selMission < 0) needs a save slot (it saves a checkpoint as it starts).
  std::string Blocked() const override {
    return nav::Blocked(nav::GetSel().selMission < 0 ? nav::Action::DevRoom : nav::Action::MissionSelect);
  }
  void Run(const Ctx& c) override {
    nav::Selection s = nav::GetSel();
    if (s.selMission < 0)
      nav::StartDevRoom(c.w, -s.selMission - 1, s.selDifficulty);
    else {
      nav::MissionSelect(c.w, s.selMission, s.selDifficulty, s.selPoint);
    }
  }
};

}  // namespace

// Menu order on the Practice tab.
std::vector<Feature*> CreateNavFeatures() {
  return {new MissionSelect, new LoadCheckpoint, new Teleport,     new RestartMission,
          new KeepCheckpoint, new SavePosition,   new LoadPosition, new DeleteMyCheckpoint};
}

}  // namespace features
