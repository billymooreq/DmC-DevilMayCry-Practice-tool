#include "config.h"

#include <windows.h>

#include <algorithm>

#include <atomic>
#include <fstream>

#include "damagelog.h"
#include "enemies.h"
#include "features.h"
#include "log.h"
#include "nav.h"
#include "util.h"
#include "visuals.h"

Config g_cfg;

using nlohmann::json;

static std::wstring ConfigPath() { return util::InExeDir(L"DmCPracticeTool.json"); }

static std::atomic<ULONGLONG> g_dirtyAt{0};

bool LoadConfig() {
  std::ifstream f(ConfigPath());
  if (!f) {
    LOG("config: no DmCPracticeTool.json, using defaults");
    features::LoadJson(json::object(), json::array());
    return false;
  }
  try {
    json j = json::parse(f);
    Config c;
    c.menuKey = j.value("menu_key", c.menuKey);
    c.showDevTab = j.value("show_dev_tab", c.showDevTab);
    c.savesListRows = std::min(40.0f, std::max(2.0f, j.value("saves_list_rows", 6.0f)));
    if (j.contains("window") && j["window"].is_object()) {
      const json& w = j["window"];
      c.winX = w.value("x", 0.0f);
      c.winY = w.value("y", 0.0f);
      c.winW = w.value("w", 0.0f);
      c.winH = w.value("h", 0.0f);
    }
    if (j.contains("hooks")) {
      const json& h = j["hooks"];
      c.hookD3D9 = h.value("d3d9", c.hookD3D9);
      c.renderHook = h.value("render", c.renderHook);
      // config_version 1 saved the then-default "present"; the vtable mode replaces it (OBS-safe drawing order).
      if (j.value("config_version", 1) < 2 && c.renderHook == "present") c.renderHook = "vtable";
      c.hookWndProc = h.value("wndproc", c.hookWndProc);
      c.hookDInput = h.value("dinput", c.hookDInput);
      c.hookXInput = h.value("xinput", c.hookXInput);
      c.hookCursor = h.value("cursor", c.hookCursor);
    }
    if (j.contains("debug")) {
      const json& d = j["debug"];
      c.debugMenuOpenAtStart = d.value("menu_open_at_start", c.debugMenuOpenAtStart);
      c.dumpArrayLimit = d.value("dump_array_limit", c.dumpArrayLimit);
      c.peLog = d.value("pe_log", c.peLog);
      c.peLogFilter = d.value("pe_log_filter", c.peLogFilter);
    }
    if (j.contains("features") && j["features"].is_object()) c.features = j["features"];
    if (j.contains("presets") && j["presets"].is_array()) c.presets = j["presets"];
    g_cfg = c;
    features::LoadJson(c.features, c.presets);
    if (j.contains("navigation")) nav::LoadJson(j["navigation"]);
    if (j.contains("enemies")) enemies::LoadJson(j["enemies"]);
    if (j.contains("damage_log")) damagelog::LoadJson(j["damage_log"]);
    if (j.contains("visuals")) visuals::LoadJson(j["visuals"]);
    LOG("config: loaded (menu_key=0x%02X render=%s d3d9=%d wndproc=%d dinput=%d xinput=%d cursor=%d presets=%zu)",
        c.menuKey, c.renderHook.c_str(), c.hookD3D9, c.hookWndProc, c.hookDInput, c.hookXInput, c.hookCursor,
        c.presets.size());
    return true;
  } catch (const std::exception& e) {
    LOG("config: parse error: %s (defaults kept)", e.what());
    return false;
  }
}

bool SaveConfig() {
  g_dirtyAt = 0;
  Config& c = g_cfg;
  features::SaveJson(c.features, c.presets);
  json j;
  j["config_version"] = 2;
  j["menu_key"] = c.menuKey;
  j["show_dev_tab"] = c.showDevTab;
  j["saves_list_rows"] = c.savesListRows;
  if (c.winW > 0) j["window"] = {{"x", c.winX}, {"y", c.winY}, {"w", c.winW}, {"h", c.winH}};
  j["hooks"] = {{"d3d9", c.hookD3D9},     {"render", c.renderHook}, {"wndproc", c.hookWndProc},
                {"dinput", c.hookDInput}, {"xinput", c.hookXInput}, {"cursor", c.hookCursor}};
  j["debug"] = {{"menu_open_at_start", c.debugMenuOpenAtStart},
                {"dump_array_limit", c.dumpArrayLimit},
                {"pe_log", c.peLog},
                {"pe_log_filter", c.peLogFilter}};
  j["features"] = c.features;
  j["presets"] = c.presets;
  j["navigation"] = nav::SaveJson();
  j["enemies"] = enemies::SaveJson();
  j["damage_log"] = damagelog::SaveJson();
  j["visuals"] = visuals::SaveJson();
  std::ofstream f(ConfigPath(), std::ios::trunc);
  if (!f) {
    LOG("config: cannot write DmCPracticeTool.json");
    return false;
  }
  f << j.dump(2) << "\n";
  LOG("config: saved");
  return true;
}

void MarkConfigDirty() { g_dirtyAt = GetTickCount64(); }

void ConfigAutosaveTick() {
  ULONGLONG t = g_dirtyAt;
  if (t && GetTickCount64() - t > 1000) SaveConfig();
}
