#include "compat.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <cstdarg>
#include <iterator>
#include "log.h"
#include "savefile.h"
#include "steam_guard.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"

namespace compat {
namespace {

std::atomic<bool> g_busy{false};
std::mutex g_mu;
std::string g_result;

// Classes our features read or call into (unlock, saves, cheats, enemies, navigation, visuals).
const char* const kClasses[] = {
    "GameEngine", "NTGameEngine", "DVGamerProfile", "NTGamerProfile", "DVSaveManager", "DVMapInfo",
    "PlayerController", "DVPlayerController", "NTPawn", "DVPawn", "DVGameInfo", "NTGameInfo_SP", "NTWorldInfo",
    "NTGameStatsManager", "NTCombat_UpgradesComponent", "DVExecFunctions", "DVEnemyManager", "NTCheckpoint",
    "NTCombat_Nucleus", "DVCombat_Nucleus", "NTCombat_ProcessStrike", "NTCombat_StrikeVolume_Arc",
    "NTCombat_StrikeVolume_Capsule", "NTCombat_Projectile", "NTCharacterTemplate", "NTAIController",
    "NTCam_CameraController", "DVSeqVar_EnemyWaveData", "DVSeqAct_BeginEncounter", "DVSeqAct_BossHealthBar",
    "DVSeqAct_PlayCinematicAndMatinee", "SeqAct_NT_SetCheckPoint", "LevelStreamingVolume", "BlockingVolume",
    "DVShopComponent", "NTGFxMovieManager", "DVGFxMovieHandler_Frontend_Callbacks", "NTBaseEngine",
};

// "Class.Function" pairs called through ProcessEvent or watched in the hook.
const char* const kFunctions[] = {
    "NTCombat_Nucleus.ContactInternal", "DVCombat_Nucleus.ContactInternal", "DVEnemyManager.SpawnEnemy",
    "DVExecFunctions.LoadSingleSpawnLevel", "DVExecFunctions.NTKillAll", "NTGamerProfile.SaveProfile",
    "DVGamerProfile.UnlockAllMissions", "DVGamerProfile.UnlockAllSecretMissions", "DVGameInfo.UnlockUpgrade",
    "NTCombat_UpgradesComponent.UnlockAllUpgrades", "NTWorldInfo.NTStripUnlockingHacks",
    "DVSaveManager.EnumerateContent", "DVSaveManager.IsBusy", "DVSaveManager.GetActiveSlotStatus",
    "DVMapInfo.LoadSaveSlot", "NTCheckpoint.ClearCheckpoint", "NTCheckpoint.SetCheckpointName",
    "NTGameInfo_SP.RevertCheckpoint", "NTBaseEngine.StopAnyPlayingCutscene", "PlayerController.ConsoleCommand",
    "NTGFxMovieManager.CloseAllIngameUI", "DVShopComponent.ResetSpecialGoldOrbCount",
    "DVGFxMovieHandler_Frontend_Callbacks.ContinueFromCheckpoint", "DVPawn.SetHealth", "NTPawn.SetNTPhysics",
    "NTAnimatedActor.ForcedSetLocation", "DVPawn.CleanupOnscreen", "DVSeqAct_BossHealthBar.Activated",
    "NTCombat_StrikeVolume_Arc.GetPhysXCollisionParameters", "NTCombat_StrikeVolume_Capsule.TransformWithOffsets",
    "NTProcessBase.ProcessTick",
};

std::string Fmt(const char* f, ...) {
  char b[1024];
  va_list a;
  va_start(a, f);
  vsnprintf(b, sizeof b, f, a);
  va_end(a);
  return b;
}

std::string FileVersion(const std::wstring& path) {
  DWORD h = 0, n = GetFileVersionInfoSizeW(path.c_str(), &h);
  if (!n) return "?";
  std::vector<uint8_t> buf(n);
  if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return "?";
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return "?";
  return Fmt("%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
             LOWORD(fi->dwFileVersionLS));
}

std::string Now() {
  time_t t = time(nullptr);
  tm lt{};
  localtime_s(&lt, &t);
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &lt);
  return b;
}

uintptr_t Rva(uintptr_t va) { return va ? va - GetGameVersion().base : 0; }

// "8B 0D ?? ..." in the executable sections of the exe file on disk -> RVAs (at most maxHits).
std::vector<uint32_t> FileCodeHits(const savefile::Bytes& f, const char* sig, size_t maxHits) {
  std::vector<int> pat;
  for (const char* p = sig; *p;) {
    if (*p == ' ') { ++p; continue; }
    if (*p == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
    pat.push_back((int)strtoul(p, nullptr, 16));
    p += 2;
  }
  std::vector<uint32_t> hits;
  if (f.size() < sizeof(IMAGE_DOS_HEADER)) return hits;
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(f.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > f.size()) return hits;
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(f.data() + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (!(s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    size_t start = s[i].PointerToRawData, size = s[i].SizeOfRawData;
    if (start + size > f.size()) size = f.size() > start ? f.size() - start : 0;
    const uint8_t* b = f.data() + start;
    for (size_t j = 0; j + pat.size() <= size; ++j) {
      size_t k = 0;
      while (k < pat.size() && (pat[k] < 0 || b[j + k] == pat[k])) ++k;
      if (k == pat.size()) {
        hits.push_back(s[i].VirtualAddress + (uint32_t)j);
        if (hits.size() >= maxHits) return hits;
      }
    }
  }
  return hits;
}

std::string Build() {
  const VersionInfo& v = GetGameVersion();
  std::string r;
  auto line = [&r](const std::string& s) { r += s + "\n"; };
  line("DmC Practice Tool - compatibility report");
  line(Fmt("tool version: %s", kToolVersion));
  line(Fmt("created: %s", Now().c_str()));
  line("");

  // --- the game exe (file name only: the install path can contain the user name)
  line("[game exe]");
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring wp = path;
  std::wstring file = wp.substr(wp.find_last_of(L"\\/") + 1);
  line("file: " + util::WideToUtf8(file));
  std::ifstream in(wp, std::ios::binary);
  savefile::Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  line(Fmt("size: %zu bytes", data.size()));
  line("sha256: " + (data.empty() ? std::string("?") : savefile::Sha256Hex(data)));
  line("FileVersion: " + FileVersion(wp));
  line(Fmt("TimeDateStamp: 0x%08X", v.timeDateStamp));
  line(Fmt("SizeOfImage: 0x%08X", v.sizeOfImage));
  line(Fmt("detected: %s (%s)%s", v.label, v.name, FeaturesAllowed() ? "" : " - features locked"));
  {
    auto base = v.base;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    line(Fmt("entry point RVA: 0x%08X", nt->OptionalHeader.AddressOfEntryPoint));
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      char name[9] = {};
      memcpy(name, s[i].Name, 8);
      line(Fmt("section %-8s RVA 0x%08X size 0x%08X flags 0x%08X", name, s[i].VirtualAddress, s[i].Misc.VirtualSize,
               s[i].Characteristics));
    }
  }
  line(Fmt("d3d9.dll loaded: %s, d3d11.dll loaded: %s", GetModuleHandleW(L"d3d9.dll") ? "yes" : "no",
           GetModuleHandleW(L"d3d11.dll") ? "yes" : "no"));
  line("");

  // --- signatures: counted in the exe file (our own hooks have already patched some of them in memory)
  line("[signatures]");
  if (data.empty()) line("exe file not readable: counting in memory (hooked functions show as not found)");
  struct S {
    std::string name;
    const char* pattern;
    int addrOffset;
  };
  std::vector<S> sigs;
  for (const ue3::SigInfo& s : ue3::Signatures()) sigs.push_back({s.name, s.pattern, s.addrOffset});
  sigs.push_back({"ProcessEvent", engine::ProcessEventSignature(), -1});
  int sigOk = 0;
  for (const S& s : sigs) {
    std::vector<uintptr_t> hits;  // VAs in the running image
    if (data.empty()) {
      hits = ue3::SigScanAll(s.pattern, 16);
    } else {
      for (uint32_t rva : FileCodeHits(data, s.pattern, 16)) hits.push_back(v.base + rva);
    }
    std::string at;
    for (uintptr_t h : hits) at += Fmt(" 0x%X", (unsigned)Rva(h));
    line(Fmt("%-13s %s: %zu match%s%s%s", s.name.c_str(), hits.empty() ? "NOT FOUND" : "found", hits.size(),
             hits.size() == 1 ? "" : "es", hits.size() >= 16 ? " (or more)" : "", hits.empty() ? "" : (" at RVA" + at).c_str()));
    if (hits.size() == 1) ++sigOk;
    if (!hits.empty() && s.addrOffset >= 0) {
      uintptr_t addr = util::ReadOr<uintptr_t>(hits[0] + s.addrOffset);
      ue3::TArrayRaw a{};
      bool ok = util::Read(addr, a);
      line(Fmt("              -> TArray at RVA 0x%X: %s", (unsigned)Rva(addr),
               ok ? Fmt("Num %d, Max %d", a.num, a.max).c_str() : "not readable"));
    }
    line(std::string("              pattern: ") + s.pattern);
  }
  line(Fmt("unique matches: %d of %zu", sigOk, sigs.size()));
  line("");

  // --- UE3 reflection (a full scan if the signature path did not work; read-only)
  line("[ue3]");
  if (!ue3::Ready()) {
    line("auto init: " + ue3::StatusText());
    line("running a full scan (read-only, can take a minute)...");
    ue3::Init(false);
  }
  line(std::string("ready: ") + (ue3::Ready() ? "yes" : "NO"));
  line("status: " + ue3::StatusText());
  line(Fmt("names: %d, objects: %d", ue3::NumNames(), ue3::NumObjects()));
  {
    const ue3::Layout& l = ue3::L();
    line(Fmt("GNames RVA 0x%X, GObjects RVA 0x%X", (unsigned)Rva(l.gnames), (unsigned)Rva(l.gobjects)));
  }
  line("layout:");
  r += ue3::LayoutReport();
  line("");

  // --- classes and functions our features need
  int clsOk = 0, fnOk = 0;
  line("[classes]");
  if (ue3::Ready()) {
    for (const char* c : kClasses) {
      bool ok = ue3::FindObject("Class", c) != 0;
      clsOk += ok;
      line(Fmt("%-40s %s", c, ok ? "found" : "NOT FOUND"));
    }
  } else {
    line("skipped (UE3 not ready)");
  }
  line("");
  line("[functions]");
  if (ue3::Ready()) {
    for (const char* f : kFunctions) {
      std::string full = f;
      size_t dot = full.find('.');
      std::string cls = full.substr(0, dot), fn = full.substr(dot + 1);
      uintptr_t o = ue3::FindObject("Function", fn.c_str(), cls.c_str());
      std::string st;
      if (o) {
        ++fnOk;
        st = Fmt("found (params %d, native %u)", ue3::FuncNumParms(o), (unsigned)ue3::FuncNative(o));
      } else if (uintptr_t any = ue3::FindObject("Function", fn.c_str())) {
        st = "NOT FOUND here, exists in " + ue3::ObjName(ue3::ObjOuter(any));
      } else {
        st = "NOT FOUND";
      }
      line(Fmt("%-58s %s", f, st.c_str()));
    }
  } else {
    line("skipped (UE3 not ready)");
  }
  line("");

  // --- hooks
  line("[hooks]");
  line(Fmt("ProcessEvent hook: %s (RVA 0x%X)", engine::Hooked() ? "on" : "off", (unsigned)Rva(engine::ProcessEventAddr())));
  line(Fmt("Steam achievements guard: %s", steam::GuardInstalled() ? "on" : "off"));
  line("");

  line("[summary]");
  line(Fmt("signatures %d/%zu unique, ue3 %s, classes %d/%zu, functions %d/%zu", sigOk, sigs.size(),
           ue3::Ready() ? "ready" : "NOT ready", clsOk, std::size(kClasses), fnOk, std::size(kFunctions)));
  return r;
}

}  // namespace

std::wstring ReportPath() { return util::InExeDir(L"DmCPT_compat_report.txt"); }

bool Busy() { return g_busy; }

std::string LastResult() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_result;
}

void CreateReportAsync() {
  if (g_busy.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_result = "Creating the report...";
  }
  std::thread([] {
    LOG("compat: creating report");
    std::string text = Build();
    std::ofstream out(ReportPath(), std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    std::string res = out ? "Report saved: DmCPT_compat_report.txt (next to the game exe)." : "Could not write the report file.";
    size_t s = text.rfind("[summary]");
    if (s != std::string::npos) LOG("compat: %s", text.substr(s + 10).c_str());
    LOG("compat: %s", res.c_str());
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_result = res;
    }
    g_busy = false;
  }).detach();
}

}  // namespace compat
