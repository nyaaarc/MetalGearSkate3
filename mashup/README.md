# mashup/

The Skate engine part of [2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
by chasmlol, copied at commit `f608f85e407ff1b7689d54a9aafdd16e95711ac4` (2026-10-02), the same
way [GTA San AnSkateas](https://github.com/ryglizzy/GTA-San-AnSkateas) vendors it.

| Path | What |
|---|---|
| `skate/crates/` | `skate-core`, `skate-data`, `skate-net` from [SK8-ENGINE/skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) at `cb79689` (GPL-3.0-only), plus `skate-host`, the headless host `skate-ffi` wraps |
| `skate/crates/skate-audio`, `skate-audio-fma` | Skate 3's audio runtime (native AEMS evaluator, voice graph, MixMap, Splice one-shots, granular rolling bed) from [SK8-ENGINE/skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine) at `b3c96793` (GPL-3.0-only), copied unchanged (`Cargo.toml` and `src/`; their separate `tests/` and `examples/` left out) |
| `skate/converter/` | Builds the converter that prepares skate data from the player's own `default.xex` |
| `docs/SKATE.md` | How the mashup uses the engine in IW4L |
| `LICENSE`, `NOTICE` | The mashup's Apache-2.0 licence and notice, kept as required |

Local changes (record each, with the reason):

- `skate-host/src/physics/bridge.rs`: added `Session::board_parts()`, returning the seven board
  part world transforms, so `skate-ffi` can export the board pose (the physics field is private).
- `skate-core/src/physics/native_arithmetic.rs`: `dot3`/`dot4` flush subnormal results to zero,
  as Xenon's VMX does in non-Java mode. The riding up-vector filters decay to ~1e-20 residues, so a
  squared length came out ~1e-40, passed callers' `== 0` tests, and its rsqrt refinement overflowed
  (`length` returned -inf, then `0 * -inf` gave NaN). The NaN reached the reckoning frame, the
  skeleton roots and the board torque, and the skater could not start again ("Non-finite
  torque_acceleration before shared solve"), typically on a landing after a bail respawn.
- For the skater's sound (backports from SK8-ENGINE `b3c96793`, the engine's later audio inputs;
  each marked `MGSK` in the code):
  - `skate-core/src/physics/board_ground.rs`: `WheelLineState::audio_surfaces` / `seam_patterns`
    (per-wheel surface tag bits 0..6 and 12..15) and `BoardGroundState::part_audio_surfaces`
    (the trucks' and deck's contact surface). Observation only.
  - `skate-host/src/physics/animation_input.rs`: `AnimationInput::audio`, the animation's push /
    brake / footstep events latched as they occur. Observation only.
  - `skate-host/src/physics/foot_physical_output.rs`: `FootPhysicalOutputs::deck_box()`.
  - `skate-host/src/audio_state.rs` (new): upstream `game_audio/skate_events.rs`'s audio-state
    builder and `observe` without Bevy or logs (`AudioObserver`, `Riding`); `board_in_water` is
    always false (MGS3 collision has no water surfaces). `bridge.rs`: `Session::audio_observe`.
  - `skate-host/Cargo.toml` depends on `skate-audio`; the workspace lists the two audio crates.
