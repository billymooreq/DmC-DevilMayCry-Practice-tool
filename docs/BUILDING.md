# Building from source

## Requirements

- Windows 10/11.
- **Visual Studio 2022 Build Tools** (or Visual Studio 2022) with the workload *Desktop development with C++*:
  MSVC x86/x64 toolset and a Windows 10/11 SDK. CMake and Ninja come with it.
- No other dependencies: Dear ImGui, MinHook and nlohmann/json are included in `third_party/`.

The game is 32-bit, so the DLL must be built for **x86**.

## Build

From PowerShell in the repository folder:

```powershell
.\tools\build.ps1 -Config Release
```

The result is `build\Release\dinput8.dll`. Other options: `-Config RelWithDebInfo` (with a PDB), `-Clean`
(fresh build folder), `-SimulateUnknown` (test build that treats every game build as unknown, output in
`build\Release-unknown\`).

Without the script, from an *x86 Native Tools Command Prompt for VS 2022*:

```bat
cmake -S . -B build\Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\Release
```

The C runtime is linked statically, so the DLL needs no VC++ redistributable.

## Test without the game

`build\Release\test_host.exe [path\to\dinput8.dll]` opens a small D3D9 window, loads the DLL, checks the
DirectInput forwarding, renders a few frames, does a device reset and saves a screenshot (`test_shot.bmp`).
Results go to `test_result.txt`.

## Project structure

| Path | What |
|---|---|
| `src/dllmain.cpp`, `src/proxy.cpp`, `src/dinput8.def` | DLL entry, proxy exports to the real `dinput8.dll`, DirectInput hooks |
| `src/version.*` | game build detection, tool version (`kToolVersion`) |
| `src/d3d9_hook.*`, `src/menu.*`, `src/input.*` | D3D9 hooks, ImGui menu, keyboard / mouse / gamepad |
| `src/ue3/` | UE3 reflection (GNames, GObjects, layouts), ProcessEvent hook and calls, Dev dumps |
| `src/features.*`, `src/feature_list.cpp`, `src/hotkeys.*` | feature registry, Cheats features, binds and presets |
| `src/nav*`, `src/realcp.*`, `src/cpsets.*` | Practice tab: missions, checkpoints, positions, shared lists |
| `src/enemies.*`, `src/enemy_features.cpp`, `src/damagelog.*` | Enemies tab and the damage log |
| `src/visuals.*`, `src/visual_features.cpp` | Visuals tab |
| `src/unlock.*`, `src/steam_guard.*` | unlocks, save backups, Steam achievements guard |
| `src/savefile.*`, `src/saveslots.*` | save file format, `.dmcpt` packages, Saves tab |
| `src/compat.*` | compatibility report |
| `src/autorun.*` | test scripts (only run when a `DmCPT_autorun.txt` exists next to the exe) |
| `src/config.*` | `DmCPracticeTool.json` |
| `data/missions.json` | built-in checkpoint catalog (embedded into the DLL) |
| `tests/` | offline test host and save format test |
| `third_party/` | Dear ImGui 1.91.8, MinHook 1.3.4, nlohmann/json 3.11.3 (see `VERSIONS.txt`) |

## Adding a game build

1. Run the tool on that build and create the compatibility report (Settings).
2. If the signatures and the UE3 layout were found, add the build's `TimeDateStamp` and `SizeOfImage` to
   `kBuilds` in `src/version.cpp` and test every tab.
3. If a signature is missing, find the code again for that build and add a second pattern.
