// #29: the keys of the Save states tab - "Load slot N" (F5-F9) and "Save to slot N" (Alt+F5-F9 since #30; Shift before).
// They are cheats (Practice Tool Active, off in Speedrun mode) and only take the key while save states are on.
#include <windows.h>

#include <string>
#include <vector>

#include "features.h"
#include "hotkeys.h"
#include "savestate.h"

namespace features {
namespace {

const char* const kLoadIds[] = {"ss_load_1", "ss_load_2", "ss_load_3", "ss_load_4", "ss_load_5"};
const char* const kSaveIds[] = {"ss_save_1", "ss_save_2", "ss_save_3", "ss_save_4", "ss_save_5"};
const char* const kLoadNames[] = {"Load slot 1", "Load slot 2", "Load slot 3", "Load slot 4", "Load slot 5"};
const char* const kSaveNames[] = {"Save to slot 1", "Save to slot 2", "Save to slot 3", "Save to slot 4",
                                  "Save to slot 5"};

class SlotAction : public Feature {
 public:
  SlotAction(int slot, bool save) : slot_(slot), save_(save) {
    id = save ? kSaveIds[slot] : kLoadIds[slot];
    name = save ? kSaveNames[slot] : kLoadNames[slot];
    group = Group::SaveStates;
    action = true;
    anywhere = true;  // also in the main menu / on a loading screen: the player is told why it cannot run
    help = save ? "Saves this moment into the slot (a used slot is replaced). The slot lasts until you quit the game."
                : "Brings back the slot's moment: Dante, enemies and the level exactly as saved.";
    defBind.vk = VK_F5 + slot;
    defBind.mods = save ? hotkeys::kAlt : 0;  // #30: Alt (was Shift up to 2.0.0, moved in config.cpp)
  }
  bool KeyActive() const override { return savestate::Armed(); }
  std::string Blocked() const override {
    return savestate::Armed() ? "" : "save states are off (turn them on above, then restart the game)";
  }
  void Run(const Ctx&) override { savestate::Request(save_ ? 1 : 2, slot_); }

 private:
  int slot_;
  bool save_;
};

}  // namespace

std::vector<Feature*> CreateSaveStateFeatures() {
  std::vector<Feature*> v;
  for (int i = 0; i < savestate::kSlotCount; ++i) {
    v.push_back(new SlotAction(i, false));
    v.push_back(new SlotAction(i, true));
  }
  return v;
}

}  // namespace features
