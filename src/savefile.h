#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// DmC save files offline: Blowfish, the wrapper header (SHA1 + Steam account id), slot previews and the
// .dmcpt package (stored zip: PROFILE / INFO / CHECKPNT decrypted with the account id zeroed + manifest.json).
// No game dependency: also used by the offline test host. Format notes: research/save_format.md.
namespace savefile {

using Bytes = std::vector<uint8_t>;

enum Kind { Profile, Info, Checkpoint, KindCount };
const char* KindName(Kind k);                    // "PROFILE", "INFO", "CHECKPNT"
std::wstring SlotFileName(int slot, Kind k);     // L"DMC5_SAVE_00_PROFILE" (slot 0-based)

// Blowfish-ECB with the game's key; whole 8-byte blocks, a tail (never in real files) is copied as is.
Bytes Decrypt(const Bytes& enc);
Bytes Encrypt(const Bytes& dec);

constexpr size_t kHeaderSize = 0x24;
struct Header {
  uint32_t version = 0, size = 0, account = 0, magic = 0;
  bool sha1Ok = false;
};
// Decrypted file: u32 1 | SHA1(payload) | u32 size | u32 Steam account id | u32 0x01100001 | payload | pad.
bool ParseHeader(const Bytes& dec, Header& h, std::string* err);
void SetAccount(Bytes& dec, uint32_t account);  // not part of the hash
Bytes Payload(const Bytes& dec);                // empty if the header is bad
Bytes Wrap(const Bytes& payload, uint32_t account);  // header + payload + zero padding to 8

std::string Sha1Hex(const uint8_t* p, size_t n);
std::string Sha256Hex(const Bytes& b);

// ------------------------------------------------------------------ preview
const char* DifficultyName(int d);   // "Dante Must Die"
const char* DifficultyShort(int d);  // "DMD"
const char* MissionName(int m);      // 0-based: "Furnace of Souls"
const char* StoryItemName(int id);   // EDVUpgradeItem: "Osiris (scythe)"...

struct InfoData {
  bool valid = false;      // has the numbers v1.3 shows (v1.0 writes and shows only the text lines)
  bool hasText = false;    // has the text lines v1.0 shows ("Time played : 01:02:03"...)
  int version = -1, timePlayed = -1, highestMission = -1, highestDifficulty = -1;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;
};
struct CheckpointData {
  bool valid = false;      // header parsed
  bool hasCheckpoint = false;  // a mission checkpoint is saved (map name present)
  int difficulty = -1, mission = -1;  // mission 0-based
  int day = 0, month = 0, year = 0, secondsOfDay = 0;
  std::string map, name;   // "CH17_main", "CH18_Desolace"
};
struct ProfileData {
  bool valid = false;
  std::string error;
  int version = -1;
  int completed[7] = {};   // missions completed per difficulty
  int missionsPerDifficulty = 0;
  int secretCompleted = 0, secretTotal = 0;
  std::vector<std::string> skills;  // combat upgrade tags
  int xp = 0, pointsTotal = 0, pointsUnspent = 0;
  std::vector<int> abilities;       // EDVUpgradeItem ids
  bool statsOk = false;
  std::map<std::string, double> stats;  // named stats (bool/int/float/double as double)
  double Stat(const char* name, double def = 0) const;
};
struct Summary {
  InfoData info;
  CheckpointData checkpoint;
  ProfileData profile;
  std::string Mission() const;     // "M17 Furnace of Souls" or "-"
  std::string Checkpoint() const;  // "Server Room" (from "cServerRoom"), "" without a checkpoint
  std::string OneLine() const;     // "M12 Under Siege · Dante Must Die · Server Room"
  std::string Brief() const;       // "M12 · DMD" (one list row)
  std::string DefaultName() const; // "M12 DMD — Server Room": default package name
  std::string Difficulty() const;  // of the checkpoint, else the highest one
  std::string TimePlayed() const;  // "1:02:03"
};
InfoData ParseInfo(const Bytes& payload);
CheckpointData ParseCheckpoint(const Bytes& payload);
ProfileData ParseProfile(const Bytes& payload);
// INFO for a profile that both game versions show: the numbers v1.3 reads and the English text lines v1.0 reads
// (time played from GENERAL_TOTALGAMETIME, highest mission/difficulty like DVGamerProfile.WriteSaveSlotMetadata,
// the given "last played" time). Payload only.
Bytes BuildInfo(const ProfileData& p, int year, int month, int day, int hour, int minute);

// ------------------------------------------------------------------ slot set + package
struct SlotFiles {
  Bytes dec[KindCount];  // decrypted, header included; empty = missing
  bool Has(Kind k) const { return !dec[k].empty(); }
};
// Reads + decrypts + checks the header of each present file. Profile and checkpoint are required.
bool CheckSet(const SlotFiles& s, std::string* err);
Summary Summarize(const SlotFiles& s);

// The summary as JSON (manifest "summary" object) and as menu lines.
std::string SummaryJson(const Summary& s);  // serialized object
std::vector<std::string> SummaryLines(const Summary& s);

struct Manifest {
  int formatVersion = 0;
  std::string name;  // player-given name (format 1 packages from tool 0.3 have none)
  std::string toolVersion, gameVersion, exportedAt, notes, sourceSlot;
  std::map<std::string, std::string> sha256;  // file name in the zip -> hex
  std::string summaryJson;                    // as stored
};
constexpr int kFormatVersion = 1;

// Package = stored zip { PROFILE, INFO, CHECKPNT, manifest.json }. Files are stored decrypted, account 0.
// The manifest hashes cover the save files only, so the name and notes can change without breaking them.
Bytes BuildPackage(const SlotFiles& s, const std::string& name, const std::string& notes,
                   const std::string& toolVersion, const std::string& gameVersion, const std::string& exportedAt,
                   int sourceSlot);
// Same package with another name in the manifest; the save files are copied byte for byte.
bool RenamePackage(const Bytes& zip, const std::string& name, Bytes& out, std::string* err);
// "M12 DMD — Server Room" -> "M12 DMD - Server Room": safe as a Windows file name (no extension).
std::string SafeFileName(const std::string& name);
// Unzips, checks the manifest hashes and the headers. On success files are ready for SetAccount + Encrypt.
bool ReadPackage(const Bytes& zip, SlotFiles& out, Manifest& m, std::string* err);

// Minimal stored-zip helpers (no compression: the files are small and stay inspectable).
Bytes ZipStore(const std::vector<std::pair<std::string, Bytes>>& files);
bool ZipRead(const Bytes& zip, std::vector<std::pair<std::string, Bytes>>& files, std::string* err);

}  // namespace savefile
