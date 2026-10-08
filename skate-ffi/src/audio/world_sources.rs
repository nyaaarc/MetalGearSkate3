//! Stand-in for upstream `game_audio/world_sources.rs`: the world sources (traffic, pedestrians)
//! are not ported, so their manifest tuning is kept as raw JSON and never read.
#![allow(dead_code)]

#[derive(Clone, Debug, Default, serde::Deserialize, serde::Serialize)]
#[serde(transparent)]
pub(crate) struct WorldTuningJson(pub serde_json::Value);
