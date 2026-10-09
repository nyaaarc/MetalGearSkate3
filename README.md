# Metal Gear Skate

Skate 3 skating inside METAL GEAR SOLID 3 (Master Collection Vol.1, Steam). Snake gets
on a board driven by the Skate 3 engine, riding MGS3's own stages and collision, with
Skate 3's controls, tricks, grinds and bails.

**No game files are included.** Setup builds what the mod needs from your own copies of both
games.

**Status: early but playable.** Snake skates on the genuine Skate 3 engine on MGS3's real stage
collision: pushing, flip tricks, grinds and bails, posed from the skater, on a textured board,
with the game's camera following. See "What works" below.

## What you need

1. **METAL GEAR SOLID 3** from Master Collection Vol.1 on Steam (64-bit `METAL GEAR SOLID3.exe`).
2. **Skate 3 for Xbox 360**: your own disc image (`.iso`), or an extracted copy (a folder with
   `default.xex` and `data`). A PS3 copy won't work.
3. **A controller.**

## Installing

1. Download a release zip and unpack it anywhere (or build from source, below).
2. Close the game and run `setup.cmd`. It:
   - finds MGS3 through Steam (or asks for its folder);
   - installs an ASI loader if there isn't one ([Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
     v9.7.4 as `wininet.dll`; `-WithHDFix` installs [MGSHDFix](https://github.com/ShizCalev/MGSHDFix) 4.1.2 instead);
   - copies `MetalGearSkate.asi`, `MetalGearSkate.core.dll` (as `plugins\MetalGearSkate\core.dll`) and
     `skate_ffi.dll` into the game's `plugins` folder;
   - asks for your Skate 3 `.iso` (or folder, or `default.xex`), unpacks it with
     [extract-xiso](https://github.com/XboxDev/extract-xiso) and converts the skate data from it with
     the converter of [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
     v0.4.0, into `%LOCALAPPDATA%\MetalGearSkate` (about 180 MB; the 6 GB unpacked copy can be deleted after);
   - with Python 3 and numpy installed, exports the skater's sounds from your disc (decoded with
     [vgmstream](https://github.com/vgmstream/vgmstream) r2117; about 1.3 GB, a few minutes);
     without them skating works, silently;
   - writes `plugins\MetalGearSkate.ini` and, if Python is installed, rides the engine for 10 s to check it.

   Every download is asked for first and pinned by version and SHA-256. Options:
   `setup.cmd -DryRun` (show the actions only), `-Skate3 <path>`, `-GameDir <path>`, `-DataDir <path>`,
   `-DataOnly` (only reconvert the Skate 3 data), `-SkipConvert`, `-Yes`.
3. Start MGS3, load a stage (Groznyj Grad, `s091a`, from the developer menu's stage select is a
   good one) and press **F5** to start or stop skating.

Uninstall: delete `plugins\MetalGearSkate.asi`, the `plugins\MetalGearSkate` folder, `plugins\skate_ffi.dll` and `plugins\MetalGearSkate.ini`,
and the `%LOCALAPPDATA%\MetalGearSkate` folder.

## Controls

**F5** (keyboard) starts or stops skating where Snake stands. On the board, Skate 3's own controls
(Xbox names; on PlayStation layouts A = cross, B = circle, Y = triangle):

| Input | Does |
|---|---|
| A | push |
| B (hold) | brake |
| Left stick | steer (carve) |
| Right stick: pull back, then flick up | ollie (flick up-left: kickflip, up-right: heelflip) |
| Right stick: push up, then flick back | nollie |
| Ollie onto a ledge or rail | grind |
| Y | step off the board and back on |

## Trying it (developers)

Put `MetalGearSkate.asi`, `skate_ffi.dll`, `MetalGearSkate.core.dll` (as `MetalGearSkate\core.dll`) and a `MetalGearSkate.ini` (see
`mgs3-plugin/MetalGearSkate.example.ini`) in the game's `plugins` folder, load a flat stage such as
Groznyj Grad (`s091a`), and press **F5** to start or stop skating with an Xbox controller.
The stage's real collision is loaded on F5; the board is drawn with Skate 3's own textures, Snake takes the skater's pose, and the view switches to Skate 3's chase camera.

## Repository layout

| Folder | What |
|---|---|
| `mgs3-plugin/` | The game side, in C++: a small loader (`MetalGearSkate.asi`, an ASI plugin) and the plugin itself (`MetalGearSkate.core.dll`), which the loader can swap while the game runs |
| `skate-ffi/` | A 64-bit Rust DLL (`skate_ffi.dll`) wrapping the Skate engine in a C API |
| `mashup/` | The Skate engine crates, taken from [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup) at `f608f85` with its licence and notice |
| `setup.cmd`, `tools/setup.ps1` | Player setup (above) |
| `package.cmd`, `tools/package.ps1` | Builds the release zip in `dist\` (binaries, setup, README, licences; no game data) |
| `docs/` | MGS3 format and reverse-engineering notes, dev log |

## Building from source

1. Install [Visual Studio Build Tools 2022](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
   with **Desktop development with C++**, and [Rust](https://rustup.rs).
2. Run `build.cmd`. It produces `mgs3-plugin\build\Release\MetalGearSkate.asi`,
   `mgs3-plugin\build\Release\MetalGearSkate.core.dll` and
   `skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll`.
3. To test: `python -I tools/gametest.py install` (or `setup.cmd`), then start the game.
   `MetalGearSkate.log` appears beside the plugin. After a plugin change, `build.cmd` and
   `python -I tools/gametest.py reload` swap the new core in without restarting the game.
   `python -I tools/regress.py` then runs the regression suite in the game: pushes, tricks,
   grinds, bails, riding metrics, audio, the stage's edge, skate on/off and hot reloads, each
   with numeric pass criteria (`docs/TESTING.md`, "Regression gate").

## What works

- `MetalGearSkate.asi` loads inside MGS3 (alongside MGSHDFix 4.1.2) and loads `skate_ffi.dll`; confirmed in game.
- `skate_ffi.dll` runs the genuine Skate 3 engine headless on converted data: `tools/ffi_smoke.py`
  puts the skater on a flat floor, pushes, and he rolls (~7.5 units/s, engine units look like metres).
- The plugin finds Snake by signature on every launch and reads his real position (writing it
  moves him in game). Camera eye/target and Snake's skeleton are mapped (`docs/formats.md`).

- Skating in game: F5 hands Snake to the Skate 3 engine using the game's own pad state, so any
  Steam Input controller works. Checked in game (run 15, s091a): pushes build 2.7, 4.9, 7.6 m/s;
  ollie, kickflip, heelflip and nollie leave the ground (~1 m) and land; carving works, and holding B
  brakes (5.6 to 0.1 m/s in 2 s, checked by the regression gate).
  Grinds (50-50 into 5-0 on a ledge) and bails (wipeout, then the engine's respawn) work (run 16).
  Snake is posed from the skater on the board, and the board hides behind the scene and his legs.
- Stage collision: `.geom` decoded (`tools/geom.py`, Rust `skate-ffi/src/geom.rs`, identical output);
  the current stage's collision is loaded into the engine on F5.
- Riding off the stage puts the skater back where he last rode (Snake would otherwise fall to his death).
- Bails no longer break the engine: a subnormal dot product (which the Xbox 360 flushed to zero)
  turned into NaN on the landing after a bail respawn; it is flushed now. If the engine still
  fails, the plugin swaps in a spare session built in the background and resumes at the last spot.
- Board data for drawing: `mgsk_board_pose` gives the engine's seven board bodies (deck, trucks,
  wheels) as world transforms, and `mgsk_board_mesh` reads each part's mesh from the converted
  `skater.glb` in that body's frame; `tools/board_check.py` checks both off-game.
- The board is drawn into the game's frame (Present hook) with the game's own view-projection,
  read from its vertex-shader constants each frame; in game it sits on the ground under Snake.
- While skating, Snake is drawn in the skater's pose (his GPU bone palette is rewritten from
  `mgsk_snake_pose` each frame): pushing, riding and bailing.
- Smooth motion: the engine ticks once per game frame. Snake and the board are posed from the
  camera the frame is actually drawn with. In the soak test (`tools/soak.py`) every frame moves
  exactly one tick, and the camera's frame-to-frame jerk is 0.3-3.3 mm at p95 (it was 40-160 mm).
- While skating, MGS3's own camera follows Skate 3's chase camera (`mgsk_camera`; eye, aim and
  field of view set in the game's camera update), so the game draws and culls the scene from
  there and riding in any direction shows the whole stage.
- Grind rails: `mgsk_install_geom` finds the stage's grindable lips (ledges, kerbs, box and
  railing tops; `skate-ffi/src/rails.rs`) and gives them to the engine as rails. Offline,
  `tools/rails_check.py --grind` ollies onto 0.5 m ledges in s091a and the engine grinds
  (FiftyFifty/FiveO). Not yet tried in game.
- The skater's sound is Skate 3's own audio runtime (`skate-audio` from the Skate 3 Rust Engine:
  the AEMS patch programs, voice graph, MixMap, Splice one-shots and granular rolling bed),
  driven each engine tick by the engine's audio state and played through the default output
  device while skating: rolling by speed and surface, pushes, pops, flips, landings, grinds,
  skids, bails. Offline, `tools/audio_check.py` rides pushes, an ollie, a kickflip, a brake, the
  grind and the bail recipes in s091a and every one makes sound on the right ticks (the landing
  is the loudest moment, the grind sounds through `GrindFiftyFifty`). Not yet heard in game.
- Retargeting groundwork: Snake's 57-bone skeleton decoded from `sna_def.mdl` (`tools/mdl_bones.py`),
  the skater's 36 bones exported (`mgsk_bone_count`/`mgsk_bone_name`/`mgsk_bones`), and a
  Snake-to-skater bone map with axis and rest-pose notes in `docs/retarget.json`.
- Skatepark groundwork (offline, `docs/skatepark.md`): `tools/park_props.py` reads all 661 of
  Skate 3's Object Dropper props and Skate.Park editor pieces from the player's `parkassets.big`
  (render meshes, textures, retail collision and grind splines). `tools/props_ride.py` checks
  that the engine rides their collision as is: a kicker launches the skater and he airs out of
  a quarter pipe.

## What's missing

- A stable lookup for the camera and skeleton
- A held weapon and Snake's shadow are unchecked while skating (his gear and head now follow the pose)
- Board textures not yet checked in game (simple diffuse lighting, no specular)
- Settings menu
- Skateparks in game: props (and later a whole spot such as Black Box Park) placed on MGS3 stages,
  combined with the stage collision and drawn like the board. The plan is in `docs/skatepark.md`.
- Audio beyond the skater: Skate 3's ambience, world emitters and speech aren't played (MGS3's own
  sound keeps playing); the skater's sound hasn't been listened to in game yet
- Setup is tested in dry runs and data-only runs (ISO → extract → convert → engine check); a full
  install on a clean machine hasn't been tried yet

## Credits

- Skate engine: [skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) (GPL-3.0)
  by dumbad and contributors, as used in [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
  by chasmlol, a fork of [IW4L](https://github.com/vladtrc/iw4L) by vladtrc (Apache-2.0).
- Repository structure follows [GTA San AnSkateas](https://github.com/ryglizzy/GTA-San-AnSkateas) by ryglizzy.
- Direct3D 11 hooking approach (Present, state save/restore, coexisting hooks) studied from
  [MGSHDFix](https://github.com/ShizCalev/MGSHDFix) by ShizCalev and contributors (MIT).
- Skater audio: `skate-audio` / `skate-audio-fma` (vendored in `mashup/`), the host files in
  `skate-ffi/src/audio/` (ported from `game_audio/`) and the disc audio export in
  `tools/skate_audio_export/` (`tools/asset_pipeline/`) all come from
  [skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) at `b3c96793` (GPL-3.0),
  whose audio port credits its own specs and the community's research; the export's BIG/RefPack
  readers are MIT (Skate 3 Custom Engine Layer contributors, notices kept with them).
- Grind rail finder (`skate-ffi/src/rails.rs`) ported from `render_anim/src/skate/rails.rs` in
  [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup) by chasmlol (Apache-2.0).
- Setup downloads, each under its own licence (kept beside the files it installs):
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG (MIT),
  [MGSHDFix](https://github.com/ShizCalev/MGSHDFix) (MIT, optional),
  [extract-xiso](https://github.com/XboxDev/extract-xiso) (its `LICENSE.TXT`), the Skate 3 data
  converter from [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup) v0.4.0
  (Apache-2.0, with its NumPy, Pillow and Python licences) and
  [vgmstream](https://github.com/vgmstream/vgmstream) r2117 (ISC, its `COPYING`).
- Release zips also carry the licences of [safetyhook](https://github.com/cursey/safetyhook) (BSL-1.0),
  Zydis/Zycore (MIT) and the Rust crates linked into `skate_ffi.dll` (`licenses/rust-crates.md`).
- MGS3 format research: Jayveer, Joy-Division, GirianSeed and others; see `docs/formats.md`.
- Skate 3 park data (`tools/park_props.py`, our own code): the RX2 model, texture, ClusteredMesh
  and tSplineData layouts were learned from the MIT-licensed readers that
  [skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) vendors: `tools/vendor/utt`
  (rx2_parser, mdl_parser) by duckyinnit, and the Skate 3 Custom Engine Layer contributors'
  `retail_collision_mesh.py` / `retail_grind_splines.py`. BIG archives are read with the
  MIT BIG/RefPack readers already vendored in `tools/skate_audio_export`.

## License

GPL-3.0-only (see `LICENSE`), to match the Skate 3 Rust Engine. Unofficial fan project, not
affiliated with Konami or EA. You need to own METAL GEAR SOLID 3 and Skate 3.
