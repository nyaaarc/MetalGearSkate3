# Metal Gear Skate

Skate 3 skating inside METAL GEAR SOLID 3 (Master Collection Vol.1, Steam). Snake gets
on a board driven by the Skate 3 engine, riding MGS3's own stages and collision, with
Skate 3's controls, tricks, grinds and bails.

**No game files are included.** Setup will build what the mod needs from your own copies
of both games.

**Status: early.** Snake can skate on the genuine Skate 3 engine with the stage's real collision;
no board or skating animation yet. See "What works" below.

## What you need

1. **METAL GEAR SOLID 3** from Master Collection Vol.1 on Steam (64-bit `METAL GEAR SOLID3.exe`).
2. **Skate 3 for Xbox 360**, your own disc's ISO or Games on Demand copy, extracted to a
   folder with `default.xex` and `data` in it (`extract-xiso -x "Skate 3.iso"`).
3. **An ASI loader** in the MGS3 folder ([Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)).
4. **A controller.**

## Trying it (developers)

Put `MetalGearSkate.asi`, `skate_ffi.dll` and a `MetalGearSkate.ini` (see
`mgs3-plugin/MetalGearSkate.example.ini`) in the game's `plugins` folder, load a flat stage such as
Groznyj Grad (`s091a`), and press **F5** to start or stop skating with an Xbox controller.
The stage's real collision is loaded on F5; Snake still plays his walk animations, and no board is drawn yet.

## Repository layout

| Folder | What |
|---|---|
| `mgs3-plugin/` | The game side: a C++ ASI plugin (`MetalGearSkate.asi`) loaded into MGS3 |
| `skate-ffi/` | A 64-bit Rust DLL (`skate_ffi.dll`) wrapping the Skate engine in a C API |
| `mashup/` | The Skate engine crates, taken from [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup) at `f608f85` with its licence and notice |
| `release/` | Player setup and the release packer (not written yet) |
| `docs/` | MGS3 format and reverse-engineering notes, dev log |

## Building from source

1. Install [Visual Studio Build Tools 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
   with **Desktop development with C++**, and [Rust](https://rustup.rs).
2. Run `build.cmd`. It produces `mgs3-plugin\build\Release\MetalGearSkate.asi` and
   `skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll`.
3. To test: copy both next to `METAL GEAR SOLID3.exe` (with an ASI loader installed) and
   start the game. `MetalGearSkate.log` appears beside the plugin.

## What works

- `MetalGearSkate.asi` loads inside MGS3 (alongside MGSHDFix 4.1.2) and loads `skate_ffi.dll`; confirmed in game.
- `skate_ffi.dll` runs the genuine Skate 3 engine headless on converted data: `tools/ffi_smoke.py`
  puts the skater on a flat floor, pushes, and he rolls (~7.5 units/s, engine units look like metres).
- The plugin finds Snake by signature on every launch and reads his real position (writing it
  moves him in game). Camera look-at/eye and Snake's skeleton are mapped (`docs/formats.md`).

- Skating in game: F5 hands Snake to the Skate 3 engine (push, carve, brake on the stick) using the
  game's own pad state, so any Steam Input controller works; Snake faces the direction of travel.
- Stage collision: `.geom` decoded (`tools/geom.py`, Rust `skate-ffi/src/geom.rs`, identical output);
  the current stage's collision is loaded into the engine on F5.
- Board data for drawing: `mgsk_board_pose` gives the engine's seven board bodies (deck, trucks,
  wheels) as world transforms, and `mgsk_board_mesh` reads each part's mesh from the converted
  `skater.glb` in that body's frame; `tools/board_check.py` checks both off-game.
- The board is drawn into the game's frame (Present hook) with the game's own view-projection,
  read from its vertex-shader constants each frame; in game it sits on the ground under Snake.
- While skating, the whole frame is drawn from Skate 3's own chase camera (`mgsk_camera`): the
  game's camera constants are rewritten per draw, so scenery, Snake and the board all follow it.
- Retargeting groundwork: Snake's 57-bone skeleton decoded from `sna_def.mdl` (`tools/mdl_bones.py`),
  the skater's 36 bones exported (`mgsk_bone_count`/`mgsk_bone_name`/`mgsk_bones`), and a
  Snake-to-skater bone map with axis and rest-pose notes in `docs/retarget.json`.

## What's missing

- A stable lookup for the camera and skeleton
- Feeding MGS3 collision to the skate world; posing Snake from the skater (`mgsk_snake_pose` computes his bones, not applied in game)
- Drawing the board in game: flat colours only, no scene occlusion (no depth buffer is bound at Present)
- Setup (finding MGS3, converting your Skate 3 files), settings menu, audio

## Credits

- Skate engine: [skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) (GPL-3.0)
  by dumbad and contributors, as used in [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
  by chasmlol, a fork of [IW4L](https://github.com/vladtrc/iw4L) by vladtrc (Apache-2.0).
- Repository structure follows [GTA San AnSkateas](https://github.com/ryglizzy/GTA-San-AnSkateas) by ryglizzy.
- Direct3D 11 hooking approach (Present, state save/restore, coexisting hooks) studied from
  [MGSHDFix](https://github.com/ShizCalev/MGSHDFix) by ShizCalev and contributors (MIT).
- MGS3 format research: Jayveer, Joy-Division, GirianSeed and others; see `docs/formats.md`.

## License

GPL-3.0-only (see `LICENSE`), to match the Skate 3 Rust Engine. Unofficial fan project, not
affiliated with Konami or EA. You need to own METAL GEAR SOLID 3 and Skate 3.
