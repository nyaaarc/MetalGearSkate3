# MGS3 formats: what we know

Target: METAL GEAR SOLID 3 from Master Collection Vol.1 (Steam), v3.0.1. Research done
2026-10-07. Anything marked **unverified** needs checking against a real install.

## Install layout

From the file list in [MGS3 Community Bugfix Compilation](https://github.com/ShizCalev/MGS3-Community-Bugfix-Compilation)
(`mgs3 mc 3.0.1 crc32s.csv`, MIT) and the [KCEJ-Wiki HD FileSystem page](https://github.com/Joy-Division/KCEJ-Wiki/blob/master/Game/MetalGearSolidHD/FileSystem.md).
Assets are unpacked, not archived, and sorted as `assets/<ext>/<region>/`.

| Path | What | Reader / spec |
|---|---|---|
| `assets/mdl/us/_win/<name>.mdl` + `.cmdl` | Models. Stage geometry is split into chunks (`s003a0.cmdl`, `s003a1_pond1.cmdl`); stage props carry `_stage_<stage>` | Jayveer's MGS-Master-Collection-Noesis (`cmdl.h`: magic `MODL`, some big-endian blocks; `mdl.h`) |
| `assets/tri/<region>/<hash>_stage_<stage>.tri` | PS2-era texture containers | same repo, `tri.cpp` |
| `textures/flatlist/_win/<strcode>.ctxr`, `hqtex/flatlist/_win/` | HD textures, roughly DDS ARGB8888 plus mips | [CtxrTool](https://github.com/Jayveer/CtxrTool), [CTXR-Converter](https://github.com/316austin316/CTXR-Converter) |
| `assets/geom/us/<stage>.geom` | Collision (decoded below; `tools/geom.py`) | none found; our own decode |
| `assets/{nav,zon,lt2}/us/<stage>.*` | Navmesh, zones, lighting (`_d`/`_n` = day/night) | none found |
| `<region>/stage/<stage>/manifest.txt`, `bp_assets.txt` | Indexes from names to hashed asset paths | plain text |
| `.mtar` / `.gcx` / `.sdx` | Animation / scripts / audio | Noesis plugin / [Gcx](https://github.com/Jayveer/Gcx) / — |

Stage IDs look like `s001a` to `s171b`. **Unverified:** in the file list, `us/stage/*` has
only `.sdx` and the manifests sit under `jp/` and `fr/`.

## Plan for the first stage

1. Read the `.cmdl` chunks for one stage as the render mesh (spec: Noesis plugin, SeaLouse's
   MGS2 CMDL code as a cross-reference).
2. Resolve textures through `bp_assets.txt`/`manifest.txt` and decode `.ctxr`.
3. Collision: build an avian trimesh from the render mesh first. Decode `.geom` later,
   using the GirianSeed decomp's `map.c`/`env_stage.c` as a spec.

## Licensing of sources

Almost every Jayveer repo has no license. Read them as a specification and write a
clean Rust implementation; do not copy code. Licensed and usable directly: Joy-Division
[tools-mgs](https://github.com/Joy-Division/tools-mgs) (Zlib), [JoyDict](https://github.com/Joy-Division/JoyDict)
(StrCode hash dictionary), [gcx_decompiler](https://github.com/oct0xor/gcx_decompiler) (MIT).

## Decompilation

- [GirianSeed/mgs3](https://github.com/GirianSeed/mgs3): PS2 MGS3 decompilation in C, no
  license, active. Has `map.c`, `env_stage.c`, `fmt_mdl.h`, libgv/libdg. Best source for stage
  and collision structures. Read only; nothing from it goes in this repo.
- [FoxdieTeam/mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing): MGS1, same engine lineage.
- GameDecompLibrary has no Metal Gear entries.
- **Do not use** `stevefan1999-personal/MGS2-Source`: it appears to be leaked Bluepoint source.

## Skate 3 mechanics (observation only, no PC release)

- Flick-It gestures are stick paths graded on speed and accuracy, sampled at 120 Hz
  ([Game Developer, 2008](https://www.gamedeveloper.com/game-platforms/ea-s-blackwood-text-based-prototype-perfected-i-skate-i-s-control-scheme)).
- Classic gestures ([skate wiki](https://skate.fandom.com/wiki/Tricks), [EA on the Classic preset](https://www.ea.com/games/skate/skate/news/get-control)):
  hold down to set up (longer = more pop); down→up ollie; down→up-diagonal kickflip/heelflip;
  down→big scoop pop shuvit; mirrored from up for nollies; slight down/up hold = manual / nose
  manual; triggers = grabs.
- Rider is physics-driven with blended animation; bails come from landings the body can't absorb.
- Open-source controllers to study: [3deric/godot-skateboarding-game](https://github.com/3deric/godot-skateboarding-game) (MIT).

## Verified against the installed game (2026-10-08)

- Install: `C:\Program Files (x86)\Steam\steamapps\common\MGS3`, 18 GB.
- `METAL GEAR SOLID3.exe` and `Engine.dll` are x64. `launcher.exe` + `UnityPlayer.dll` are a
  Unity launcher in front of the game. `Renderer.dll` is not a PE file (it starts with an
  `ar` archive header).
- The exe carries a `.bind` section, i.e. a SteamStub wrapper, so its on-disk `.text` is
  not readable statically. We do not strip it. Work on the running process instead
  (signature scans over the unpacked code, as other Master Collection mods do), plus the
  GirianSeed PS2 decomp for structures.
- ASI loader proxy: `Engine.dll` imports `WININET.dll`, so Ultimate ASI Loader x64 named
  `wininet.dll` in the game folder gets loaded. The exe imports `d3d11.dll`/`dxgi.dll`,
  also usable.
- Confirmed: `us/stage/<stage>/` holds only `.sdx` files (104 stage folders);
  `assets/geom/us/*.geom` exists per stage (e.g. `c001b.geom`). `assets/` has
  `cvd eft far gcx geom ico img la2 lt2 mdl mtar mtsq nav rat raw rcm rpd slot tri var zon`.

## Existing Master Collection mods to build on

[MGSHDFix](https://github.com/ShizCalev/MGSHDFix) (MIT, active, ~880 stars) hooks the
running MC executables by signature scan, with safetyhook + zydis + spdlog + inipp. It
installs through Ultimate ASI Loader as `wininet.dll`. Its code uses the PS2-era KCEJ
names (`GM_`, `GV_`, `DG_`), the same names the GirianSeed decomp uses. MIT, so we can
reuse its code with attribution.

What it already locates for **MGS3** (`src/resources/gamevars.cpp`, `game_funcs.hpp`):
`LinkVarBuf` (`mgs3_linkvarbuf.hpp` maps the save/game-state buffer), `GV_PauseLevel`,
`GM_GameStatus`, `GM_MenuStatus`, `DG_Clock`, `GM_SetArea`/`GM_GetArea` (current stage),
the stage list (`game_stages.hpp`), a dev-menu stage-select start option, camera-offset
and map-relight fixes.

What it locates only for **MGS2** so far: `GM_PlayerWork`, `GM_PlayerBody`,
`GM_CameraDir`, `GM_CameraTarget`. Same engine family, so these are the first MGS3
analogues to find, guided by the MGS2 patterns and the GirianSeed decomp.

[MGS3 Community Bugfix Compilation](https://github.com/ShizCalev/MGS3-Community-Bugfix-Compilation)
(MIT) is asset-side (textures/models); useful for its full MC file list.

## Runtime findings (MGS3 MC 3.0.0)

- **Camera system (static), `exe+1D39D80`:** a table of camera objects (0x5B0 bytes each;
  entry 0 is the main camera, `exe+AFB80` compares against it). `+0x10` pointer to a heap
  camera-target object, `+0x40` **eye** (float3 + w = 400, mm), `+0x50` **target** (float3 +
  w). (Earlier notes had these two swapped: in s091a the target sits on Snake and the eye
  ~3.9 m behind and 1 m above.) Smoothing (`exe+213xxx`, weights of 1/6) rewrites them every
  frame, so a plain write is undone. The update `exe+AF050` (rcx = camera; found by its body
  `0F 10 41 40 48 8B 79 10 4C 8D B1 E0 02 00 00` at +0x73) copies them to working copies
  `+0x2D0`/`+0x2E0`, builds the view with a look-at (`exe+C4290`: out, eye, target; fixed up)
  plus roll (`+0x94`, i16) and an offset matrix (`+0x580`), optionally clamps eye and target to
  boxes in the target object (`+0x1E0..+0x1F0`, `+0x200..+0x210`, when its `+0xC` bits 8/9
  are set; skipped when camera flag `0x100` is set) and adds eased offsets (`+0x60`, `+0x70`).
  It then turns eye/target into angles: pitch `+0x320` = atan2(-dy, horizontal), yaw `+0x322`
  = atan2(dx, dz) (i16, 65536 = turn), distance `+0x328`. **If the yaw is more than 0x78E3
  (~170 degrees) from a reference yaw at `+0x92`, or the pitch exceeds 0x38E3 (~80 degrees),
  it forces an 80-degree overhead view along `+0x92`** (`exe+AF835..AF88F`). Zoom: `+0x9C`
  (1.9 in s091a) times `+0xA0` (1.0) goes to `+0x32C` and on to the projection setup with the
  view (`exe+?DE60`); the vertical projection scale is 1.281 x zoom (2.434 at 1.9). Writing eye,
  target, `+0x92` (the chase camera's own yaw) and `+0x9C` at the start of the update puts the
  game's camera, and so its drawing and culling, anywhere (`camera.cpp`, `render camera drive`).
- **`linkvarbuf+0xA58` (= `exe+1E236CC` this build):** int32 copy of the look-at point,
  written by `exe+AFB9B` (`cvttss2si ecx,[rsi]; mov rax,[linkvarbuf]; mov [rax+0xA58],ecx`).
- The look-at point is about 2 m above the ground, so it's not Snake's root. Its negation
  appears in view-transform copies (`-pos`, w=1) used by the renderer (`exe+CBE91` copies them).
- Not yet found: Snake's own actor position. No exact x/z copy of the look-at point exists
  anywhere else (float, int16, int32, fixed-point, double).
- **Snake's skeleton (world space):** an array of 4×4 float matrices, stride 0x40, rows
  X/Y/Z axis then translation `(x, y, z, 1)` in mm. ~62 entries; 8–57 are Snake (legs,
  spine, arms, 34 finger bones). Feet sit at ground height. Heap, rebuilt every frame by a
  `memcpy` (`VCRUNTIME140.dll`) and by `exe+C4A9A` / `exe+CE8FC` (render-object update,
  `rbp`/`rcx` = owning object). It's output, so writing it does nothing until we hook after
  those writers.
- **`exe+1D76B20`:** static `(src, dst)` pointer pair, duplicated at `+0x10`; `dst` points
  into the bone array, `src` at another heap block (the copy source). `exe+1D76B00` points
  at `exe+1D758D0` (probably a list head). A stable way into the skeleton every launch.
- **Bandana tails:** static table of 0x50-byte records at `exe+1D78510..`, position +
  quaternion + segment length, two alternating chains. Writing a position sticks (physics).
- Snake's gameplay (actor) position is still not found; every float/int copy of his position
  tracks the bones or the camera. Plan: hook the bone writers instead (see devlog).
- **Snake's gameplay position: FOUND.** float3 at the heap address `pos` (this run
  `0x1EADFA4C950`), value ≈ waist height (feet + ~800 mm). Writing x moved Snake in game
  and the render root (`obj+0x50`) followed; his feet stayed planted at the old spot
  (foot IK/ground-contact state, probably the static records at `exe+1D786A0..` with
  y = ground). Layout around it: `pos-0x10` = `(2.0, 2.0, …)`, `pos+0x1C` ≈ 0.31 (heading?).
- **Static pointers to Snake (candidates, verify across a restart):** `exe+1E16CD8 → pos-0x10`,
  `exe+1E16CC0 → pos-0x130`, `exe+1D792F0`/`exe+1D79308 → pos-0x130`,
  `exe+1D2E5C0 → pos-0x160`, `exe+1E22AC0 → pos-0x1F0`.
- **Snake's render object:** heap, `+0x20` 4×4 world matrix (translation at `+0x50`),
  `+0xB0` bone count (57), `+0xC0` bone matrix array; `+0x00` → static model data
  (`exe+1E222A0`). `exe+CE760` copies its bones (plus a global origin offset at
  `[[exe+1D79840]+0x1D0]`) into a shadow buffer; that buffer is what the earlier lift test hit.
- **Render camera (D3D11 vertex-shader constants).** The camera object above is *not* what the
  frame is drawn with. Every stock 3D vertex shader (dumped from memory, D3DDisassemble) binds one
  `$Globals` cbuffer at b0, compiled `row_major` (rows contiguous, `clip = M * v`), >= 3472 bytes:
  `gVS_FogColor` @0, `gVS_FogParam` @16, `gVS_LightDir` @32, `gVS_LightCol` @96, `gVS_Pers` @480,
  `gVS_Opt0` @544, `gVS_Mat0` @608 (rigid models), `gVS_Mat[]` float4x3 from @624 (skinned, view
  space bones). Rigid: `clip = gVS_Pers * gVS_Mat0 * pos`. For stage geometry `gVS_Mat0` is a pure
  translation by minus the camera's look-at point and `gVS_Pers` is projection x view rotation with
  no translation (row 3 = camera forward, unit; e.g. s091a: x row `-1.06 * (1,0,0)`, y row scale
  ~2.36-2.43, z row `(~0, ~0, ~0, 50)`). So depth is `50 / w` (w = mm along forward; nearer =
  larger, no far plane) and the pair's product is the view-projection for the full back buffer.
  Skinned draws use a `gVS_Pers` with translation folded in. The game fills `$Globals` with
  `Map(WRITE_DISCARD)` per draw (~1500/frame on s091a), never `UpdateSubresource`. No depth
  buffer is bound at Present; the scene's is a depth-stencil view bound while main-camera
  draws fill `$Globals`: 3440x1440 `R24G8_TYPELESS` (format 44) x4 MSAA, the one Snake is drawn
  into; on some frames a 1x one of the same size and format is the most used instead (run 12).
  The plugin copies the x4 one at Present (typeless copy, read through
  `R24_UNORM_X8_TYPELESS`) and tests the board against it in its pixel shader; whether it still
  holds the scene by Present is unconfirmed (`render depth_debug 1` logs both depths). `g_View`/`g_ViewProjection` exist in the layout but are unused.
  Every main-camera `gVS_Pers` (rows 0, 1, 3 orthogonal, row 3 unit, row 2 = `(~0, ~0, ~0, k)`)
  factors as `S * T(d)`: S the frame's projection x view rotation, d = 0 for stage and rigid
  models (`gVS_Mat0 = T(-L) * world`, L = look-at) and an object origin minus L for skinned ones
  (checked: k = 50.006 after taking d out of Snake's). So the world is `T(L + d) * gVS_Mat0 * v`,
  and replacing `gVS_Pers` by `VP * T(L + d)` (VP with row 2 = `(0, 0, 0, k)`) redraws the draw
  from any camera VP. The game draws a stage piece before any other main-camera draw (0 draws
  before L was known per frame on s091a). Light/shadow passes have `row 3 = (0, 0, 0, 1)`.
- **Bone palette (skinned draws).** `gVS_Mat[64]`, `row_major float3x4` at byte 624 of `$Globals`
  (48 bytes per slot), indexed per vertex by `BLENDINDICES` (4 bones, `BLENDWEIGHT`); each mesh
  draw uses a few slots (bone remap per draw; ~550 Snake draws/frame). For Snake every slot is,
  exactly (0.0 mm on a dump): `[R^T | T - O - R^T * rest]`, with `R`/`T` a bone's world matrix as
  the game computed it (rows = axes, translation row, mm; the same values as `[obj+0xC0]`),
  `rest` that bone's rest position from `sna_def.mdl` (no rest rotations), and `O` the render
  object's translation (`obj+0x20` row 3) = `L + d` from that draw's `gVS_Pers`. So a slot is
  bone x inverse bind relative to O. The palette is *not* read from `[obj+0xC0]` after
  `exe+CE760` starts: overwriting those bones there changes nothing on screen. His gear is a
  separate skinned object (another O, ~30 draws).
- **Signatures (MC 3.0.0, both unique in live code):**
  - `48 8B 0D ?? ?? ?? ?? 8B E8 48 85 C9 74 08 8B 81 D8` (disp at +3) → `exe+1E16CC0`,
    pointer to Snake's actor (`pos-0x130`). Used at dozens of sites with a null check.
  - `48 8B 05 ?? ?? ?? ?? 0F BF 4B F8 0F BF 50 42 E8` (disp at +3) → `exe+1E16CD8`,
    pointer to Snake's movement block (`pos-0x10`). The plugin uses this one (`game.cpp`).

### Stage loading (how the developer menu does it)

- `GM_SetArea(int id, const char* name)`: `id` is `StrCode` (24-bit rotate-5-and-add) of the
  *previous* area, normally `GM_SaveArea` (link-variable buffer `+0x10`): `select` = 8888768,
  `s091a` = 4778632, `s092b` = 4778665.
- A developer-menu stage load does not call `GM_SetArea` itself. It queues the load (GM_Result
  9999, `GM_LoadRequest` 16) and the game loop at `exe+9D8B0` calls `GM_SetArea(GM_SaveArea,
  name)` (call site `exe+9D945`, logged as "called from exe+9D94A"), taking `name` from the
  GCL scenario stream (`exe+E4D90` reads the stream pointer at `[rip+0x1D47505]` and parses an
  entry with `exe+E4E60`, which also yields extra values, probably the entry's start point).
  `GCL_ChangeSenerioCode(StrCode("scenerio"))` runs once, when entering stage select.
- The stage list's `[+]` entries hold variants that set the story state: e.g. `s092a[+]` has
  `(s092a_0)`, `(s092a)Night`, `(s092b)Night`, `(s092c)Morning`, `back`. A variant writes about 240
  values into the first ~2 KiB of the link-variable buffer (item counts 999, flags, time of day)
  and picks where Snake starts.
- The harness's direct load (`game::load_stage_now`: `GM_SetArea` + `GCL_ChangeSenerioCode` +
  request 16) skips that. On a fresh launch it has no story state: s092a/s092b put Snake at
  (-306, -97062, 244), under the stage (instant game over), and s091a starts with a scripted
  run-in that never ends (he runs into the trench wall, ignoring the pad). After a menu load the
  state persists, so later direct loads start differently (s092b on the floor; s091a in the
  escape sequence's run-in at (17339, 800, -37146)). Writing the menu's 2 KiB of state back
  before a direct load does not fix the spawn, so the start point is set elsewhere (the scenario
  stream entry). Link-variable `+1636/+1640/+1644` (int32 x, y, z) track Snake's live position;
  they are not the spawn input.

## `.geom` (collision) — decoded, reader `tools/geom.py`

Stages load geom via `<lang>/stage/<stage>/manifest.txt` (`s001a` → `assets/geom/us/c001b.geom`,
so not every stage has a same-named file). Little-endian, mm, Y up. Offsets are absolute file
offsets unless noted. **[V]** = verified against the bytes (all 94 files where stated),
**[L]** = likely, **[?]** = unknown/guess.

Check: `python -I tools/geom.py c001b.geom --probe X,Y,Z` lists every surface in the vertical
line through a point and names the floor (highest up-facing surface at or below Y+50). Against
the eight v001a Snake samples: six waist-height samples sit 800–816 mm above that floor, the feet
sample (-10337, 1250, 22145) is 6 mm off. **The eighth sample, (7170, ~1200 feet, 17840), does
not fit any surface and we treat it as a bad sample, not a decode gap** [L]:
- Floor there is y=239 (961 mm below); the line holds only 239, 3340 (tree canopy), 5549/5877
  (down-facing), 7168. Neither reading fits: feet → 1200, waist → floor 400 (161 mm off).
- Sample A (5893, 897, 18312), 1.36 m away, stands 814 mm above floor y=83, and the floor rises
  in one smooth slope from A to H (83, 126, 144, 161, 179, 196, 211, 225, 239 at eighths). A
  1200 mm floor at H would need a 1 m step inside that slope.
- Within 3 m of H no floor lies within 150 mm of 1200 (72 points on rings of 0.5/1/2/3 m; the
  closest floors are 100–670). The only structure nearby is a 3 m-tall L-shaped block
  (x 4625–7625, z 16000–18000 / 16875, mesh boxes 0x6EC30 and 0x71B00, walls only, no top
  or roof) whose north wall H is 965 mm outside of.
- Nothing outside the decoded triangles supplies a surface: the one unplaced section-1 shape
  sits at x -4000..-876; the 0x41 boxes near H are 2 m-tall columns from y 100 (tops ≈2163);
  the 0x21 boxes are area volumes several metres across; no float3 in sections 5/6/7 lies
  within 2.5 m of H at y 700–1700.
So H was most likely not taken standing (e.g. mid-fall or mid-animation; v001a opens with the
HALO landing). Pass criterion used: 7 of 8, with H excluded for the reasons above.

**Header**
- `0x00` magic `82 F8 F1 0B` [V 93/94; `s211a1` has `B2 F0 F1 0B` and parses the same]. `0x08` u32
  = section-table slots incl. terminator [V]. `0x18` = 0x90 [V]. `0x04`, `0x14` u32 offsets [?].
- `0x20` section table, 16 B `(u32 type, u32 n, u64 offset)`, types ⊂ {1,5,6,7}, ended by type 8
  or a zero slot [V]. Section 1 = shared meshes for instances (below) [V]; 5/6/7 are not needed for
  collision triangles [?: 6 is a linked list of 48 B nodes with ids and points; 7 is 0x30 records].
  `n` is not a record count [?].
- `0x18` u32 = 0x90 = start of the **grid list**: 0x40 B descriptors, one or more [V 94/94]:
  `+0` origin float3, `+0x10` u16×3 cell size, `+0x18` u16×3 dims (nx, ny, nz), `+0x20` u32
  1 = last descriptor (else 0), `+0x24` u32 0x00300008 (cell stride 0x30, 8 slots) [L],
  `+0x28` u64 end of this grid's data [L; 0 in a few], `+0x30` u32 cell table, `+0x38` u32
  group-header table (0 = empty grid, e.g. `s163b`, two in `s005a1`). The first cell table
  always starts right after the list (0x90 + 0x40·n) [V]. **45 of 94 files have 2–17 grids**
  (c004b 4, s091a 5, s121a 2, s182a 17); c001b has one. Grids are separate areas of the
  stage (s121a grid 1 is a building block south of grid 0's corridor) [V by render].

**Cells** — 0x30 B each from the grid's cell table. Cell `i`: `ix = i % nx`, `iz = (i / nx) % nz`, `iy = i / (nx·nz)`;
origin + index·size [V: 0x2012 pool origins and instance boxes fall in their cell]. `+0` u32 mask,
bit `k-4` set ⇔ u32 at `+4k` is used; values are byte offsets from the grid's group-header table
to the **head of a header run** [V]. In practice slot 5 → solid (0x11/0x10), slot 6 → 0x21,
slot 8 → 0x41 [V c001b]. Cells overlap loosely: a run's triangles often extend into the
neighbouring cells.

**Header runs** [V 94/94]: a header whose type has low nibble 0 is followed by another header
of the same run (the next 0x30 B); the last has low nibble 1. Walking the runs from every
cell reaches every header in every table exactly once (0 missed, 0 shared, never runs into
another head). The earlier "unreferenced headers" were these continuations: 1510 solid
headers across the files, e.g. most of s112a's building interior. Section-1 shapes use the
same runs, and there the head's `+0x28` u64 points just past the run (456/456 match).

**Group header** — 0x30 B: `+0` u16 type (bits 4–7 of the low byte = layer: 0x1_ solid, 0x2_,
0x4_; low nibble 0 = run continues, 1 = last, 3 = seen only in unplaced section-1 shapes;
high byte [?]), `+2` u16 [?], `+4` u32 offset of the *last* record in 16 B units from the first [V],
`+8` -1 [V], `+0x14` u32 material/flags [L], `+0x20` u32 first record [V].

**Records** — 16 B aligned, a doubly linked list per group [V, all chains close in 94 files]:
`+0` u16 kind, `+2` u16 (high byte = item count, low byte flags), `+4` / `+8` signed 28-bit
links to next / previous record in 16 B units (0 = none), `+0x10` u32 id [?].
- `0x2012` triangle mesh [V]: `count = (+2) >> 8` triangles of 0x30 B from `+0x20`.
  Face at `S`: `S+0x0C` u32 material [L], `S+0x10` six u32 offsets *relative to S*:
  `v0, v1, v2, v3, normal, origin`. If `v3` is the same offset as `v0` the face is a triangle;
  otherwise it is a **planar convex quad** `v0 v1 v2 v3` = triangles (v0,v1,v2) + (v0,v2,v3)
  [V: 1262 of 7216 faces in c001b, all with v3 in the face plane and opposite v1 across the
  v0–v2 diagonal]. One s162a0 vertex is an all-zero unit with tag 0, which the quad needs as
  (0,0,0). Each vertex/normal is 8 B `(i16 x, i16 y, i16 z, u16 tag)`:
  tag 1 = position in mm relative to `origin`, tag 32000 = unit normal ×32000. `origin` is float4
  `(x, y, z, w)` at the end of the shared vertex pool (often outside the record). w = 2.0 for
  world-space meshes; ≈ -1.9992 for section-1 meshes whose origin is minus their box half size
  [? meaning of w; scale 1 fits: 100% of 7298 stored normals equal cross(v1-v0, v2-v0), so the
  winding is counter-clockwise seen from the normal side].
- `0x2004` mesh box [V]: `+0x0C` child link (16 B units; 5 → 0x2012 at `+0x50`, 0 → empty box,
  145 across all files), `+0x20` float3 half size + u32 material, `+0x30` float3 centre.
  `0x2014` is the same thing (only in `s211a1`).
- `0x2005` instances [V]: `count = (+2) >> 8` entries of 0x90 B from `+0x20`: `+0` half size +
  material, `+0x10` local box centre, `+0x30` 3 rotation rows (float4 each), `+0x60` position,
  `+0x70` u64 → the head of a header run in section 1 (all headers of the run hold the mesh;
  106 shapes have more than one header). World =
  `local · R + position` (rows are basis vectors) [V: 1314/1314 rotated instance boxes overlap
  their cell; transpose gives 1229]. c001b's trees are these (15 m trunks with branch quads).
- `0x2024` axis-aligned box: `+0x20` half size, `+0x30` centre. `0x2023` oriented box: `+0x20` half,
  `+0x50` rotation rows, `+0x80` centre. Layer 0x21 only [V layout; ? purpose: some enclose
  Snake's walking space, so not solid — camera/area volumes?].
- `0x2044` / `0x2043`: the same box layouts plus trailing packed data [?], layer 0x41 only [? purpose].
- Material words seen: 0x10DC2, 0x810DC2, 0x10808000 (tree leaves), 0x292DF4, … [? bit meaning].

**Section 1** starts with a table of 0x90 B entries laid out like `0x2005` entries (identity
rotation, zero position, `+0x70` → run head), ending at a zero pointer [V]. Every instance
target is in it. 705 of 1346 shapes across the files are placed by no instance; 224 of those
are world-positioned meshes away from the origin (c004b: 3 shapes, 6615 triangles, inside the
grid area). Their use is unknown [?: script-placed objects or alternate states?].
`tools/geom.py --loose` exports them as separate objects; the default export leaves them out.

**Reader output**: default = every solid run of every grid, plus instances, with exact
repeats removed (same triangle reached from several cells/placements; the raw count is in
`--stats`). The OBJ welds vertices at 0.1 mm. Every 16 B-aligned linked record of a known
kind in all 94 files is reached by grids, instances or `--loose` shapes (`--boxes` for 0x21/0x41).
A 16 B scan for known kind words finds unreached hits whose links are both zero (e.g. four
"0x2004" in s162a at 0x7A570…); every one lies inside the 0x30 B face array of a reached
0x2012 or the body of a reached 0x2005/box record (a face whose first word happens to read
0x2004, 0x2005, …), so they are not records [V 94/94]. The other hits have links that point
nowhere sensible (8-byte vertex units with tag 1 whose x is 8212 = 0x2014, e.g. 9 in s075a).

MGS1's HZD (FoxdieTeam/mgs_reversing `fmt_hzd.h`) is not the layout; vocabulary only.
- **Controller state (emulated DualShock 2, MC 3.0.0):**
  - Raw pads from Steam Input at `exe+1E2C4F8`, 0x24 bytes each: `+0` u32 status (0xF0000 bits),
    `+8` u16 buttons, `+0xC` sticks RX RY LX LY (0..255, centre 0x80, up = 0), `+0x10..` pressure
    bytes (Cross → `FF`). Read directly by other code too (e.g. exe+24AE6, exe+1912AF).
  - Processed pads at `exe+1D798F8`, 0x44 bytes each, 2 pads: `+0` i32 (-1 when connected),
    `+4` u16 buttons, `+6` u16 (0x30 seen), `+8` RX RY LX LY. Gameplay reads these.
  - Buttons are PS2 bits, active high. Verified: Cross = 0x4000 (Steam Controller "A"),
    left stick Y forward = 0. Rest assumed standard PS2 order.
  - Per-frame pad update `exe+111DF0` copies raw → processed for 2 pads (calls `exe+111C80`
    per pad). Signature `48 8D 2D ?? ?? ?? ?? 44 8B F1 48 89 6C 24 68` (disp at +3) → pads.
    `exe+111C80(dst=rcx, src=rdx)` is the per-pad copy: src = the 0x44 records at exe+1D798F8
    (Steam Input side), dst = the 0x24 records at exe+1E2C500 (src+4 → dst+0, 0x18 bytes). Earlier
    notes calling 1E2C500 "raw" had the direction backwards. Blanking
    the processed pad after the update is NOT enough (Snake kept moving: later copies come from
    raw); the plugin hooks the per-pad copy and blanks the raw record before it runs.
- **Snake's facing:** i16 at `control+0x42` (target copy at `+0x4A`), 65536 = full turn.
  Found by turning in place. Zero direction and handedness vs the Skate engine not yet matched.

## Snake's skeleton

- **Model:** gameplay Snake is `assets/mdl/us/sna_def.mdl` (model strcode `413aa8` = `GV_StrCode("sna_def")`).
  It is in no stage `manifest.txt`, so it's loaded as a resident asset. The camo variants
  `sna_def_slot_camoufla-*.mdl` have the identical skeleton; cutscene `sna_def_dmo.mdl` has 89
  bones (the same 57 plus duplicated finger chains). Reader: `tools/mdl_bones.py`.
- **`.mdl` bone table** (`MDC ` magic; spec from Jayveer's Noesis `mdl.h`, read only): header
  `+0x04` bone count, `+0x18` bone offset (0x50); bones 0x50 bytes: strcode, flags, int32 parent
  (-1 root, always an earlier index), float4 position relative to parent, float4 rest position
  in model space, float4 bounds max/min. **Translation only**: no rest rotations, so each bone's
  bind orientation is identity and a runtime bone matrix is that bone's model-to-world transform.
  Names are 24-bit strcode hashes; brute force up to 4 chars and common bone words found none.
- **57 bones**, matching the render object's count at `obj+0xB0`. Model origin = bone 0 at the waist;
  +Y up, mm, model faces +Z. Rest pose is a **T-pose** (arms along ±X at y = 400, palms down).
  (The older "~62 entries, 8–57 Snake" figure was a rough range in the shadow buffer, not a count.)

| Bones | What (side by X sign; anatomical side unconfirmed) |
|---|---|
| 0 | root / pelvis (waist, origin); legs hang from it |
| 1, 2 | lower spine (y 30), chest (y 220; parent of neck and clavicles) |
| 3, 4 | neck (440), head (560) |
| 5–9 / 10–14 | +X / −X arm: clavicle (±50), upper arm (±180), elbow (±440), forearm twist (±580), wrist (±700) |
| 15–18 / 19–22 | +X / −X leg: hip joint (±100, −100), knee (−550), ankle (−1000), toe (−1100, z +100) |
| 23 / 40 | palm root (zero length, at the wrist), parent of all fingers |
| 24–26, 27–29, 30–32 | index, middle, thumb (thumb points +Z); −X side 41–49 |
| 33; 34–36, 37–39 | ring/little metacarpal; ring, little fingers; −X side 50–56 |

- Height: toe joint to head joint 1660 mm (skater 1.565 m); legs ~8% longer than the skater's,
  torso and arms within 2%. Bone map, axis and rest-pose notes: `docs/retarget.json`.
- **Skater side** (`mgsk_bone_name`/`mgsk_bones`, root-relative): TRAJECTORY, HIPS, SPINE..SPINE3,
  NECK, NECK1, HEAD, RIGHT/LEFT SHOULDER, ARM, FOREARM, HAND, RIGHT/LEFT UPLEG, LEG, FOOT, TOEBASE,
  SKATEBOARD_ROOT, trucks and wheels, four `*_REPARENTED` helpers. LEFT* is +X; A-pose bind.

### Retargeting (`skate-ffi/src/retarget.rs`)

- **API:** `mgsk_retarget_init(session, sna_def.mdl path, mirror_x)` once, then
  `mgsk_snake_pose(session, out, 57)` per tick: 57 world matrices in the game's layout (rows
  X/Y/Z axis, translation, mm), ready for `obj+0xC0`.
- **Skater bind:** inverse of `skater.glb`'s inverse bind matrices, with the converter's bone
  basis undone (engine axes X, Y, Z = GLB joint X, Z, -Y; checked: parent-local child offsets
  then equal the live `mgsk_bones` ones to 1 mm).
- **Rotation:** mapped bone `i` (skater bone `s`): `R_i = R_root * R_s(t) * R_s(bind)^-1 * C_i`, `C_i`
  the shortest arc from Snake's rest bone direction to the skater's bind direction (towards the
  first mapped child with a different skater bone; leaves use parent->bone). Bone matrices are
  orthonormalised first (they carry ~1% scale). Unmapped bones take their parent's rotation.
- **Position:** Snake's rest offsets down his hierarchy (lengths kept exactly), then the whole
  skeleton shifted so his ankle midpoint (17, 21) sits on the skater's. Anchoring at the hips
  instead would sink his feet ~70 mm into the deck (legs 8% longer).
- **`mirror_x`:** reflects the skater through his own sagittal plane (root-local X) and swaps
  LEFT/RIGHT. Because the skater stands sideways on the board, this also flips the stance (feet
  swap board ends). Off until MGS3's handedness is settled.
- **Check:** `python -I tools/retarget_check.py <assets> <sna_def.mdl> --png out.png` (stick
  figures, bone lengths, ankle gap, limb directions). Unit tests: `cargo test retarget`.

## Board textures (`skater.glb`)

The converter embeds the textures as PNG (8-bit RGBA, non-interlaced) in buffer views of
`private/skater.glb`. The board uses three of the ten materials, all opaque with a white base
colour factor: `Retail_SkateBoard` (deck and grip, 512x512: grip tape on the top half, the
bottom graphic below it, edge wood on the right), `Retail_SkateTruck` (128x128, also the deck
part's baseplates) and `Retail_SkateWheel` (128x128). Their alpha channel is not coverage
(means 62 and 28), so it is ignored. `mgsk_board_ranges` gives each board part's index runs per
texture (slots are the board materials in glTF order: 0 deck, 1 truck, 2 wheel) and
`mgsk_board_texture` the decoded RGBA8; UVs are glTF's (0,0 = top-left texel), all within [0, 1].
The plugin builds a box-filtered mip chain and samples them as UNORM (the game's back buffer is
UNORM, gamma-space).
