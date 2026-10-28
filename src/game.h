#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Live game state for the in-mission features. Everything here is re-read every frame on the game thread:
// the player pawn, its controller and the world change on every map load, checkpoint and restart, so nothing
// game-side is cached beyond one frame (only the engine object, which lives as long as the process).
namespace game {

// ---- reflection helpers (any thread; offsets cached per class + name)
struct Field {
  int off = -1;
  uint32_t mask = 0;  // BoolProperty bit
  uintptr_t prop = 0;  // the UProperty
  bool ok() const { return off >= 0; }
};
Field FieldOf(uintptr_t obj, const char* name);  // along the object's class chain
float GetFloat(uintptr_t obj, const char* name, float def = 0.0f);
bool SetFloat(uintptr_t obj, const char* name, float v);
int GetInt(uintptr_t obj, const char* name, int def = 0);
bool SetInt(uintptr_t obj, const char* name, int v);
uint8_t GetByte(uintptr_t obj, const char* name, uint8_t def = 0);
bool SetByte(uintptr_t obj, const char* name, uint8_t v);
bool GetBool(uintptr_t obj, const char* name, bool def = false);
bool SetBool(uintptr_t obj, const char* name, bool v);
uintptr_t GetObj(uintptr_t obj, const char* name);  // 0 unless the value is a live UObject
std::string GetStr(uintptr_t obj, const char* name);   // FString property as UTF-8 ("" if absent)
std::string GetName(uintptr_t obj, const char* name);  // FName property ("" if absent or None)
// True while obj is still registered in GObjects at its own index (not destroyed / garbage collected).
bool Alive(uintptr_t obj);
std::string TopPackage(uintptr_t obj);

struct Vec {
  float x = 0, y = 0, z = 0;
};

// ---- per-frame world (game thread)
struct World {
  uintptr_t engine = 0, pc = 0, pawn = 0, world = 0, gameInfo = 0, cheat = 0;
  std::string map;
  bool frontEnd = false;
  bool mission = false;  // a mission map with a live player character (DVPawn_Player)
  unsigned gen = 0;      // changes whenever the world or the player character changes (map, checkpoint, restart)
  float realDt = 0;      // real seconds since the previous frame
  float gameDt = 0;      // world time since the previous frame (0 while paused)
};
const World& W();  // game thread only
void OnFrame();    // game thread, once per rendered frame (from the ProcessEvent hook)

// Why game features can (not) run now, for the menu. Any thread.
struct Status {
  bool ready = false;    // UE3 + ProcessEvent hook + engine found
  bool mission = false;
  bool frontEnd = false;
  std::string map;
  std::string reason;    // short, player-facing
};
Status CurrentStatus();

// The game's camera of the last frame: PlayerCamera.CameraCache.POV (FOV is horizontal, in degrees). Game thread.
struct Pov {
  Vec loc;
  int rot[3] = {0, 0, 0};  // pitch, yaw, roll (65536 = 360 degrees)
  float fov = 90.0f;
};
bool CameraPov(const World& w, Pov& out);

// Active enemies of the current mission (DVGameInfo.m_EnemyManager). Game thread.
std::vector<uintptr_t> Pawns();
bool IsEnemy(uintptr_t pawn);  // AI-controlled, not the player character

// Tokens (NTTokenManager of a pawn). Game thread. Owner name must already exist in GNames.
bool HasToken(uintptr_t pawn, const char* token, const char* owner = nullptr);
bool CreateToken(uintptr_t pawn, const char* token, const char* owner);
bool DeleteToken(uintptr_t pawn, const char* token, const char* owner);

}  // namespace game
