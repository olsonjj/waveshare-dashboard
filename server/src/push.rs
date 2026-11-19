//! push.rs — the socket pusher: the board-facing half of the transport.
//!
//! The board connects to this TCP listener (it never polls); the server
//! writes one complete `ScreenState` JSON line per second to the current
//! client. When the board drops or reconnects, the next connection
//! simply replaces the current one (multiple consumers are out of scope
//! this version) — the following 1 s tick resumes the stream. No
//! auth/encryption: trusted LAN (spec "Out of Scope").

use std::io::Write;
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::buckets::{self, DayState};
use crate::clock::local_parts;
use crate::contract::ScreenState;

pub fn now_epoch() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock is after the unix epoch")
        .as_secs() as i64
}

/// Serve forever: accept clients on `listener`, push a JSON line every
/// second to the current one. `state` is shared with the poller.
pub fn serve(listener: TcpListener, state: Arc<Mutex<DayState>>) -> std::io::Result<()> {
    listener.set_nonblocking(false)?;
    let current: Arc<Mutex<Option<TcpStream>>> = Arc::new(Mutex::new(None));

    // pusher: drift-corrected 1 s tick
    {
        let state = state.clone();
        let current = current.clone();
        thread::spawn(move || {
            let mut next = Instant::now() + Duration::from_secs(1);
            loop {
                let now = Instant::now();
                if next > now {
                    thread::sleep(next - now);
                }
                next += Duration::from_secs(1);

                let epoch = now_epoch();
                let line = {
                    let mut st = state.lock().expect("state lock");
                    // local-midnight rollover on the push tick, so the board
                    // starts a fresh, empty day even before the first Envoy
                    // poll of the morning
                    let lp = local_parts(epoch);
                    buckets::rollover_if_new_date(&mut st, &lp.date);
                    ScreenState::from_day(&st, epoch).to_json_line()
                };
                let mut cur = current.lock().expect("client lock");
                if let Some(stream) = cur.as_mut() {
                    if stream.write_all(line.as_bytes()).is_err() {
                        *cur = None; // board dropped; wait for reconnect
                    }
                }
            }
        });
    }

    loop {
        let (stream, peer) = listener.accept()?;
        println!("[push] board connected from {peer}");
        // replace any previous connection (dropping the old TcpStream
        // closes it; the pusher's next write error clears it otherwise)
        *current.lock().expect("client lock") = Some(stream);
    }
}
