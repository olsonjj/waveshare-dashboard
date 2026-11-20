//! solar-push-server — host side of the push-based solar dashboard.
//!
//! Polls the Enphase Envoy every 60 s, accumulates today's 15-minute
//! production buckets, persists them in SQLite, and pushes the complete
//! screen state to the board as newline-delimited JSON over TCP every
//! second (see `spec.md` / the `ScreenState` contract in `contract.rs`).

pub mod buckets;
pub mod clock;
pub mod config;
pub mod contract;
pub mod envoy;
pub mod push;
pub mod store;
