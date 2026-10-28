#include "unlock.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "log.h"
#include "saveslots.h"
#include "steam_guard.h"
#include "ue3/dumper.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

using util::ReadOr;

namespace unlock {
namespace {

std::mutex g_repMu;
std::vector<std::string> g_report;
std::atomic<bool> g_busy{false};

void Rep(const char* fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  LOG("unlock: %s", b);
  std::lock_guard<std::mutex> lk(g_repMu);
  g_report.emplace_back(b);
}

void ClearReport() {
  std::lock_guard<std::mutex> lk(g_repMu);
  g_report.clear();
}

using engine::OnGame;

// ------------------------------------------------------------------ reflection helpers
struct PropRef {
  int offset = -1;
  uint32_t mask = 0;  // BoolProperty bit
};

// FName from a string the way UE3 splits it: "MISSION_HIGHESTCOMPLETION_1" = entry "MISSION_HIGHESTCOMPLETION"
// with Number 2 (suffix + 1); suffixes with a leading zero ("SM01") are part of the entry. False if the entry
// does not exist in GNames.
bool SplitName(const char* s, ue3::FNameRaw& out) {
  std::string str = s;
  size_t us = str.rfind('_');
  out = {-1, 0};
  if (us != std::string::npos && us + 1 < str.size() && str[us + 1] != '0' && str.size() - us - 1 <= 9 &&
      str.find_first_not_of("0123456789", us + 1) == std::string::npos) {
    out.index = ue3::FindNameIndex(str.substr(0, us).c_str());
    out.number = atoi(str.c_str() + us + 1) + 1;
    if (out.index > 0) return true;
  }
  out = {ue3::FindNameIndex(s), 0};
  return out.index > 0;
}

// Property object by name along a struct/class chain.
uintptr_t FindField(uintptr_t structObj, const char* name) {
  ue3::FNameRaw n{};
  if (!SplitName(name, n)) return 0;
  for (uintptr_t s = structObj; s; s = ue3::StructSuper(s)) {
    int guard = 0;
    for (uintptr_t f = ue3::StructChildren(s); f && guard < 10000; f = ue3::FieldNext(f), ++guard)
      if (ue3::ObjNameIndex(f) == n.index && ReadOr<int32_t>(f + ue3::L().objName + 4) == n.number) return f;
  }
  return 0;
}

PropRef FindProp(uintptr_t structObj, const char* name) {
  PropRef r;
  uintptr_t f = FindField(structObj, name);
  if (!f) return r;
  r.offset = ue3::PropOffset(f);
  if (ue3::ClassNameOf(f) == "BoolProperty") r.mask = ue3::PropExtraU32(f);
  return r;
}

// TArray property: offset in the owner, element stride, element struct (for StructProperty elements).
struct ArrayRef {
  int offset = -1;
  int stride = 0;
  uintptr_t elemStruct = 0;
  bool Valid() const { return offset >= 0 && stride > 0; }
};

ArrayRef FindArray(uintptr_t structObj, const char* name) {
  ArrayRef r;
  uintptr_t f = FindField(structObj, name);
  if (!f || ue3::ClassNameOf(f) != "ArrayProperty") return r;
  uintptr_t inner = ue3::PropExtra(f);
  if (!ue3::IsObject(inner)) return r;
  r.offset = ue3::PropOffset(f);
  r.stride = ue3::PropElementSize(inner);
  if (ue3::ClassNameOf(inner) == "StructProperty") r.elemStruct = ue3::PropExtra(inner);
  return r;
}

// Elements of a TArray property of obj (data pointer + count), empty if absent.
ue3::TArrayRaw ReadArray(uintptr_t obj, const ArrayRef& a) {
  ue3::TArrayRaw t{};
  if (!a.Valid() || !util::Read(obj + a.offset, t) || t.num < 0 || t.num > 100000 || !t.data) t = {};
  return t;
}

std::wstring ReadFString(uintptr_t addr) {
  ue3::TArrayRaw s{};
  if (!util::Read(addr, s) || !s.data || s.num <= 0 || s.num > 4096) return {};
  return util::ReadWString(s.data, s.num);
}

PropRef ObjProp(uintptr_t obj, const char* name) { return FindProp(ue3::ObjClass(obj), name); }

uintptr_t ReadObjProp(uintptr_t obj, const char* name) {
  PropRef p = ObjProp(obj, name);
  if (p.offset < 0) return 0;
  uintptr_t v = ReadOr<uintptr_t>(obj + p.offset);
  return ue3::IsObject(v) ? v : 0;
}

bool IsDefault(uintptr_t o) { return ue3::ObjName(o).rfind("Default__", 0) == 0; }

std::string TopPackage(uintptr_t o) {
  uintptr_t top = o;
  for (int d = 0; d < 32; ++d) {
    uintptr_t outer = ue3::ObjOuter(top);
    if (!outer) break;
    top = outer;
  }
  return ue3::ObjName(top);
}

uintptr_t Func(const char* cls, const char* name) { return ue3::FindObject("Function", name, cls); }

uintptr_t DefaultObject(const char* cls) {
  return ue3::FindObject(cls, (std::string("Default__") + cls).c_str());
}

// StatId values live in the defaults of DVPickupStats / DVStats (StatId = struct { FName StatName; }).
// index >= 0: element of a TArray<StatId> property.
bool StatId(const char* cls, const char* prop, ue3::FNameRaw& out, int index = -1) {
  uintptr_t cdo = DefaultObject(cls);
  if (!cdo) return false;
  if (index >= 0) {
    ue3::TArrayRaw a = ReadArray(cdo, FindArray(ue3::ObjClass(cdo), prop));
    return index < a.num && util::Read(a.data + index * 8, out) && out.index > 0;
  }
  PropRef p = ObjProp(cdo, prop);
  return p.offset >= 0 && util::Read(cdo + p.offset, out) && out.index > 0;
}

// Stat named directly (lost souls, secret mission doors...). The name must already exist in GNames: the
// game creates these names when it loads its config / stat tables.
bool NamedStat(const char* name, ue3::FNameRaw& out) { return SplitName(name, out); }

// Static natives are called with a live object as context (see engine::Call); the profile is always alive.
float StatFloat(uintptr_t ctx, const ue3::FNameRaw& id) {
  engine::Call c(ctx, Func("NTGameStatsManager", "FloatAdder_Get"));
  c.Raw(c.ParamName(0).c_str(), &id, 8);
  return c.Invoke() ? c.RetFloat() : -1.0f;
}
bool StatBool(uintptr_t ctx, const ue3::FNameRaw& id) {
  engine::Call c(ctx, Func("NTGameStatsManager", "Bool_Get"));
  c.Raw(c.ParamName(0).c_str(), &id, 8);
  return c.Invoke() && c.RetBool();
}
int StatCounter(uintptr_t ctx, const ue3::FNameRaw& id) {
  engine::Call c(ctx, Func("NTGameStatsManager", "Counter_Get"));
  c.Raw(c.ParamName(0).c_str(), &id, 8);
  return c.Invoke() ? c.RetInt() : -1;
}
// Bool_Set if not set yet; true = changed.
bool StatSetBool(uintptr_t ctx, const ue3::FNameRaw& id) {
  if (StatBool(ctx, id)) return false;
  engine::Call c(ctx, Func("NTGameStatsManager", "Bool_Set"));
  c.Raw(c.ParamName(0).c_str(), &id, 8);
  return c.Invoke();
}
// Counter_Set(value) only when the counter is lower: what the player already has is never lowered.
bool StatCounterAtLeast(uintptr_t ctx, const ue3::FNameRaw& id, int value) {
  if (StatCounter(ctx, id) >= value) return false;
  engine::Call c(ctx, Func("NTGameStatsManager", "Counter_Set"));
  c.Raw(c.ParamName(0).c_str(), &id, 8).Int(c.ParamName(1).c_str(), value);
  return c.Invoke();
}
float StatFloatGet(uintptr_t ctx, const char* fn, const ue3::FNameRaw& id) {
  engine::Call c(ctx, Func("NTGameStatsManager", fn));
  c.Raw(c.ParamName(0).c_str(), &id, 8);
  return c.Invoke() ? c.RetFloat() : -1.0f;
}
void StatFloatSet(uintptr_t ctx, const char* fn, const ue3::FNameRaw& id, float v) {
  engine::Call c(ctx, Func("NTGameStatsManager", fn));
  c.Raw(c.ParamName(0).c_str(), &id, 8).Float(c.ParamName(1).c_str(), v);
  c.Invoke();
}

// ------------------------------------------------------------------ NTStripUnlockingHacks bypass
// The retail build compiles ANTWorldInfo::NTStripUnlockingHacks() to "return 1", which turns the developer
// unlock functions of DVGamerProfile into no-ops. Only inside our game-thread task, UFunction::Func of that
// function is swapped for a thunk that runs the original (keeps the script stack in sync) and returns 0.
using Exec_t = void(__thiscall*)(void* self, void* stack, void* result);
Exec_t g_origStrip = nullptr;

void __fastcall StripFalse(void* self, void* /*edx*/, void* stack, void* result) {
  g_origStrip(self, stack, result);
  if (result) *reinterpret_cast<uint32_t*>(result) = 0;
}

struct StripBypass {
  uintptr_t slot = 0, saved = 0;
  StripBypass() {
    uintptr_t fn = Func("NTWorldInfo", "NTStripUnlockingHacks");
    if (!fn || ue3::L().funcFunc < 0) return;
    slot = fn + ue3::L().funcFunc;
    saved = ReadOr<uintptr_t>(slot);
    if (!saved) {
      slot = 0;
      return;
    }
    g_origStrip = reinterpret_cast<Exec_t>(saved);
    *reinterpret_cast<uintptr_t*>(slot) = reinterpret_cast<uintptr_t>(&StripFalse);
  }
  ~StripBypass() {
    if (slot) *reinterpret_cast<uintptr_t*>(slot) = saved;
  }
  bool Active() const { return slot != 0; }
};

// ------------------------------------------------------------------ steps (game thread)
int CountCompleted(uintptr_t profile, int* total) {
  *total = 0;
  PropRef arr = ObjProp(profile, "m_aMissionStatus_Difficulty");
  uintptr_t progStruct = ue3::FindObject("ScriptStruct", "MissionProgression", "DVGamerProfile");
  uintptr_t arrStruct = ue3::FindObject("ScriptStruct", "MissionProgressionArray", "DVGamerProfile");
  if (arr.offset < 0 || !progStruct || !arrStruct) return -1;
  PropRef inner = FindProp(arrStruct, "m_Progression");
  PropRef done = FindProp(progStruct, "m_bCompleted");
  int outerSize = ue3::StructSize(arrStruct), progSize = ue3::StructSize(progStruct);
  ue3::TArrayRaw diffs{};
  util::Read(profile + arr.offset, diffs);
  int count = 0;
  for (int d = 0; d < diffs.num && d < 16; ++d) {
    ue3::TArrayRaw prog{};
    util::Read(diffs.data + d * outerSize + inner.offset, prog);
    for (int m = 0; m < prog.num && m < 64; ++m) {
      ++*total;
      uint32_t v = ReadOr<uint32_t>(prog.data + m * progSize + done.offset);
      count += (v & (done.mask ? done.mask : 1)) != 0;
    }
  }
  return count;
}

void SetBoolProp(uintptr_t obj, const char* name, bool value) {
  PropRef p = ObjProp(obj, name);
  if (p.offset < 0) return;
  uint32_t* d = reinterpret_cast<uint32_t*>(obj + p.offset);
  uint32_t m = p.mask ? p.mask : 1;
  *d = value ? (*d | m) : (*d & ~m);
}

bool CallSimple(uintptr_t obj, const char* fn) {
  engine::Call c(obj, fn);
  bool ok = c.Invoke();
  if (!ok) Rep("  %s: FAIL (%s)", fn, c.Error().c_str());
  return ok;
}

bool StepMissions(const Refs& r) {
  int total = 0, before = CountCompleted(r.profile, &total);
  CallSimple(r.profile, "UnlockAllMissions");
  int after = CountCompleted(r.profile, &total);
  SetBoolProp(r.profile, "m_bIsNewGame", false);  // as DVExecFunctions.UnlockAllMissions does
  bool ok = total > 0 && after == total;
  Rep("%s Missions: completed %d -> %d of %d (all difficulties)", ok ? "OK  " : "FAIL", before, after, total);
  return ok;
}

// ------------------------------------------------------------------ mission select config (DefaultUI.ini)
// DVGFxContainer_MissionSelectScreen.DefaultCompletionList: per mission the Mundus keys, secret mission doors
// and lost souls that the game counts for the mission's completion % (GetCompletionValues);
// DefaultSecretMissionList: the 21 secret missions (SecretMissionID = their stat, e.g. "SM01").
struct Pickup {
  ue3::FNameRaw name{};
  int type = 0;  // eMundusKeyType: Bronze, Silver, Gold, Ivory
};
struct MissionCollectibles {
  std::vector<Pickup> keys, doors;
  std::vector<ue3::FNameRaw> souls;
};

std::vector<MissionCollectibles> CompletionList() {
  std::vector<MissionCollectibles> out;
  uintptr_t cdo = DefaultObject("DVGFxContainer_MissionSelectScreen");
  if (!cdo) return out;
  ArrayRef list = FindArray(ue3::ObjClass(cdo), "DefaultCompletionList");
  if (!list.elemStruct) return out;
  ArrayRef keys = FindArray(list.elemStruct, "KeyNames"), doors = FindArray(list.elemStruct, "DoorNames");
  ArrayRef souls = FindArray(list.elemStruct, "SoulNames");
  if (!keys.elemStruct || !souls.Valid()) return out;
  PropRef pName = FindProp(keys.elemStruct, "PickupName"), pType = FindProp(keys.elemStruct, "PickupType");
  auto pairs = [&](uintptr_t e, const ArrayRef& a, std::vector<Pickup>& dst) {
    ue3::TArrayRaw t = ReadArray(e, a);
    for (int j = 0; j < t.num && j < 64; ++j) {
      Pickup p;
      util::Read(t.data + j * a.stride + pName.offset, p.name);
      p.type = ReadOr<uint8_t>(t.data + j * a.stride + pType.offset) & 3;
      dst.push_back(p);
    }
  };
  ue3::TArrayRaw l = ReadArray(cdo, list);
  for (int i = 0; i < l.num && i < 64; ++i) {
    uintptr_t e = l.data + i * list.stride;
    MissionCollectibles mc;
    pairs(e, keys, mc.keys);
    pairs(e, doors, mc.doors);
    ue3::TArrayRaw s = ReadArray(e, souls);
    for (int j = 0; j < s.num && j < 64; ++j) mc.souls.push_back(ReadOr<ue3::FNameRaw>(s.data + j * souls.stride));
    out.push_back(mc);
  }
  return out;
}

std::vector<std::string> SecretMissionIds() {
  std::vector<std::string> out;
  uintptr_t cdo = DefaultObject("DVGFxContainer_MissionSelectScreen");
  if (!cdo) return out;
  ArrayRef list = FindArray(ue3::ObjClass(cdo), "DefaultSecretMissionList");
  if (!list.elemStruct) return out;
  PropRef id = FindProp(list.elemStruct, "SecretMissionID");
  ue3::TArrayRaw l = ReadArray(cdo, list);
  for (int i = 0; i < l.num && i < 64; ++i)
    out.push_back(util::WideToUtf8(ReadFString(l.data + i * list.stride + id.offset)));
  return out;
}

// Raises the per key type counters (stats screen) of a TArray<StatId> property of DVStats and its total.
void RaiseTypedCounters(uintptr_t ctx, const char* arrayProp, const char* totalProp, const int perType[4]) {
  ue3::FNameRaw s{};
  int total = 0;
  for (int t = 0; t < 4; ++t) {
    total += perType[t];
    if (StatId("DVStats", arrayProp, s, t)) StatCounterAtLeast(ctx, s, perType[t]);
  }
  if (totalProp && StatId("DVStats", totalProp, s)) StatCounterAtLeast(ctx, s, total);
}

// Secret missions unlocked AND completed, as DVSecretMissionManager.ExitSecretMissionArea records a win:
// door stat SMxx = best rank (1 = door opened, 2..8 = won with D..SSS) and m_aSecretMissionStatus[i].m_bCompleted.
// A won mission gets the lowest winning rank (2) only if it has none: ranks are the player's results, the
// separate "Set all ranks SSS" raises them. The reward pickups (Pickup_SecretMissionNN) are marked as taken:
// the HP/DT steps give their effect already.
bool StepSecret(const Refs& r) {
  bool ok = CallSimple(r.profile, "UnlockAllSecretMissions");
  std::vector<std::string> ids = SecretMissionIds();
  ArrayRef status = FindArray(ue3::ObjClass(r.profile), "m_aSecretMissionStatus");
  PropRef done = status.elemStruct ? FindProp(status.elemStruct, "m_bCompleted") : PropRef{};
  ue3::TArrayRaw st = ReadArray(r.profile, status);
  int n = (int)ids.size(), unlocked = 0, completed = 0, rewards = 0;
  for (int i = 0; i < n; ++i) {
    ue3::FNameRaw id{};
    if (!NamedStat(ids[i].c_str(), id)) continue;
    StatCounterAtLeast(r.profile, id, 2);
    unlocked += StatCounter(r.profile, id) >= 2;
    if (i < st.num && done.offset >= 0) {
      uint32_t* d = reinterpret_cast<uint32_t*>(st.data + i * status.stride + done.offset);
      *d |= done.mask ? done.mask : 1;
      completed += (*d & (done.mask ? done.mask : 1)) != 0;
    }
    char pk[48];
    snprintf(pk, sizeof pk, "Pickup_SecretMission%02d", i + 1);
    ue3::FNameRaw reward{};
    if (NamedStat(pk, reward)) {
      StatSetBool(r.profile, reward);
      rewards += StatBool(r.profile, reward);
    }
  }
  int perType[4] = {};
  for (const MissionCollectibles& mc : CompletionList())
    for (const Pickup& d : mc.doors) ++perType[d.type];
  RaiseTypedCounters(r.profile, "SECRETMISSIONS_UNLOCKED", "SECRETMISSIONS_UNLOCKED_TOTAL", perType);
  RaiseTypedCounters(r.profile, "SECRETMISSIONS_COMPLETED", "SECRETMISSIONS_COMPLETED_TOTAL", perType);
  RaiseTypedCounters(r.profile, "COLLECTIBLES_MUNDUSDOORS", "COLLECTIBLES_MUNDUSDOORS_TOTAL", perType);
  ok = ok && n > 0 && unlocked == n && completed == n;
  Rep("%s Secret missions: %d found, won %d, completed flag %d, rewards taken %d (door stats %d/%d/%d/%d)",
      ok ? "OK  " : "FAIL", n, unlocked, completed, rewards, perType[0], perType[1], perType[2], perType[3]);
  return ok;
}

bool StepKeys(const Refs& r) {
  bool ok = CallSimple(r.profile, "GiveAllMundusKeys");
  int perType[4] = {};
  for (const MissionCollectibles& mc : CompletionList())
    for (const Pickup& k : mc.keys) ++perType[k.type];
  RaiseTypedCounters(r.profile, "COLLECTIBLES_MUNDUSKEYS", "COLLECTIBLES_MUNDUSKEYS_TOTAL", perType);
  Rep("%s Mundus keys: GiveAllMundusKeys() (key pickups + MUNDUSKEY counters), found %d/%d/%d/%d",
      ok ? "OK  " : "FAIL", perType[0], perType[1], perType[2], perType[3]);
  return ok;
}

// Story weapons & abilities (EDVUpgradeItem) are what the game stores in DVGamerProfile.m_UnlockedAbilities:
// RestartPlayer gives them to Dante on every spawn, the shop shows skills of missing ones as UNKNOWN/locked.
// The set is the one of a 100% save: Angel/Demon mode, Glide + boosts, Angel Lift (+to pawn, whip extend),
// Demon Pull (+to pawn, whip extend), Ebony & Ivory, Revenant, Kablooey, Osiris, Arbiter, Aquila, Eryx, Devil
// Trigger. Not given: Lift/Pull skill X/A (unused), Doppelganger, Trick Up/Down, swords (Vergil's DLC).
// Done as the game does it: DVGameInfo.UnlockUpgrade(true, item) on the live upgrade system, then the
// profile copies the list (DVGamerProfile.SaveUpgrades only adds missing ids).
const int kStoryItems[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 15, 16, 17, 18, 19, 20, 21, 22};

int CountAbilities(uintptr_t profile) {
  ue3::TArrayRaw a = ReadArray(profile, FindArray(ue3::ObjClass(profile), "m_UnlockedAbilities"));
  int have = 0;
  for (int id : kStoryItems)
    for (int i = 0; i < a.num && i < 256; ++i)
      if (ReadOr<int32_t>(a.data + i * 4) == id) {
        ++have;
        break;
      }
  return have;
}

bool StepStory(const Refs& r) {
  const int total = (int)(sizeof kStoryItems / sizeof kStoryItems[0]);
  if (!r.game) {
    Rep("FAIL Story weapons & abilities: no live DVGameInfo");
    return false;
  }
  int before = CountAbilities(r.profile);
  for (int id : kStoryItems) {
    engine::Call c(r.game, "UnlockUpgrade");
    c.Bool(c.ParamName(0).c_str(), true).Byte(c.ParamName(1).c_str(), (uint8_t)id).Bool(c.ParamName(2).c_str(), false);
    if (!c.Invoke()) Rep("  UnlockUpgrade(%d): FAIL %s", id, c.Error().c_str());
  }
  CallSimple(r.profile, "SaveUpgrades");
  int after = CountAbilities(r.profile);
  // "Item acquired" screens already seen (DVItemAcquireSystem.m_ItemActivatedStat), as after a full game.
  int seen = 0;
  uintptr_t acq = ReadObjProp(r.game, "m_ItemAcquireSystem");
  if (acq) {
    ArrayRef stats = FindArray(ue3::ObjClass(acq), "m_ItemActivatedStat");
    ue3::TArrayRaw t = ReadArray(acq, stats);
    for (int i = 0; i < t.num && i < 64; ++i) {
      ue3::FNameRaw id = ReadOr<ue3::FNameRaw>(t.data + i * stats.stride);
      if (id.index <= 0) continue;
      StatSetBool(r.profile, id);
      seen += StatBool(r.profile, id);
    }
  }
  bool ok = after == total;
  Rep("%s Story weapons & abilities: %d -> %d of %d (m_UnlockedAbilities), item screens seen %d", ok ? "OK  " : "FAIL",
      before, after, total, seen);
  return ok;
}

// Lost souls of every mission (stat bools named in DefaultCompletionList) + their counters, then the game's
// own UpdateCompletionPercentage for all 20 missions (keys + doors + souls -> MISSION_HIGHESTCOMPLETION_N,
// CheckAll100PercentCompletion). Needs the keys and secret mission steps first for 100%.
bool StepCollectibles(const Refs& r) {
  std::vector<MissionCollectibles> list = CompletionList();
  int souls = 0, have = 0;
  for (const MissionCollectibles& mc : list)
    for (const ue3::FNameRaw& s : mc.souls) {
      ++souls;
      StatSetBool(r.profile, s);
      have += StatBool(r.profile, s);
    }
  ue3::FNameRaw stat{};
  if (StatId("DVPickupStats", "LOSTSOUL", stat)) StatCounterAtLeast(r.profile, stat, have);
  if (StatId("DVStats", "COLLECTIBLES_LOSTSOULS", stat)) StatCounterAtLeast(r.profile, stat, have);
  int full = 0, missions = (int)list.size();
  for (int m = 0; m < missions; ++m) {
    engine::Call c(r.profile, "UpdateCompletionPercentage");
    c.Int(c.ParamName(0).c_str(), m);
    c.Invoke();
    char name[48];
    snprintf(name, sizeof name, "MISSION_HIGHESTCOMPLETION_%d", m + 1);
    if (StatId("DVStats", name, stat)) full += StatCounter(r.profile, stat) >= 100;
  }
  bool ok = missions > 0 && souls > 0 && have == souls && full == missions;
  Rep("%s Lost Souls %d/%d, missions at 100%% completion %d/%d", ok ? "OK  " : "FAIL", have, souls, full, missions);
  return ok;
}

// What a finished game leaves in the profile besides the missions: "game complete" screens seen for every
// difficulty, tutorials seen, not a new game, OTHER_GAMECOMPLETED, and the XP level cap with its upgrade
// points (DVGamerProfile.AddUpgradePointFromXP per rank of DVGameplayParameters_Player.m_UpgradeRanks).
bool StepCompletion(const Refs& r) {
  const char* flags[] = {"m_bCompleteGameSeen_DMC", "m_bCompleteGameSeen_SOS", "m_bCompleteGameSeen_DMD",
                         "m_bCompleteGameSeen_HOH", "m_bCompleteGameSeen_HAH", "m_bStyleSystemTutorialSeen",
                         "m_bDevilTriggerTutorialSeen", "m_bUpgradeHUDTutorialSeen"};
  for (const char* f : flags) SetBoolProp(r.profile, f, true);
  SetBoolProp(r.profile, "m_bIsNewGame", false);
  ue3::FNameRaw stat{};
  if (StatId("DVStats", "OTHER_GAMECOMPLETED", stat)) StatCounterAtLeast(r.profile, stat, 1);

  PropRef xp = ObjProp(r.profile, "m_iCurrentXP"), total = ObjProp(r.profile, "m_iUpgradePointsTotal");
  PropRef fromXp = ObjProp(r.profile, "m_iUpgradePointsFromXP"), pct = ObjProp(r.profile, "m_fPercentageThroughUpgrade");
  uintptr_t params = DefaultObject("DVGameplayParameters_Player");
  ArrayRef ranks = params ? FindArray(ue3::ObjClass(params), "m_UpgradeRanks") : ArrayRef{};
  ue3::TArrayRaw rk = params ? ReadArray(params, ranks) : ue3::TArrayRaw{};
  bool xpOk = false;
  int points = -1;
  if (rk.num > 0 && xp.offset >= 0 && total.offset >= 0 && fromXp.offset >= 0) {
    auto i32 = [&](const PropRef& p) { return reinterpret_cast<int32_t*>(r.profile + p.offset); };
    int cap = ReadOr<int32_t>(rk.data + (rk.num - 1) * ranks.stride);
    if (*i32(xp) < cap) *i32(xp) = cap;
    for (int guard = 0; guard < 512 && *i32(total) < rk.num && *i32(fromXp) < rk.num; ++guard) {
      if (*i32(xp) < ReadOr<int32_t>(rk.data + *i32(fromXp) * ranks.stride)) break;
      if (!CallSimple(r.profile, "AddUpgradePointFromXP")) break;
    }
    if (pct.offset >= 0) *reinterpret_cast<float*>(r.profile + pct.offset) = 100.0f;
    points = *i32(total);
    xpOk = points >= rk.num;
  }
  // The rank table lives in Dante's gameplay archetype, which the main menu does not load (only its empty
  // class defaults): then the XP is left as it is - it does not lock anything, all skills are unlocked anyway.
  if (rk.num > 0)
    Rep("%s Completion: game-complete screens x5, tutorials seen; XP cap %s (upgrade points %d of %d ranks)",
        xpOk ? "OK  " : "FAIL", xpOk ? "reached" : "NOT set", points, rk.num);
  else
    Rep("OK   Completion: game-complete screens x5, tutorials seen (XP left as is: rank table not loaded in the menu)");
  return rk.num == 0 || xpOk;
}

// Concept art (Extras): every CONCEPTART_STAT entry of the live DVUnlocksParams ("Unlocks.Unlocks") unlocks when
// its stat reaches m_iValueRequired. As the developers' DVExecFunctions.ConceptArtCheat: raise the stat to the
// required value, never lower it. Except the play time (GENERAL_TOTALGAMETIME, shown as "Time played" on the save
// slot): the user's decision is to keep the real time, so the one art that needs 50 hours stays locked.
bool StepConceptArt(const Refs& r) {
  uintptr_t unlocks = 0;
  for (int i = 0, n = ue3::NumObjects(); i < n && !unlocks; ++i) {
    uintptr_t o = ue3::Object(i);
    if (o && ue3::ClassNameOf(o) == "DVUnlocksParams" && !IsDefault(o)) unlocks = o;
  }
  if (!unlocks) {
    Rep("FAIL Concept art: DVUnlocksParams not loaded");
    return false;
  }
  ArrayRef cats = FindArray(ue3::ObjClass(unlocks), "m_aConceptArtCategoriesData");
  ArrayRef arts = cats.elemStruct ? FindArray(cats.elemStruct, "m_aConceptArtData") : ArrayRef{};
  if (!arts.elemStruct) {
    Rep("FAIL Concept art: unknown DVUnlocksParams layout");
    return false;
  }
  uintptr_t st = arts.elemStruct;
  PropRef type = FindProp(st, "m_ConceptArtType"), statId = FindProp(st, "m_nStatID");
  PropRef req = FindProp(st, "m_iValueRequired"), isTimer = FindProp(st, "m_bStatIsTimer");
  PropRef isApp = FindProp(st, "m_bStatIsAppTimer"), isFloat = FindProp(st, "m_bStatIsFloat");
  ue3::FNameRaw playTime{};
  bool havePlayTime = StatId("DVStats", "GENERAL_TOTALGAMETIME", playTime);
  int entries = 0, met = 0, raised = 0, kept = 0;
  ue3::TArrayRaw c = ReadArray(unlocks, cats);
  for (int i = 0; i < c.num && i < 64; ++i) {
    ue3::TArrayRaw a = ReadArray(c.data + i * cats.stride, arts);
    for (int j = 0; j < a.num && j < 512; ++j) {
      uintptr_t e = a.data + j * arts.stride;
      if (ReadOr<uint8_t>(e + type.offset) != 1) continue;  // CONCEPTART_STAT
      ++entries;
      ue3::FNameRaw id = ReadOr<ue3::FNameRaw>(e + statId.offset);
      int need = ReadOr<int32_t>(e + req.offset);
      uint32_t bits = ReadOr<uint32_t>(e + isTimer.offset);
      const char* get = (bits & isTimer.mask) ? "Timer_Get" : (bits & isApp.mask) ? "AppTimer_Get"
                        : (bits & isFloat.mask) ? "FloatAdder_Get" : nullptr;
      int value = get ? (int)StatFloatGet(r.profile, get, id) : StatCounter(r.profile, id);
      bool isPlayTime = (havePlayTime && id.index == playTime.index && id.number == playTime.number) ||
                        ue3::NameOf(id) == "GENERAL_TOTALGAMETIME";
      if (value < need && isPlayTime) {
        ++kept;
        continue;
      }
      if (value < need) {
        if (get) {
          std::string set = get;
          set.replace(set.size() - 3, 3, "Set");
          StatFloatSet(r.profile, set.c_str(), id, (float)need);
        } else {
          StatCounterAtLeast(r.profile, id, need);
        }
        ++raised;
        LOG("unlock: concept art stat %s %d -> %d", ue3::NameOf(id).c_str(), value, need);
        value = get ? (int)StatFloatGet(r.profile, get, id) : StatCounter(r.profile, id);
      }
      met += value >= need;
    }
  }
  bool ok = entries > 0 && met + kept == entries;
  Rep("%s Concept art: %d/%d unlocked (%d stat(s) raised, %d left locked: play time is not changed)", ok ? "OK  " : "FAIL",
      met, entries, raised, kept);
  return ok;
}

// "Set all ranks SSS" (separate button, never part of Unlock All): every completed mission on every difficulty
// gets SSS in all four rank categories (MissionProgression rank 6 = SSS) and 100% completion; every secret
// mission gets the best rank 8 (SSS) and the completed flag. Scores and times are left as they are.
bool StepRanksSSS(const Refs& r) {
  PropRef arr = ObjProp(r.profile, "m_aMissionStatus_Difficulty");
  uintptr_t progStruct = ue3::FindObject("ScriptStruct", "MissionProgression", "DVGamerProfile");
  uintptr_t arrStruct = ue3::FindObject("ScriptStruct", "MissionProgressionArray", "DVGamerProfile");
  if (arr.offset < 0 || !progStruct || !arrStruct) {
    Rep("FAIL Ranks SSS: mission progression layout not found");
    return false;
  }
  PropRef inner = FindProp(arrStruct, "m_Progression"), done = FindProp(progStruct, "m_bCompleted");
  PropRef comp = FindProp(progStruct, "m_iCompletion");
  const char* rankFields[] = {"m_iMissionRank", "m_iStyleRank", "m_iTimeRank", "m_iCompletionRank"};
  int outerSize = ue3::StructSize(arrStruct), progSize = ue3::StructSize(progStruct), set = 0;
  ue3::TArrayRaw diffs{};
  util::Read(r.profile + arr.offset, diffs);
  for (int d = 0; d < diffs.num && d < 16; ++d) {
    ue3::TArrayRaw prog{};
    util::Read(diffs.data + d * outerSize + inner.offset, prog);
    for (int m = 0; m < prog.num && m < 64; ++m) {
      uintptr_t e = prog.data + m * progSize;
      if (!(ReadOr<uint32_t>(e + done.offset) & (done.mask ? done.mask : 1))) continue;
      for (const char* f : rankFields) {
        PropRef p = FindProp(progStruct, f);
        if (p.offset >= 0) *reinterpret_cast<int32_t*>(e + p.offset) = 6;
      }
      if (comp.offset >= 0 && ReadOr<int32_t>(e + comp.offset) < 100) *reinterpret_cast<int32_t*>(e + comp.offset) = 100;
      ++set;
    }
  }
  int secret = 0;
  std::vector<std::string> ids = SecretMissionIds();
  ArrayRef status = FindArray(ue3::ObjClass(r.profile), "m_aSecretMissionStatus");
  PropRef sdone = status.elemStruct ? FindProp(status.elemStruct, "m_bCompleted") : PropRef{};
  ue3::TArrayRaw st = ReadArray(r.profile, status);
  for (size_t i = 0; i < ids.size(); ++i) {
    ue3::FNameRaw id{};
    if (!NamedStat(ids[i].c_str(), id)) continue;
    StatCounterAtLeast(r.profile, id, 8);
    if ((int)i < st.num && sdone.offset >= 0)
      *reinterpret_cast<uint32_t*>(st.data + i * status.stride + sdone.offset) |= sdone.mask ? sdone.mask : 1;
    secret += StatCounter(r.profile, id) >= 8;
  }
  bool ok = set > 0 && secret == (int)ids.size();
  Rep("%s Ranks SSS: %d completed mission(s) set to SSS, secret missions SSS %d/%zu", ok ? "OK  " : "FAIL", set,
      secret, ids.size());
  return ok;
}

bool StepUpgrades(const Refs& r) {
  if (!r.upgrades) {
    Rep("FAIL Upgrades: profile.m_UpgradesUnlocked not found");
    return false;
  }
  PropRef set = ObjProp(r.upgrades, "m_UnlockedUpgrades");  // TSet: first member is the elements TArray
  auto count = [&] { return set.offset >= 0 ? ReadOr<int32_t>(r.upgrades + set.offset + 4) : -1; };
  int before = count();
  bool ok = CallSimple(r.upgrades, "UnlockAllUpgrades");
  int after = count();
  engine::Call has(r.upgrades, "HasUpgrade");
  has.Name(has.ParamName(0).c_str(), "Dante_Rebellion_1000Stabs_u1");
  bool hasStabs = has.Invoke() && has.RetBool();
  ok = ok && hasStabs;
  Rep("%s Upgrades/skills: unlocked %d -> %d, HasUpgrade(Dante_Rebellion_1000Stabs_u1)=%d", ok ? "OK  " : "FAIL",
      before, after, (int)hasStabs);
  return ok;
}

// HP / DT: buy the shop items for free (Cheat=true, as the developers' GiveHealthUpgrade does) until the shop
// is sold out, then add the rest up to the game's own cap (the orbs assembled from fragments in the world).
bool StepShop(const Refs& r, bool health) {
  const char* what = health ? "HP ups" : "DT ups";
  if (!r.shop) {
    Rep("FAIL %s: no live DVShopComponent (DVGameInfo.m_ShopSystem)", what);
    return false;
  }
  ue3::FNameRaw stat{};
  bool haveStat = StatId("DVPickupStats", health ? "MAXHEALTH_PLAYER" : "MAXTRIGGERPOINTS_PLAYER", stat);
  float before = haveStat ? StatFloat(r.profile, stat) : -1;
  PropRef maxBuy = ObjProp(r.shop, health ? "BlueOrbMaxPurchase" : "PurpleOrbMaxPurchase");
  PropRef cap = ObjProp(r.shop, health ? "MaxHealthIncrease" : "MaxDevilTriggerIncrease");
  int maxPurchase = maxBuy.offset >= 0 ? ReadOr<int32_t>(r.shop + maxBuy.offset) : 0;
  float capValue = cap.offset >= 0 ? ReadOr<float>(r.shop + cap.offset) : 0;
  int bought = 0;
  for (int i = 0; i < 16; ++i) {
    engine::Call cnt(r.shop, health ? "GetBlueOrbShopCount" : "GetPurpleOrbShopCount");
    if (!cnt.Invoke() || cnt.RetInt() >= maxPurchase) break;
    engine::Call buy(r.shop, health ? "BuyMaxHealth" : "BuyMaxTriggerPoints");
    buy.Bool(buy.ParamName(0).c_str(), true);
    if (!buy.Invoke() || !buy.RetBool()) break;
    ++bought;
  }
  engine::Call ext(r.shop, health ? "ExtendPlayerMaxHealth" : "ExtendMaxTriggerPoints");
  ext.Float(ext.ParamName(0).c_str(), capValue);  // the function clamps to the cap itself
  bool extOk = ext.Invoke();
  float after = haveStat ? StatFloat(r.profile, stat) : -1;
  bool ok = extOk && haveStat && capValue > 0 && after >= capValue - 0.5f;
  Rep("%s %s: shop bought %d (max %d), %s %.0f -> %.0f (cap %.0f)", ok ? "OK  " : "FAIL", what, bought, maxPurchase,
      health ? "MAXHEALTH_PLAYER" : "MAXTRIGGERPOINTS_PLAYER", before, after, capValue);
  return ok;
}

bool ProfileHasDLCID(uintptr_t profile, const wchar_t* id) {
  PropRef p = ObjProp(profile, "m_UnlockedDLCIDs");
  ue3::TArrayRaw a{};
  if (p.offset < 0 || !util::Read(profile + p.offset, a)) return false;
  for (int i = 0; i < a.num && i < 256; ++i) {
    ue3::TArrayRaw s{};
    util::Read(a.data + i * 12, s);
    if (s.data && util::ReadWString(s.data, 128) == id) return true;
  }
  return false;
}

// Costumes / NG+ rewards exactly as DVGamerProfile.UpdateAchievement grants them on game completion (minus
// its Session_UnlockAchievement calls): Human -> "WhiteHair" tag + Costume_DanteBlackHair, Son of Sparda ->
// Costume_DanteNoCoat, Dante Must Die -> Perk_SuperDante. Paid DLC costumes (Costume_Pack1_*) are not touched.
// "WhiteHairPatch" (Dante's hair streak, a story tag of the default costume) is also in every finished save.
bool StepCostumes(const Refs& r) {
  for (const wchar_t* tag : {L"WhiteHairPatch", L"WhiteHair"}) {
    engine::Call c(r.profile, "AddCharacterTag");
    c.Str(c.ParamName(0).c_str(), tag);
    c.Invoke();
  }
  const wchar_t* ids[] = {L"Costume_DanteBlackHair", L"Costume_DanteNoCoat", L"Perk_SuperDante"};
  int have = 0;
  for (const wchar_t* id : ids) {
    if (!ProfileHasDLCID(r.profile, id)) {
      engine::Call c(r.profile, "UnlockCodeManifest");
      c.Str(c.ParamName(0).c_str(), id).Bool(c.ParamName(1).c_str(), false);
      if (!c.Invoke()) Rep("  UnlockCodeManifest(%s): FAIL %s", util::WideToUtf8(id).c_str(), c.Error().c_str());
    }
    have += ProfileHasDLCID(r.profile, id);
  }
  bool ok = have == 3;
  Rep("%s Costumes/NG+: %d/3 (BlackHair, NoCoat, Super Dante) + WhiteHair tag", ok ? "OK  " : "FAIL", have);
  return ok;
}

bool RunStep(Step s, const Refs& r) {
  switch (s) {
    case Missions: return StepMissions(r);
    case StoryItems: return StepStory(r);
    case SecretMissions: return StepSecret(r);
    case MundusKeys: return StepKeys(r);
    case Collectibles: return StepCollectibles(r);
    case Upgrades: return StepUpgrades(r);
    case HealthUps: return StepShop(r, true);
    case TriggerUps: return StepShop(r, false);
    case Costumes: return StepCostumes(r);
    case Completion: return StepCompletion(r);
    case ConceptArt: return StepConceptArt(r);
    case RanksSSS: return StepRanksSSS(r);
    default: return false;
  }
}

// ------------------------------------------------------------------ save
int SlotStatusOnGame(uintptr_t ctx) {
  engine::Call c(ctx, Func("DVSaveManager", "GetActiveSlotStatus"));
  return c.Invoke() ? c.RetByte() : -1;
}

bool SaveBusy(uintptr_t ctx) {
  engine::Call c(ctx, Func("DVSaveManager", "IsBusy"));
  return c.Invoke() && c.RetBool();
}

// NTGamerProfile.SaveProfile(bSaveConsumables, bSaveUpgrades, bSavingForCheckpoint, bSavingForLevelCompletion)
// -> WriteSaveSlotMetadata + CallSaveContent -> DVSaveManager.SaveContent (async). All flags false: in the
// front end the transient consumables and the GameInfo upgrade list are empty and would overwrite real data.
bool SaveOnGame(uintptr_t profile) {
  engine::Call c(profile, "SaveProfile");
  bool ok = c.Invoke();
  Rep("%s Save: %s", ok ? "OK  " : "FAIL", c.Describe().c_str());
  return ok;
}

}  // namespace

// ------------------------------------------------------------------ public
Refs Locate() {
  Refs r;
  if (!ue3::Init()) {
    r.error = "UE3 not ready: " + ue3::StatusText();
    return r;
  }
  int n = ue3::NumObjects();
  std::vector<uintptr_t> shops, games;
  for (int i = 0; i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o || IsDefault(o)) continue;
    std::string cn = ue3::ClassNameOf(o);
    if (!r.engine && cn == "DVGameEngine") r.engine = o;
    if (!r.pc && cn != "Class" && ue3::IsA(o, "PlayerController")) r.pc = o;
    if (cn == "DVShopComponent") shops.push_back(o);
    if (cn == "DVGameInfo") games.push_back(o);
  }
  if (r.engine) r.profile = ReadObjProp(r.engine, "GGamerProfileData");
  if (r.profile) r.upgrades = ReadObjProp(r.profile, "m_UpgradesUnlocked");
  if (r.pc) {
    r.cheat = ReadObjProp(r.pc, "CheatManager");
    r.map = TopPackage(r.pc);
  }
  for (uintptr_t s : shops)  // the shop of the current map (old maps may linger until GC)
    if (TopPackage(s) == r.map) r.shop = s;
  for (uintptr_t g : games)
    if (TopPackage(g) == r.map) r.game = g;
  if (!r.profile) r.error = "DVGamerProfile not found (GEngine.GGamerProfileData)";
  return r;
}

bool IsFrontEnd(const std::string& map) {
  std::string m = map;
  for (auto& c : m) c = (char)tolower((unsigned char)c);
  return m.rfind("mainmenu", 0) == 0;
}

const char* StepId(Step s) {
  static const char* ids[] = {"missions", "story",   "secret",   "keys",       "collectibles", "upgrades",
                              "hp",       "dt",      "costumes", "completion", "conceptart",   "ranks_sss"};
  return s <= RanksSSS ? ids[s] : "?";
}

const char* StepLabel(Step s) {
  static const char* labels[] = {"Missions (all difficulties)", "Story weapons & abilities", "Secret Missions",
                                 "Mundus Keys", "Lost Souls / 100% completion", "Upgrades / Skills",
                                 "HP ups (max)", "DT ups (max)", "Costumes / Super Dante",
                                 "Game completed / XP cap", "Concept art", "Set all ranks SSS"};
  return s <= RanksSSS ? labels[s] : "?";
}

int SlotStatus() {
  Refs r = Locate();
  if (!r.profile || !engine::Hooked()) return -1;
  uintptr_t ctx = r.profile;
  return OnGame<int>([ctx] { return SlotStatusOnGame(ctx); }, 3000);
}

void Run(std::vector<Step> steps, bool withSave) {
  if (slots::Busy()) return;
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) return;
  ClearReport();
  std::thread([steps, withSave] {
    auto finish = [] { g_busy = false; };
    Refs r = Locate();
    if (!r.error.empty() || !engine::Hooked()) {
      Rep("FAIL: %s", r.error.empty() ? "ProcessEvent hook not installed" : r.error.c_str());
      return finish();
    }
    if (!IsFrontEnd(r.map)) {
      Rep("FAIL: only in the main menu (current map: %s)", r.map.c_str());
      return finish();
    }
    if (!steam::InstallGuard()) {
      Rep("FAIL: Steam achievements guard is not installed - nothing changed");
      return finish();
    }
    uintptr_t profile = r.profile;
    int slot = OnGame<int>([profile] { return SlotStatusOnGame(profile); }, 3000);
    if (slot != 3 && slot != 4) {  // ESaveSlotStatus: SSS_LOADED / SSS_LOADEDNOTOWNER
      Rep("FAIL: save slot is not loaded (status %d) - open your save in the main menu first", slot);
      return finish();
    }
    std::string err;
    std::wstring backup = steam::BackupSaves("before_unlock", &err);
    if (backup.empty()) {
      Rep("FAIL: save backup failed (%s) - nothing changed", err.c_str());
      return finish();
    }
    Rep("backup: %s", util::WideToUtf8(backup).c_str());
    steam::BeginBlock();
    int blockedBefore = steam::BlockedCount();
    int okCount = OnGame<int>(
        [r, steps] {
          StripBypass bypass;
          if (!bypass.Active()) Rep("warning: NTStripUnlockingHacks bypass not active");
          int ok = 0;
          for (Step s : steps) ok += RunStep(s, r);
          return ok;
        },
        30000);
    bool saved = !withSave;
    if (withSave) {
      saved = OnGame<bool>([profile] { return SaveOnGame(profile); }, 10000);
      for (int i = 0; i < 60; ++i) {  // DVSaveManager.SaveContent is asynchronous
        Sleep(250);
        if (!OnGame<bool>([profile] { return SaveBusy(profile); }, 3000)) break;
      }
    }
    steam::EndBlockAfter(15000);
    Rep("done: %d/%zu step(s) ok%s; Steam calls blocked: %d", okCount, steps.size(),
        withSave ? (saved ? ", saved" : ", SAVE FAILED") : ", not saved", steam::BlockedCount() - blockedBefore);
    dumper::RunAsync(dumper::Kind::Profile);  // "after" snapshot
    finish();
  }).detach();
}

void RunFixture() {
  if (slots::Busy()) return;
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) return;
  ClearReport();
  std::thread([] {
    Refs r = Locate();
    if (!r.error.empty() || !IsFrontEnd(r.map) || !r.game || !r.shop || !r.upgrades || !steam::InstallGuard()) {
      Rep("FAIL fixture: main menu with a save loaded needed (%s)", r.error.c_str());
      g_busy = false;
      return;
    }
    steam::BeginBlock();
    OnGame<int>(
        [r] {
          StripBypass bypass;
          StepMissions(r);
          for (int id : {0, 1, 2, 5, 10, 15, 16, 18, 19}) {  // no Aquila, Eryx, Kablooey, Devil Trigger
            engine::Call c(r.game, "UnlockUpgrade");
            c.Bool(c.ParamName(0).c_str(), true).Byte(c.ParamName(1).c_str(), (uint8_t)id).Bool(c.ParamName(2).c_str(), false);
            c.Invoke();
          }
          CallSimple(r.profile, "SaveUpgrades");
          for (const char* tag :
               {"Dante_Abilities_Jump", "Dante_Abilities_DoubleJump", "Dante_Evade", "Dante_Birthrights_Lift",
                "Dante_Birthrights_Pull", "Dante_Rebellion_ComboA", "Dante_Rebellion_ComboB", "Dante_Rebellion_Stinger",
                "Dante_Rebellion_Stinger_u1", "Dante_Rebellion_Drive", "Dante_Scythe_ComboA", "Dante_Axe_ComboA",
                "Dante_EnI_Shoot", "Dante_Shotgun_Shoot"}) {
            engine::Call c(r.upgrades, "UnlockUpgrade");
            c.Name(c.ParamName(0).c_str(), tag);
            c.Invoke();
          }
          for (const char* fn : {"BuyMaxHealth", "BuyMaxTriggerPoints"}) {
            engine::Call c(r.shop, fn);
            c.Bool(c.ParamName(0).c_str(), true);
            c.Invoke();
          }
          const std::pair<const char*, int> items[] = {{"VitalStar_Small", 2}, {"VitalStar_Large", 1},
                                                       {"DevilTriggerStar_Small", 1}, {"Orb_Gold", 1},
                                                       {"Orb_Red", 23456}};
          for (auto& [name, v] : items) {
            ue3::FNameRaw id{};
            if (NamedStat(name, id)) StatCounterAtLeast(r.profile, id, v);
          }
          PropRef pts = ObjProp(r.profile, "m_iUnspentUpgradePoints");
          if (pts.offset >= 0) *reinterpret_cast<int32_t*>(r.profile + pts.offset) = 3;
          return 0;
        },
        30000);
    bool saved = OnGame<bool>([r] { return SaveOnGame(r.profile); }, 10000);
    for (int i = 0; i < 60; ++i) {
      Sleep(250);
      if (!OnGame<bool>([r] { return SaveBusy(r.profile); }, 3000)) break;
    }
    steam::EndBlockAfter(15000);
    Rep("fixture done, saved %d", (int)saved);
    dumper::RunAsync(dumper::Kind::Profile);
    g_busy = false;
  }).detach();
}

void RunFromScript(const std::string& what) {
  std::vector<Step> steps;
  if (what == "all") {
    for (int s = 0; s < StepCount; ++s) steps.push_back((Step)s);
  } else if (what != "save") {
    for (int s = 0; s <= RanksSSS; ++s)
      if (what == StepId((Step)s)) steps.push_back((Step)s);
    if (steps.empty()) {
      LOG("unlock: unknown step '%s'", what.c_str());
      return;
    }
  }
  Run(steps, true);
}

const char* StateText(State s) {
  switch (s) {
    case State::Initializing: return "UE3 initializing...";
    case State::NoHook: return "game hook not installed";
    case State::Loading: return "game loading...";
    case State::InMission: return "in mission";
    case State::NoSave: return "main menu, save not loaded";
    case State::Ready: return "ready";
  }
  return "?";
}

Status CurrentStatus() {
  static std::mutex mu;
  static Status cached;
  static ULONGLONG last = 0;
  static std::atomic<bool> refreshing{false};
  {
    std::lock_guard<std::mutex> lk(mu);
    // While a run is busy the game thread is taken by it: keep the last status.
    if (GetTickCount64() - last < 2000 || refreshing || g_busy) return cached;
    last = GetTickCount64();
    refreshing = true;
  }
  std::thread([] {
    Status s;
    if (!ue3::Ready() && !ue3::Init(true)) {
      s.state = State::Initializing;
      s.detail = ue3::StatusText();
    } else if (!engine::Hooked()) {
      s.state = State::NoHook;
      s.detail = "ProcessEvent hook not installed, see log";
    } else if (!steam::GuardInstalled()) {
      s.state = State::NoHook;
      s.detail = "Steam achievements guard not installed yet";
    } else {
      Refs r = Locate();
      s.map = r.map;
      if (r.map.empty() || !engine::GameThreadKnown()) {
        s.state = State::Loading;
        s.detail = r.map.empty() ? "no player controller yet" : "game thread not seen yet";
      } else if (!IsFrontEnd(r.map)) {
        s.state = State::InMission;
        s.detail = "map " + r.map;
      } else if (!r.profile) {
        s.state = State::NoSave;
        s.detail = r.error;
      } else {
        uintptr_t profile = r.profile;
        int slot = OnGame<int>([profile] { return SlotStatusOnGame(profile); }, 3000);
        s.state = (slot == 3 || slot == 4) ? State::Ready : State::NoSave;  // SSS_LOADED / SSS_LOADEDNOTOWNER
        if (s.state != State::Ready) s.detail = "save slot status " + std::to_string(slot) + ", choose your save";
      }
    }
    std::lock_guard<std::mutex> lk(mu);
    if (s.state != cached.state || s.map != cached.map)
      LOG("unlock: status -> %s%s%s", StateText(s.state), s.detail.empty() ? "" : " | ", s.detail.c_str());
    cached = s;
    refreshing = false;
  }).detach();
  std::lock_guard<std::mutex> lk(mu);
  return cached;
}

void RunTestCalls() {
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) return;
  ClearReport();
  std::thread([] {
    Refs r = Locate();
    if (!r.error.empty() || !engine::Hooked()) {
      Rep("FAIL: %s", r.error.empty() ? "ProcessEvent hook not installed" : r.error.c_str());
      g_busy = false;
      return;
    }
    Rep("profile %s", ue3::FullName(r.profile).c_str());
    Rep("upgrades %s", r.upgrades ? ue3::FullName(r.upgrades).c_str() : "null");
    Rep("shop %s", r.shop ? ue3::FullName(r.shop).c_str() : "null");
    Rep("pc %s (map %s)", r.pc ? ue3::FullName(r.pc).c_str() : "null", r.map.c_str());
    OnGame<int>(
        [r] {
          {
            engine::Call c(r.profile, "GetTotalScore");
            Rep("%s -> %s, ret %d", c.Describe().c_str(), c.Invoke() ? "ok" : "FAIL", c.RetInt());
          }
          if (r.upgrades) {
            engine::Call c(r.upgrades, "HasUpgrade");
            c.Name(c.ParamName(0).c_str(), "Dante_Abilities_Jump");
            Rep("%s [Dante_Abilities_Jump] -> %s, ret %d", c.Describe().c_str(), c.Invoke() ? "ok" : "FAIL",
                (int)c.RetBool());
          }
          {  // static native with a live context: must return 1 in the retail build
            engine::Call c(r.profile, Func("NTWorldInfo", "NTStripUnlockingHacks"));
            Rep("NTStripUnlockingHacks (ctx profile) -> %s, ret %d", c.Invoke() ? "ok" : "FAIL", (int)c.RetBool());
          }
          {
            StripBypass b;
            engine::Call c(r.profile, Func("NTWorldInfo", "NTStripUnlockingHacks"));
            Rep("NTStripUnlockingHacks with bypass (active=%d) -> ret %d", (int)b.Active(),
                c.Invoke() ? (int)c.RetBool() : -1);
          }
          Rep("save slot status %d (3 = loaded), saving busy %d", SlotStatusOnGame(r.profile), (int)SaveBusy(r.profile));
          ue3::FNameRaw hp{}, dt{}, hpSet{}, dtSet{};
          if (StatId("DVPickupStats", "MAXHEALTH_PLAYER", hp) && StatId("DVPickupStats", "MAXTRIGGERPOINTS_PLAYER", dt) &&
              StatId("DVPickupStats", "MAXHEALTH_SET", hpSet) && StatId("DVPickupStats", "MAXTRIGGERPOINTS_SET", dtSet))
            Rep("MAXHEALTH_PLAYER %.0f (set %d), MAXTRIGGERPOINTS_PLAYER %.0f (set %d)", StatFloat(r.profile, hp),
                (int)StatBool(r.profile, hpSet), StatFloat(r.profile, dt), (int)StatBool(r.profile, dtSet));
          if (r.upgrades) {
            PropRef set = ObjProp(r.upgrades, "m_UnlockedUpgrades");
            Rep("combat upgrades unlocked: %d", set.offset >= 0 ? ReadOr<int32_t>(r.upgrades + set.offset + 4) : -1);
          }
          Rep("DLC ids: BlackHair %d, NoCoat %d, SuperDante %d", (int)ProfileHasDLCID(r.profile, L"Costume_DanteBlackHair"),
              (int)ProfileHasDLCID(r.profile, L"Costume_DanteNoCoat"), (int)ProfileHasDLCID(r.profile, L"Perk_SuperDante"));
          int total = 0, done = CountCompleted(r.profile, &total);
          Rep("missions completed %d of %d", done, total);
          Rep("story weapons & abilities %d of %zu", CountAbilities(r.profile), sizeof kStoryItems / sizeof kStoryItems[0]);
          int souls = 0, soulsHave = 0;
          for (const MissionCollectibles& mc : CompletionList())
            for (const ue3::FNameRaw& s : mc.souls) ++souls, soulsHave += StatBool(r.profile, s);
          Rep("lost souls %d of %d, secret missions %zu, game info %s", soulsHave, souls, SecretMissionIds().size(),
              r.game ? ue3::FullName(r.game).c_str() : "null");
          Rep("test calls done");
          return 0;
        },
        10000);
    g_busy = false;
  }).detach();
}

std::vector<std::string> Report() {
  std::lock_guard<std::mutex> lk(g_repMu);
  return g_report;
}

bool Busy() { return g_busy; }

}  // namespace unlock
