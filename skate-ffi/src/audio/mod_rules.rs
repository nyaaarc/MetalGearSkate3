//! Stand-in for upstream `game_audio/mod_rules.rs`: there are no mods here, so no rule mutes.
#![allow(dead_code)]

pub(crate) struct RuleSet;

impl RuleSet {
    pub(crate) fn mutes(&self, _row: &super::mod_audio::EventRow) -> bool {
        false
    }
}
