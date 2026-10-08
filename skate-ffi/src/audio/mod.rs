//! The skater's sounds: Skate 3's own audio runtime (`skate-audio`, the native AEMS evaluator,
//! voice graph, MixMap, Splice one-shots and granular rolling bed) driven by the engine's
//! per-tick audio state, rendering 48 kHz stereo for the plugin to play.
//!
//! The host files are ported from SK8-ENGINE/skate-3-rust-engine
//! `crates/skate-game/src/game_audio/` at b3c96793 (GPL-3.0-only), with the Bevy systems, mods,
//! world sources, ambience and speech left out: `library.rs` (the exported audio install),
//! `player_audio.rs` (the player components), `grain_bed.rs` (the rolling bed), `timing.rs`.
//! `host.rs` is ours: upstream `native.rs`'s per-frame order without Bevy.
#![allow(dead_code)]

pub(crate) mod compat;
pub(crate) mod grain_bed;
pub(crate) mod host;
pub(crate) mod library;
pub(crate) mod mod_audio;
pub(crate) mod mod_rules;
pub(crate) mod player_audio;
pub(crate) mod skate_events;
pub(crate) mod skate_mods;
pub(crate) mod timing;
pub(crate) mod world_sources;

pub(crate) use library::Library;
