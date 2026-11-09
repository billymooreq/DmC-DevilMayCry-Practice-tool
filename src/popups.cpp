#include "popups.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "features.h"
#include "game.h"
#include "log.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace popups {
namespace {

constexpr uint8_t kItemGoldOrbUse = 2;  // ItemAcquiredList.Item_GoldOrb_Use: "use a gold orb?" - a choice

std::atomic<bool> g_on{false};
uintptr_t g_fnTutUpdate = 0, g_fnTutCheck = 0, g_fnTutActivated = 0;
std::vector<uintptr_t> g_tutorials;  // DVSeqAct_TutorialMessage seen running (game thread only)
int g_itemsClosed = 0, g_tutorialsClosed = 0, g_secretStarts = 0;

ue3::TArrayRaw Arr(uintptr_t obj, const char* name) {
  ue3::TArrayRaw a{};
  game::Field f = game::FieldOf(obj, name);
  if (!f.ok() || !util::Read(obj + f.off, a) || a.num < 0 || a.num > 4096) return {};
  return a;
}

// The first live object of a class (cached, re-found when it died; a scan at most every 2 s).
struct Cached {
  const char* cls;
  uintptr_t obj = 0;
  uint64_t lastScan = 0;
  uintptr_t Get() {
    if (obj && game::Alive(obj) && ue3::ClassNameOf(obj) == cls) return obj;
    obj = 0;
    uint64_t now = GetTickCount64();
    if (now - lastScan < 2000) return 0;
    lastScan = now;
    for (int i = 0, n = ue3::NumObjects(); i < n; ++i) {
      uintptr_t o = ue3::Object(i);
      if (o && ue3::ClassNameOf(o) == cls && ue3::ObjName(o).find("Default__") != 0) {
        obj = o;
        break;
      }
    }
    return obj;
  }
};
Cached g_ingameUI{"DVGFxMovieHandler_IngameUI"};
Cached g_callbacks{"DVGFxMovieHandler_IngameUI_Callbacks"};

// The "item acquired" screen is open (IngameUI.m_LoadedComponents has a DVGFxContainer_ItemAcquiredScreen).
uintptr_t ItemScreen() {
  uintptr_t ui = g_ingameUI.Get();
  if (!ui) return 0;
  ue3::TArrayRaw a = Arr(ui, "m_LoadedComponents");
  for (int i = 0; i < a.num; ++i) {
    uintptr_t c = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (ue3::IsObject(c) && ue3::ClassNameOf(c) == "DVGFxContainer_ItemAcquiredScreen") return c;
  }
  return 0;
}

class SkipPopups : public features::Feature {
 public:
  SkipPopups() {
    id = "skip_popups";
    name = "Skip hints & pop-ups";
    group = features::Group::Cutscenes;
    anywhere = true;
    help = "Closes pop-ups the moment they appear, as if you pressed OK: \"new weapon / ability / item\" screens, "
           "tutorial hints (also the ones that pause the game) and the \"Press Back to begin\" screen of secret "
           "missions. What the pop-up gives you is still given.\n"
           "Not closed: the pause menu, mission results, the gold orb \"use it?\" choice and confirmations.";
  }
  bool Enable(const features::Ctx&) override {
    if (!g_fnTutUpdate) {
      g_fnTutUpdate = ue3::FindObject("Function", "Update", "DVSeqAct_TutorialMessage");
      g_fnTutCheck = ue3::FindObject("Function", "CheckButtonPress", "DVSeqAct_TutorialMessage");
      g_fnTutActivated = ue3::FindObject("Function", "Activated", "DVSeqAct_TutorialMessage");
    }
    g_on = true;
    return true;
  }
  void Disable() override {
    g_on = false;
    g_tutorials.clear();
  }
  void Tick(const features::Ctx& c) override {
    uintptr_t pc = c.w.pc;
    if (!pc || !ue3::IsA(pc, "DVPlayerController")) return;
    // 1) Item / weapon / ability acquired: the OK callback, one item per frame (the queue shows the next one).
    if (uintptr_t scr = ItemScreen()) {
      ue3::TArrayRaw q = Arr(pc, "m_aItemsAcquired");
      uint8_t first = q.num > 0 ? util::ReadOr<uint8_t>(q.data) : 0;
      uint8_t shown = game::GetByte(scr, "lastItemAcquired");
      static uintptr_t lastScr = 0;
      static uint8_t lastItem = 0;
      static uint64_t lastCall = 0;
      uint64_t now = GetTickCount64();
      bool again = scr != lastScr || shown != lastItem || now - lastCall > 700;  // one OK per shown item
      if (again && first != kItemGoldOrbUse && shown != kItemGoldOrbUse && game::GetObj(scr, "m_gfxObject")) {
        lastScr = scr;
        lastItem = shown;
        lastCall = now;
        if (uintptr_t cb = g_callbacks.Get()) {
          engine::Call call(cb, "Death_ResumeGame");
          if (call.Invoke()) {
            ++g_itemsClosed;
            LOG("popups: item screen closed (item %u, %d left in the queue)", shown, q.num > 0 ? q.num - 1 : 0);
          }
        }
      }
    }
    // 2) Tutorial hints of the level script: their own Hide().
    for (size_t i = 0; i < g_tutorials.size();) {
      uintptr_t t = g_tutorials[i];
      if (!game::Alive(t)) {
        g_tutorials.erase(g_tutorials.begin() + i);
        continue;
      }
      if (game::GetBool(t, "m_CurrentlyActive")) {
        bool paused = game::GetBool(t, "m_bPaused");
        engine::Call h(t, "Hide");
        if (h.Invoke()) {
          ++g_tutorialsClosed;
          LOG("popups: tutorial hint %s closed (%s, '%s')", ue3::ObjName(t).c_str(),
              paused ? "it paused the game" : "no pause", game::GetStr(t, "m_LocaliseTag").c_str());
        }
      }
      ++i;
    }
    // 3) Secret missions: "Press Back to begin" - start as soon as the game accepts the button.
    if (c.w.gameInfo) {
      uintptr_t sm = game::GetObj(c.w.gameInfo, "m_SecretMissionsManager");
      if (sm && game::GetBool(sm, "m_bReadyButtonAllowed")) {
        engine::Call r(sm, "ReadyButtonPressed");
        if (r.Invoke()) {
          ++g_secretStarts;
          LOG("popups: secret mission started (ready button)");
        }
      }
    }
  }
};

}  // namespace

features::Feature* CreateFeature() { return new SkipPopups; }

void OnEvent(uintptr_t self, uintptr_t fn) {
  if (!g_on || !fn || !self) return;
  if (fn != g_fnTutUpdate && fn != g_fnTutCheck && fn != g_fnTutActivated) return;
  if (std::find(g_tutorials.begin(), g_tutorials.end(), self) == g_tutorials.end() && g_tutorials.size() < 64)
    g_tutorials.push_back(self);
}

std::vector<std::string> DebugLines() {
  char b[160];
  snprintf(b, sizeof b, "items closed %d, tutorial hints closed %d, secret missions started %d, tracked hints %zu",
           g_itemsClosed, g_tutorialsClosed, g_secretStarts, g_tutorials.size());
  return {b};
}

}  // namespace popups
