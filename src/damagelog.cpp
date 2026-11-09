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
#include "ui.h"
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
std::atomic<bool> g_hideInGame{false};  // "Hide log in-game": not drawn while the menu is closed (still counting)
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
  g_hideInGame = j.value("hide_in_game", false);
}

nlohmann::json SaveJson() {
  std::lock_guard<std::mutex> lk(g_mu);
  nlohmann::json j = {{"filter", g_filter}, {"tab", g_tab}, {"merge_weak_spot", g_mergeWeak},
                      {"hide_in_game", g_hideInGame.load()}};
  if (g_w > 0) {
    j["x"] = g_x;
    j["y"] = g_y;
    j["w"] = g_w;
    j["h"] = g_h;
  }
  return j;
}

// All text of the log window: opaque white (user, #14: grey / red / faded text was hard to read over the game).
constexpr ImU32 kLogText = IM_COL32(255, 255, 255, 255);

void Draw(bool menuOpen) {
  if (!features::Get("damage_log").on) return;
  if (g_hideInGame && !menuOpen) return;  // still counting: every hit is there when the menu opens
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 ds = io.DisplaySize;
  if (ds.x <= 0 || ds.y <= 0) return;
  float s = ui::Screen() * ui::kBoost;
  auto P = [s](float css) { return css * s; };
  static ImVec2 placedFor;
  static bool placed = false;
  if (!placed || placedFor.x != ds.x || placedFor.y != ds.y) {
    placed = true;
    placedFor = ds;
    ImVec2 size = g_w > 0 ? ImVec2(g_w * ds.x, g_h * ds.y) : ImVec2(P(420), P(340));
    ImVec2 pos = g_x >= 0 ? ImVec2(g_x * ds.x, g_y * ds.y) : ImVec2(ds.x - size.x - P(20), ds.y * 0.30f);  // right, below the style meter
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  }
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
  if (!menuOpen) fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::SetNextWindowSizeConstraints(ImVec2(P(300), P(120)), ds);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, menuOpen ? IM_COL32(8, 8, 10, 220) : IM_COL32(8, 8, 10, 150));
  ImFont* mono = ui::Get(ui::F::Mono, P(13));
  ImGui::PushFont(mono);
  bool open = ImGui::Begin("##DamageLog", nullptr, fl);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
  if (open) {
    ImGui::SetWindowFontScale(P(13) / mono->FontSize);
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float W = sz.x;
    // header: DAMAGE LOG ............ All hits · Hits
    float hpx = P(13);
    ui::Text(dl, ui::F::Title, hpx, ImVec2(np.x + P(10), np.y + P(6)), kLogText, "DAMAGE LOG", hpx * 0.14f);
    static const char* kFilters[] = {"All hits", "Dante's hits", "Hits on Dante"};
    std::string mode = std::string(kFilters[g_filter]) + " \xC2\xB7 " + (g_tab == 0 ? "Hits" : "Moves");
    float mpx = P(11);
    ImVec2 ms = ui::Measure(ui::F::Mono, mpx, mode.c_str());
    ui::Text(dl, ui::F::Mono, mpx, ImVec2(np.x + W - P(10) - ms.x, np.y + P(6) + (hpx - mpx) * 0.6f), kLogText, mode.c_str());
    float hy = np.y + P(6) + hpx * 1.2f + P(3);
    dl->AddRectFilled(ImVec2(np.x, hy), ImVec2(np.x + W, hy + std::max(1.0f, std::round(P(2)))), ui::kAccent);
    ImGui::SetCursorScreenPos(ImVec2(np.x, hy + P(2)));
    // columns TIME | MOVE | DMG | HP (Moves: HITS | MOVE | TOTAL | PER HIT), a thin line between the columns. TIME, DMG
    // and HP are only as wide as what they hold (user: much narrower), MOVE gets the rest.
    std::vector<Row> rows;
    std::vector<MoveSum> sum;
    if (g_tab == 0) rows = Rows(g_filter, g_mergeWeak); else sum = Summary(g_filter);
    float px = P(13), rowH = P(22), pad = P(8), gap = P(10), hs = P(11);
    float cw = ui::Measure(ui::F::Mono, px, "0").x;
    auto headW = [&](const char* t) { return ui::Measure(ui::F::MonoBold, hs, t, hs * 0.1f).x; };
    size_t nT = 1, nD = 1, nH = 1;
    if (g_tab == 0) {
      for (const Row& r : rows) {
        char tb[32];
        snprintf(tb, sizeof tb, "%.1f", r.e->t);
        nT = std::max(nT, strlen(tb));
        nD = std::max(nD, Num(r.dmg).size());
        if (r.e->hpBefore >= 0 && r.hpAfter >= 0) nH = std::max(nH, Num(r.e->hpBefore).size() + 3 + Num(r.hpAfter).size());
      }
    } else {
      for (const MoveSum& m : sum) {
        nT = std::max(nT, std::to_string(m.hits).size());
        nD = std::max(nD, Num(m.total).size());
        nH = std::max(nH, Num(m.hits ? m.total / m.hits : 0).size());
      }
    }
    float cT = std::max(cw * nT, headW(g_tab == 0 ? "TIME" : "HITS"));
    float cD = std::max(cw * nD, headW(g_tab == 0 ? "DMG" : "TOTAL"));
    float cH = std::max(cw * nH, headW(g_tab == 0 ? "HP" : "PER HIT"));
    float xT = np.x + pad, xM = xT + cT + gap, xH = np.x + W - pad - cH, xD = xH - gap - cD;
    float mW = std::max(P(20), xD - gap - xM);
    auto head = [&](float y, const char* a, const char* b, const char* c, const char* d) {
      float sp = hs;
      ui::Text(dl, ui::F::MonoBold, sp, ImVec2(xT, y), kLogText, a, sp * 0.1f);
      ui::Text(dl, ui::F::MonoBold, sp, ImVec2(xM, y), kLogText, b, sp * 0.1f);
      ImVec2 cs = ui::Measure(ui::F::MonoBold, sp, c, sp * 0.1f);
      ui::Text(dl, ui::F::MonoBold, sp, ImVec2(xD + cD - cs.x, y), kLogText, c, sp * 0.1f);
      ui::Text(dl, ui::F::MonoBold, sp, ImVec2(xH, y), kLogText, d, sp * 0.1f);
    };
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    float headH = P(4) + P(11) * 1.3f + P(4);
    size_t n = g_tab == 0 ? rows.size() : sum.size();
    if (n == 0) {
      ui::Text(dl, ui::F::Mono, px, ImVec2(np.x + pad, c0.y + P(6)), kLogText, "No hits yet.");
    } else {
      if (g_tab == 0) head(c0.y + P(4), "TIME", "MOVE", "DMG", "HP");
      else head(c0.y + P(4), "HITS", "MOVE", "TOTAL", "PER HIT");
      ImGui::SetCursorScreenPos(ImVec2(np.x, c0.y + headH));
      ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::max(3.0f, P(5)));
      ImGui::BeginChild("##rows", ImVec2(W, std::max(P(20), np.y + sz.y - (c0.y + headH) - P(4))), 0,
                        menuOpen ? 0 : ImGuiWindowFlags_NoInputs);
      ImGui::PopStyleVar();
      // rows touch each other: without this the default item spacing is added under every row, the clipper (told
      // rowH) under-counts the list and the last rows can never be scrolled to
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
      ImDrawList* rl = ImGui::GetWindowDrawList();
      ImGuiListClipper clip;
      clip.Begin((int)n, rowH);
      while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          ImVec2 rp = ImGui::GetCursorScreenPos();
          ImGui::Dummy(ImVec2(W, rowH));
          bool hov = menuOpen && ImGui::IsItemHovered();
          if (i % 2) rl->AddRectFilled(rp, ImVec2(rp.x + W, rp.y + rowH), IM_COL32(255, 255, 255, 12));  // stripes
          float lw = std::max(1.0f, std::round(P(1)));
          for (float lx : {xM - gap * 0.5f, xD - gap * 0.5f, xH - gap * 0.5f})
            rl->AddRectFilled(ImVec2(std::floor(lx), rp.y), ImVec2(std::floor(lx) + lw, rp.y + rowH), IM_COL32(0x3A, 0x3A, 0x3A, 255));
          float ty = std::floor(rp.y + (rowH - px) * 0.5f);
          if (g_tab == 1) {
            const MoveSum& m = sum[i];
            char a[16], t[32], ph[32];
            snprintf(a, sizeof a, "%d", m.hits);
            snprintf(t, sizeof t, "%s", Num(m.total).c_str());
            snprintf(ph, sizeof ph, "%s", Num(m.hits ? m.total / m.hits : 0).c_str());
            ui::Text(rl, ui::F::Mono, px, ImVec2(xT, ty), kLogText, a);
            ui::Text(rl, ui::F::Mono, px, ImVec2(xM, ty), kLogText, ui::Ellipsize(ui::F::Mono, px, m.move, mW).c_str());
            ImVec2 ts = ui::Measure(ui::F::Mono, px, t);
            ui::Text(rl, ui::F::Mono, px, ImVec2(xD + cD - ts.x, ty), kLogText, t);
            ui::Text(rl, ui::F::Mono, px, ImVec2(xH, ty), kLogText, ph);
            continue;
          }
          const Row& r = rows[i];
          const Entry& e = *r.e;
          char tb[32];
          snprintf(tb, sizeof tb, "%.1f", e.t);  // seconds since the log started
          ui::Text(rl, ui::F::Mono, px, ImVec2(xT, ty), kLogText, tb);
          // move: Dante's hits as "move > victim" (user, #14: who was hit), other hits as "attacker > victim"
          std::string mv = e.who == kByDante ? (e.victim.empty() ? e.move : e.move + " \xE2\x80\xBA " + e.victim)
                                             : e.attacker + " \xE2\x80\xBA " + e.victim;
          bool weak = r.weak > 0 && !e.weak;
          const char* ws = " + weak spot";
          float wsW = weak ? ui::Measure(ui::F::MonoBold, px, ws).x : 0;
          std::string shown = ui::Ellipsize(ui::F::Mono, px, mv, mW - wsW);
          ui::Text(rl, ui::F::Mono, px, ImVec2(xM, ty), kLogText, shown.c_str());
          if (weak)
            ui::Text(rl, ui::F::MonoBold, px, ImVec2(xM + ui::Measure(ui::F::Mono, px, shown.c_str()).x, ty), kLogText, ws);
          std::string d = Num(r.dmg);
          bool red = weak || e.weak;
          ui::F df = red ? ui::F::MonoBold : ui::F::Mono;
          ImVec2 dsz = ui::Measure(df, px, d.c_str());
          ui::Text(rl, df, px, ImVec2(xD + cD - dsz.x, ty), kLogText, d.c_str());
          std::string hp = e.hpBefore >= 0 && r.hpAfter >= 0 ? Num(e.hpBefore) + " \xE2\x80\xBA " + Num(r.hpAfter) : "-";
          ui::Text(rl, ui::F::Mono, px, ImVec2(xH, ty), kLogText,
                   ui::Ellipsize(ui::F::Mono, px, hp, cH).c_str());
          if (hov) {
            std::string tip = e.attacker + " > " + e.victim + (e.moveId.empty() ? "" : "\n" + e.moveId) + "\n";
            if (weak)
              tip += "Hit: " + Num(r.dmg - r.weak) + "   Weak spot: " + Num(r.weak) + "   Total: " + Num(r.dmg);
            else
              tip += "Damage with the tool's multipliers: " + Num(e.out) + "   game's own: " + Num(e.game) +
                     (e.final >= 0 ? "   after armour: " + Num(e.final) : "");
            ImGui::SetTooltip("%s", tip.c_str());
          }
        }
      // follow the newest hit unless the player scrolled up
      if (g_tab == 0 && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - rowH * 2) ImGui::SetScrollHereY(1.0f);
      ImGui::PopStyleVar();
      ImGui::EndChild();
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

void AddTestRows() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_t0 < 0) g_t0 = Now();
  double t = Now() - g_t0;
  auto add = [&](uint8_t who, const char* a, const char* v, const char* move, float dmg, float before, float after,
                 bool weak, double dt) {
    Entry e;
    e.seq = ++g_seq;
    e.t = t + dt;
    e.who = who;
    e.attacker = a;
    e.victim = v;
    e.move = move;
    e.moveId = "TEST";
    e.game = e.out = dmg;
    e.hpBefore = before;
    e.hpAfter = after;
    e.weak = weak;
    g_log.push_back(e);
    if (g_log.size() > kMaxEntries) g_log.pop_front();
  };
  add(kByDante, "Dante", "TEST Boss", "TEST Aerial Rave", 285, 5000, 4715, false, 0.0);
  add(kByDante, "Dante", "TEST Boss", "Weak spot", 420, 4715, 4295, true, 0.02);
  add(kOnDante, "TEST Stygian", "Dante", "TEST Pounce", 40, 600, 560, false, 0.5);
  add(kByDante, "Dante", "TEST Stygian", "TEST Rebellion Combo A with a very long move name that does not fit", 38,
      600, 562, false, 0.9);
  LOG("damagelog: %d test rows added", 4);
}

int Filter() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_filter;
}
int View() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_tab;
}
void SetView(int v) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_tab = std::clamp(v, 0, 1);
  MarkConfigDirty();
}
bool MergeWeak() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_mergeWeak;
}
bool HideInGame() { return g_hideInGame; }
void SetHideInGame(bool on) {
  g_hideInGame = on;
  MarkConfigDirty();
}
size_t Count() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_log.size();
}

}  // namespace damagelog
