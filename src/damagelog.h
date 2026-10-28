#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game.h"
#include "json.hpp"

// Damage log: every hit in the mission (who hit whom, how much, with which move), in a small window that stays
// on screen with the menu closed. Fed from the one damage point of the game, NTCombat_Nucleus.ContactInternal
// (feature_list.cpp). Ring buffer of the last kMaxEntries hits, so a long fight costs no frame time.
namespace damagelog {

constexpr int kMaxEntries = 2000;

void SetOn(bool on);  // the "Damage log" feature
bool On();

// Game thread, before the game applies the hit. gameDamage = what the game asked for, outDamage = after our
// multipliers (what the log shows; the victim's armour / shields are applied by the game after this point).
void OnHit(const game::World& w, uintptr_t contact, uintptr_t attacker, uintptr_t victim, float gameDamage,
           float outDamage);
// Game thread, right after the game processed that hit: victim's health after it, damage after armour.
void AfterHit(uintptr_t nucleus, uintptr_t contact);
void OnFrame();  // game thread: bosses' "after" health, read a few frames after the hit

// Render thread: the window (inputs only while the menu is open).
void Draw(bool menuOpen);

void LoadJson(const nlohmann::json& j);  // window position, filter
nlohmann::json SaveJson();

// Autorun / Dev
void Clear();
void SetFilter(int f);  // 0 all, 1 Dante's hits, 2 hits on Dante
void SetMergeWeak(bool on);  // weak spot damage added to its hit
bool ExportCsv(std::string* path);
std::vector<std::string> DebugLines(int last);
std::vector<std::string> SummaryLines();

}  // namespace damagelog
