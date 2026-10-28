// Damage log window. Research (where the numbers and move names come from): research/enemies.md.
#include "damagelog.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>

#include "config.h"
#include "enemies.h"
#include "features.h"
#include "imgui.h"
#include "log.h"
#include "ue3/ue3.h"
#include "util.h"

namespace damagelog {
namespace {

enum Who : uint8_t { kOther = 0, kByDante = 1, kOnDante = 2 };

struct Entry {
  uint64_t seq = 0;
  double t = 0;  // seconds since the log started
  uint8_t who = kOther;
  std::string attacker, victim, move, moveId, type;
  float game = 0, out = 0;  // asked by the game / after the tool's multipliers (what the log shows)
  float hpBefore = -1, hpAfter = -1;  // victim's health around the hit (-1 = not a character / not known)
  float final = -1;                   // Contact.DamageAmount after the game's armour, shields, clamps
  bool weak = false;                  // damage the level passes on to a boss for a hit on a weak spot
};

// Hits being processed by the game right now (OnHit before the call, AfterHit after it; a hit can cause another).
struct Open {
  uint64_t seq;
  uintptr_t contact, victim;
};
std::vector<Open> g_open;  // game thread
// Hits on a boss: its bar moves after the hit (the phase a frame later, a bar the level script drives up to a few
// frames later), so "after" is read 0.3 s later, or taken from the next hit on the same boss if that comes first.
struct Late {
  uint64_t seq;
  uintptr_t victim;
  ULONGLONG due;
};
std::vector<Late> g_late;  // game thread, under g_mu

std::mutex g_mu;
std::deque<Entry> g_log;

Entry* BySeq(uint64_t seq) {  // under g_mu
  if (g_log.empty() || seq < g_log.front().seq || seq > g_log.back().seq) return nullptr;
  return &g_log[(size_t)(seq - g_log.front().seq)];
}
uint64_t g_seq = 0;
double g_t0 = -1;
std::atomic<bool> g_on{false};
int g_filter = 0;  // 0 all, 1 Dante's hits, 2 hits on Dante
bool g_mergeWeak = true;  // weak spot damage added to the hit that caused it (one row) or shown on its own row
int g_tab = 0;     // 0 hits, 1 moves
float g_x = -1, g_y = 0, g_w = 0, g_h = 0;  // window, fractions of the screen
std::string g_msg;

// ---- move names: the game's own list, DevilGame/Localization/INT/DevilGame.int [DojoComboNames]
std::map<std::string, std::string> g_combo;  // lower-case key -> name
bool g_comboLoaded = false;

std::string Lower(std::string s) {
  for (char& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

void LoadComboNames() {
  if (g_comboLoaded) return;
  g_comboLoaded = true;
  std::ifstream f(util::InExeDir(L"..\\..\\DevilGame\\Localization\\INT\\DevilGame.int"));
  bool in = false;
  for (std::string line; std::getline(f, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line[0] == '[') {
      in = line == "[DojoComboNames]";
      continue;
    }
    size_t eq = line.find('=');
    if (in && eq != std::string::npos && eq > 0) g_combo[Lower(line.substr(0, eq))] = line.substr(eq + 1);
  }
  LOG("damagelog: %zu move names from DevilGame.int", g_combo.size());
}

const std::string* Combo(const std::string& key) {
  if (key.empty()) return nullptr;
  auto it = g_combo.find(Lower(key));
  if (it == g_combo.end()) it = g_combo.find(Lower("Combo_" + key));
  return it == g_combo.end() ? nullptr : &it->second;
}

// "Rebellion_Stinger_Contact" -> "Rebellion Stinger"
std::string Pretty(std::string s) {
  for (const char* suffix : {"_Contact", "Contact", "_Def", "_ContactDef"})
    if (s.size() > strlen(suffix) && s.compare(s.size() - strlen(suffix), strlen(suffix), suffix) == 0)
      s.resize(s.size() - strlen(suffix));
  for (char& c : s)
    if (c == '_') c = ' ';
  return s;
}

std::string LastSegment(const std::string& path) {
  size_t d = path.find_last_of(".:");
  return d == std::string::npos ? path : path.substr(d + 1);
}

// Dante's hits carry the name the game shows for the move (DVCombat_Contact.StyleDisplayName: "Stinger", "Drive",
// "Hacker" for a Rebellion combo, "Ebony and Ivory"...) and the attack id (StyleAttackName:
// "Melee_Rebellion_ComboA_2" = 2nd hit of that combo). Enemy hits have neither; their contact definition path names
// the action: "Rage_Combat.Actions.R_Attack_Pounce:DVCombat_StrikeDef_1.DVCombat_ContactDef_18".
void MoveOf(uintptr_t contact, uintptr_t attacker, std::string& name, std::string& id) {
  LoadComboNames();
  std::string disp = game::GetName(contact, "StyleDisplayName"), attack = game::GetName(contact, "StyleAttackName");
  std::string def = game::GetStr(contact, "DefinitionPathName");
  id = !attack.empty() ? attack + "  |  " + def : def;
  if (!disp.empty() || !attack.empty()) {
    const std::string* n = Combo(disp);
    if (!n) n = Combo(attack);
    name = n ? *n : !disp.empty() ? disp : Pretty(attack);
    // a combo hit: "Hacker 2"
    size_t u = attack.find_last_of('_');
    if (attack.find("Combo") != std::string::npos && u != std::string::npos && u + 2 == attack.size() &&
        isdigit((unsigned char)attack.back()))
      name += " " + attack.substr(u + 1);
    return;
  }
  if (def.find("SeqAct_TakeDamage") != std::string::npos) {
    name = "Weak spot";
    return;
  }
  size_t colon = def.find(':');
  std::string action = def.substr(0, colon);
  size_t dot = action.find(".Actions.");
  if (colon != std::string::npos && dot != std::string::npos) {
    action = action.substr(dot + 9);
    // "R_Attack_Pounce" -> "Attack Pounce", "FD_Spear" -> "Spear", "Action_Dante_X" -> "X"
    for (const char* pre : {"Action_Dante_", "Action_"})
      if (action.rfind(pre, 0) == 0) action = action.substr(strlen(pre));
    size_t u = action.find('_');
    if (u != std::string::npos && u <= 3) action = action.substr(u + 1);
    name = Pretty(action);
    return;
  }
  // traps and level objects: their own class ("DVGunShardsActor" -> "GunShards"); a character without a move:
  // its body (charges, landing on someone)
  std::string cls = attacker ? ue3::ClassNameOf(attacker) : "";
  if (cls.find("Pawn") != std::string::npos) {
    name = "Body hit";
    return;
  }
  for (const char* pre : {"DV", "NT"})
    if (cls.rfind(pre, 0) == 0) cls = cls.substr(2);
  if (cls.size() > 5 && cls.compare(cls.size() - 5, 5, "Actor") == 0) cls.resize(cls.size() - 5);
  name = !cls.empty() ? cls : "-";
}

std::string TypeName(uintptr_t contact) {
  uintptr_t t = game::GetObj(contact, "DamageType");
  if (!t) return "-";
  std::string n = ue3::ObjName(t);
  for (const char* pre : {"NTDmgType_", "DVDmgType_", "DmgType_", "NTDamageType_", "DVDamageType_"})
    if (n.rfind(pre, 0) == 0) return n.substr(strlen(pre));
  return n;
}

double Now() {
  static LARGE_INTEGER freq{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)freq.QuadPart;
}

bool Pass(const Entry& e, int filter) { return filter == 0 || (filter == 1 && e.who == kByDante) || (filter == 2 && e.who == kOnDante); }

std::string Clock(double t) {
  char b[32];
  int m = (int)(t / 60);
  snprintf(b, sizeof b, "%02d:%05.2f", m, t - m * 60);
  return b;
}

std::string Num(float v) {
  char b[32];
  snprintf(b, sizeof b, std::fabs(v - std::round(v)) < 0.05f ? "%.0f" : "%.1f", v);
  return b;
}

// One line of the log as shown. With "Add weak spot to the hit" a weak spot entry is folded into the hit just
// before it on the same boss (the level turns that hit into the weak spot damage a frame or two later).
struct Row {
  const Entry* e;
  float dmg, game;  // the hit (+ its weak spot)
  float weak = 0;   // of which weak spot
  float hpAfter;
};
std::vector<Row> Rows(int filter, bool merge) {  // under g_mu
  std::vector<Row> rows;
  rows.reserve(g_log.size());
  for (const Entry& e : g_log) {
    if (!Pass(e, filter)) continue;
    if (merge && e.weak)
      for (size_t i = rows.size(); i-- > 0 && i + 8 > rows.size();) {
        Row& r = rows[i];
        if (r.e->weak || r.e->victim != e.victim || r.e->attacker != e.attacker || e.t - r.e->t > 0.3) continue;
        r.dmg += e.out;
        r.game += e.game;
        r.weak += e.out;
        if (e.hpAfter >= 0) r.hpAfter = e.hpAfter;
        goto merged;
      }
    rows.push_back({&e, e.out, e.game, e.weak ? e.out : 0.0f, e.hpAfter});
  merged:;
  }
  return rows;
}

struct MoveSum {
  std::string move;
  int hits = 0;
  float total = 0;
};
std::vector<MoveSum> Summary(int filter) {
  std::map<std::string, MoveSum> m;
  for (const Row& r : Rows(filter, g_mergeWeak)) {
    MoveSum& s = m[r.e->move];
    s.move = r.e->move;
    ++s.hits;
    s.total += r.dmg;
  }
  std::vector<MoveSum> v;
  for (auto& [k, s] : m) v.push_back(s);
  std::sort(v.begin(), v.end(), [](const MoveSum& a, const MoveSum& b) { return a.total > b.total; });
  return v;
}

std::string Csv(std::string s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string o = "\"";
  for (char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
  return o + "\"";
}

}  // namespace

void SetOn(bool on) { g_on = on; }
bool On() { return g_on; }

void OnHit(const game::World& w, uintptr_t contact, uintptr_t attacker, uintptr_t victim, float gameDamage,
           float outDamage) {
  if (!g_on || (gameDamage <= 0 && outDamage <= 0)) return;  // pushes and touches that do no damage
  Entry e;
  e.who = attacker == w.pawn && victim != w.pawn ? kByDante : victim == w.pawn ? kOnDante : kOther;
  e.attacker = enemies::ActorName(w, attacker);
  e.victim = enemies::ActorName(w, victim);
  MoveOf(contact, attacker, e.move, e.moveId);
  e.type = TypeName(contact);
  e.weak = e.move == "Weak spot";
  e.game = gameDamage;
  e.out = outDamage;
  float mx = 0;
  if (!enemies::Health(victim, &e.hpBefore, &mx)) e.hpBefore = -1;
  double now = Now();
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_t0 < 0) g_t0 = now;
  e.t = now - g_t0;
  e.seq = ++g_seq;
  for (auto it = g_late.begin(); it != g_late.end();) {  // the previous hit on this boss: its "after" is this "before"
    if (it->victim == victim && e.hpBefore >= 0) {
      if (Entry* p = BySeq(it->seq)) p->hpAfter = e.hpBefore;
      it = g_late.erase(it);
    } else {
      ++it;
    }
  }
  if (g_open.size() < 32) g_open.push_back({e.seq, contact, victim});
  g_log.push_back(std::move(e));
  while ((int)g_log.size() > kMaxEntries) g_log.pop_front();
}

void AfterHit(uintptr_t, uintptr_t contact) {
  if (g_open.empty()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  for (size_t i = g_open.size(); i-- > 0;) {
    if (g_open[i].contact != contact) continue;
    Open o = g_open[i];
    g_open.resize(i);  // and anything opened inside it that never closed
    Entry* e = BySeq(o.seq);
    if (!e) return;
    float mx = 0;
    bool boss = false;
    if (e->hpBefore >= 0 && !enemies::Health(o.victim, &e->hpAfter, &mx, &boss)) e->hpAfter = -1;
    if (boss && g_late.size() < 64) g_late.push_back({o.seq, o.victim, GetTickCount64() + 300});
    if (ue3::IsObject(contact)) e->final = game::GetFloat(contact, "DamageAmount", -1.0f);
    return;
  }
}

void OnFrame() {
  if (g_late.empty()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto it = g_late.begin(); it != g_late.end();) {
    if (GetTickCount64() < it->due) {
      ++it;
      continue;
    }
    float mx = 0;
    if (Entry* p = BySeq(it->seq))
      if (!enemies::Health(it->victim, &p->hpAfter, &mx)) p->hpAfter = -1;
    it = g_late.erase(it);
  }
}

void Clear() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_log.clear();
  g_t0 = -1;
  g_msg.clear();
}

void SetMergeWeak(bool on) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_mergeWeak = on;
}

void SetFilter(int f) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_filter = std::clamp(f, 0, 2);
}

bool ExportCsv(std::string* path) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::wstring dir = util::InExeDir(L"DmCPT_logs");
  CreateDirectoryW(dir.c_str(), nullptr);
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf(name, 64, L"\\damage_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  std::wstring file = dir + name;
  std::ofstream f(file, std::ios::trunc | std::ios::binary);
  if (!f) return false;
  f << "time_s,attacker,victim,move,damage,of_which_weak_spot,damage_without_tool_multipliers,hp_before,hp_after,type,"
       "move_id\n";
  for (const Row& r : Rows(g_filter, g_mergeWeak)) {
    const Entry& e = *r.e;
    char t[32];
    snprintf(t, sizeof t, "%.3f", e.t);
    f << t << ',' << Csv(e.attacker) << ',' << Csv(e.victim) << ',' << Csv(e.move) << ',' << Num(r.dmg) << ','
      << (r.weak > 0 ? Num(r.weak) : "") << ',' << Num(r.game) << ',' << (e.hpBefore >= 0 ? Num(e.hpBefore) : "")
      << ',' << (r.hpAfter >= 0 ? Num(r.hpAfter) : "") << ',' << Csv(e.type) << ',' << Csv(e.moveId) << "\n";
  }
  f << "\nmove,hits,total_damage\n";
  for (const MoveSum& s : Summary(g_filter)) f << Csv(s.move) << ',' << s.hits << ',' << Num(s.total) << "\n";
  if (path) *path = util::WideToUtf8(file);
  LOG("damagelog: exported %s", util::WideToUtf8(file).c_str());
  return true;
}

std::vector<std::string> DebugLines(int last) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<std::string> out;
  std::vector<Row> rows = Rows(g_filter, g_mergeWeak);
  size_t from = rows.size() > (size_t)last ? rows.size() - last : 0;
  for (size_t i = from; i < rows.size(); ++i) {
    const Row& r = rows[i];
    const Entry& e = *r.e;
    char b[512];
    snprintf(b, sizeof b, "%s %s -> %s  %s  %s (weak %s, game %s)  HP %s -> %s  %s  id '%s'", Clock(e.t).c_str(),
             e.attacker.c_str(), e.victim.c_str(), e.move.c_str(), Num(r.dmg).c_str(), Num(r.weak).c_str(),
             Num(r.game).c_str(), Num(e.hpBefore).c_str(), Num(r.hpAfter).c_str(), e.type.c_str(), e.moveId.c_str());
    out.push_back(b);
  }
  return out;
}

std::vector<std::string> SummaryLines() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<std::string> out;
  for (const MoveSum& s : Summary(g_filter)) out.push_back(s.move + ": " + std::to_string(s.hits) + " hits, " + Num(s.total));
  return out;
}

void LoadJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_x = j.value("x", -1.0f);
  g_y = j.value("y", 0.0f);
  g_w = j.value("w", 0.0f);
  g_h = j.value("h", 0.0f);
  g_filter = std::clamp(j.value("filter", 0), 0, 2);
  g_tab = std::clamp(j.value("tab", 0), 0, 1);
  g_mergeWeak = j.value("merge_weak_spot", true);
}

nlohmann::json SaveJson() {
  std::lock_guard<std::mutex> lk(g_mu);
  nlohmann::json j = {{"filter", g_filter}, {"tab", g_tab}, {"merge_weak_spot", g_mergeWeak}};
  if (g_w > 0) {
    j["x"] = g_x;
    j["y"] = g_y;
    j["w"] = g_w;
    j["h"] = g_h;
  }
  return j;
}

void Draw(bool menuOpen) {
  if (!features::Get("damage_log").on) return;
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  if (ds.x <= 0 || ds.y <= 0) return;
  float fs = ImGui::GetFontSize();
  static ImVec2 placedFor;
  static bool placed = false;
  if (!placed || placedFor.x != ds.x || placedFor.y != ds.y) {
    placed = true;
    placedFor = ds;
    ImVec2 size = g_w > 0 ? ImVec2(g_w * ds.x, g_h * ds.y) : ImVec2(fs * 40, fs * 16);
    ImVec2 pos = g_x >= 0 ? ImVec2(g_x * ds.x, g_y * ds.y) : ImVec2(ds.x - size.x - fs, ds.y * 0.30f);  // right, below the style meter
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  }
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing;
  if (!menuOpen) fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::SetNextWindowBgAlpha(menuOpen ? 0.92f : 0.55f);
  ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 20, fs * 7), ds);
  if (ImGui::Begin("Damage log", nullptr, fl)) {
    // position / size remembered as fractions of the screen
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 np(std::max(0.0f, std::min(p.x, ds.x - sz.x)), std::max(0.0f, std::min(p.y, ds.y - sz.y)));
    if (np.x != p.x || np.y != p.y) ImGui::SetWindowPos(np);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      float fx = np.x / ds.x, fy = np.y / ds.y, fw = sz.x / ds.x, fh = sz.y / ds.y;
      if (std::fabs(fx - g_x) > 0.001f || std::fabs(fy - g_y) > 0.001f || std::fabs(fw - g_w) > 0.001f ||
          std::fabs(fh - g_h) > 0.001f) {
        g_x = fx, g_y = fy, g_w = fw, g_h = fh;
        MarkConfigDirty();
      }
    }
    int f = g_filter;
    ImGui::RadioButton("All", &f, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Dante's hits", &f, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Hits on Dante", &f, 2);
    if (f != g_filter) {
      g_filter = f;
      MarkConfigDirty();
    }
    if (menuOpen) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) {
        g_log.clear();
        g_t0 = -1;
        g_msg.clear();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Export CSV")) {
        g_mu.unlock();
        std::string path;
        bool ok = ExportCsv(&path);
        g_mu.lock();
        size_t s = path.find("DmCPT_logs");
        g_msg = ok ? "Saved " + (s != std::string::npos ? path.substr(s) : path) : "Could not save the file.";
      }
      ImGui::SetItemTooltip("Saves the hits shown (and the move summary) as a table file in DmCPT_logs next to "
                            "the game.");
    }
    if (menuOpen) {
      if (ImGui::Checkbox("Add weak spot to the hit", &g_mergeWeak)) MarkConfigDirty();
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(fs * 28.0f);
        ImGui::TextUnformatted("Some bosses take extra damage when you hit a weak spot; the game counts it as a "
                               "second hit (\"Weak spot\"). On: it is added to the hit that caused it, one line per "
                               "hit (hover the damage to see both parts). Off: a line of its own.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
    if (!g_msg.empty()) ImGui::TextDisabled("%s", g_msg.c_str());
    if (ImGui::BeginTabBar("dl")) {
      if (ImGui::BeginTabItem("Hits", nullptr, g_tab == 0 && !menuOpen ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (menuOpen && g_tab != 0) g_tab = 0, MarkConfigDirty();
        std::vector<Row> rows = Rows(g_filter, g_mergeWeak);
        ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                             ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("hits", 6, tf)) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, fs * 3.6f);
          ImGui::TableSetupColumn("Who", ImGuiTableColumnFlags_WidthStretch, 2.2f);
          ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthStretch, 1.8f);
          ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
          ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, fs * 5.6f);
          ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
          ImGui::TableHeadersRow();
          ImGuiListClipper clip;
          clip.Begin((int)rows.size());
          while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
              const Row& r = rows[i];
              const Entry& e = *r.e;
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextDisabled("%s", Clock(e.t).c_str());
              ImGui::TableNextColumn();
              ImGui::Text("%s > %s", e.attacker.c_str(), e.victim.c_str());
              ImGui::TableNextColumn();
              if (r.weak > 0 && !e.weak) {
                ImGui::Text("%s + weak spot", e.move.c_str());
              } else {
                ImGui::TextUnformatted(e.move.c_str());
              }
              if (menuOpen && ImGui::IsItemHovered() && !e.moveId.empty()) ImGui::SetTooltip("%s", e.moveId.c_str());
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(Num(r.dmg).c_str());
              if (menuOpen && ImGui::IsItemHovered()) {
                if (r.weak > 0 && !e.weak)
                  ImGui::SetTooltip("Hit: %s\nWeak spot: %s\nTotal: %s\nWithout the tool's damage multipliers: %s",
                                    Num(r.dmg - r.weak).c_str(), Num(r.weak).c_str(), Num(r.dmg).c_str(),
                                    Num(r.game).c_str());
                else
                  ImGui::SetTooltip("Damage of the hit, with the tool's damage multipliers: %s\nWithout them (the "
                                    "game's own value): %s\nAfter the victim's armour / shields: %s",
                                    Num(e.out).c_str(), Num(e.game).c_str(), e.final >= 0 ? Num(e.final).c_str() : "?");
              }
              ImGui::TableNextColumn();
              if (e.hpBefore >= 0 && r.hpAfter >= 0) {
                if (r.hpAfter < e.hpBefore)
                  ImGui::Text("%s > %s", Num(e.hpBefore).c_str(), Num(r.hpAfter).c_str());
                else
                  ImGui::TextDisabled("%s > %s", Num(e.hpBefore).c_str(), Num(r.hpAfter).c_str());
                if (menuOpen && ImGui::IsItemHovered())
                  ImGui::SetTooltip("Health of %s before and after this hit.%s", e.victim.c_str(),
                                    r.hpAfter >= e.hpBefore ? "\nNo change: health locked (Infinite HP, Infinite enemy "
                                                              "HP) or the hit was blocked." : "");
              } else {
                ImGui::TextDisabled("-");
              }
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(e.type.c_str());
            }
          // follow the newest hit unless the player scrolled up
          if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - fs * 2) ImGui::SetScrollHereY(1.0f);
          ImGui::EndTable();
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Moves", nullptr, g_tab == 1 && !menuOpen ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (menuOpen && g_tab != 1) g_tab = 1, MarkConfigDirty();
        std::vector<MoveSum> sum = Summary(g_filter);
        if (ImGui::BeginTable("moves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthStretch, 2.0f);
          ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, fs * 2.8f);
          ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, fs * 3.6f);
          ImGui::TableSetupColumn("Per hit", ImGuiTableColumnFlags_WidthFixed, fs * 3.6f);
          ImGui::TableHeadersRow();
          for (const MoveSum& s : sum) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.move.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.hits);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Num(s.total).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Num(s.hits ? s.total / s.hits : 0).c_str());
          }
          ImGui::EndTable();
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
}

}  // namespace damagelog
