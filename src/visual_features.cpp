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
               enemies ? "Also the capsule the enemy moves with (faint lines). Hits do not use it."
                       : "Also Dante's movement capsule (faint lines). Hits do not use it."}};
  }
};

class InvisibleWalls : public Visual {
  void Apply(const Ctx& c) override { visuals::SetWallGrid(c.p[0] != 0); }

 public:
  InvisibleWalls()
      : Visual(Cat::Walls, "vis_invisible_walls", "Invisible walls",
               "Yellow: walls you cannot see in the game but Dante cannot walk through.") {
    params = {{"grid", "Grid (1 m)", 1, 0, 1, "", ParamKind::Toggle, {},
               "A 1 m grid on the walls near Dante, so their size and height are easy to read."}};
  }
};

class AttackHitboxes : public Visual {
  void Apply(const Ctx& c) override { visuals::SetLinger(c.p[0]); }

 public:
  AttackHitboxes()
      : Visual(Cat::Hitbox, "vis_hitboxes", "Attack hitboxes",
               "Draws attack hitboxes while they can hit: Dante's pink, enemies' red. Hits without a zone (guns on "
               "a locked target, grabs) show as a line to the target.") {
    params = {{"linger", "Keep on screen", 0.3f, 0.0f, 2.0f, "%.1f s", ParamKind::Slider, {},
               "How long an attack hitbox stays on screen after the hit.", 0.1f, 10.0f}};
  }
};

class AreaDamage : public Visual {
  void Apply(const Ctx& c) override { visuals::SetAreaLinger(c.p[0]); }

 public:
  AreaDamage()
      : Visual(Cat::Area, "vis_area_damage", "Explosions & area damage",
               "Teal: explosions, shock waves, poison and other damage zones, while they can hurt. Grenades and "
               "bombs in flight: a small shape with a ring for where they will explode.") {
    params = {{"linger", "Keep on screen", 0.5f, 0.0f, 2.0f, "%.1f s", ParamKind::Slider, {},
               "How long an explosion stays on screen after it stops hurting.", 0.1f, 10.0f}};
  }
};

}  // namespace

// Menu order on the Visuals tab.
std::vector<Feature*> CreateVisualFeatures() {
  return {
      new CharacterCollision(true, "vis_enemy_collision", "Enemy collision",
                             "Draws enemy bodies (orange): the shapes attacks are checked against."),
      new CharacterCollision(false, "vis_player_collision", "Player collision",
                             "Draws Dante's body (blue): the shapes enemy attacks must touch to hit him."),
      new AttackHitboxes,
      new AreaDamage,
      new Visual(Cat::HitMarker, "vis_hit_markers", "Hit markers",
                 "Marks where hits land (white: Dante's hits, light red: hits on Dante). Fades after a second."),
      new Visual(Cat::Hazard, "vis_level_hazards", "Level hazards",
                 "Lime: parts of the level that hurt when touched (fire, spikes, poison, pits). Faint: switched off "
                 "for now, or a trap for enemies only (Dante can knock them in)."),
      new Visual(Cat::Cutscene, "vis_cutscene_triggers", "Cutscene triggers",
                 "Purple boxes that start cutscenes. Faint: already used, or not switched on yet."),
      new Visual(Cat::Arena, "vis_arena_triggers", "Arena triggers",
                 "Green boxes that start arena fights or bring the next wave. Faint: already used, or not switched "
                 "on yet."),
      new InvisibleWalls,
      new Visual(Cat::OtherTrigger, "vis_other_triggers", "Other triggers",
                 "Grey: all other level triggers (doors, music, camera, tutorials, loading...)."),
  };
}

}  // namespace features
