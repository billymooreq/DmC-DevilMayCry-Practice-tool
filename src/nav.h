#pragma once
#include <string>
#include <vector>

#include "game.h"
#include "json.hpp"

// Navigation (Practice tab): mission catalog, restart, checkpoints, mission select, teleports, saved positions.
// Research behind every choice: research/navigation.md.
//
// How the game loads a checkpoint (and we do the same): NTCheckpoint (engine.GCheckPointData) holds the map and
// checkpoint name; on a map load NTGameInfo_SP.FindCheckpoint() picks the Kismet action SeqAct_NT_SetCheckPoint
// with that name (or MapInfo.m_FirstCheckpointName), spawns Dante at its player start and fires its "restarted"
// outputs, so the level scripts set up enemies, arenas and doors for that point. We clear the saved checkpoint
// (no snapshot of a place we never reached), write the name and open the map: the level does the rest.
namespace nav {

enum class Kind { Start, Checkpoint, Arena, Cutscene };
const char* KindName(Kind k);  // json: "start", "checkpoint", "arena", "cutscene"

struct Point {
  std::string id;    // checkpoint name in the level ("cServerRoom"), or "start" / an arena id
  std::string name;  // shown in the menu ("Server Room")
  Kind kind = Kind::Checkpoint;
  bool hasPos = false;
  game::Vec pos;
  int yaw = 0;  // Unreal rotation units (65536 = 360 degrees)
  std::string cp;  // the level's checkpoint name (for the start: MapInfo.m_FirstCheckpointName, may be "")
  bool Loadable() const { return kind == Kind::Start || kind == Kind::Checkpoint; }
};

struct Mission {
  std::string id;    // "M12", "SM05"
  std::string map;   // "Ch12_Main"
  std::string name;  // "Mission 12 - Under Siege"
  bool secret = false;
  int index = -1;    // story: mission index 0..19 (profile m_iMapNumber); secret: 0..20
  std::vector<Point> points;  // in the order of the mission
};

// ---- catalog (missions.json built into the DLL; DmCPT_missions.json next to the game exe wins if present)
void Init();  // loads the catalog (once)
const std::vector<Mission>& Missions();
int FindMission(const std::string& map);  // case-insensitive, -1 = not a catalog map
std::string CatalogSource();              // "built in" / "DmCPT_missions.json" / error text
bool ExportCatalog(std::string* msg);     // writes the built-in catalog to DmCPT_missions.json for editing
std::string CatalogText();                // the catalog in use, as the json file
constexpr const char* kDefaultList = "Default checkpoints";
std::string ListName();                   // name of the checkpoint list in use (kDefaultList = the built-in one)
bool UseBuiltin();                        // back to the built-in list (removes DmCPT_missions.json). Render thread.
// Imported checkpoint set: becomes the catalog in use (and DmCPT_missions.json); its "list_name" is shown.
// Render thread.
bool ReplaceCatalog(const std::string& text, std::string* err);

// The player edits the points of a mission in the menu (up / down / delete / rename); every change is saved to
// DmCPT_missions.json at once. Render thread only (the only writer); the game thread copies a mission under a lock.
bool MovePoint(int mission, int point, int dir);  // dir -1 = up, +1 = down; the mission start stays first
bool DeletePoint(int mission, int point);         // not the mission start
bool RenamePoint(int mission, int point, const std::string& name);
// A checkpoint the player created that is not in the list: inserted after point `after`. Returns its index (-1).
int AddPoint(int mission, const Point& p, int after);
int FindPoint(int mission, const std::string& checkpoint);  // by the level's checkpoint name, -1
std::string NiceName(const std::string& checkpoint);        // "cDiningFightWon" -> "Dining Fight Won"
bool RenameMission(int mission, const std::string& name);
bool ResetMission(int mission);                   // back to the built-in name and list of this mission
bool Edited(int mission);                         // name / order / points differ from the built-in list

// ---- developers' test maps in Mission select (listed before the missions). Selection.selMission < 0 picks one:
// -1 = DevRooms()[0]. Only maps checked in the game are here (research: docs/debug.md).
struct DevRoom {
  const char* map;   // "Combat"
  const char* name;  // "Dev room"
};
const std::vector<DevRoom>& DevRooms();
int FindDevRoom(const std::string& map);  // case-insensitive, -1

constexpr int kDifficulties = 7;
const char* DifficultyName(int mode);  // 0 Human .. 6 Hell and Hell
constexpr int kPosSlots = 3;     // Dante's positions 1-3 (Practice tab)
constexpr int kAllPosSlots = 6;  // + enemy positions 4-6 (Enemies tab: spawn there); all kept until the map changes

// ---- state for the menu (any thread)
struct Snapshot {
  bool ready = false;     // game version known, engine found
  bool mission = false;   // Dante in a mission map
  bool frontEnd = false;  // main menu
  bool cutscene = false;
  bool busy = false;      // a load we started is not finished yet
  std::string map;
  int missionIndex = -1;  // into Missions()
  int devRoom = -1;       // into DevRooms() when Dante is in one
  int slotStatus = -1;    // main menu: DVSaveManager.GetActiveSlotStatus (3/4 = a save slot is loaded)
  int difficulty = -1;    // current difficulty mode 0..6 (-1 unknown)
  bool slot[kAllPosSlots] = {};
};
Snapshot Now();

// Why an action cannot run now ("" = it can). Player-facing, short ("in a mission only", "cutscene"...).
enum class Action { Restart, LoadCheckpoint, MissionSelect, Teleport, SavePos, LoadPos, Keep, DevRoom };
std::string Blocked(Action a, int slot = 0);

// ---- what the player picked in the menu (any thread; saved in DmCPracticeTool.json). Binds and presets use the
// selection made in the menu.
struct Selection {
  int point = 0;           // in the current mission (0 = mission start)
  std::string pointMap;    // the map "point" belongs to: another mission = back to 0
  int selMission = 0;      // Mission select: index into Missions()
  int selDifficulty = 0;   // 0..6
  int selPoint = 0;        // 0 = mission start
  int slot = 0;            // position slot for Save/Load position buttons
  bool useReal = true;     // always: a created checkpoint loads like Continue (the old switch is ignored)
};
Selection GetSel();
void SetSel(const Selection& s);
int PointFor(const Selection& s, const std::string& map);  // s.point if it belongs to map, else 0
void LoadJson(const nlohmann::json& j);
nlohmann::json SaveJson();

// ---- actions (game thread, run by the Navigation features). Each re-checks Blocked() and logs a refusal.
void OnFrame(const game::World& w);  // every frame, before the actions
void RestartMission(const game::World& w);
void LoadPoint(const game::World& w, int point);  // checkpoint / start of the current mission
// ownCheckpoints: a point with a checkpoint the player created loads that one (Continue); false = always the
// level's own start (Timers > Speedrun "Start run").
void MissionSelect(const game::World& w, int mission, int difficulty, int point, bool ownCheckpoints = true);
// Leaves the mission for the game's main menu ("open mainmenu_startup", the front-end map). Game thread.
bool OpenMainMenu(const game::World& w);
void StartDevRoom(const game::World& w, int room, int difficulty);
void Teleport(const game::World& w, int point);
void SavePos(const game::World& w, int slot);  // 0-2 Dante's, 3-5 enemy positions
void LoadPos(const game::World& w, int slot);
bool SavedPosition(int slot, game::Vec* loc, int* yaw);  // game thread: what Save position stored (enemy spawn)

// ---- Dev / autorun
std::vector<std::string> LiveCheckpoints();  // checkpoints in the loaded map (game thread)
// Visits every catalog mission (or [first, last]) and writes DmCPT_dumps\missions_crawl.json (worker thread).
void Crawl(int first, int last);

}  // namespace nav
