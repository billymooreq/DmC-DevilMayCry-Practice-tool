// Visuals tab: collision wireframes. The work is in visuals.cpp.
#include "features.h"
#include "visuals.h"

namespace features {
namespace {

using visuals::Cat;

class Visual : public Feature {
  Cat cat_;

 protected:
  virtual void Apply(const Ctx&) {}

 public:
  Visual(Cat c, const char* i, const char* n, const char* h) : cat_(c) {
    id = i;
    name = n;
    help = h;
    group = Group::Visuals;
  }
  bool Enable(const Ctx& c) override {
    Apply(c);
    visuals::SetOn(cat_, true);
    return true;
  }
  void Tick(const Ctx& c) override { Apply(c); }
  void Disable() override { visuals::SetOn(cat_, false); }
};

class CharacterCollision : public Visual {
  bool enemies_;
  void Apply(const Ctx& c) override { visuals::SetMoveCapsule(enemies_, c.p[0] != 0); }

 public:
  CharacterCollision(bool enemies, const char* i, const char* n, const char* h)
      : Visual(enemies ? Cat::Enemy : Cat::Player, i, n, h), enemies_(enemies) {
    params = {{"capsule", "Movement capsule", 0, 0, 1, "", ParamKind::Toggle, {},
               enemies ? "Also the upright capsule the enemy walks and pushes with (faint lines). Hits do not use it."
                       : "Also the upright capsule Dante walks and pushes with (faint lines). Hits do not use it."}};
  }
};

class InvisibleWalls : public Visual {
  void Apply(const Ctx& c) override { visuals::SetWallGrid(c.p[0] != 0); }

 public:
  InvisibleWalls()
      : Visual(Cat::Walls, "vis_invisible_walls", "Invisible walls",
               "Walls you cannot see but Dante cannot walk through: the level's blocking areas that stop him right "
               "now. Walls the level has switched off, or that stop only enemies, are not shown.") {
    params = {{"grid", "Grid", 1, 0, 1, "", ParamKind::Toggle, {},
               "A 1 m grid on the walls near Dante, so their size and height are easy to read."}};
  }
};

class AttackHitboxes : public Visual {
  void Apply(const Ctx& c) override { visuals::SetLinger(c.p[0]); }

 public:
  AttackHitboxes()
      : Visual(Cat::Hitbox, "vis_hitboxes", "Attack hitboxes",
               "The zone each attack checks for hits, while it is active: Dante's in pink, enemies' in red. Many "
               "attacks are active only for a few frames, so the shape stays on screen a little longer and fades. "
               "Hits without a zone (guns on a locked target, grabs) show as a line to the target.") {
    params = {{"linger", "Keep on screen", 0.3f, 0.0f, 2.0f, "%.1f s", ParamKind::Slider, {},
               nullptr}};
  }
};

}  // namespace

// Menu order on the Visuals tab.
std::vector<Feature*> CreateVisualFeatures() {
  return {
      new CharacterCollision(true, "vis_enemy_collision", "Enemy collision",
                             "The shapes on each enemy's body that attacks are checked against (capsules and balls on "
                             "the bones; they move with the animation). A hit counts only where an attack touches them."),
      new CharacterCollision(false, "vis_player_collision", "Player collision",
                             "The same for Dante: the shapes enemy attacks must touch to hit him."),
      new AttackHitboxes,
      new Visual(Cat::HitMarker, "vis_hit_markers", "Hit markers",
                 "A small star where a hit really counted (white: Dante's hits, red: hits on Dante). Handy to check "
                 "that the hitbox you see matches the game. Fades after a second."),
      new Visual(Cat::Cutscene, "vis_cutscene_triggers", "Cutscene triggers",
                 "Areas that start a cutscene when Dante walks in. Faint: already used, or not switched on by the "
                 "level yet."),
      new Visual(Cat::Arena, "vis_arena_triggers", "Arena triggers",
                 "Areas that start a fight (\"Arena\") or bring the next wave of enemies into it (\"Arena wave\"). "
                 "Faint: already used, or not switched on by the level yet."),
      new InvisibleWalls,
      new Visual(Cat::OtherTrigger, "vis_other_triggers", "Other triggers",
                 "All other walk-in areas of the level script (doors, music, camera, tutorials, loading...)."),
  };
}

}  // namespace features
