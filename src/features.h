#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "game.h"
#include "hotkeys.h"
#include "json.hpp"

// In-mission features ("cheats"): one registry, a clean Enable/Disable per feature, binds and presets.
//
// A feature is ON when the player ticked it ("manual") OR an active preset contains it. It is APPLIED to the game
// only while it is on, the game version is known and a mission is running; the game-thread frame callback applies
// and removes it, and re-applies it after every map change / checkpoint / restart (the player character is a new
// object then). Disable() puts back everything the feature changed. With nothing on, the game is untouched.
namespace features {

// Cheats tab: Player, Enemies, Game, Visuals (groups with a heading, in this order). Practice tab: Navigation
// (later: enemy spawn, damage log).
enum class Group { Player, Enemies, Game, Visuals, Navigation, Positions, Missions, EnemyTools, DamageLog };
const char* GroupName(Group g);

// Parameters are floats everywhere (json, presets); the kind only changes the widget:
// Slider (min..max), Toggle (0/1 checkbox), Choice (index into choices, min 0, max = count - 1).
enum class ParamKind { Slider, Toggle, Choice };

struct ParamDef {
  const char* id;     // json key
  const char* label;  // shown next to the widget
  float def, min, max;
  const char* fmt;    // Slider: ImGui format, e.g. "%.1fx"
  ParamKind kind = ParamKind::Slider;
  std::vector<const char*> choices = {};  // Choice: option names
  const char* help = nullptr;             // Toggle / Choice: tooltip
  std::string Text(float v) const;        // "1.5x", "on", "Level flight"
};

constexpr int kMaxParams = 2;
using Params = std::array<float, kMaxParams>;

struct Ctx {
  const game::World& w;
  Params p;
};

class Feature {
 public:
  const char* id = "";
  const char* name = "";
  Group group = Group::Player;  // inside a group: registration order = most used first
  const char* help = "";
  bool action = false;            // one-shot button instead of a checkbox (navigation)
  bool frontEndOk = false;        // action: may also run in the main menu (Mission select)
  bool noBind = false;            // no key / preset for it (Teleport to point, Create checkpoint)
  std::vector<ParamDef> params;
  const char* loserTo = nullptr;  // id of a feature that wins when both are on (shown as a warning)

  virtual ~Feature() = default;
  // Game thread, in a mission. Enable: false = try again next frame. Disable must restore every change it made
  // and only touch objects that are still alive (game::Alive) - after a map change they are gone.
  virtual bool Enable(const Ctx&) { return true; }
  virtual void Tick(const Ctx&) {}
  virtual void Disable() {}
  virtual void Run(const Ctx&) {}  // action features
  // Action features: why the button is greyed out now ("" = available). Any thread, cheap.
  virtual std::string Blocked() const { return ""; }
};

void Init();  // registers the features, sets the frame/event callbacks (init thread, once)
const std::vector<Feature*>& All();
Feature* Find(const std::string& id);
bool AnyInGroup(Group g);

// ---- state, any thread
struct View {
  bool manual = false;     // ticked by the player
  bool on = false;         // manual or by an active preset
  bool viaPreset = false;  // on because of a preset
  bool applied = false;    // working in the game right now
  std::string why;         // why it is on but not applied ("in a mission only", "Infinite HP wins", ...)
  Params p{};              // effective parameters
  Params manualP{};
};
View Get(const std::string& id);
void SetManual(const std::string& id, bool on);
void ToggleManual(const std::string& id);
void SetParam(const std::string& id, int i, float v);  // manual parameter (also ticks the feature)
void Trigger(const std::string& id);                     // action features, with the menu parameters
void Trigger(const std::string& id, const Params& p);    // with a preset's parameters

// ---- binds and presets (render thread; Lock() while touching Data from the UI)
struct PresetItem {
  std::string id;
  Params p{};
};
struct Preset {
  std::string name;
  hotkeys::Bind bind;
  std::vector<PresetItem> items;
  bool active = false;     // runtime only, every game start begins with all presets off
  uint64_t activatedAt = 0;
};
struct Data {
  std::vector<hotkeys::Bind> binds;  // parallel to All()
  std::vector<Preset> presets;
};
std::unique_lock<std::recursive_mutex> Lock();
Data& D();  // with Lock() held
void TogglePreset(int index);
void MarkDirty();  // config changed: autosave

// "Practice Tool Active": true from the first time any feature was turned on in this game session, until exit.
bool OverlaySticky();
void ForceOverlay();  // autorun tests only

// Game status line for the menu (any thread).
game::Status Status();

// Config (DmCPracticeTool.json): binds, parameters, presets. On/off states are not saved: every game start
// begins with everything off.
void LoadJson(const nlohmann::json& features, const nlohmann::json& presets);
void SaveJson(nlohmann::json& features, nlohmann::json& presets);

// Dev/autorun: log every hit (attacker, victim, damage before/after our multipliers).
void SetHitLog(bool on);
void SetGlideTrace(bool on);  // Dev/autorun: glide physics every frame

// Autorun / debug: one line per feature with its state.
std::string DebugText();

}  // namespace features
