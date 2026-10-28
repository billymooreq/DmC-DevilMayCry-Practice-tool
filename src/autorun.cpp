#include "autorun.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "compat.h"
#include "config.h"
#include "d3d9_hook.h"
#include "features.h"
#include "hotkeys.h"
#include "game.h"
#include "proxy.h"
#include "log.h"
#include "menu.h"
#include "nav.h"
#include "cpsets.h"
#include "damagelog.h"
#include "enemies.h"
#include "state.h"
#include "saveslots.h"
#include "steam_guard.h"
#include "ue3/dumper.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "unlock.h"
#include "util.h"
#include "visuals.h"

namespace autorun {
namespace {

void WaitIdle() {
  for (int i = 0; i < 600 && (dumper::Busy() || unlock::Busy() || slots::Busy()); ++i) Sleep(500);
}

void Dump(dumper::Kind k, const std::string& arg = {}) {
  WaitIdle();
  dumper::RunAsync(k, arg);
  Sleep(300);
  WaitIdle();
}

BOOL CALLBACK CloseGameWindow(HWND h, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(h)) PostMessageW(h, WM_CLOSE, 0, 0);
  return TRUE;
}

std::vector<std::string> LoadScript(const std::wstring& path) {
  std::vector<std::string> cmds;
  std::ifstream f(path);
  if (!f) return cmds;
  for (std::string line; std::getline(f, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line[0] != '#') cmds.push_back(line);
  }
  f.close();
  DeleteFileW(path.c_str());  // one shot
  return cmds;
}

void Exec(const std::vector<std::string>& cmds);

std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> w;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) break;
    size_t e = s.find(' ', i);
    if (e == std::string::npos) e = s.size();
    w.push_back(s.substr(i, e - i));
    i = e;
  }
  return w;
}

// "pawn.CombatBehaviour.m_Health": root (pawn, pc, world, game, cheat, engine or 0x<address>) + object
// properties. Gives the owner object of the last segment and the last segment name ("" = the object itself).
bool ResolvePath(const std::string& path, uintptr_t& obj, std::string& last) {
  std::vector<std::string> seg;
  size_t st = 0;
  for (size_t d; (d = path.find('.', st)) != std::string::npos; st = d + 1) seg.push_back(path.substr(st, d - st));
  seg.push_back(path.substr(st));
  const game::World& w = game::W();
  const std::string& r = seg[0];
  obj = r == "pawn" ? w.pawn : r == "pc" ? w.pc : r == "world" ? w.world : r == "game" ? w.gameInfo
      : r == "cheat" ? w.cheat : r == "engine" ? w.engine : 0;
  if (r.rfind("0x", 0) == 0) obj = (uintptr_t)strtoul(r.c_str(), nullptr, 16);
  if (!obj) return false;
  for (size_t i = 1; i + 1 < seg.size(); ++i) {
    obj = game::GetObj(obj, seg[i].c_str());
    if (!obj) return false;
  }
  last = seg.size() > 1 ? seg.back() : "";
  return true;
}

std::string ValueText(uintptr_t obj, const std::string& name) {
  if (name.empty()) return ue3::FullName(obj) + " @" + util::Hex(obj);
  game::Field f = game::FieldOf(obj, name.c_str());
  if (!f.ok()) return "<no field>";
  std::string c = ue3::ClassNameOf(f.prop);
  uintptr_t a = obj + f.off;
  char b[160];
  if (c == "FloatProperty") snprintf(b, sizeof b, "%g", util::ReadOr<float>(a));
  else if (c == "IntProperty") snprintf(b, sizeof b, "%d", util::ReadOr<int>(a));
  else if (c == "ByteProperty") snprintf(b, sizeof b, "%u", util::ReadOr<uint8_t>(a));
  else if (c == "BoolProperty") snprintf(b, sizeof b, "%s", game::GetBool(obj, name.c_str()) ? "true" : "false");
  else if (c == "NameProperty") return "'" + ue3::ReadFName(a) + "'";
  else if (c == "ObjectProperty" || c == "ComponentProperty" || c == "ClassProperty") {
    uintptr_t v = util::ReadOr<uintptr_t>(a);
    return v && ue3::IsObject(v) ? ue3::FullName(v) + " @" + util::Hex(v) : util::Hex(v);
  } else if (c == "StructProperty") {
    std::string out = "{";
    for (int i = 0; i < std::min(ue3::PropElementSize(f.prop), 64); i += 4) {
      snprintf(b, sizeof b, "%s%g", i ? " " : "", util::ReadOr<float>(a + i));
      out += b;
    }
    return out + "} (as floats)";
  } else snprintf(b, sizeof b, "<%s>", c.c_str());
  return b;
}

bool SetValue(uintptr_t obj, const std::string& name, const std::string& v) {
  game::Field f = game::FieldOf(obj, name.c_str());
  if (!f.ok()) return false;
  std::string c = ue3::ClassNameOf(f.prop);
  if (c == "FloatProperty") return game::SetFloat(obj, name.c_str(), (float)atof(v.c_str()));
  if (c == "IntProperty") return game::SetInt(obj, name.c_str(), atoi(v.c_str()));
  if (c == "ByteProperty") return game::SetByte(obj, name.c_str(), (uint8_t)atoi(v.c_str()));
  if (c == "BoolProperty") return game::SetBool(obj, name.c_str(), v == "1" || v == "true");
  return false;
}

// Calls obj.func with positional args converted by parameter type.
std::string CallWithArgs(uintptr_t obj, const std::string& func, const std::vector<std::string>& args) {
  engine::Call c(obj, func.c_str());
  if (!c.Valid()) return "FAIL " + c.Error();
  std::vector<engine::Param> ps = engine::Params(c.Function());
  size_t ai = 0;
  for (const engine::Param& p : ps) {
    if (p.flags & 0x400) continue;
    if (ai >= args.size()) break;
    const std::string& a = args[ai++];
    if (p.type == "IntProperty") c.Int(p.name.c_str(), atoi(a.c_str()));
    else if (p.type == "FloatProperty") c.Float(p.name.c_str(), (float)atof(a.c_str()));
    else if (p.type == "BoolProperty") c.Bool(p.name.c_str(), a == "1" || a == "true");
    else if (p.type == "ByteProperty") c.Byte(p.name.c_str(), (uint8_t)atoi(a.c_str()));
    else if (p.type == "NameProperty") c.Name(p.name.c_str(), a.c_str());
    else if (p.type == "StrProperty") {  // '+' = space ("open+DevilTesting")
      std::string t = a;
      for (char& ch : t)
        if (ch == '+') ch = ' ';
      c.Str(p.name.c_str(), util::Utf8ToWide(t));
    }
    else if (p.type == "ObjectProperty") c.Obj(p.name.c_str(), (uintptr_t)strtoul(a.c_str(), nullptr, 16));
    else if (p.type == "StructProperty" && a.find(',') != std::string::npos) {  // vector "x,y,z"
      float v[3] = {};
      sscanf(a.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
      c.Raw(p.name.c_str(), v, sizeof v);
    }
  }
  bool ok = c.Invoke();
  char b[96];
  snprintf(b, sizeof b, " -> %s, ret int %d / float %g", ok ? "ok" : "FAIL", c.RetInt(), c.RetFloat());
  return c.Describe() + b;
}

// Short state of the player and the enemies (research / tests).
void LogPlayer() {
  std::string out = engine::OnGame<std::string>([] {
    const game::World& w = game::W();
    std::string s = "map '" + w.map + "' mission=" + std::to_string(w.mission) + " gen=" + std::to_string(w.gen);
    if (!w.pawn) return s + " (no player pawn)";
    uintptr_t cb = game::GetObj(w.pawn, "CombatBehaviour");
    char b[640];
    snprintf(b, sizeof b,
             "\n  pawn %s HP %g/%g Physics %u NTPhysics %u CTD %g loc %s | world TD %g | combat %s CanTakeDamage %d "
             "DieInOneHit %d CheatDamageScalar %g",
             ue3::ObjName(w.pawn).c_str(), game::GetFloat(w.pawn, "m_Health"), game::GetFloat(w.pawn, "m_MaxHealth"),
             game::GetByte(w.pawn, "Physics"), game::GetByte(w.pawn, "NTPhysics"),
             game::GetFloat(w.pawn, "CustomTimeDilation"), ValueText(w.pawn, "Location").c_str(),
             game::GetFloat(w.world, "TimeDilation"), cb ? ue3::ObjName(cb).c_str() : "none",
             cb ? (int)game::GetBool(cb, "m_CanTakeDamage") : -1, cb ? (int)game::GetBool(cb, "m_DieInOneHit") : -1,
             cb ? game::GetFloat(cb, "m_CheatDamageScalar") : -1.0f);
    s += b;
    int n = 0;
    for (uintptr_t p : game::Pawns()) {
      if (!game::IsEnemy(p)) continue;
      if (++n > 12) continue;
      snprintf(b, sizeof b, "\n  enemy %s HP %g/%g CTD %g", ue3::ObjName(p).c_str(), game::GetFloat(p, "m_Health"),
               game::GetFloat(p, "m_MaxHealth"), game::GetFloat(p, "CustomTimeDilation"));
      s += b;
    }
    return s + "\n  enemies: " + std::to_string(n);
  });
  LOG("autorun: player: %s", out.c_str());
}

int DikOf(const std::string& k) {
  static const std::pair<const char*, int> map[] = {
      {"w", 0x11}, {"a", 0x1E}, {"s", 0x1F}, {"d", 0x20}, {"q", 0x10}, {"e", 0x12}, {"f", 0x21}, {"space", 0x39},
      {"ctrl", 0x1D}, {"shift", 0x2A}, {"esc", 0x01}, {"enter", 0x1C}, {"tab", 0x0F}, {"1", 0x02}, {"2", 0x03},
      {"3", 0x04}, {"up", 0xC8}, {"down", 0xD0}, {"left", 0xCB}, {"right", 0xCD}, {"r", 0x13}, {"c", 0x2E}};
  for (auto& [n, d] : map)
    if (k == n) return d;
  return (int)strtoul(k.c_str(), nullptr, 0);
}

void Run(std::vector<std::string> cmds) {
  // Front end reached: UE3 ready (ue3::StartAutoInit), a game thread seen, the player controller lives in the
  // main-menu map.
  for (int i = 0; i < 600; ++i) {
    if (ue3::Ready() && engine::Hooked() && engine::GameThreadKnown()) {
      unlock::Refs r = unlock::Locate();
      if (r.profile && unlock::IsFrontEnd(r.map)) break;
    }
    Sleep(1000);
  }
  LOG("autorun: front end reached, %zu command(s)", cmds.size());
  Exec(cmds);
  // Test sessions only (started with a script): later scripts dropped into the same file run as they appear,
  // so a test can go on in a running game. A normal launch never gets here.
  std::wstring path = util::InExeDir(L"DmCPT_autorun.txt");
  for (;;) {
    Sleep(1000);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    Sleep(300);  // let the writer finish
    std::vector<std::string> more = LoadScript(path);
    LOG("autorun: live script, %zu command(s)", more.size());
    Exec(more);
  }
}

void Exec(const std::vector<std::string>& cmds) {
  for (const std::string& line : cmds) {
    std::string cmd = line, arg;
    if (size_t sp = line.find(' '); sp != std::string::npos) {
      cmd = line.substr(0, sp);
      arg = line.substr(sp + 1);
    }
    LOG("autorun: > %s", line.c_str());
    if (cmd == "wait") Sleep((DWORD)(atof(arg.c_str()) * 1000));
    else if (cmd == "key") {  // key <vk hex or dec>: posted to the game window (works without focus)
      UINT vk = (UINT)strtoul(arg.c_str(), nullptr, 0);
      EnumWindows([](HWND h, LPARAM vkp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
          UINT sc = MapVirtualKeyW((UINT)vkp, MAPVK_VK_TO_VSC);
          PostMessageW(h, WM_KEYDOWN, vkp, 1 | (sc << 16));
          Sleep(80);
          PostMessageW(h, WM_KEYUP, vkp, 1 | (sc << 16) | (3u << 30));
        }
        return TRUE;
      }, (LPARAM)vk);
    } else if (cmd == "screenshot") {
      std::wstring dir = util::InExeDir(L"DmCPT_dumps\\");
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring p = dir + util::Utf8ToWide(arg.empty() ? "shot" : arg) + L".bmp";
      LOG("autorun: screenshot %s", d3d9hook::Screenshot(p) ? "ok" : "FAILED");
    } else if (cmd == "slot") {
      LOG("autorun: save slot status %d", unlock::SlotStatus());
    } else if (cmd == "wait_slot") {  // wait until the save slot is loaded (status 3/4), max <arg> s
      int maxS = arg.empty() ? 60 : atoi(arg.c_str()), st = -1;
      for (int i = 0; i < maxS; ++i) {
        st = unlock::SlotStatus();
        if (st == 3 || st == 4) break;
        Sleep(1000);
      }
      LOG("autorun: save slot status %d", st);
    }
    else if (cmd == "test_calls") { WaitIdle(); unlock::RunTestCalls(); Sleep(500); WaitIdle(); }
    else if (cmd == "dump_names") Dump(dumper::Kind::Names);
    else if (cmd == "dump_objects") Dump(dumper::Kind::Objects);
    else if (cmd == "dump_classes") Dump(dumper::Kind::Classes, arg);  // [Class1,Class2]
    else if (cmd == "dump_profile") Dump(dumper::Kind::Profile);
    else if (cmd == "dump_scripts") {  // dump_scripts [Class1,Class2;keyword1,keyword2]
      WaitIdle();
      dumper::RunAsync(dumper::Kind::Scripts, arg);
      Sleep(300);
      WaitIdle();
    } else if (cmd == "dump_live") {  // dump_live Class1,Class2: live objects of these classes, all fields
      WaitIdle();
      dumper::RunAsync(dumper::Kind::Profile, arg);
      Sleep(300);
      WaitIdle();
    } else if (cmd == "press") {  // press <key> <seconds>: held in the game's DirectInput keyboard
      std::vector<std::string> a = Words(arg);
      int dik = a.empty() ? 0 : DikOf(a[0]);
      dinject::Key(dik, true);
      Sleep((DWORD)((a.size() > 1 ? atof(a[1].c_str()) : 0.15) * 1000));
      dinject::Key(dik, false);
    } else if (cmd == "hold" || cmd == "release") {
      dinject::Key(DikOf(arg), cmd == "hold");
    } else if (cmd == "click") {  // click <0|1> [seconds]: mouse button in DirectInput
      std::vector<std::string> a = Words(arg);
      int b = a.empty() ? 0 : atoi(a[0].c_str());
      dinject::MouseButton(b, true);
      Sleep((DWORD)((a.size() > 1 ? atof(a[1].c_str()) : 0.1) * 1000));
      dinject::MouseButton(b, false);
    } else if (cmd == "pelog") {  // pelog 0|1 [filter]
      size_t sp = arg.find(' ');
      engine::SetPeLog(atoi(arg.c_str()) != 0, sp == std::string::npos ? "" : arg.substr(sp + 1));
    } else if (cmd == "player") {
      LogPlayer();
    } else if (cmd == "get" || cmd == "set") {  // get pawn.CombatBehaviour.m_DieInOneHit | set <path> <value>
      std::vector<std::string> a = Words(arg);
      std::string res = engine::OnGame<std::string>([a, cmd] {
        uintptr_t o = 0;
        std::string last;
        if (a.empty() || !ResolvePath(a[0], o, last)) return std::string("<unresolved>");
        if (cmd == "set" && a.size() > 1) SetValue(o, last, a[1]);
        return ValueText(o, last);
      });
      LOG("autorun: %s = %s", a.empty() ? "" : a[0].c_str(), res.c_str());
    } else if (cmd == "call") {  // call <path-to-object> <Func> [args]: e.g. call cheat NTKillAll
      std::vector<std::string> a = Words(arg);
      std::string res = engine::OnGame<std::string>([a] {
        uintptr_t o = 0;
        std::string last;
        if (a.size() < 2 || !ResolvePath(a[0] + ".", o, last)) return std::string("<unresolved>");
        return CallWithArgs(o, a[1], std::vector<std::string>(a.begin() + 2, a.end()));
      });
      LOG("autorun: call %s", res.c_str());
    }
    else if (cmd == "dump_layout") Dump(dumper::Kind::Layout);
    else if (cmd == "backup") {
      std::string err;
      std::wstring d = steam::BackupSaves(arg.empty() ? "autorun" : arg.c_str(), &err);
      LOG("autorun: backup %s", d.empty() ? err.c_str() : util::WideToUtf8(d).c_str());
    } else if (cmd == "unlock") {
      WaitIdle();
      unlock::RunFromScript(arg);
      Sleep(500);
      WaitIdle();
    } else if (cmd == "fixture") {
      WaitIdle();
      unlock::RunFixture();
      Sleep(500);
      WaitIdle();
    } else if (cmd == "export" || cmd == "import" || cmd == "load_slot" || cmd == "slots") {
      WaitIdle();
      if (cmd == "export") slots::ExportFromScript(arg);
      else if (cmd == "import") slots::ImportFromScript(arg);
      else if (cmd == "load_slot") slots::LoadSlot(atoi(arg.c_str()) - 1);
      else slots::Refresh();
      Sleep(500);
      WaitIdle();
      for (const std::string& l : slots::Report()) LOG("autorun: | %s", l.c_str());
      if (cmd == "slots")
        for (const slots::SlotInfo& si : slots::Slots())
          LOG("autorun: slot %d %s %s", si.index + 1, si.used ? "used" : "empty",
              si.used ? savefile::SummaryLines(si.sum)[0].c_str() : "");
    } else if (cmd == "feature") {  // feature <id> 0|1
      std::vector<std::string> a = Words(arg);
      if (a.size() >= 2) features::SetManual(a[0], a[1] == "1");
    } else if (cmd == "param") {  // param <id> <index> <value>
      std::vector<std::string> a = Words(arg);
      if (a.size() >= 3) features::SetParam(a[0], atoi(a[1].c_str()), (float)atof(a[2].c_str()));
    } else if (cmd == "trigger") {
      features::Trigger(arg);
    } else if (cmd == "features") {
      LOG("autorun: features:\n%s", features::DebugText().c_str());
    } else if (cmd == "bind") {  // bind <feature id> <vk>: keyboard bind as if set in the menu
      std::vector<std::string> a = Words(arg);
      auto lk = features::Lock();
      const auto& all = features::All();
      for (size_t i = 0; a.size() >= 2 && i < all.size(); ++i)
        if (a[0] == all[i]->id) features::D().binds[i] = hotkeys::Bind{(int)strtoul(a[1].c_str(), nullptr, 0), 0, 0};
      features::MarkDirty();
    } else if (cmd == "preset_add") {  // preset_add <name> <vk> <id[:param]>,<id>...
      std::vector<std::string> a = Words(arg);
      if (a.size() >= 3) {
        features::Preset p;
        p.name = a[0];
        p.bind.vk = (int)strtoul(a[1].c_str(), nullptr, 0);
        size_t st = 0;
        std::string list = a[2];
        while (st <= list.size()) {
          size_t c = list.find(',', st);
          std::string item = list.substr(st, c == std::string::npos ? std::string::npos : c - st);
          features::PresetItem it;
          size_t colon = item.find(':');
          it.id = item.substr(0, colon);
          if (features::Feature* f = features::Find(it.id)) {
            for (size_t k = 0; k < f->params.size() && k < (size_t)features::kMaxParams; ++k) it.p[k] = f->params[k].def;
            if (colon != std::string::npos) it.p[0] = (float)atof(item.c_str() + colon + 1);
            p.items.push_back(it);
          }
          if (c == std::string::npos) break;
          st = c + 1;
        }
        auto lk = features::Lock();
        features::D().presets.push_back(p);
        features::MarkDirty();
      }
    } else if (cmd == "presets_clear") {
      auto lk = features::Lock();
      features::D().presets.clear();
      features::MarkDirty();
    } else if (cmd == "hotkey") {  // hotkey <vk>: as if the key was pressed (goes through the bind poller)
      hotkeys::FakePress((int)strtoul(arg.c_str(), nullptr, 0));
    } else if (cmd == "glidetrace") {
      features::SetGlideTrace(atoi(arg.c_str()) != 0);
    } else if (cmd == "hitlog") {
      features::SetHitLog(atoi(arg.c_str()) != 0);
    } else if (cmd == "cfg_save") {
      SaveConfig();
    } else if (cmd == "cfg_load") {
      LoadConfig();
    } else if (cmd == "poke") {  // poke <path-to-object> <byte offset> <float>: e.g. a struct member
      std::vector<std::string> a = Words(arg);
      std::string res = engine::OnGame<std::string>([a] {
        uintptr_t o = 0;
        std::string last;
        if (a.size() < 3 || !ResolvePath(a[0] + ".", o, last)) return std::string("<unresolved>");
        *reinterpret_cast<float*>(o + strtoul(a[1].c_str(), nullptr, 0)) = (float)atof(a[2].c_str());
        return ue3::FullName(o) + " +" + a[1] + " = " + a[2];
      });
      LOG("autorun: poke %s", res.c_str());
    } else if (cmd == "sendkey") {  // sendkey <vk> [seconds]: real key via SendInput (the game must be in front)
      std::vector<std::string> a = Words(arg);
      UINT vk = a.empty() ? 0 : (UINT)strtoul(a[0].c_str(), nullptr, 0);
      INPUT in[1] = {};
      in[0].type = INPUT_KEYBOARD;
      in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
      in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
      SendInput(1, in, sizeof(INPUT));
      Sleep((DWORD)((a.size() > 1 ? atof(a[1].c_str()) : 0.1) * 1000));
      in[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
      SendInput(1, in, sizeof(INPUT));
    } else if (cmd == "wait_mission") {  // wait_mission <max s>: until the player character is in a mission
      int maxS = arg.empty() ? 90 : atoi(arg.c_str());
      bool ok = false;
      for (int i = 0; i < maxS * 4 && !(ok = features::Status().mission); ++i) Sleep(250);
      LOG("autorun: wait_mission -> %s (%s)", ok ? "in mission" : "TIMEOUT", features::Status().map.c_str());
    } else if (cmd == "overlay_force") {  // tests only: show the label without a cheat
      features::ForceOverlay();
    } else if (cmd == "ui_details") {  // ui_details 0|1: open every "Details" block on the Saves tab
      menu::ForceDetails(atoi(arg.c_str()) != 0);
    } else if (cmd == "ui_popup") {  // ui_popup rename|import <import entry index, 0-based> | close
      std::vector<std::string> a = Words(arg);
      if (!a.empty()) menu::OpenSavePopup(a[0], a.size() > 1 ? atoi(a[1].c_str()) : -1);
    } else if (cmd == "rename") {  // rename <import entry index, 0-based> <new name...>
      size_t sp = arg.find(' ');
      if (sp != std::string::npos) slots::Rename(atoi(arg.c_str()), arg.substr(sp + 1));
    } else if (cmd == "compat_report") {  // Settings > Create report, waits for the file
      compat::CreateReportAsync();
      for (int i = 0; i < 1800 && compat::Busy(); ++i) Sleep(100);
      LOG("autorun: compat_report: %s", compat::LastResult().c_str());
    } else if (cmd == "menu") {  // menu 0|1 [tab]: show/hide the tool menu, optionally select a tab
      state::menuOpen = atoi(arg.c_str()) != 0;
      if (size_t sp = arg.find(' '); sp != std::string::npos) menu::SelectTab(arg.substr(sp + 1));
    } else if (cmd == "call_live") {  // call_live <Class> <Func> [args]: on the first live object of that class
      std::vector<std::string> a = Words(arg);
      std::string res = engine::OnGame<std::string>([a] {
        if (a.size() < 2) return std::string("<usage>");
        uintptr_t cls = ue3::FindObject("Class", a[0].c_str());
        for (int i = 0, n = ue3::NumObjects(); cls && i < n; ++i) {
          uintptr_t o = ue3::Object(i);
          if (o && ue3::ObjClass(o) == cls && ue3::ObjName(o).rfind("Default__", 0) != 0)
            return CallWithArgs(o, a[1], std::vector<std::string>(a.begin() + 2, a.end()));
        }
        return std::string("<no live object>");
      });
      LOG("autorun: call_live %s", res.c_str());
    } else if (cmd == "set_all") {  // set_all <Class> <field> <value>: on every live object of exactly that class
      std::vector<std::string> a = Words(arg);
      int n = engine::OnGame<int>([a] {
        if (a.size() < 3) return -1;
        uintptr_t cls = ue3::FindObject("Class", a[0].c_str());
        int k = 0;
        for (int i = 0, c = ue3::NumObjects(); cls && i < c; ++i) {
          uintptr_t o = ue3::Object(i);
          if (o && ue3::ObjClass(o) == cls && ue3::ObjName(o).rfind("Default__", 0) != 0 && SetValue(o, a[1], a[2])) ++k;
        }
        return k;
      });
      LOG("autorun: set_all %s -> %d objects", arg.c_str(), n);
    } else if (cmd == "nav_levels") {  // nav_levels [all]: streaming levels now + what the saved checkpoint holds
      bool loadAll = arg == "all";  // research: request every streaming level
      std::string res = engine::OnGame<std::string>([loadAll] {
        const game::World& w = game::W();
        std::string s;
        char b[256];
        game::Field f = w.world ? game::FieldOf(w.world, "StreamingLevels") : game::Field{};
        ue3::TArrayRaw a{};
        int loaded = 0, visible = 0;
        if (f.ok() && util::Read(w.world + f.off, a) && a.num > 0 && a.num < 2000)
          for (int i = 0; i < a.num; ++i) {
            uintptr_t ls = util::ReadOr<uintptr_t>(a.data + i * 4);
            if (!ls || !ue3::IsObject(ls)) continue;
            game::Field pn = game::FieldOf(ls, "PackageName");
            bool L = game::GetObj(ls, "LoadedLevel") != 0, V = game::GetBool(ls, "bIsVisible");
            loaded += L;
            visible += V;
            if (loadAll) {
              game::SetBool(ls, "bShouldBeLoaded", true);
              game::SetBool(ls, "bShouldBeVisible", true);
            }
            snprintf(b, sizeof b, "\n  %-40s %-26s want L%d V%d  is L%d V%d", pn.ok() ? ue3::ReadFName(ls + pn.off).c_str() : "?",
                     ue3::ClassNameOf(ls).c_str(), game::GetBool(ls, "bShouldBeLoaded"),
                     game::GetBool(ls, "bShouldBeVisible"), L, V);
            s += b;
          }
        snprintf(b, sizeof b, "map %s: %d streaming levels, %d loaded, %d visible", w.map.c_str(), a.num, loaded, visible);
        std::string out = b + s;
        uintptr_t cp = w.engine ? game::GetObj(w.engine, "GCheckPointData") : 0;
        if (cp) {
          game::Field cn = game::FieldOf(cp, "m_CheckpointName");
          ue3::TArrayRaw nm{};
          std::string name;
          if (cn.ok() && util::Read(cp + cn.off, nm) && nm.data && nm.num > 0 && nm.num < 256)
            name = util::WideToUtf8(util::ReadWString(nm.data, nm.num));
          out += "\n  checkpoint data: '" + name + "'";
          for (const char* arr : {"m_LevelRecords", "m_LevelNamesToLoad", "m_VolumeRecords"}) {
            game::Field af = game::FieldOf(cp, arr);
            ue3::TArrayRaw r{};
            if (!af.ok() || !util::Read(cp + af.off, r)) continue;
            int el = std::string(arr) == "m_LevelNamesToLoad" ? 0x14 : 0xC;
            out += "\n  " + std::string(arr) + " (" + std::to_string(r.num) + "):";
            for (int i = 0; i < r.num && i < 300; ++i) {
              uintptr_t e = r.data + i * el;
              uint32_t bits = el == 0xC ? util::ReadOr<uint32_t>(e + 8) : 0;
              out += (i % 6 ? " " : "\n    ") + ue3::ReadFName(e) + (el == 0xC ? "[" + std::to_string(bits & 3) + "]" : "");
            }
          }
        }
        return out;
      });
      for (size_t st = 0; st < res.size();) {  // one log line per level: long lines get cut by the log
        size_t e = res.find('\n', st);
        LOG("autorun: nav_levels %s", res.substr(st, e == std::string::npos ? std::string::npos : e - st).c_str());
        if (e == std::string::npos) break;
        st = e + 1;
      }
    } else if (cmd == "set_export" || cmd == "set_import" || cmd == "set_default") {  // + set_default
      std::string msg;
      bool ok = cmd == "set_export"   ? cpsets::Export(arg, &msg)
                : cmd == "set_default" ? nav::UseBuiltin()
                                       : cpsets::Import(cpsets::Dir() + L"\\" + util::Utf8ToWide(arg), &msg);
      LOG("autorun: list in use '%s' (%s), %zu set file(s)", nav::ListName().c_str(), nav::CatalogSource().c_str(),
          cpsets::List().size());
      LOG("autorun: %s -> %s: %s", cmd.c_str(), ok ? "ok" : "FAIL", msg.c_str());
      const auto& ms = nav::Missions();
      if (ms.size() > 1 && ms[1].points.size() > 1) LOG("autorun: M02 point 2 is now '%s'", ms[1].points[1].name.c_str());
    } else if (cmd == "nav_ren") {  // nav_ren <point | -1 = the mission> <new name...> (current mission)
      size_t sp = arg.find(' ');
      int mi = nav::Now().missionIndex, pt = atoi(arg.c_str());
      std::string name = sp == std::string::npos ? "" : arg.substr(sp + 1);
      bool ok = pt < 0 ? nav::RenameMission(mi, name) : nav::RenamePoint(mi, pt, name);
      LOG("autorun: nav_ren mission %d point %d '%s' -> %s", mi, pt, name.c_str(), ok ? "ok" : "FAIL");
    } else if (cmd == "nav_move" || cmd == "nav_del") {  // nav_move <point> <-1|1> | nav_del <point> (current mission)
      std::vector<std::string> a = Words(arg);
      int mi = nav::Now().missionIndex, pt = a.empty() ? -1 : atoi(a[0].c_str());
      bool ok = cmd == "nav_del" ? nav::DeletePoint(mi, pt) : nav::MovePoint(mi, pt, a.size() > 1 ? atoi(a[1].c_str()) : 1);
      LOG("autorun: %s mission %d point %d -> %s (edited %d)", cmd.c_str(), mi, pt, ok ? "ok" : "FAIL", nav::Edited(mi));
    } else if (cmd == "nav_crawl") {  // nav_crawl [first] [last]: visit catalog missions, write missions_crawl.json
      std::vector<std::string> a = Words(arg);
      nav::Crawl(a.size() > 0 ? atoi(a[0].c_str()) : 0, a.size() > 1 ? atoi(a[1].c_str()) : -1);
    } else if (cmd == "nav_list") {  // checkpoints of the loaded map, as the level has them
      std::vector<std::string> l = engine::OnGame<std::vector<std::string>>([] { return nav::LiveCheckpoints(); });
      for (const std::string& x : l) LOG("autorun: cp %s", x.c_str());
      nav::Snapshot sn = nav::Now();
      LOG("autorun: nav: map %s mission %d difficulty %d cutscene %d busy %d slots %d%d%d", sn.map.c_str(),
          sn.missionIndex, sn.difficulty, sn.cutscene, sn.busy, sn.slot[0], sn.slot[1], sn.slot[2]);
    } else if (cmd == "nav_sel") {  // nav_sel point|mission|difficulty|mpoint|slot <value> [map for point]
      std::vector<std::string> a = Words(arg);
      if (a.size() >= 2) {
        nav::Selection sl = nav::GetSel();
        int v = atoi(a[1].c_str());
        if (a[0] == "point") {
          sl.point = v;
          sl.pointMap = a.size() > 2 ? a[2] : nav::Now().map;
        } else if (a[0] == "mission") {
          // by catalog id ("M12", "SM05") or index
          sl.selMission = v;
          for (size_t i = 0; i < nav::Missions().size(); ++i)
            if (nav::Missions()[i].id == a[1]) sl.selMission = (int)i;
        } else if (a[0] == "difficulty") sl.selDifficulty = v;
        else if (a[0] == "mpoint") sl.selPoint = v;
        else if (a[0] == "slot") sl.slot = v;
        nav::SetSel(sl);
        LOG("autorun: nav_sel point %d (%s) mission %d difficulty %d mpoint %d slot %d", sl.point,
            sl.pointMap.c_str(), sl.selMission, sl.selDifficulty, sl.selPoint, sl.slot);
      }
    } else if (cmd == "enemy_sel") {  // enemy_sel kind|count|where|dist|slot <value>
      std::vector<std::string> a = Words(arg);
      if (a.size() >= 2) {
        enemies::Selection s = enemies::GetSel();
        if (a[0] == "kind") s.kind = a[1];
        if (a[0] == "count") s.count = atoi(a[1].c_str());
        if (a[0] == "where") s.where = atoi(a[1].c_str());
        if (a[0] == "dist") s.distance = (float)atof(a[1].c_str());
        if (a[0] == "slot") s.posSlot = atoi(a[1].c_str());
        enemies::SetSel(s);
        s = enemies::GetSel();
        LOG("autorun: enemy_sel %s x%d where %d dist %.1f slot %d", s.kind.c_str(), s.count, s.where, s.distance,
            s.posSlot);
      }
    } else if (cmd == "enemy_list") {
      std::vector<std::string> lines = engine::OnGame<std::vector<std::string>>([] { return enemies::DebugLines(); });
      for (const std::string& l : lines) LOG("autorun: enemy %s", l.c_str());
      enemies::Snapshot s = enemies::Now();
      LOG("autorun: enemies active %d spawned %d loading %d msg '%s'", s.active, s.spawned, s.loading,
          s.message.c_str());
    } else if (cmd == "dmglog") {  // dmglog [last N]
      for (const std::string& l : damagelog::DebugLines(arg.empty() ? 30 : atoi(arg.c_str())))
        LOG("autorun: dmg %s", l.c_str());
    } else if (cmd == "dmglog_sum") {
      for (const std::string& l : damagelog::SummaryLines()) LOG("autorun: dmgsum %s", l.c_str());
    } else if (cmd == "fps") {  // fps <seconds>: average frames per second over that time
      double sec = arg.empty() ? 5.0 : atof(arg.c_str());
      unsigned f0 = engine::FrameCount();
      ULONGLONG t0 = GetTickCount64();
      Sleep((DWORD)(sec * 1000));
      double fps = (engine::FrameCount() - f0) * 1000.0 / (double)(GetTickCount64() - t0);
      LOG("autorun: fps %.1f over %.1f s", fps, sec);
    } else if (cmd == "dmglog_clear") {
      damagelog::Clear();
    } else if (cmd == "dmglog_merge") {
      damagelog::SetMergeWeak(arg == "1");
    } else if (cmd == "dmglog_filter") {
      damagelog::SetFilter(atoi(arg.c_str()));
    } else if (cmd == "dmglog_export") {
      std::string p;
      LOG("autorun: dmglog_export %s", damagelog::ExportCsv(&p) ? p.c_str() : "FAILED");
    } else if (cmd == "vis") {  // vis [triggers | probe <N>]: visuals stats, the level's triggers, strike probes
      std::vector<std::string> lines =
          engine::OnGame<std::vector<std::string>>([arg] { return visuals::DebugLines(arg); });
      for (const std::string& l : lines) LOG("autorun: vis %s", l.c_str());
    } else if (cmd == "d3d_trace") {  // one frame of D3D9 render targets / depth / clears / draws in the log
      d3d9hook::RequestTrace();
      Sleep(500);
    } else if (cmd == "vis_opt") {  // vis_opt labels|dist|width|occlude|plane|occall <value>
      std::vector<std::string> a = Words(arg);
      visuals::Options o = visuals::GetOptions();
      if (a.size() > 1 && a[0] == "labels") o.labels = a[1] != "0";
      if (a.size() > 1 && a[0] == "dist") o.maxDistance = (float)atof(a[1].c_str());
      if (a.size() > 1 && a[0] == "width") o.thickness = (float)atof(a[1].c_str());
      if (a.size() > 1 && a[0] == "occlude") o.occlude = a[1] != "0";
      // Dev: a screen-wide plane this many metres ahead, depth tested like the lines (0 = off)
      if (a.size() > 1 && a[0] == "plane") visuals::SetDepthTestPlane((float)atof(a[1].c_str()));
      if (a.size() > 1 && a[0] == "occall") visuals::SetOccludeAll(a[1] != "0");  // Dev: bodies too
      visuals::SetOptions(o);
      LOG("autorun: vis_opt labels %d dist %.0f width %.1f occlude %d (hiding %s)", o.labels, o.maxDistance,
          o.thickness, o.occlude, visuals::OcclusionLive() ? "works" : "not available");
    } else if (cmd == "wait_idle_nav") {  // wait until a navigation load is finished, max <arg> s
      int maxS = arg.empty() ? 90 : atoi(arg.c_str());
      for (int i = 0; i < maxS * 4 && nav::Now().busy; ++i) Sleep(250);
      LOG("autorun: nav busy=%d map %s", nav::Now().busy, nav::Now().map.c_str());
    } else if (cmd == "exit") {
      LOG("autorun: closing the game");
      EnumWindows(CloseGameWindow, 0);
    } else {
      LOG("autorun: unknown command '%s'", cmd.c_str());
    }
  }
  LOG("autorun: done");
}

}  // namespace

void Start() {
  std::wstring path = util::InExeDir(L"DmCPT_autorun.txt");
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  std::vector<std::string> cmds = LoadScript(path);  // one shot: a normal launch never runs it again
  LOG("autorun: loaded %zu command(s) from DmCPT_autorun.txt (file deleted)", cmds.size());
  std::thread(Run, cmds).detach();
}

}  // namespace autorun
