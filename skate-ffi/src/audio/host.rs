//! The audio host: upstream `game_audio/native.rs` (SK8-ENGINE b3c96793, GPL-3.0-only) without
//! Bevy. It starts the native runtime from the exported install (Csis projects, the boot
//! utilities, the player's AEMS / Splice banks and wheel recordings, the MixMap, the granular
//! rolling bed), runs one host pass per engine tick in upstream's order (`mixmap_frame` +
//! `mixmap_tick` + the bed's `update`), and renders 48 kHz interleaved stereo on demand for the
//! plugin's output thread. World emitters, ambience, speech, the front end and mods are not
//! ported: only the skater's own sounds play.
use std::collections::HashMap;
use std::path::Path;
use std::sync::{Arc, Mutex};

use skate_audio::formats::Project;
use skate_audio::mixmap::{MixMap, keys};
use skate_audio::runtime::Runtime;

use super::grain_bed::Bed;
use super::library::Library;
use super::player_audio::PlayerAudio;
use super::skate_events::Riding;

/// One engine tick (60 Hz): the host ticks once per tick; the MixMap evaluates on every second
/// one (the console's 30 Hz, `skate_audio::mixmap::cadence`).
pub(crate) const MIX_STEP: f32 = 1.0 / 60.0;

/// What [`AudioHost::start`] turned on, for the log.
#[derive(Debug, Default, Clone)]
pub(crate) struct Started {
    pub projects: usize,
    pub banks: usize,
    pub mixmap: bool,
    pub bed: bool,
    pub components: bool,
}

pub(crate) struct AudioHost {
    pub(crate) shared: Arc<Mutex<Runtime>>,
    library: Library,
    /// Bank stem → runtime bank id.
    banks: HashMap<String, usize>,
    mixmap: Option<MixMap>,
    player: Option<PlayerAudio>,
    bed: Option<Bed>,
    cadence: skate_audio::mixmap::cadence::Cadence,
    /// The flag inputs are held between evaluations (set once, at the first pass).
    holds: bool,
    /// The last tick's wheels, for the seam interpolation pair (None across a cut).
    cut: bool,
    pub(crate) started: Started,
}

impl AudioHost {
    /// Start from `<asset_root>/private/audio/audio_manifest.json` (setup's audio export).
    pub(crate) fn start(asset_root: &Path) -> Result<Self, String> {
        // Which compiled copy of the DSP loops runs (hardware FMA or plain; same output bits).
        let _ = skate_audio::dsp::init_fma();
        let library = Library::load(asset_root)?;
        let files = library.aems();
        if files.projects.is_empty() {
            return Err("the audio install has no AEMS projects (rerun the audio export)".into());
        }
        let mut runtime = Runtime::new();
        let mut started = Started::default();
        for file in &files.projects {
            let bytes = library.read(file).map_err(|e| format!("{file}: {e}"))?;
            let project = Project::parse(file, &bytes).map_err(|e| e.to_string())?;
            runtime.install_project(&project);
            started.projects += 1;
        }
        let mixmap = match &files.mixmap {
            Some(file) => {
                let bytes = library.read(file).map_err(|e| format!("{file}: {e}"))?;
                let parsed = skate_audio::mixmap::MixMapFile::parse(&bytes).map_err(|e| e.to_string())?;
                // Retail's free-skate instance layout (upstream WorldInstances::RETAIL).
                Some(MixMap::new(&parsed, &skate_audio::mixmap::RETAIL_INSTANCES))
            }
            None => None,
        };
        started.mixmap = mixmap.is_some();
        let bed = if mixmap.is_some() { Bed::new(&library) } else { None };
        started.bed = bed.is_some();
        let player = mixmap.as_ref().map(|_| PlayerAudio::new(library.player_tuning(), true));
        // The environment (reverb) network and the eEQChain buses; the FlangeSub returns; the
        // FootStep SubMix graphs.
        let (presets, eq) = library.bus_tuning();
        if !presets.is_empty() || !eq.is_empty() {
            runtime.mixer.buses.env.presets = presets;
            runtime.mixer.buses.eq.set_records(&eq);
        }
        if let Some([a, b]) = library.flange_presets() {
            runtime.mixer.buses.flange.set_presets(a, b);
        }
        runtime.mixer.buses.submix.enabled = true;
        let mut host = Self {
            shared: Arc::new(Mutex::new(runtime)),
            library,
            banks: HashMap::new(),
            mixmap,
            player,
            bed,
            cadence: Default::default(),
            holds: false,
            cut: true,
            started,
        };
        // Retail's boot order: c_emitter_utility -> Start_up_Play_ctl (Common.abk, the seam
        // shuffles) -> c_foley_utility (with the tricks); every program shares one generator.
        host.ensure_bank("emitter_utility")?;
        host.load_player_banks();
        host.post_class("c_emitter_utility");
        if host.ensure_bank(skate_audio::player::seams::UTILITY_BANK).is_ok() {
            host.post_class(skate_audio::player::seams::UTILITY);
        }
        if host.player.as_ref().is_some_and(|p| p.components && p.tricks_on) {
            host.post_class(skate_audio::player::tricks::FOLEY_UTILITY);
        }
        host.started.banks = host.banks.len();
        host.started.components = host.player.as_ref().is_some_and(|p| p.components);
        Ok(host)
    }

    fn post_class(&self, class: &str) {
        if let Ok(mut runtime) = self.shared.lock() {
            if let Some(id) = runtime.eval.class_id(class) {
                runtime.post(id, &[]);
            }
        }
    }

    fn ensure_bank(&mut self, stem: &str) -> Result<usize, String> {
        if let Some(&id) = self.banks.get(stem) {
            return Ok(id);
        }
        let (bank, pcm) = self.library.bank_source(stem)?.load()?;
        let mut runtime = self.shared.lock().map_err(|_| "audio lock poisoned")?;
        let id = runtime.load_bank(bank, pcm);
        drop(runtime);
        self.banks.insert(stem.to_owned(), id);
        Ok(id)
    }

    fn set_player_group(&self, id: usize) {
        if let Ok(mut runtime) = self.shared.lock() {
            runtime.mixer.set_bank_group(id, skate_audio::mixer::GROUP_PLAYER);
        }
    }

    /// The player components' banks (upstream `load_player_banks` / `load_optional_player_banks`).
    fn load_player_banks(&mut self) {
        if !self.player.as_ref().is_some_and(|p| p.components) {
            return;
        }
        for stem in super::player_audio::BANKS {
            match self.ensure_bank(stem) {
                Ok(id) => self.set_player_group(id),
                Err(_) => {
                    if let Some(p) = &mut self.player {
                        p.components = false;
                    }
                    return;
                }
            }
        }
        use super::player_audio::{RATTLE_BANKS, ROLLING_BANKS, SLIDE_BANKS, TREATMENT_BANKS, TRICKS_BANKS};
        let mut optional = |banks: &[&str]| -> bool {
            let mut first = false;
            for (i, stem) in banks.iter().enumerate() {
                if let Ok(id) = self.ensure_bank(stem) {
                    self.set_player_group(id);
                    first |= i == 0;
                }
            }
            first
        };
        let (rolling, rattle, slide, tricks, treatment) =
            (optional(ROLLING_BANKS), optional(RATTLE_BANKS), optional(SLIDE_BANKS), optional(TRICKS_BANKS), optional(TREATMENT_BANKS));
        // The Splice banks (pops, landings, touchdowns), decoded up front.
        let mut first = false;
        for (i, stem) in super::player_audio::SPLICE_BANKS.iter().enumerate() {
            if let Some((bank, pcm)) = self.library.splice_bank(stem) {
                if let Ok(mut runtime) = self.shared.lock() {
                    let rt = &mut *runtime;
                    rt.splice.load_bank(stem, bank, pcm, &mut rt.mixer);
                    first |= i == 0;
                }
            }
        }
        // SFXObj_Wheels' spin-down recordings.
        let streams: Vec<_> = super::player_audio::WHEEL_STREAMS.iter().map(|n| self.library.wheels_pcm(n)).collect();
        let wheels = streams.iter().all(Option::is_some);
        if let Ok(mut runtime) = self.shared.lock() {
            runtime.load_streams(streams);
        }
        let materials = self.library.footstep_materials();
        let contacts = self.library.contacts_tuning();
        if let Some(p) = &mut self.player {
            (p.rolling_on, p.rattle_on, p.slide_on, p.tricks_on, p.treatment_on) = (rolling, rattle, slide, tricks, treatment);
            p.contacts_on = first;
            p.set_footstep_materials(materials);
            p.footsteps_on = first;
            p.contact_tuning = contacts;
            p.wheels_on = wheels;
        }
    }

    /// A teleport / respawn: no seam interpolation or camera velocity across it.
    pub(crate) fn cut(&mut self) {
        self.cut = true;
        if let Some(p) = &mut self.player {
            p.reset_listener();
        }
    }

    /// One host pass for the engine tick just simulated: upstream `mixmap_frame`, `mixmap_tick`
    /// and the bed's `update`, one physics step each. `listener` = the camera's position and view
    /// direction (engine units; None: the skater's own position).
    pub(crate) fn tick(&mut self, riding: &Riding, listener: Option<([f32; 3], [f32; 3])>) {
        let Some(m) = &mut self.mixmap else { return };
        let s = riding.audio;
        let before = if self.cut { s.wheel_position } else { riding.wheels_before };
        self.cut = false;
        // Class_Seams at the board's wheels.
        if let Some(player) = &mut self.player {
            player.seam_alpha = Some(1.0);
            player.step_wheels(before, s.wheel_position);
            if let Ok(mut runtime) = self.shared.lock() {
                player.seam_frame(m, &s, MIX_STEP, &mut runtime);
            }
        }
        // One step: the console cadence decides whether it completes an evaluation.
        let calls = self.cadence.advance(1);
        for id in 1..=4 {
            m.set_input(keys::MASTER, id, 32767);
        }
        for id in [1, 2, 5] {
            m.set_input(keys::MUSIC, id, 32767);
        }
        // SFXObj_Reverb's first step: Reverb.in0..6 from the preset being faded to.
        let reverb = self.shared.lock().ok().map(|r| r.mixer.buses.env.reverb_inputs());
        match reverb {
            Some(v) => {
                for (id, x) in v.into_iter().enumerate() {
                    m.set_input(keys::REVERB, id, x);
                }
            }
            None => m.set_input(keys::REVERB, 5, 32767),
        }
        m.set_input(keys::PAUSE, 0, 0);
        if let Some(bed) = &mut self.bed {
            bed.slew_calls = Some(calls);
        }
        if !self.holds {
            hold_flag_inputs(m);
            self.holds = true;
        }
        // The jittered eEQChain buses take the walk's values once per console evaluation.
        if calls > 0 {
            let jitter = self.player.as_ref().and_then(|p| p.eq_jitter());
            if let Ok(mut runtime) = self.shared.lock() {
                runtime.mixer.buses.eq.clear(jitter);
            }
        }
        if let Some(player) = &mut self.player {
            let l = listener.map(|(eye, view)| player.listener(eye, view, MIX_STEP, &s));
            player.jitter_steps = Some(calls);
            player.teleport_effect = None;
            player.write_inputs(m, &s, l.as_ref());
        }
        let routing = self.player.as_ref().is_some_and(|p| p.components && p.rolling_on);
        if let Some(bed) = &mut self.bed {
            bed.write_inputs(m, &s, !routing);
        }
        let speed_scale = self.bed.as_ref().and_then(Bed::push_scale);
        let loose = PlayerAudio::loose_board(&s, riding);
        if let (Some(player), Ok(mut runtime)) = (&mut self.player, self.shared.lock()) {
            player.process(m, &s, &mut runtime, speed_scale, loose);
        }
        for _ in 0..calls {
            m.tick(skate_audio::mixmap::cadence::CONSOLE_DT);
        }
        if let (Some(player), Ok(mut runtime)) = (&mut self.player, self.shared.lock()) {
            player.update(m, &s, &mut runtime, speed_scale, loose);
        }
        // SFXObj_Reverb's update: the FlangeSub returns' levels and the global env scale.
        if let Ok(mut runtime) = self.shared.lock() {
            runtime.mixer.buses.flange.frame(std::array::from_fn(|i| m.level(keys::REVERB, i)));
            if reverb.is_some() {
                runtime.mixer.buses.env.scale_frame(m.level(keys::REVERB, 4));
            }
        }
        // The granular rolling bed, after the MixMap tick.
        let routed = self.player.as_mut().filter(|p| p.components && p.rolling_on).map(|p| (std::mem::take(&mut p.routed.grains), p.routed.primary));
        static NO_TUNING: std::sync::OnceLock<skate_audio::player::tuning::PlayerTuning> = std::sync::OnceLock::new();
        let seams = self.player.as_ref().map_or_else(|| NO_TUNING.get_or_init(Default::default), |p| &p.tuning);
        if let (Some(bed), Some(m)) = (&mut self.bed, &self.mixmap) {
            super::grain_bed::step(bed, &self.library, m, &self.shared, riding, MIX_STEP, seams, routed);
        }
    }
}

/// Render `out.len() / 2` frames of interleaved 48 kHz stereo (silence if the lock is poisoned).
pub(crate) fn render(shared: &Mutex<Runtime>, out: &mut [f32]) {
    match shared.lock() {
        Ok(mut runtime) => runtime.fill_stereo(out),
        Err(_) => out.fill(0.0),
    }
}

/// The one-frame flag inputs the console's writers set for a whole evaluation, held between our
/// 60 Hz writes and the 30 Hz evaluations (upstream `native::hold_flag_inputs`): Contacts 1 / 6,
/// Rail 1, SkateBoard 0 / 4, Cracks 0.
fn hold_flag_inputs(m: &mut MixMap) {
    for (key, id) in [(keys::contacts(0), 1), (keys::contacts(0), 6), (keys::rail(0), 1), (keys::skateboard(0), 0), (keys::skateboard(0), 4), (keys::cracks(0), 0)] {
        m.hold_input(key, id);
    }
}
