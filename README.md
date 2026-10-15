# Faction Wardrobe

An SKSE plugin for Skyrim Special Edition / Anniversary Edition (runtime 1.6.1170): pick an outfit for any faction in-game, and every member of that faction wears it. Changes show up live.

## Features (v0.2.2)
- In-game editor in **SKSE Menu Framework**: search a faction, pick armor pieces from any loaded plugin (ESP/ESM/ESL), mix pieces from different mods
- Separate outfits for male and female members; empty means "leave them as they are"
- Priority per faction for NPCs who belong to several
- Real outfits: the game keeps NPCs dressed through `resetinventory`, cell resets and reloads; nothing runs every frame
- Nothing is written into your saves; uninstalling leaves NPCs in their vanilla outfits
- **Revert this faction**, **Revert all factions**, and an on/off switch that keeps your rules

## Requirements
- Skyrim SE/AE 1.6.1170, [SKSE](https://skse.silverlock.org/), [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- SKSE Menu Framework 2 (Nexus) for the in-game editor

## Install
Install with your mod manager and enable **FactionWardrobe.esp** (ESL-flagged, no load order slot). Settings are saved to `Data/SKSE/Plugins/FactionWardrobe.json`; with MO2 they may land in Overwrite.

## How it works
`FactionWardrobe.esp` holds 128 empty outfit records. Each faction rule gets one per sex, filled at runtime with the pieces you chose, and set as the default outfit of the members' NPC records. The change is undone just before each save and redone right after, so saves only ever contain vanilla outfits.

Generic NPCs often share one NPC record; when NPCs sharing a record would get different outfits, the first one loaded decides.

## Files
| Path | What it is |
|---|---|
| `src/main.cpp` | the plugin: settings, outfit logic, editor, save handling |
| `src/Rules.h` | which faction rule wins for an NPC (priority) |
| `src/PCH.h` | precompiled header |
| `src/SKSEMenuFramework.h` | SKSE Menu Framework 2 API header |
| `tests/tests.cpp` | rule-selection tests (no game needed) |
| `esp/FactionWardrobe.esp` | the 128 outfit slots (ESL) |
| `esp_tools/make_slots.py` | Python script that generates `FactionWardrobe.esp` |
| `FactionWardrobe.json` | default (empty) settings |
| `build.bat` | one-click build |

## Building
Requirements: Windows, Git, Visual Studio 2022 Build Tools with "Desktop development with C++" (includes CMake).

1. Edit `MODDIR` at the top of `build.bat` to point at your mod folder.
2. Run `build.bat`. It downloads vcpkg and CommonLibSSE-NG v11.0.0 on the first run (15-30 minutes), builds `build/Release/FactionWardrobe.dll` and copies the DLL, the esp and the default settings into `MODDIR`.
3. Tests: `ctest -C Release --test-dir build`.

## License
GPL-3.0-or-later (see `LICENSE` and `NOTICE.txt`).
Built with [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), licensed GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source).
