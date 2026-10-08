//! Minimal stand-ins for the Bevy audio-asset types `library.rs` names (its clip cache served
//! upstream's Bevy-played ambience, which is not ported). Nothing here plays sound.
#![allow(dead_code)]
use std::marker::PhantomData;

pub(crate) struct AudioSource {
    pub bytes: std::sync::Arc<[u8]>,
}

pub(crate) struct Handle<T>(u64, PhantomData<T>);

impl<T> Clone for Handle<T> {
    fn clone(&self) -> Self {
        Self(self.0, PhantomData)
    }
}

impl<T> Handle<T> {
    pub(crate) fn id(&self) -> u64 {
        self.0
    }
}

pub(crate) struct Assets<T> {
    next: u64,
    items: std::collections::HashMap<u64, T>,
}

impl<T> Default for Assets<T> {
    fn default() -> Self {
        Self { next: 0, items: Default::default() }
    }
}

impl<T> Assets<T> {
    pub(crate) fn add(&mut self, item: T) -> Handle<T> {
        self.next += 1;
        self.items.insert(self.next, item);
        Handle(self.next, PhantomData)
    }
    pub(crate) fn remove(&mut self, id: u64) {
        self.items.remove(&id);
    }
}
