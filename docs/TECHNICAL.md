# How DmC Practice Tool works

Technical notes for people who want to understand, check or extend the tool. Player documentation is in the
[README](../README.md).

## Overview

The game (`DMC-DevilMayCry.exe`, 32-bit, Unreal Engine 3) imports `DirectInput8Create` from `dinput8.dll`. There
is no `dinput8.dll` in the game folder, so Windows first looks next to the exe: the tool is a **proxy
`dinput8.dll`**. It forwards every DirectInput export to the real `System32\dinput8.dll` and starts its own code in
a background thread (nothing heavy runs under the loader lock).

Everything happens inside the game process. No files of the game are modified; the tool only writes its own files
next to the exe (see "Files" below).

## Startup

1. **Game build detection** (`src/version.cpp`) — reads the PE header of the main module: `TimeDateStamp` and
   `SizeOfImage` are compared with a table of known builds. No hashing at startup. An unknown build keeps the menu
   working but locks every feature that changes the game.
2. **Hooks** — Win32 (`WndProc`, cursor), DirectInput (`GetDeviceState`/`GetDeviceData`, muted while the menu is
   open), XInput, and Direct3D 9.
3. **UE3 reflection** initialises in the background as soon as the game has loaded its names and objects.
4. **Steam achievements guard** — hooks a few `ISteamUserStats` methods so achievements and Steam stats are
   blocked after the first cheat (and while an unlock runs).

## Direct3D 9 and the menu

`src/d3d9_hook.cpp` creates a dummy D3D9 device to read the device vtable, then hooks `Present`, `Reset` and a few
state functions. The menu ([Dear ImGui](https://github.com/ocornut/imgui), win32 + dx9 backends) is drawn from
the **vtable slot of `Present`**, which runs before inline detours of capture software — this keeps the menu and
the "Practice Tool Active" label visible in OBS game capture. Other modes (`present`, `endscene`) can be chosen in
the config (`hooks.render`).

The game's DirectX 11 renderer is not supported (D3D9 is the game's default).

## UE3 reflection

`src/ue3/ue3.cpp` finds the engine globals and learns the object layouts **on live data**, so offsets are not
hardcoded per build:

- **GNames** (`TArray<FNameEntry*>`) and **GObjects** (`TArray<UObject*>`) are found by byte signatures of code
  that references them. If the signatures don't match (another build), a full scan of the writable sections looks
  for arrays whose contents look like names / objects.
- Field offsets of `UObject`, `UField`, `UStruct`, `UProperty`, `UFunction`, `UClass` are discovered with known
  objects (`Core.Object.Vector` with X/Y/Z, `Engine.Actor.Location`, functions whose friendly name equals their
  name, and so on).
- With that, the tool finds classes, properties and functions by name (`FindObject`, property offsets by name).

## Calling the game: ProcessEvent

`UObject::ProcessEvent` (found by signature) is hooked. The hook does three things:

- **Game-thread task queue.** Calling UnrealScript functions is only safe on the game thread. Tool code posts
  tasks; the outermost `ProcessEvent` call on the game thread runs them (`engine::Post`, `engine::OnGame`).
- **Per-frame callback.** The render thread counts frames in `Present`; the first top-level `ProcessEvent` after
  a new frame runs the features once per frame on the game thread.
- **Observers** of every call before and after the original (cheap pointer compares): used for damage, hitboxes,
  boss health bars.

## Damage

Every hit goes through the event `NTCombat_Nucleus.ContactInternal(Contact)` (`DVCombat_Nucleus` for Dante and the
enemies). The victim is the nucleus' `Owner`, the attacker is `Contact.OriginatorActor`. Damage multipliers and
one-hit kills change `Contact.DamageAmount` before the game processes the hit; the damage log reads the victim's
health right after the call returns.

## Features

- A **feature registry** (`src/features.cpp`): every feature has an id, a group, parameters (slider / toggle /
  choice), optional bind. A feature is on when it is on in the menu **or** in an active preset; it is applied
  once per frame and undone when turned off, when the map changes or when Dante is replaced.
- Examples: Infinite HP = `bGodMode` on the controller + health refill; speeds = `WorldInfo.TimeDilation` /
  `CustomTimeDilation` multiplied on top of the game's own value (hit-stops keep working); noclip = physics off +
  `ForcedSetLocation` + the game's own movement-denial tokens.
- Navigation (`src/nav.cpp`) uses the game's checkpoint system: `NTCheckpoint.SetCheckpointName` + reopening the
  map, so the level puts Dante at the checkpoint and runs its scripts. "Created" checkpoints are copies of the
  game's own checkpoint file, loaded back through the slot. The built-in catalog is `data/missions.json`
  (embedded as a resource; `DmCPT_missions.json` next to the exe overrides it).
- Visuals (`src/visuals.cpp`): body shapes from the skeletal mesh physics asset, attack zones from the active
  strike processes, Kismet touch triggers classified by walking their script links, blocking volumes. Lines can be
  hidden behind the level geometry using the scene depth buffer (drawn from the `Clear` hook just before the game
  clears it).

## Unlocks

`src/unlock.cpp` works on the live profile object (`DVGamerProfile`), mostly through the game's own functions
(`UnlockAllMissions`, `UnlockAllSecretMissions`, `UnlockUpgrade`, stat counters…), then saves with the game's own
`NTGamerProfile.SaveProfile`. The game's anti-cheat function `NTWorldInfo.NTStripUnlockingHacks` is disabled only
for the duration of the unlock. Before any change the save files are copied to `DmCPT_backups\`. Counters are only
raised, never lowered; ranks and scores are not touched (except by the separate "Set all ranks SSS").

## Save files and the `.dmcpt` package

DmC keeps three files per slot in Steam Cloud (`DMC5_SAVE_<nn>_PROFILE`, `_INFO`, `_CHECKPNT`). They are encrypted
with Blowfish-ECB. After decryption: `u32 1 | SHA1(payload) | u32 size | u32 Steam account id | u32 0x01100001 |
payload`. The account id binds a save to a Steam account.

A **`.dmcpt`** file is a zip (stored, no compression) with:

- `PROFILE`, `INFO`, `CHECKPNT` — decrypted, with the Steam account id set to 0 (so anyone can import it);
- `manifest.json` — `format: "dmcpt"`, `format_version`, `name`, `notes`, `tool_version`, `game_version`,
  `exported_at`, `source_slot`, per-file `size` + `sha256`, and a `summary` (mission, difficulty, checkpoint,
  progress) for the menu.

Import encrypts the files again with the importing player's account id and writes them through Steam's remote
storage API, so Steam Cloud stays consistent.

## The `.dmcptset` checkpoint set

A zip with `manifest.json` (`format: "dmcpt-checkpoint-set"`, `version`, `name`, `created`, `tool_version`,
`game_version`), `missions.json` (the checkpoint list in the same format as `data/missions.json`) and
`checkpoints/<map>/<name>.chk` (created checkpoints).

## Unknown game builds and the compatibility report

On an unknown build the menu works, everything that changes the game is locked, and the Dev tab is shown. Settings
→ **Create report** writes `DmCPT_compat_report.txt` (read-only, no personal data): exe size, SHA-256,
FileVersion, TimeDateStamp, SizeOfImage, sections; every signature with its number of matches (counted in the exe
file on disk); whether GNames/GObjects and the layout were found; and the classes / functions the features use.
That is what's needed to add a build to the table in `src/version.cpp`.

## Supported game builds

| Shown as | Internal name | TimeDateStamp | SizeOfImage | FileVersion | Status |
|---|---|---|---|---|---|
| v1.3 | `latest_2025` | `0x682EAB06` | `0x02DA8000` | 1.0.8916.1 | supported (current Steam build, 2025 patch) |
| v1.0 | `release_2013` | `0x50C2491C` | `0x02D93000` | — (no version resource) | supported (January 2013 release, exe built 2012-12-07) |

| Shown as | exe size | SHA-256 |
|---|---|---|
| v1.3 | 45 903 440 | `C36516FDFC927A062DE6676745D6419DEFD20F0E1453D30512C944A62ED5B409` |
| v1.0 | 45 808 128 | `02F12DDB71FFCA869A4EE4603CD9EBA887A14A06BF9FFF69D9A48418B36156F6` |

### What differs between v1.0 and v1.3 (and how the tool handles it)

- **Addresses:** GNames RVA `0x26F7BA8` / `0x270AC38`, GObjects `0x26F7BD8` / `0x270AC68`, ProcessEvent
  `0x7B010` / `0x7B7B0` (v1.0 / v1.3). The tool never hardcodes them: the same three byte signatures are unique
  in both exes.
- **UE3 object layout** (UObject, UField, UStruct, UProperty, UFunction incl. `Func` +0xA8, UClass CDO) is
  identical; it is calibrated at start anyway.
- **Class layout:** 2847 of 2879 dumped `DV*`/`NT*`/engine classes are identical. `PlayerController` and its
  subclasses are 0x10 bytes smaller in v1.0 (e.g. `DVPlayerController_Player.m_UnlockAllUpgrades` is +0x7FC
  instead of +0x80C), `NTInput` 4 bytes smaller. All fields are looked up by name through reflection, so
  nothing changes for the tool. `DVGamerProfile` and the save classes are identical.
- **UnrealScript:** the functions the tool calls or relies on (`UnlockAllMissions`, `UnlockAllSecretMissions`,
  `UnlockForQA`, `UpdateAchievement`, `SaveProfile`, `SaveUpgrades`, `DVGameInfo.UnlockUpgrade`,
  `NTStripUnlockingHacks`, `LoadSaveSlot`, `EnumerateContent`, exec cheats) have identical bytecode
  (object/name operands compared by name). Differences are in menus, leaderboards, shop and the save slot
  screen.
- **Steam:** both versions ship the same `steam_api.dll` (same interfaces); the save key is the same.
- **Saves:** same Steam App ID 220440, same folder, same PROFILE format (profile data version 34). The INFO
  file (save slot screen text) differs: v1.3 writes 9 numbers; v1.0 writes the same numbers plus 3 ready-made
  text lines and shows only the text, so a slot written by v1.3 looks like "NEW SAVE" in v1.0 but loads fine.

## Files the tool writes (in `Binaries\Win32`)

| File / folder | What |
|---|---|
| `DmCPracticeTool.json` | settings, binds, presets, window position |
| `DmCPracticeTool.log` | log of the last game start |
| `DmCPT_missions.json` | your edited checkpoint list (only if you edit it) |
| `DmCPT_checkpoints\` | created checkpoints |
| `DmCPT_sets\` | checkpoint lists to share |
| `DmCPT_saves\import\`, `DmCPT_saves\export\` | save packages |
| `DmCPT_backups\` | save backups before Unlock / Import |
| `DmCPT_logs\` | damage log CSV files |
| `DmCPT_dumps\` | Dev tab dumps |
| `DmCPT_compat_report.txt` | compatibility report |
