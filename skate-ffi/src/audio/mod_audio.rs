//! Stand-in for upstream `game_audio/mod_audio.rs` (the modding event API): only the types the
//! ported player components use. Nothing is recorded (no mods here) and no rules apply; the
//! `Observed` wrapper passes every call through to the real host.
#![allow(dead_code)]

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EventKind {
    Post,
    Release,
    Splice,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Source {
    Player,
}

/// One audio event (upstream's row; built at the post sites, then dropped).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct EventRow {
    pub kind: EventKind,
    pub source: Source,
    pub class: &'static str,
    pub slot: &'static str,
    pub id: i32,
    pub owner: u64,
}

/// A post site's buffer: always None here (no subscriber).
pub(crate) type EventBuf = Option<Vec<EventRow>>;

#[inline]
pub(crate) fn record(_buf: &mut EventBuf, _row: EventRow) {}

/// A pass-through Splice host (upstream records starts and applies mod rules here).
pub(crate) struct Observed<'a, 'b> {
    inner: &'a mut dyn skate_audio::player::contacts::SpliceHost,
    _rows: std::marker::PhantomData<&'b mut EventBuf>,
}

impl<'a, 'b> Observed<'a, 'b> {
    pub(crate) fn new(inner: &'a mut dyn skate_audio::player::contacts::SpliceHost, _rows: &'b mut EventBuf, _source: Source, _owner: u64) -> Self {
        Self { inner, _rows: std::marker::PhantomData }
    }

    pub(crate) fn rules(self, _rules: Option<&'b super::mod_rules::RuleSet>) -> Self {
        self
    }

    pub(crate) fn slot(self, _slot: &'static str) -> Self {
        self
    }
}

impl skate_audio::player::contacts::SpliceHost for Observed<'_, '_> {
    fn set_route(&mut self, route: skate_audio::bus::Route) {
        self.inner.set_route(route);
    }
    fn set_submix(&mut self, submix: Option<skate_audio::player::footsteps::Submix>) {
        self.inner.set_submix(submix);
    }
    fn start(&mut self, bank: &str, id: u32, block: [f32; 6]) -> Option<skate_audio::splice::SoundId> {
        self.inner.start(bank, id, block)
    }
    fn update(&mut self, sound: skate_audio::splice::SoundId, block: [f32; 6]) {
        self.inner.update(sound, block);
    }
    fn alive(&self, sound: skate_audio::splice::SoundId) -> bool {
        self.inner.alive(sound)
    }
    fn release(&mut self, sound: skate_audio::splice::SoundId) {
        self.inner.release(sound);
    }
}

/// A player component slot's name and index for event rows.
pub(crate) fn player_slot(_slot: &skate_audio::player::components::Slot) -> (&'static str, i32) {
    ("", 0)
}
