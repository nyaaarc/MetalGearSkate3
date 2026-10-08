//! Stand-in for upstream's `skate-mods` crate as `library.rs` uses it: audio content overlays.
//! There are no mods here (`Library::load` passes no overlays), so these are never reached with
//! data; they exist so the ported loader compiles unchanged.
#![allow(dead_code)]

pub(crate) mod audio_content {
    use std::collections::BTreeSet;
    use std::path::Path;

    pub(crate) const MAX_BINARY_BYTES: u64 = 0;

    #[derive(Debug, Default)]
    pub(crate) struct Add {
        pub projects: Vec<String>,
    }

    #[derive(Debug, Default)]
    pub(crate) struct AudioOverlay {
        pub add: Add,
    }

    /// A mod map's audio (opaque here; never present in an install's manifest).
    #[derive(Clone, Debug, Default, PartialEq, serde::Deserialize, serde::Serialize)]
    #[serde(transparent)]
    pub(crate) struct MapAudioDef(pub serde_json::Value);

    pub(crate) struct SpeechClips;

    impl SpeechClips {
        pub(crate) fn needed(_overlay: &AudioOverlay) -> bool {
            false
        }
        pub(crate) fn load(_root: &Path, _manifest: &serde_json::Value) -> Self {
            Self
        }
    }

    pub(crate) fn project_symbols(_bytes: &[u8], _file: &str) -> Result<Vec<(u8, String)>, String> {
        Ok(Vec::new())
    }

    pub(crate) fn project_clash(_taken: &BTreeSet<(u8, String)>, _symbols: &[(u8, String)]) -> Option<String> {
        None
    }
}

pub(crate) mod audio_merge {
    use std::collections::BTreeMap;

    #[derive(Clone, Debug, Default)]
    pub(crate) struct Message {
        pub owner: String,
        pub text: String,
    }

    #[derive(Clone, Debug, Default)]
    pub(crate) struct Owners(BTreeMap<String, String>);

    impl Owners {
        pub(crate) fn all(&self) -> &BTreeMap<String, String> {
            &self.0
        }
    }

    #[derive(Debug, Default)]
    pub(crate) struct Report {
        pub conflicts: Vec<Message>,
        pub warnings: Vec<Message>,
    }

    pub(crate) struct Source<'a> {
        pub id: &'a str,
        pub overlay: &'a super::audio_content::AudioOverlay,
    }

    pub(crate) fn merge_one_with(
        _value: &mut serde_json::Value,
        _source: &Source,
        _owners: &mut Owners,
        _report: &mut Report,
        _speech: Option<&super::audio_content::SpeechClips>,
    ) {
    }

    /// `mod:<id>/<path>` references: none here (every file is the install's).
    pub(crate) fn split_mod_ref(_file: &str) -> Option<(&str, &str)> {
        None
    }
}

pub(crate) fn read_bounded(_root: &std::path::Path, _file: &str, _limit: u64) -> Result<Vec<u8>, String> {
    Err("no mods".into())
}
