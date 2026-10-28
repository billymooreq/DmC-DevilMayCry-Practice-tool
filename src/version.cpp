#include "version.h"

#include <windows.h>

namespace {

struct KnownBuild {
  GameVersion version;
  const char* name;
  const char* label;
  uint32_t timeDateStamp;
  uint32_t sizeOfImage;
};

// latest_2025 = "v1.3" for players: current Steam build, exe SHA-256 C36516FD...B409, FileVersion 1.0.8916.1.
// release_2013 = "v1.0" (January 2013 release): not detected yet, its TimeDateStamp/SizeOfImage go here.
constexpr KnownBuild kBuilds[] = {
    {GameVersion::Latest2025, "latest_2025", "v1.3", 0x682EAB06, 0x02DA8000},
};
constexpr KnownBuild kLabels[] = {
    {GameVersion::Latest2025, "latest_2025", "v1.3", 0, 0},
    {GameVersion::Release2013, "release_2013", "v1.0", 0, 0},
};

VersionInfo Detect() {
  VersionInfo vi{GameVersion::Unknown, "unknown", "unknown build", 0, 0, 0};
  auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  vi.base = base;
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return vi;
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return vi;
  vi.timeDateStamp = nt->FileHeader.TimeDateStamp;
  vi.sizeOfImage = nt->OptionalHeader.SizeOfImage;
#ifdef DMCPT_SIMULATE_UNKNOWN_BUILD
  // Test build (cmake -DDMCPT_SIMULATE_UNKNOWN_BUILD=ON): behaves as on a game build we do not know.
  return vi;
#endif
  for (const auto& b : kBuilds) {
    if (b.timeDateStamp == vi.timeDateStamp && b.sizeOfImage == vi.sizeOfImage) {
      vi.version = b.version;
      vi.name = b.name;
      vi.label = b.label;
    }
  }
  return vi;
}

}  // namespace

const VersionInfo& GetGameVersion() {
  static const VersionInfo vi = Detect();
  return vi;
}

std::string VersionLabel(const std::string& internalName) {
  for (const auto& b : kLabels)
    if (internalName == b.name) return b.label;
  return internalName.empty() ? "unknown build" : internalName;
}
