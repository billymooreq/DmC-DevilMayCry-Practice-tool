cmake_minimum_required(VERSION 3.20)
project(DmCPracticeTool LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "DmC is a 32-bit game: configure with an x86 toolchain (VsDevCmd -arch=x86)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL must not depend on a VC++ redist being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
add_compile_options(/utf-8 /W3 /MP)

set(TP ${CMAKE_SOURCE_DIR}/third_party)

# --- MinHook ---------------------------------------------------------------
add_library(minhook STATIC
  ${TP}/minhook/src/buffer.c
  ${TP}/minhook/src/hook.c
  ${TP}/minhook/src/trampoline.c
  ${TP}/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

# --- Dear ImGui (win32 + dx9 backends) --------------------------------------
add_library(imgui STATIC
  ${TP}/imgui/imgui.cpp
  ${TP}/imgui/imgui_draw.cpp
  ${TP}/imgui/imgui_tables.cpp
  ${TP}/imgui/imgui_widgets.cpp
  ${TP}/imgui/backends/imgui_impl_win32.cpp
  ${TP}/imgui/backends/imgui_impl_dx9.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends)

# --- dinput8.dll proxy -------------------------------------------------------
add_library(dinput8 SHARED
  src/dllmain.cpp
  src/proxy.cpp
  src/log.cpp
  src/util.cpp
  src/version.cpp
  src/config.cpp
  src/hooks.cpp
  src/d3d9_hook.cpp
  src/input.cpp
  src/menu.cpp
  src/ue3/ue3.cpp
  src/ue3/dumper.cpp
  src/ue3/engine.cpp
  src/steam_guard.cpp
  src/unlock.cpp
  src/savefile.cpp
  src/saveslots.cpp
  src/autorun.cpp
  src/game.cpp
  src/features.cpp
  src/feature_list.cpp
  src/hotkeys.cpp
  src/nav.cpp
  src/nav_features.cpp
  src/realcp.cpp
  src/cpsets.cpp
  src/enemies.cpp
  src/enemy_features.cpp
  src/damagelog.cpp
  src/visuals.cpp
  src/visual_features.cpp
  src/compat.cpp
  src/resources.rc
  src/dinput8.def)
target_include_directories(dinput8 PRIVATE src ${TP}/nlohmann)
target_link_libraries(dinput8 PRIVATE minhook imgui d3d9 bcrypt dxguid version)
set_target_properties(dinput8 PROPERTIES OUTPUT_NAME dinput8)
# Test build: acts as on an unknown game build (menu works, game features locked) to check that behaviour.
option(DMCPT_SIMULATE_UNKNOWN_BUILD "Treat every game build as unknown (testing only)" OFF)
if(DMCPT_SIMULATE_UNKNOWN_BUILD)
  target_compile_definitions(dinput8 PRIVATE DMCPT_SIMULATE_UNKNOWN_BUILD)
endif()
# the catalog is built into the DLL: rebuild when it changes
set_source_files_properties(src/resources.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/data/missions.json)

# --- offline test host (not deployed) ---------------------------------------
add_executable(test_host tests/test_host.cpp)
# no dinput8.lib here: the name would resolve to our own import library
target_link_libraries(test_host PRIVATE d3d9 dxguid)

# --- offline save/package test (not deployed): python-free check of savefile.cpp on real save files ----
add_executable(save_test tests/save_test.cpp src/savefile.cpp)
target_include_directories(save_test PRIVATE src ${TP}/nlohmann)
target_link_libraries(save_test PRIVATE bcrypt)
