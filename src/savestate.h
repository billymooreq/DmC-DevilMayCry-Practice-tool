#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Save states (#25-28 research, #29: the Save states tab): emulator-style save states inside one run of the game.
// See docs/savestates.md.
namespace savestate {

constexpr int kSlotCount = 5;  // #29: Save states tab, slots 1-5

// DllMain: thread-name catcher (SetThreadName exception) and the VirtualAlloc owner table. No logging here.
void InstallEarly();

// Research: threads (name, start module, where they wait), the memory map by owner, heaps. Lines for the log.
std::vector<std::string> Probe(bool stacks);
// Research: raw stack scan of a thread (return addresses into modules, with the call target where it is direct).
std::vector<std::string> StackScan(unsigned long tid, int maxLines);

// DllMain: config "savestates" (#29, Save states tab "Enable save states"; older configs "dev_savestates"; a plain
// look into DmCPracticeTool.json). Then window_mode hooks the game's CreateDevice and calls OnGameDevice: save states
// are on from the first D3D9 resource. Off = the game and the tool run as without save states.
bool WantEarly();
void OnGameDevice(void* device);  // IDirect3DDevice9*

// Init thread: hooks FEngineLoop::Tick (the point where the game is paused for a save / load).
void Init();
bool Available();
// On in this run of the game (from its start, see WantEarly; autorun "ss arm" for tests).
bool Armed();
void SetArmed(bool on);
// d3d9_hook: render thread, every Present / before a device Reset.
void OnRenderFrame();
void OnDeviceReset();
void OnPresent(void* device);  // #29: before the tool draws: the picture of a slot just saved
// 1 = save, 2 = load, 3 = empty, 4 = empty all; done at the start of the next frame. The player is told the result
// (menu::Toast + menu::Notice).
void Request(int op, int slot);
int ActiveSlot();
void SetActiveSlot(int slot);
std::string Message();
std::string SlotText(int slot);
std::vector<std::string> HeapStats();
void DevUi();  // Dev tab section (debug)

// #29: the Save states tab (render thread; copies).
struct SlotView {
  bool valid = false;
  std::string map, dropWhy;  // dropWhy: why it became empty by itself
  std::string checkpoint;    // the game's checkpoint at the save ("" = from the mission start)
  int hour = 0, minute = 0;
  float pos[3] = {};
  float hp = 0, hpMax = 0, dt = 0, dtMax = 0;
  bool packed = false;
  int loads = 0;
  unsigned thumbGen = 0;
};
SlotView SlotInfo(int slot);
// The slot's picture (BGRA, w x h) when it changed since *gen (then *gen = its generation; an empty picture = none).
bool Thumb(int slot, unsigned* gen, std::vector<uint8_t>* bgra, int* w, int* h);
struct MemView {
  uint64_t storeBytes = 0;  // page file: packed slots
  uint64_t rawBytes = 0;    // page file: slots not packed yet
  uint64_t heldBytes = 0;   // the game's address space kept for slots (released by the game since their saves)
  uint64_t freeBytes = 0;   // free address space of the game
  int oldOpenings = 0;      // slots' map openings other than the current one
  bool packing = false;
};
MemView Memory();

// autorun "ss <...>" (save/load/status/...).
std::vector<std::string> Command(const std::vector<std::string>& args);

}  // namespace savestate
