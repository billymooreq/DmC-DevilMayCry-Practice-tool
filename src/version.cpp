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
// release_2013 = "v1.0" for players: January 2013 release (exe built 2012-12-07), SHA-256 02F12DDB...56F6, no
// FileVersion resource. Same UE3 layout and signatures as latest_2025; all class/function lookups go by name.
// nonsteam_2013 = "v1.3 nonsteam": exe built 2013-02-27 (SHA-256 9E18A390...), in RELOADED-based repacks (with
// their Steam emulator or with Valve's steam_api.dll). Classes = v1.3 except 8; the LiveSplit .asl has no entry for
// it (ModuleMemorySize 47886336), the tool finds the loading flag by signature (exe+0x2755180).
constexpr KnownBuild kBuilds[] = {
    {GameVersion::Latest2025, "latest_2025", "v1.3", 0x682EAB06, 0x02DA8000},
    {GameVersion::Release2013, "release_2013", "v1.0", 0x50C2491C, 0x02D93000},
    {GameVersion::NonSteam2013, "nonsteam_2013", "v1.3 nonsteam", 0x512E1B2E, 0x02DAB000},
};
constexpr KnownBuild kLabels[] = {
    {GameVersion::Latest2025, "latest_2025", "v1.3", 0, 0},
    {GameVersion::Release2013, "release_2013", "v1.0", 0, 0},
    {GameVersion::NonSteam2013, "nonsteam_2013", "v1.3 nonsteam", 0, 0},
};

// Files Steam emulators put next to the game exe (RELOADED/3DM: steam_api.ini, Goldberg: steam_settings...).
bool EmulatorFilesNextToExe() {
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  std::wstring dir(exe, n);
  dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
  for (const wchar_t* f : {L"steam_api.ini", L"steam_emu.ini", L"SmartSteamEmu.ini", L"ALI213.ini", L"cream_api.ini",
                           L"CODEX.ini", L"CPY.ini", L"3dmgame.ini", L"valve.ini", L"steam_interfaces.txt",
                           L"steam_settings", L"local_save.txt"})
    if (GetFileAttributesW((dir + f).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  return false;
}

VersionInfo Detect() {
  VersionInfo vi{GameVersion::Unknown, "unknown", "unknown build", 0, 0, 0, false};
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
  vi.nonSteam = vi.version == GameVersion::NonSteam2013 || EmulatorFilesNextToExe();
  if (vi.nonSteam && vi.version != GameVersion::NonSteam2013) {  // a Steam build run through an emulator
    static const std::string label = std::string(vi.label) + " nonsteam";
    vi.label = label.c_str();
  }
  return vi;
}

}  // namespace

namespace {
bool g_pretendUnknown = false;
}

bool SlotScreenReadsText() {
  GameVersion v = GetGameVersion().version;
  return v == GameVersion::Release2013 || v == GameVersion::NonSteam2013;
}

bool GameVersionKnown() { return !g_pretendUnknown && GetGameVersion().version != GameVersion::Unknown; }
void SetPretendUnknown(bool on) { g_pretendUnknown = on; }

const VersionInfo& GetGameVersion() {
  static const VersionInfo vi = Detect();
  return vi;
}

std::string VersionLabel(const std::string& internalName) {
  for (const auto& b : kLabels)
    if (internalName == b.name) return b.label;
  return internalName.empty() ? "unknown build" : internalName;
}
