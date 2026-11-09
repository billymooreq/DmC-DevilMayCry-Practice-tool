#pragma once
#include <cstdint>
#include <string>

// Practice tool version (Settings > About, compatibility report, save package manifests, logs). Separate from
// the game version. The only place to change it for a release.
inline constexpr const char kToolVersion[] = "0.11.0";

// Internal build names stay in logs, dumps and save package manifests; the player only sees the label
// ("v1.3", "v1.0", "unknown build").
// NonSteam2013: a v1.3 build of February 2013 that is only found in non-Steam copies (repacks); its classes are
// v1.3's (8 of 2879 differ), but its save slot screen reads the INFO text lines like v1.0.
enum class GameVersion { Unknown, Latest2025, Release2013, NonSteam2013 };

struct VersionInfo {
  GameVersion version;
  const char* name;   // internal: "latest_2025" / "release_2013" / "nonsteam_2013" / "unknown"
  const char* label;  // shown to the player: "v1.3" / "v1.0" / "v1.3 nonsteam" / "unknown build"
  uint32_t timeDateStamp;
  uint32_t sizeOfImage;
  uintptr_t base;
  // Not the Steam game: the non-Steam build, or a Steam emulator's files next to the exe (steam_api.ini...).
  // Saves then live where the emulator keeps them, never in Steam's userdata folder.
  bool nonSteam;
};
// The save slot screen of this build shows the INFO text lines (v1.0 and the non-Steam v1.3), not its numbers.
bool SlotScreenReadsText();

// Reads the PE header of the main module once (cheap, no hashing).
const VersionInfo& GetGameVersion();
// The build is one we know (v1.3 / v1.0). Dev "pretend unknown" (tests) makes it false at runtime.
bool GameVersionKnown();
void SetPretendUnknown(bool on);  // Dev / autorun: show the menu as on an unknown build
// Since 0.11 nothing is locked on an unknown build: every feature, Unlocks and Saves can be used (the game may
// still not work with them; risky writes show a yellow warning first). Kept as one switch for the call sites.
inline bool FeaturesAllowed() { return true; }
// Player-facing label for an internal build name (save package manifests): "latest_2025" -> "v1.3".
std::string VersionLabel(const std::string& internalName);
