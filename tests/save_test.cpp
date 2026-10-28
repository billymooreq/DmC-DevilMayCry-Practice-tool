// Offline check of savefile.cpp on real save files (no game):
//   save_test.exe <save dir> <slot 0-based> <out dir> [<dir with raw DMC5_SAVE_XX files> <raw slot>]
// decrypt -> header/SHA1 -> summary -> .dmcpt -> read back -> re-bind -> encrypt == original bytes.
#include <windows.h>

#include <cstdio>
#include <string>

#include "savefile.h"

using namespace savefile;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
  if (!ok) ++g_fail;
}

static bool ReadAll(const std::wstring& p, Bytes& out) {
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  out.resize(ftell(f));
  fseek(f, 0, SEEK_SET);
  bool ok = fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

static void WriteAll(const std::wstring& p, const Bytes& b) {
  FILE* f = _wfopen(p.c_str(), L"wb");
  if (!f) return;
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}

static std::wstring Find(const std::wstring& dir, const std::wstring& name) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  std::wstring found;
  if (h == INVALID_HANDLE_VALUE) return found;
  do
    if (_wcsicmp(fd.cFileName, name.c_str()) == 0) found = dir + L"\\" + fd.cFileName;
  while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    printf("usage: save_test <save dir> <slot> <out dir> [<raw dir> <raw slot>]\n");
    return 2;
  }
  std::wstring dir = argv[1], out = argv[3];
  int slot = _wtoi(argv[2]);
  SlotFiles s;
  Bytes enc[KindCount];
  uint32_t account = 0;
  for (int k = 0; k < KindCount; ++k) {
    std::wstring p = Find(dir, SlotFileName(slot, (Kind)k));
    Check(!p.empty() && ReadAll(p, enc[k]), "read slot file");
    s.dec[k] = Decrypt(enc[k]);
    Header h;
    std::string err;
    Check(ParseHeader(s.dec[k], h, &err), (std::string(KindName((Kind)k)) + " header + SHA1 " + err).c_str());
    account = h.account;
    Check(Encrypt(s.dec[k]) == enc[k], "decrypt/encrypt round trip is exact");
  }
  std::string err;
  Check(CheckSet(s, &err), ("CheckSet " + err).c_str());
  Summary sum = Summarize(s);
  for (const std::string& l : SummaryLines(sum)) printf("   %s\n", l.c_str());
  printf("   info valid %d: version %d time %d mission %d diff %d date %d-%d-%d %d:%d\n", sum.info.valid,
         sum.info.version, sum.info.timePlayed, sum.info.highestMission, sum.info.highestDifficulty, sum.info.year,
         sum.info.month, sum.info.day, sum.info.hour, sum.info.minute);
  printf("   stats %zu (ok %d), skills %zu\n", sum.profile.stats.size(), sum.profile.statsOk, sum.profile.skills.size());

  printf("   one line: %s | default name: %s | file: %s\n", sum.OneLine().c_str(), sum.DefaultName().c_str(),
         SafeFileName(sum.DefaultName()).c_str());
  Bytes pkg = BuildPackage(s, sum.DefaultName(), "test notes: M12 boss, DMD", "0.4.0", "latest_2025",
                           "2026-10-26T21:00:00+03:00", slot);
  WriteAll(out + L"\\test.dmcpt", pkg);
  SlotFiles back;
  Manifest m;
  Check(ReadPackage(pkg, back, m, &err), ("ReadPackage " + err).c_str());
  Check(m.notes == "test notes: M12 boss, DMD", "notes kept");
  Check(m.name == sum.DefaultName(), "name kept");
  // Rename: new name in the manifest, the save files (and their hashes) stay byte for byte.
  Bytes renamed;
  Check(RenamePackage(pkg, "My practice: boss \xE2\x80\x94 try 2", renamed, &err), ("RenamePackage " + err).c_str());
  SlotFiles rb;
  Manifest rm;
  Check(ReadPackage(renamed, rb, rm, &err) && rm.name == "My practice: boss \xE2\x80\x94 try 2" && rm.notes == m.notes,
        "renamed package verifies, name changed, notes kept");
  for (int k = 0; k < KindCount; ++k) Check(rb.dec[k] == back.dec[k], "renamed package: save file unchanged");
  Check(rm.sha256 == m.sha256, "renamed package: same hashes");
  Check(SafeFileName("My practice: boss \xE2\x80\x94 try 2") == "My practice_ boss - try 2", "safe file name");
  WriteAll(out + L"\\test_renamed.dmcpt", renamed);
  for (int k = 0; k < KindCount; ++k) {
    Header h;
    ParseHeader(back.dec[k], h, nullptr);
    Check(h.account == 0, "package files are not bound to an account");
    Bytes d = back.dec[k];
    SetAccount(d, account);
    Check(Encrypt(d) == enc[k], "package -> re-bind -> encrypt == original file");
  }
  // Tampering must be detected.
  Bytes bad = pkg;
  bad[200] ^= 1;
  SlotFiles tmp;
  Check(!ReadPackage(bad, tmp, m, &err), ("damaged package rejected: " + err).c_str());

  // INFO rebuilt from the profile must parse back.
  Bytes info = BuildInfo(sum.profile, sum.info.year, sum.info.month, sum.info.day, sum.info.hour, sum.info.minute);
  Check(info == Payload(s.dec[Info]) || sum.info.timePlayed != (int)sum.profile.Stat("GENERAL_TOTALGAMETIME"),
        "BuildInfo == the game's INFO (same time played)");
  InfoData id = ParseInfo(info);
  Check(id.valid && id.year == sum.info.year, "BuildInfo parses back");
  printf("   rebuilt info: time %d mission %d diff %d (file: %d %d %d)\n", id.timePlayed, id.highestMission,
         id.highestDifficulty, sum.info.timePlayed, sum.info.highestMission, sum.info.highestDifficulty);

  if (argc >= 6) {
    std::wstring raw = argv[4];
    int rs = _wtoi(argv[5]);
    SlotFiles r;
    for (int k = 0; k < KindCount; ++k) {
      std::wstring p = Find(raw, SlotFileName(rs, (Kind)k));
      Bytes e;
      if (!p.empty() && ReadAll(p, e)) r.dec[k] = Decrypt(e);
    }
    err.clear();
    Check(CheckSet(r, &err), ("raw set " + err).c_str());
    Summary rsum = Summarize(r);
    printf("   raw: info valid %d (old format expected)\n", rsum.info.valid);
    for (const std::string& l : SummaryLines(rsum)) printf("   %s\n", l.c_str());
  }
  printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL OK", g_fail);
  return g_fail ? 1 : 0;
}
