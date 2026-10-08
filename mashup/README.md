# mashup/

The Skate engine part of [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
by chasmlol, copied at commit `f608f85e407ff1b7689d54a9aafdd16e95711ac4` (2026-10-02), the same
way [GTA San AnSkateas](https://github.com/ryglizzy/GTA-San-AnSkateas) vendors it.

| Path | What |
|---|---|
| `skate/crates/` | `skate-core`, `skate-data`, `skate-net` from [SK8-ENGINE/skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) at `cb79689` (GPL-3.0-only), plus `skate-host`, the headless host `skate-ffi` wraps |
| `skate/converter/` | Builds the converter that prepares skate data from the player's own `default.xex` |
| `docs/SKATE.md` | How the mashup uses the engine in IW4L |
| `LICENSE`, `NOTICE` | The mashup's Apache-2.0 licence and notice, kept as required |

Local changes (record each, with the reason):

- `skate-host/src/physics/bridge.rs`: added `Session::board_parts()`, returning the seven board
  part world transforms, so `skate-ffi` can export the board pose (the physics field is private).
