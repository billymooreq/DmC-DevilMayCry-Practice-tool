#pragma once
#include <cstdint>
#include <string>

// Practice tool version (Settings > About, compatibility report, save package manifests, logs). Separate from
// the game version. The only place to change it for a release.
inline constexpr const char kToolVersion[] = "0.9.0";

// Internal build names stay in logs, dumps and save package manifests; the player only sees the label
// ("v1.3", "v1.0", "unknown build").
enum class GameVersion { Unknown, Latest2025, Release2013 };

struct VersionInfo {
  GameVersion version;
  const char* name;   // internal: "latest_2025" / "release_2013" / "unknown"
  const char* label;  // shown to the player: "v1.3" / "v1.0" / "unknown build"
  uint32_t timeDateStamp;
  uint32_t sizeOfImage;
  uintptr_t base;
};

// Reads the PE header of the main module once (cheap, no hashing).
const VersionInfo& GetGameVersion();
// Game features are allowed only on a known build.
inline bool FeaturesAllowed() { return GetGameVersion().version != GameVersion::Unknown; }
// Player-facing label for an internal build name (save package manifests): "latest_2025" -> "v1.3".
std::string VersionLabel(const std::string& internalName);
