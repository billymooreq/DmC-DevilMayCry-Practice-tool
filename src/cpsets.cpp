#include "cpsets.h"

#include <windows.h>

#include <ctime>

#include "json.hpp"
#include "log.h"
#include "nav.h"
#include "realcp.h"
#include "savefile.h"
#include "util.h"
#include "version.h"

using nlohmann::json;
using savefile::Bytes;

namespace cpsets {
namespace {

constexpr const char* kExt = ".dmcptset";

bool ReadAll(const std::wstring& p, Bytes& out) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(h, nullptr), got = 0;
  out.resize(size);
  bool ok = size == 0 || (ReadFile(h, out.data(), size, &got, nullptr) && got == size);
  CloseHandle(h);
  return ok;
}

bool WriteAll(const std::wstring& p, const Bytes& b) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(h, b.data(), (DWORD)b.size(), &put, nullptr) && put == b.size();
  CloseHandle(h);
  return ok;
}

Bytes FromString(const std::string& s) { return Bytes(s.begin(), s.end()); }

std::string Now() {
  time_t t = time(nullptr);
  tm l{};
  localtime_s(&l, &t);
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M", &l);
  return b;
}

// Reads a set: manifest + missions.json + checkpoints/*.chk.
bool Open(const std::wstring& path, SetInfo& info, std::string* missions, std::vector<Bytes>* chk) {
  Bytes zip;
  std::vector<std::pair<std::string, Bytes>> files;
  std::string err;
  if (!ReadAll(path, zip) || !savefile::ZipRead(zip, files, &err)) {
    info.error = err.empty() ? "cannot read the file" : err;
    return false;
  }
  bool hasList = false;
  for (auto& [name, data] : files) {
    if (name == "manifest.json") {
      try {
        json m = json::parse(std::string(data.begin(), data.end()));
        info.name = m.value("name", "");
        info.created = m.value("created", "");
        info.toolVersion = m.value("tool_version", "");
      } catch (...) {
      }
    } else if (name == "missions.json") {
      hasList = true;
      std::string text(data.begin(), data.end());
      try {
        json j = json::parse(text, nullptr, true, true);
        for (const json& m : j.at("missions")) {
          ++info.missions;
          if (m.contains("points")) info.points += (int)m["points"].size();
        }
      } catch (const std::exception& e) {
        info.error = std::string("point list is broken: ") + e.what();
        return false;
      }
      if (missions) *missions = text;
    } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".chk") == 0) {  // ZipRead drops folders
      ++info.kept;
      if (chk) chk->push_back(data);
    }
  }
  if (!hasList) {
    info.error = "no point list inside";
    return false;
  }
  if (info.name.empty()) info.name = info.file;
  return true;
}

}  // namespace

std::wstring Dir() { return util::InExeDir(L"DmCPT_sets"); }

std::vector<SetInfo> List() {
  std::vector<SetInfo> out;
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW((Dir() + L"\\*.dmcptset").c_str(), &d);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    SetInfo s;
    s.path = Dir() + L"\\" + d.cFileName;
    s.file = util::WideToUtf8(d.cFileName);
    s.ok = Open(s.path, s, nullptr, nullptr);
    out.push_back(s);
  } while (FindNextFileW(h, &d));
  FindClose(h);
  return out;
}

bool Export(const std::string& name, std::string* msg) {
  std::string clean = savefile::SafeFileName(name.empty() ? "My checkpoints" : name);
  std::vector<std::pair<std::string, Bytes>> files;
  json manifest = {{"format", "dmcpt-checkpoint-set"}, {"version", 1}, {"name", name.empty() ? clean : name},
                   {"created", Now()}, {"tool_version", kToolVersion}, {"game_version", GetGameVersion().name}};
  files.push_back({"manifest.json", FromString(manifest.dump(2))});
  files.push_back({"missions.json", FromString(nav::CatalogText())});
  int kept = 0;
  for (const realcp::Entry& e : realcp::List()) {
    Bytes b;
    if (!ReadAll(e.file, b)) continue;
    files.push_back({"checkpoints/" + savefile::SafeFileName(e.map) + "/" + savefile::SafeFileName(e.name) + ".chk", b});
    ++kept;
  }
  CreateDirectoryW(Dir().c_str(), nullptr);
  std::wstring path = Dir() + L"\\" + util::Utf8ToWide(clean) + util::Utf8ToWide(kExt);
  if (!WriteAll(path, savefile::ZipStore(files))) {
    if (msg) *msg = "Could not write the file.";
    return false;
  }
  LOG("cpsets: exported '%s' (%d real checkpoints) -> %s", clean.c_str(), kept, util::WideToUtf8(path).c_str());
  if (msg) *msg = "Saved \"" + clean + kExt + "\" in the DmCPT_sets folder. Send this file to a friend.";
  return true;
}

bool Import(const std::wstring& path, std::string* msg) {
  SetInfo info;
  info.file = util::WideToUtf8(path.substr(path.find_last_of(L'\\') + 1));
  std::string list;
  std::vector<Bytes> chk;
  if (!Open(path, info, &list, &chk)) {
    if (msg) *msg = "Can't use this file: " + info.error;
    return false;
  }
  // the set's name becomes the list's name (shown as "in use"); no backups: the built-in list is always there
  try {
    json j = json::parse(list, nullptr, true, true);
    j["list_name"] = info.name;
    list = j.dump(1);
  } catch (...) {
  }
  std::string err;
  if (!nav::ReplaceCatalog(list, &err)) {
    if (msg) *msg = "Can't use this file: " + err;
    return false;
  }
  int added = 0;
  for (const Bytes& b : chk)
    if (realcp::AddFile(b, &err)) ++added;
  realcp::Refresh();
  LOG("cpsets: imported '%s': %d points, %d/%zu real checkpoints", info.name.c_str(), info.points, added, chk.size());
  if (msg)
    *msg = "Now using \"" + info.name + "\": " + std::to_string(info.points) + " points, " + std::to_string(added) +
           " real checkpoints.";
  return true;
}

}  // namespace cpsets
