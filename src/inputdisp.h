#pragma once
#include <string>
#include <vector>

#include "game.h"
#include "json.hpp"

// Visuals > INPUT DISPLAY (#23): the player's inputs like a fighting game's training mode - a list, newest on top,
// one row per change of the input (a button pressed / let go, the direction changed), each with how many frames it
// was held. Buttons are the game's actions (Basic Melee Attack, Shoot, Jump, Angel Mode...), read from the game's
// own input object after its key / pad mapping (DVInput: the held NTB buttons + DVButtonMap: action -> button), so
// keyboard and gamepad look the same and the game's rebinding is followed. Only shows: not a cheat (no "Practice
// Tool Active", Steam not blocked, works in Speedrun mode). Research: docs/input_display.md.
namespace features {
class Feature;
}

namespace inputdisp {

std::vector<features::Feature*> CreateFeatures();  // registered by feature_list.cpp

void OnFrame(const game::World& w);  // game thread, every frame: samples the input
void Draw(bool menuOpen);            // render thread: the list on the game screen

struct Options {
  int style = 0;        // 0 Gamepad (Xbox buttons), 1 Keyboard (the keys bound in the game), 2 Text
  int frames = 0;       // 0 = 60 fps frames (1/60 s of game time), 1 = real frames (the game's drawn frames)
  int rows = 15;        // 5..30
  bool neutral = false; // also rows with nothing held
  float size = 1.0f;    // 0.5..2
  float bg = 0.4f;      // 0..1, the dark box behind the list
  float hideAfter = 0;  // seconds without input -> fades out; 0 = never
};
Options GetOptions();
void SetOptions(const Options& o);  // + autosave
void Clear();
void ResetPosition();

void LoadJson(const nlohmann::json& j);  // DmCPracticeTool.json "input_display"
nlohmann::json SaveJson();

// Autorun: "inputdisp" (state + the last rows), "inputdisp probe <s>" (raw fields of the game's input object to the
// log on every change - research), "inputdisp set <key> <value>", "inputdisp clear".
std::vector<std::string> DebugLines(int rows);
void Probe(float seconds);
bool SetByName(const std::string& key, float v);

}  // namespace inputdisp
