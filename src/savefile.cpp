#include "savefile.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "blowfish_init.h"
#include "json.hpp"

namespace savefile {
namespace {

// ------------------------------------------------------------------ Blowfish (key from the exe, see save_format.md)
constexpr char kKey[] = "McUnXzck4KM36JjY3r6Q";

struct Blowfish {
  uint32_t P[18], S[4][256];
  Blowfish() {
    memcpy(P, kBlowfishInit, sizeof P);
    memcpy(S, kBlowfishInit + 18, sizeof S);
    const size_t keyLen = sizeof kKey - 1;
    size_t k = 0;
    for (int i = 0; i < 18; ++i) {
      uint32_t d = 0;
      for (int j = 0; j < 4; ++j) d = (d << 8) | (uint8_t)kKey[k++ % keyLen];
      P[i] ^= d;
    }
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
      Enc(l, r);
      P[i] = l;
      P[i + 1] = r;
    }
    for (auto& s : S)
      for (int i = 0; i < 256; i += 2) {
        Enc(l, r);
        s[i] = l;
        s[i + 1] = r;
      }
  }
  uint32_t F(uint32_t x) const {
    return ((S[0][x >> 24] + S[1][(x >> 16) & 255]) ^ S[2][(x >> 8) & 255]) + S[3][x & 255];
  }
  void Enc(uint32_t& l, uint32_t& r) const {
    for (int i = 0; i < 16; ++i) {
      l ^= P[i];
      r ^= F(l);
      std::swap(l, r);
    }
    std::swap(l, r);
    r ^= P[16];
    l ^= P[17];
  }
  void Dec(uint32_t& l, uint32_t& r) const {
    for (int i = 17; i > 1; --i) {
      l ^= P[i];
      r ^= F(l);
      std::swap(l, r);
    }
    std::swap(l, r);
    r ^= P[1];
    l ^= P[0];
  }
};

const Blowfish& BF() {
  static const Blowfish bf;
  return bf;
}

Bytes Crypt(const Bytes& in, bool decrypt) {
  Bytes out = in;
  const Blowfish& bf = BF();
  for (size_t i = 0; i + 8 <= out.size(); i += 8) {
    uint32_t l, r;  // each half is a little-endian dword (x86 implementation)
    memcpy(&l, &out[i], 4);
    memcpy(&r, &out[i + 4], 4);
    if (decrypt)
      bf.Dec(l, r);
    else
      bf.Enc(l, r);
    memcpy(&out[i], &l, 4);
    memcpy(&out[i + 4], &r, 4);
  }
  return out;
}

// ------------------------------------------------------------------ hashing (CNG)
Bytes Hash(LPCWSTR alg, const uint8_t* p, size_t n) {
  Bytes out;
  BCRYPT_ALG_HANDLE a = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  DWORD len = 0, got = 0;
  if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) != 0) return out;
  if (BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof len, &got, 0) == 0 &&
      BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    out.resize(len);
    if (BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) != 0 || BCryptFinishHash(h, out.data(), len, 0) != 0) out.clear();
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(a, 0);
  return out;
}

std::string ToHex(const Bytes& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (uint8_t c : b) {
    s += d[c >> 4];
    s += d[c & 15];
  }
  return s;
}

uint32_t U32(const Bytes& b, size_t o) {
  uint32_t v = 0;
  if (o + 4 <= b.size()) memcpy(&v, &b[o], 4);
  return v;
}
void PutU32(Bytes& b, uint32_t v) {
  uint8_t t[4];
  memcpy(t, &v, 4);
  b.insert(b.end(), t, t + 4);
}
void PutU16(Bytes& b, uint16_t v) {
  b.push_back((uint8_t)v);
  b.push_back((uint8_t)(v >> 8));
}

// Bounds-checked reader for the serialized payloads.
struct Reader {
  const Bytes& d;
  size_t p = 0;
  bool bad = false;
  explicit Reader(const Bytes& b, size_t start = 0) : d(b), p(start) {}
  int32_t I32() {
    if (p + 4 > d.size()) {
      bad = true;
      return 0;
    }
    int32_t v;
    memcpy(&v, &d[p], 4);
    p += 4;
    return v;
  }
  uint8_t U8() {
    if (p + 1 > d.size()) {
      bad = true;
      return 0;
    }
    return d[p++];
  }
  double F64() {
    if (p + 8 > d.size()) {
      bad = true;
      return 0;
    }
    double v;
    memcpy(&v, &d[p], 8);
    p += 8;
    return v;
  }
  float F32() {
    int32_t i = I32();
    float f;
    memcpy(&f, &i, 4);
    return f;
  }
  void Skip(size_t n) {
    if (p + n > d.size())
      bad = true;
    else
      p += n;
  }
  // FString: i32 n; n > 0 ANSI n bytes, n < 0 UTF-16 -n chars (both with the terminator).
  std::string Str() {
    int32_t n = I32();
    if (bad || n == 0) return {};
    if (n > 0x10000 || n < -0x10000) {
      bad = true;
      return {};
    }
    std::string s;
    if (n > 0) {
      if (p + n > d.size()) {
        bad = true;
        return {};
      }
      s.assign((const char*)&d[p], n);
      p += n;
    } else {
      size_t bytes = (size_t)(-n) * 2;
      if (p + bytes > d.size()) {
        bad = true;
        return {};
      }
      std::wstring w((const wchar_t*)&d[p], (size_t)-n);
      p += bytes;
      int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
      s.resize(len);
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    }
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
  }
};

// INFO keys (hashed names written by DVGamerProfile.WriteSaveSlotMetadata, in this order).
enum InfoKey { kVersion, kTime, kMission, kDifficulty, kYear, kMonth, kDay, kHour, kMinute, kInfoKeys };
constexpr uint32_t kInfoHash[kInfoKeys] = {0xC098A400, 0x9816FC9B, 0x514C1933, 0xE8526DC0, 0xD4DFBF07,
                                           0x3911FB2E, 0x2EA8419A, 0x1DE3E4EE, 0xE9FD5CCC};

uint32_t Crc32(const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

}  // namespace

const char* KindName(Kind k) {
  static const char* n[] = {"PROFILE", "INFO", "CHECKPNT"};
  return k < KindCount ? n[k] : "?";
}

std::wstring SlotFileName(int slot, Kind k) {
  wchar_t b[64];
  swprintf(b, 64, L"DMC5_SAVE_%02d_%hs", slot, KindName(k));
  return b;
}

Bytes Decrypt(const Bytes& enc) { return Crypt(enc, true); }
Bytes Encrypt(const Bytes& dec) { return Crypt(dec, false); }

std::string Sha1Hex(const uint8_t* p, size_t n) { return ToHex(Hash(BCRYPT_SHA1_ALGORITHM, p, n)); }
std::string Sha256Hex(const Bytes& b) { return ToHex(Hash(BCRYPT_SHA256_ALGORITHM, b.data(), b.size())); }

bool ParseHeader(const Bytes& dec, Header& h, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  if (dec.size() < kHeaderSize) return fail("file too small");
  h.version = U32(dec, 0);
  h.size = U32(dec, 0x18);
  h.account = U32(dec, 0x1C);
  h.magic = U32(dec, 0x20);
  if (h.version != 1 || h.magic != 0x01100001) return fail("not a DmC save (bad header)");
  if (h.size > dec.size() - kHeaderSize) return fail("truncated file");
  Bytes sha = Hash(BCRYPT_SHA1_ALGORITHM, &dec[kHeaderSize], h.size);
  h.sha1Ok = sha.size() == 20 && memcmp(sha.data(), &dec[4], 20) == 0;
  if (!h.sha1Ok) return fail("checksum mismatch (damaged file)");
  return true;
}

void SetAccount(Bytes& dec, uint32_t account) {
  if (dec.size() >= kHeaderSize) memcpy(&dec[0x1C], &account, 4);
}

Bytes Payload(const Bytes& dec) {
  Header h;
  if (!ParseHeader(dec, h, nullptr)) return {};
  return Bytes(dec.begin() + kHeaderSize, dec.begin() + kHeaderSize + h.size);
}

Bytes Wrap(const Bytes& payload, uint32_t account) {
  Bytes out;
  PutU32(out, 1);
  Bytes sha = Hash(BCRYPT_SHA1_ALGORITHM, payload.data(), payload.size());
  sha.resize(20);
  out.insert(out.end(), sha.begin(), sha.end());
  PutU32(out, (uint32_t)payload.size());
  PutU32(out, account);
  PutU32(out, 0x01100001);
  out.insert(out.end(), payload.begin(), payload.end());
  while (out.size() % 8) out.push_back(0);
  return out;
}

// ------------------------------------------------------------------ names
const char* DifficultyName(int d) {
  static const char* n[] = {"Human", "Devil Hunter", "Nephilim", "Son of Sparda", "Dante Must Die",
                            "Heaven or Hell", "Hell and Hell"};
  return d >= 0 && d < 7 ? n[d] : "?";
}
const char* DifficultyShort(int d) {
  static const char* n[] = {"HUMAN", "DH", "NEPH", "SOS", "DMD", "HOH", "HAH"};
  return d >= 0 && d < 7 ? n[d] : "UNK";
}
const char* MissionName(int m) {
  static const char* n[] = {"Found",          "Home Truths",       "Bloodline",       "Under Watch",
                            "Virility",       "Secret Ingredient", "Overturn",        "Eyeless",
                            "Devil Inside",   "Bad News",          "The Order",       "Under Siege",
                            "Devil's Dalliance", "Last Dance",     "The Trade",       "The Plan",
                            "Furnace of Souls", "Demon's Den",     "Face of the Demon", "The End"};
  return m >= 0 && m < 20 ? n[m] : "?";
}
const char* StoryItemName(int id) {
  static const char* n[] = {"Angel Mode",      "Demon Mode",       "Angel Glide",     "Glide boost (wall)",
                            "Glide boost (enemy)", "Angel Lift",   "Angel Lift (enemy)", "Lift whip extend",
                            "Lift skill X",    "Lift skill A",     "Demon Pull",      "Demon Pull (enemy)",
                            "Pull whip extend", "Pull skill X",    "Pull skill A",    "Ebony & Ivory",
                            "Revenant",        "Kablooey",         "Osiris",          "Arbiter",
                            "Aquila",          "Eryx",             "Devil Trigger"};
  return id >= 0 && id < 23 ? n[id] : "other";
}

double ProfileData::Stat(const char* name, double def) const {
  auto it = stats.find(name);
  return it == stats.end() ? def : it->second;
}

// ------------------------------------------------------------------ parsers
InfoData ParseInfo(const Bytes& payload) {
  // Three tables written by DVSaveManager.SaveSlot_WriteInt / WriteFloat / WriteString: u32 n + n x {u32 key
  // hash, value}. The 2025 game writes 9 ints and empty float/string tables; 2013 files hold 3 strings only.
  InfoData d;
  uint32_t n = U32(payload, 0);
  if (n == 0 || n > 64 || payload.size() < 4 + 8 * (size_t)n) return d;
  int* dst[kInfoKeys] = {&d.version, &d.timePlayed, &d.highestMission, &d.highestDifficulty, &d.year,
                         &d.month,   &d.day,        &d.hour,           &d.minute};
  bool haveVersion = false;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t key = U32(payload, 4 + 8 * i);
    int32_t v = (int32_t)U32(payload, 8 + 8 * i);
    for (int k = 0; k < kInfoKeys; ++k)
      if (key == kInfoHash[k]) {
        *dst[k] = v;
        haveVersion |= k == kVersion;
      }
  }
  d.valid = haveVersion;
  return d;
}

CheckpointData ParseCheckpoint(const Bytes& payload) {
  // u32 1, 24 bytes (player location?), i32 difficulty, i32 mission, i32 ?, i32 11, i32 seconds of day,
  // i32 day, i32 month, i32 year, FString map, FString checkpoint, then streaming levels + Kismet state.
  CheckpointData c;
  Reader r(payload);
  if (r.I32() != 1) return c;
  r.Skip(24);
  c.difficulty = r.I32();
  c.mission = r.I32();
  r.I32();
  r.I32();
  c.secondsOfDay = r.I32();
  c.day = r.I32();
  c.month = r.I32();
  c.year = r.I32();
  if (r.bad) return c;
  c.map = r.Str();
  c.name = r.Str();
  c.valid = !r.bad;
  c.hasCheckpoint = c.valid && !c.map.empty();
  return c;
}

ProfileData ParseProfile(const Bytes& payload) {
  ProfileData p;
  Reader r(payload);
  p.version = r.I32();
  auto progression = [&](bool* completed) {
    *completed = r.U8() != 0;
    r.Skip(32);
  };
  int nd = r.I32();
  if (r.bad || nd < 0 || nd > 16) {
    p.error = "bad mission table";
    return p;
  }
  for (int d = 0; d < nd; ++d) {
    int nm = r.I32();
    if (r.bad || nm < 0 || nm > 64) {
      p.error = "bad mission table";
      return p;
    }
    p.missionsPerDifficulty = std::max(p.missionsPerDifficulty, nm);
    for (int m = 0; m < nm; ++m) {
      bool done;
      progression(&done);
      if (d < 7) p.completed[d] += done;
    }
  }
  int ns = r.I32();
  if (r.bad || ns < 0 || ns > 64) {
    p.error = "bad secret mission table";
    return p;
  }
  p.secretTotal = ns;
  for (int i = 0; i < ns; ++i) {
    bool done;
    progression(&done);
    p.secretCompleted += done;
  }
  int nb = r.I32();
  if (r.bad || nb < 0 || nb > 64) {
    p.error = "bad Bloody Palace table";
    return p;
  }
  for (int i = 0; i < nb; ++i) {
    bool done;
    progression(&done);
  }
  r.I32();  // upgrades blob size
  int nu = r.I32();
  if (r.bad || nu < 0 || nu > 4096) {
    p.error = "bad upgrades list";
    return p;
  }
  for (int i = 0; i < nu && !r.bad; ++i) {
    p.skills.push_back(r.Str());
    r.I32();
  }
  p.xp = r.I32();
  p.pointsTotal = r.I32();
  p.pointsUnspent = r.I32();
  r.I32();
  r.I32();
  r.F32();
  int na = r.I32();
  if (r.bad || na < 0 || na > 256) {
    p.error = "bad abilities list";
    return p;
  }
  for (int i = 0; i < na; ++i) p.abilities.push_back(r.I32());
  if (r.bad) {
    p.error = "truncated profile";
    return p;
  }
  p.valid = true;
  // Named stats follow the second "INT\0" marker after the abilities (see dmc_save.py).
  static const uint8_t kInt[4] = {'I', 'N', 'T', 0};
  auto find = [&](size_t from) -> size_t {
    auto it = std::search(payload.begin() + std::min(from, payload.size()), payload.end(), kInt, kInt + 4);
    return it == payload.end() ? std::string::npos : (size_t)(it - payload.begin());
  };
  size_t i = find(r.p);
  if (i != std::string::npos) i = find(i + 4);
  if (i == std::string::npos) return p;
  Reader s(payload, i + 4);
  int n = s.I32();
  if (s.bad || n < 0 || n > 100000) return p;
  for (int k = 0; k < n && !s.bad; ++k) {
    std::string name = s.Str();
    uint8_t t = s.U8();
    double v;
    if (t == 0x15 || t == 0x17)
      v = s.F64();
    else if (t == 0x16)
      v = s.F32();
    else if (t == 0x00 || t == 0x14)
      v = s.I32();
    else
      return p;  // unknown type: keep what was read
    if (!s.bad) p.stats[name] = v;
  }
  p.statsOk = !s.bad;
  return p;
}

Bytes BuildInfo(const ProfileData& p, int year, int month, int day, int hour, int minute) {
  // Highest difficulty: the hardest story difficulty (Human..DMD) with a completed mission; highest mission:
  // the next one after the last completed (capped), 1 for a new game - matches the files the game writes.
  int hd = 0, hm = 1;
  for (int d = 4; d >= 0; --d)
    if (p.completed[d] > 0) {
      hd = d;
      hm = std::min(p.completed[d] + 1, std::max(p.missionsPerDifficulty, 1));
      break;
    }
  int vals[kInfoKeys] = {1, (int)p.Stat("GENERAL_TOTALGAMETIME"), hm, hd, year, month, day, hour, minute};
  Bytes out;
  PutU32(out, kInfoKeys);
  for (int k = 0; k < kInfoKeys; ++k) {
    PutU32(out, kInfoHash[k]);
    PutU32(out, (uint32_t)vals[k]);
  }
  PutU32(out, 0);  // floats
  PutU32(out, 0);  // strings
  return out;
}

// ------------------------------------------------------------------ slot set
bool CheckSet(const SlotFiles& s, std::string* err) {
  for (int k = 0; k < KindCount; ++k) {
    if (!s.Has((Kind)k)) {
      if (k == Info) continue;  // optional: rebuilt from the profile
      if (err) *err = std::string(KindName((Kind)k)) + " file is missing";
      return false;
    }
    Header h;
    std::string e;
    if (!ParseHeader(s.dec[k], h, &e)) {
      if (err) *err = std::string(KindName((Kind)k)) + ": " + e;
      return false;
    }
  }
  ProfileData p = ParseProfile(Payload(s.dec[Profile]));
  if (!p.valid) {
    if (err) *err = "PROFILE: " + p.error;
    return false;
  }
  return true;
}

Summary Summarize(const SlotFiles& s) {
  Summary r;
  if (s.Has(Info)) r.info = ParseInfo(Payload(s.dec[Info]));
  if (s.Has(Checkpoint)) r.checkpoint = ParseCheckpoint(Payload(s.dec[Checkpoint]));
  if (s.Has(Profile)) r.profile = ParseProfile(Payload(s.dec[Profile]));
  return r;
}

std::string Summary::Mission() const {
  if (!checkpoint.hasCheckpoint) return "no checkpoint (main menu)";
  if (checkpoint.mission < 0) return checkpoint.map;  // test maps (debug rooms) are not missions
  char b[96];
  snprintf(b, sizeof b, "M%02d %s", checkpoint.mission + 1, MissionName(checkpoint.mission));
  return b;
}

std::string Summary::Checkpoint() const {
  if (!checkpoint.hasCheckpoint) return "";
  // "cServerRoom" -> "Server Room", "CH18_Desolace" -> "Desolace", "c_boss_arena" -> "boss arena"
  std::string n = checkpoint.name;
  if (n.size() > 3 && (n[0] == 'C' || n[0] == 'c') && (n[1] == 'H' || n[1] == 'h') && isdigit((unsigned char)n[2])) {
    size_t u = n.find('_');
    if (u != std::string::npos) n = n.substr(u + 1);
  }
  if (n.size() > 1 && n[0] == 'c' && (isupper((unsigned char)n[1]) || n[1] == '_')) n = n.substr(1);
  std::string out;
  for (size_t i = 0; i < n.size(); ++i) {
    char ch = n[i] == '_' ? ' ' : n[i];
    if (i > 0 && isupper((unsigned char)ch) && islower((unsigned char)n[i - 1])) out += ' ';
    if (ch != ' ' || (!out.empty() && out.back() != ' ')) out += ch;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out.empty() ? checkpoint.name : out;
}

std::string Summary::OneLine() const {
  if (!checkpoint.hasCheckpoint)
    return info.valid ? std::string("No mission in progress \xC2\xB7 highest: ") + DifficultyName(info.highestDifficulty)
                      : "No mission in progress";
  return Mission() + " \xC2\xB7 " + DifficultyName(checkpoint.difficulty) + " \xC2\xB7 " + Checkpoint();
}

std::string Summary::Brief() const {
  if (!checkpoint.hasCheckpoint) return "main menu";
  if (checkpoint.mission < 0) return checkpoint.map + " \xC2\xB7 " + DifficultyShort(checkpoint.difficulty);
  char b[48];
  snprintf(b, sizeof b, "M%02d \xC2\xB7 %s", checkpoint.mission + 1, DifficultyShort(checkpoint.difficulty));
  return b;
}

std::string Summary::DefaultName() const {
  char b[160];
  if (checkpoint.hasCheckpoint && checkpoint.mission < 0)
    snprintf(b, sizeof b, "%s %s \xE2\x80\x94 %s", checkpoint.map.c_str(), DifficultyShort(checkpoint.difficulty),
             Checkpoint().c_str());
  else if (checkpoint.hasCheckpoint)
    snprintf(b, sizeof b, "M%02d %s \xE2\x80\x94 %s", checkpoint.mission + 1, DifficultyShort(checkpoint.difficulty),
             Checkpoint().c_str());
  else
    snprintf(b, sizeof b, "Main menu %s", info.valid ? DifficultyShort(info.highestDifficulty) : "");
  std::string r = b;
  while (!r.empty() && r.back() == ' ') r.pop_back();
  return r;
}

std::string Summary::Difficulty() const {
  if (checkpoint.hasCheckpoint) return DifficultyName(checkpoint.difficulty);
  if (info.valid) return std::string("highest ") + DifficultyName(info.highestDifficulty);
  return "-";
}

std::string Summary::TimePlayed() const {
  double t = profile.Stat("GENERAL_TOTALGAMETIME", info.valid ? info.timePlayed : 0);
  long long s = (long long)t;
  char b[32];
  snprintf(b, sizeof b, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
  return b;
}

namespace {
const char* kItemStats[][2] = {
    {"red_orbs", "Orb_Red"},
    {"gold_orbs", "Orb_Gold"},
    {"vital_star_small", "VitalStar_Small"},
    {"vital_star_large", "VitalStar_Large"},
    {"devil_trigger_star_small", "DevilTriggerStar_Small"},
    {"devil_trigger_star_large", "DevilTriggerStar_Large"},
    {"health_cross_fragments_found", "COLLECTIBLES_BLUEORBFRAGMENTS"},
    {"health_crosses_bought", "COLLECTIBLES_BLUEORB"},
    {"dt_cross_fragments_found", "COLLECTIBLES_PURPLEORBFRAGMENTS"},
    {"dt_crosses_bought", "COLLECTIBLES_PURPLEORB"},
    {"health_cross_fragments_held", "OrbFragment_Blue"},
    {"dt_cross_fragments_held", "OrbFragment_Purple"},
    {"mundus_keys", "COLLECTIBLES_MUNDUSKEYS_TOTAL"},
};
}

std::string SummaryJson(const Summary& s) {
  using nlohmann::ordered_json;
  ordered_json j;
  const CheckpointData& c = s.checkpoint;
  if (c.hasCheckpoint) {
    j["mission"] = c.mission + 1;
    j["mission_name"] = MissionName(c.mission);
    j["difficulty"] = c.difficulty;
    j["difficulty_name"] = DifficultyName(c.difficulty);
    j["checkpoint"] = {{"map", c.map}, {"restart_point", c.name}};
    char b[48];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", c.year, c.month, c.day, c.secondsOfDay / 3600,
             c.secondsOfDay / 60 % 60);
    j["checkpoint"]["saved"] = b;
  } else {
    j["mission"] = nullptr;
    j["checkpoint"] = nullptr;
  }
  if (s.info.valid) {
    j["highest_mission"] = s.info.highestMission;
    j["highest_difficulty"] = s.info.highestDifficulty;
  }
  j["time_played_s"] = (long long)s.profile.Stat("GENERAL_TOTALGAMETIME", s.info.timePlayed);
  const ProfileData& p = s.profile;
  j["missions_completed"] = std::vector<int>(p.completed, p.completed + 7);
  j["secret_missions_completed"] = p.secretCompleted;
  // MAXHEALTH_PLAYER / MAXTRIGGERPOINTS_PLAYER = what HP/DT ups added on top of the base bar (caps 1000 / 1800).
  j["hp_bonus"] = p.Stat("MAXHEALTH_PLAYER");
  j["dt_bonus"] = p.Stat("MAXTRIGGERPOINTS_PLAYER");
  j["skills_count"] = p.skills.size();
  j["skills"] = p.skills;
  j["xp"] = p.xp;
  j["upgrade_points"] = {{"total", p.pointsTotal}, {"unspent", p.pointsUnspent}};
  ordered_json story = ordered_json::array();
  for (int id : p.abilities) story.push_back(StoryItemName(id));
  j["story_items"] = story;
  j["story_item_ids"] = p.abilities;
  ordered_json items;
  for (auto& kv : kItemStats) items[kv[0]] = (long long)p.Stat(kv[1]);
  j["items"] = items;
  return j.dump(1);
}

std::vector<std::string> SummaryLines(const Summary& s) {
  std::vector<std::string> out;
  char b[512];
  const ProfileData& p = s.profile;
  const CheckpointData& c = s.checkpoint;
  if (c.hasCheckpoint) {
    snprintf(b, sizeof b, "%s on %s, checkpoint %s / %s", s.Mission().c_str(), DifficultyName(c.difficulty),
             c.map.c_str(), c.name.c_str());
  } else {
    snprintf(b, sizeof b, "No mission checkpoint (starts from the main menu)%s%s", s.info.valid ? ", highest: " : "",
             s.info.valid ? DifficultyName(s.info.highestDifficulty) : "");
  }
  out.push_back(b);
  snprintf(b, sizeof b, "Time played %s, missions completed %d/%d/%d/%d/%d/%d/%d (Human..HAH), secret %d/%d",
           s.TimePlayed().c_str(), p.completed[0], p.completed[1], p.completed[2], p.completed[3], p.completed[4],
           p.completed[5], p.completed[6], p.secretCompleted, p.secretTotal);
  out.push_back(b);
  snprintf(b, sizeof b, "HP ups +%.0f of 1000, DT ups +%.0f of 1800, skills %zu, upgrade points %d (unspent %d), XP %d",
           p.Stat("MAXHEALTH_PLAYER"), p.Stat("MAXTRIGGERPOINTS_PLAYER"), p.skills.size(), p.pointsTotal,
           p.pointsUnspent, p.xp);
  out.push_back(b);
  std::string story;
  for (int id : p.abilities) story += std::string(story.empty() ? "" : ", ") + StoryItemName(id);
  out.push_back("Story items (" + std::to_string(p.abilities.size()) + "): " + (story.empty() ? "none" : story));
  snprintf(b, sizeof b,
           "Red orbs %.0f, gold orbs %.0f, vital stars S%.0f/L%.0f, DT stars S%.0f/L%.0f, HP/DT fragments %.0f/%.0f",
           p.Stat("Orb_Red"), p.Stat("Orb_Gold"), p.Stat("VitalStar_Small"), p.Stat("VitalStar_Large"),
           p.Stat("DevilTriggerStar_Small"), p.Stat("DevilTriggerStar_Large"),
           p.Stat("COLLECTIBLES_BLUEORBFRAGMENTS"), p.Stat("COLLECTIBLES_PURPLEORBFRAGMENTS"));
  out.push_back(b);
  return out;
}

// ------------------------------------------------------------------ zip (stored)
Bytes ZipStore(const std::vector<std::pair<std::string, Bytes>>& files) {
  Bytes out, central;
  for (auto& [name, data] : files) {
    uint32_t crc = Crc32(data.data(), data.size()), off = (uint32_t)out.size(), size = (uint32_t)data.size();
    auto common = [&](Bytes& b) {
      PutU16(b, 20);      // version needed
      PutU16(b, 0x0800);  // flags: UTF-8 names
      PutU16(b, 0);       // stored
      PutU16(b, 0);       // time
      PutU16(b, 0x21);    // date 1980-01-01
      PutU32(b, crc);
      PutU32(b, size);
      PutU32(b, size);
      PutU16(b, (uint16_t)name.size());
      PutU16(b, 0);  // extra
    };
    PutU32(out, 0x04034B50);
    common(out);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
    PutU32(central, 0x02014B50);
    PutU16(central, 20);  // made by
    common(central);
    PutU16(central, 0);  // comment
    PutU16(central, 0);  // disk
    PutU16(central, 0);  // internal attrs
    PutU32(central, 0);  // external attrs
    PutU32(central, off);
    central.insert(central.end(), name.begin(), name.end());
  }
  uint32_t cdOff = (uint32_t)out.size();
  out.insert(out.end(), central.begin(), central.end());
  PutU32(out, 0x06054B50);
  PutU16(out, 0);
  PutU16(out, 0);
  PutU16(out, (uint16_t)files.size());
  PutU16(out, (uint16_t)files.size());
  PutU32(out, (uint32_t)central.size());
  PutU32(out, cdOff);
  PutU16(out, 0);
  return out;
}

bool ZipRead(const Bytes& zip, std::vector<std::pair<std::string, Bytes>>& files, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  if (zip.size() < 22) return fail("not a zip file");
  size_t eocd = std::string::npos;
  for (size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i <= 22 + 0xFFFF;)
    if (U32(zip, i) == 0x06054B50) {
      eocd = i;
      break;
    }
  if (eocd == std::string::npos) return fail("not a zip file");
  uint16_t n = (uint16_t)U32(zip, eocd + 10);
  size_t p = U32(zip, eocd + 16);
  for (int i = 0; i < n; ++i) {
    if (U32(zip, p) != 0x02014B50) return fail("damaged zip (central directory)");
    uint16_t method = (uint16_t)U32(zip, p + 10);
    uint32_t crc = U32(zip, p + 16), csize = U32(zip, p + 20), usize = U32(zip, p + 24);
    uint16_t nameLen = (uint16_t)U32(zip, p + 28), extra = (uint16_t)U32(zip, p + 30),
             comment = (uint16_t)U32(zip, p + 32);
    uint32_t local = U32(zip, p + 42);
    if (p + 46 + nameLen > zip.size()) return fail("damaged zip");
    std::string name((const char*)&zip[p + 46], nameLen);
    p += 46 + nameLen + extra + comment;
    if (method != 0 || csize != usize)
      return fail("compressed zip entries are not supported (repacked package?) - use the original .dmcpt file");
    if (U32(zip, local) != 0x04034B50) return fail("damaged zip (local header)");
    size_t data = local + 30 + (uint16_t)U32(zip, local + 26) + (uint16_t)U32(zip, local + 28);
    if (data + usize > zip.size()) return fail("damaged zip (truncated)");
    Bytes b(zip.begin() + data, zip.begin() + data + usize);
    if (Crc32(b.data(), b.size()) != crc) return fail("damaged zip (CRC)");
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    files.emplace_back(name, std::move(b));
  }
  return true;
}

// ------------------------------------------------------------------ package
std::string SafeFileName(const std::string& name) {
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char ch = (unsigned char)name[i];
    if (ch == 0xE2 && i + 2 < name.size() && (unsigned char)name[i + 1] == 0x80 &&
        ((unsigned char)name[i + 2] == 0x93 || (unsigned char)name[i + 2] == 0x94)) {  // en/em dash
      out += '-';
      i += 2;
    } else if (ch < 0x20 || strchr("<>:\"/\\|?*", ch)) {
      out += '_';
    } else {
      out += (char)ch;  // UTF-8 letters are fine in Windows file names
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  size_t st = out.find_first_not_of(' ');
  out = st == std::string::npos ? "" : out.substr(st);
  if (out.size() > 80) out.resize(80);
  return out.empty() ? "save" : out;
}

bool RenamePackage(const Bytes& zip, const std::string& name, Bytes& out, std::string* err) {
  SlotFiles check;
  Manifest m;
  if (!ReadPackage(zip, check, m, err)) return false;  // never rewrite a damaged package
  std::vector<std::pair<std::string, Bytes>> files;
  if (!ZipRead(zip, files, err)) return false;
  for (auto& [fname, data] : files) {
    if (fname != "manifest.json") continue;
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(data.begin(), data.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      if (err) *err = "manifest.json is damaged";
      return false;
    }
    nlohmann::ordered_json n;  // "name" goes right after the format fields, the rest keeps its order
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "name") continue;
      n[it.key()] = it.value();
      if (it.key() == "format_version") n["name"] = name;
    }
    if (!n.contains("name")) n["name"] = name;
    std::string text = n.dump(2);
    data.assign(text.begin(), text.end());
  }
  out = ZipStore(files);
  SlotFiles again;
  Manifest m2;
  if (!ReadPackage(out, again, m2, err) || m2.name != name) {
    if (err && err->empty()) *err = "renamed package does not verify";
    return false;
  }
  return true;
}

Bytes BuildPackage(const SlotFiles& s, const std::string& name, const std::string& notes,
                   const std::string& toolVersion, const std::string& gameVersion, const std::string& exportedAt,
                   int sourceSlot) {
  using nlohmann::ordered_json;
  std::vector<std::pair<std::string, Bytes>> files;
  ordered_json hashes;
  for (int k = 0; k < KindCount; ++k) {
    if (!s.Has((Kind)k)) continue;
    Bytes d = s.dec[k];
    SetAccount(d, 0);  // the package is not bound to anyone
    hashes[KindName((Kind)k)] = {{"size", d.size()}, {"sha256", Sha256Hex(d)}};
    files.emplace_back(KindName((Kind)k), std::move(d));
  }
  ordered_json m;
  m["format"] = "dmcpt";
  m["format_version"] = kFormatVersion;
  m["name"] = name;
  m["tool"] = "DmC Practice Tool";
  m["tool_version"] = toolVersion;
  m["game_version"] = gameVersion;
  m["exported_at"] = exportedAt;
  m["source_slot"] = sourceSlot + 1;
  m["notes"] = notes;
  m["files_stored"] = "decrypted, Steam account id zeroed";
  m["files"] = hashes;
  m["summary"] = ordered_json::parse(SummaryJson(Summarize(s)));
  std::string text = m.dump(2);
  files.emplace_back("manifest.json", Bytes(text.begin(), text.end()));
  return ZipStore(files);
}

bool ReadPackage(const Bytes& zip, SlotFiles& out, Manifest& m, std::string* err) {
  std::vector<std::pair<std::string, Bytes>> files;
  if (!ZipRead(zip, files, err)) return false;
  const Bytes* manifest = nullptr;
  for (auto& [name, data] : files)
    if (name == "manifest.json") manifest = &data;
  if (!manifest) {
    if (err) *err = "manifest.json missing - not a DmC Practice Tool package";
    return false;
  }
  nlohmann::json j = nlohmann::json::parse(manifest->begin(), manifest->end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || j.value("format", "") != "dmcpt") {
    if (err) *err = "manifest.json is damaged";
    return false;
  }
  m.formatVersion = j.value("format_version", 0);
  m.toolVersion = j.value("tool_version", "");
  m.gameVersion = j.value("game_version", "");
  m.exportedAt = j.value("exported_at", "");
  m.notes = j.value("notes", "");
  m.name = j.value("name", "");
  m.sourceSlot = j.contains("source_slot") ? j["source_slot"].dump() : "";
  if (j.contains("summary")) m.summaryJson = j["summary"].dump(1);
  if (m.formatVersion < 1 || m.formatVersion > kFormatVersion) {
    if (err) *err = "package format " + std::to_string(m.formatVersion) + " is not supported by this tool version";
    return false;
  }
  for (int k = 0; k < KindCount; ++k) {
    const char* kn = KindName((Kind)k);
    for (auto& [name, data] : files) {
      if (name != kn) continue;
      std::string want = j["files"].contains(kn) ? j["files"][kn].value("sha256", "") : "";
      std::string have = Sha256Hex(data);
      m.sha256[kn] = have;
      if (want != have) {
        if (err) *err = std::string(kn) + ": hash does not match the manifest (file damaged or edited)";
        return false;
      }
      out.dec[k] = data;
    }
  }
  return CheckSet(out, err);
}

}  // namespace savefile
