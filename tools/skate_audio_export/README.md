# skate_audio_export

Decodes the player's own Skate 3 disc audio for the skater's sound (`skate-ffi/src/audio`):
`run.py` calls the pipeline below and writes `<assets>/private/audio/` (never commit its output:
it is game data). `setup.cmd` runs it after converting the skate data.

| Path | From | Licence |
|---|---|---|
| `tools/asset_pipeline/audio_export.py`, `audio_formats.py`, `vlt.py`, `world_audio.py`, `names.txt`, `fast_refpack.py` | [SK8-ENGINE/skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) `tools/asset_pipeline/` at `b3c96793`, unchanged | GPL-3.0-only (the repository's) |
| `tools/asset_pipeline/install.py` | ours: the two helpers `audio_export` takes from upstream's installer (`spawn`, `extract`) | GPL-3.0-only |
| `tools/owned_game/` | the same repository's `tools/owned_game/` (EB BIG / RefPack readers), unchanged | MIT, see `owned_game/NOTICE.md` and `vendor/university/LICENSE-PROJECT.md` |
| `tools/vendor/university/tools/vanilla_map_extraction/tools/skate3_streams.py` | the same repository's vendored copy, unchanged | MIT, `vendor/university/LICENSE-PROJECT.md` |

Decoding EA-XMA needs [vgmstream](https://github.com/vgmstream/vgmstream) (setup downloads the
pinned r2117 build after asking). The export also writes the ambience beds and world banks
upstream plays; the skater's host only loads the player's banks, grains and wheels.
