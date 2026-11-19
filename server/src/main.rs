//! main.rs — service entry point: `just run` (or `cargo run` from server/).
//!
//! Starts three things:
//!   1. the 60 s Envoy poller (folds samples into buckets, persists)
//!   2. the 1 s TCP pusher (complete ScreenState JSON line per tick)
//!   3. the accept loop for board connections (this thread)
//!
//! Ctrl-C stops the service; the board keeps its last rendered frame.

use std::net::TcpListener;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use solar_push_server::buckets::{self, DayState};
use solar_push_server::clock::local_parts;
use solar_push_server::config;
use solar_push_server::envoy::EnvoyClient;
use solar_push_server::push::{now_epoch, serve};
use solar_push_server::store;

fn main() {
    let cfg = match config::load("config.toml") {
        Ok(c) => c,
        Err(e) => {
            eprintln!("solar-push-server: {e}");
            std::process::exit(1);
        }
    };

    let conn = match store::open(&cfg.db_path) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("solar-push-server: cannot open db {}: {e}", cfg.db_path);
            std::process::exit(1);
        }
    };
    let conn = Arc::new(Mutex::new(conn));

    // load today's row, or start fresh; prune older days (today only)
    let now = now_epoch();
    let today = local_parts(now).date;
    let state = Arc::new(Mutex::new(
        store::load_today(&conn.lock().unwrap(), &today).unwrap_or_else(|| {
            println!("[state] fresh day {today}");
            DayState::new(&today)
        }),
    ));
    if let Ok(n) = store::prune(&conn.lock().unwrap(), &today) {
        if n > 0 {
            println!("[state] pruned {n} old day row(s)");
        }
    }
    {
        let st = state.lock().unwrap();
        println!(
            "[state] {} loaded: {} buckets set, peak {} W, last wh {}",
            st.date,
            st.kwh.iter().filter(|k| **k >= 0.0).count(),
            st.peak_w,
            st.last_wh
        );
    }

    // Envoy poller thread
    {
        let state = state.clone();
        let conn = conn.clone();
        let envoy = EnvoyClient::new(&cfg.envoy_host, &cfg.envoy_token);
        let interval = cfg.poll_interval_s.max(5);
        std::thread::spawn(move || loop {
            let t0 = std::time::Instant::now();
            match envoy.fetch() {
                Ok(r) => {
                    let now = now_epoch();
                    let lp = local_parts(now);
                    let mut st = state.lock().unwrap();
                    buckets::apply_sample(
                        &mut st,
                        &buckets::Sample {
                            wh_today: r.wh_today,
                            watts_now: r.watts_now,
                            now,
                        },
                        &lp.date,
                        lp.bucket,
                    );
                    st.wh_7day = r.wh_7day;
                    if let Err(e) = store::save_today(&conn.lock().unwrap(), &st) {
                        eprintln!("[poll] db save failed: {e}");
                    }
                    println!(
                        "[poll] {} W, {} Wh today, {} Wh 7d ({:?})",
                        r.watts_now,
                        r.wh_today,
                        r.wh_7day,
                        t0.elapsed()
                    );
                }
                Err(e) => eprintln!("[poll] {e} — serving cached state"),
            }
            // first poll runs immediately at startup; then every interval
            let spent = t0.elapsed();
            if spent < Duration::from_secs(interval) {
                std::thread::sleep(Duration::from_secs(interval) - spent);
            }
        });
    }

    // TCP pusher: accept loop on this thread
    let listener = match TcpListener::bind(("0.0.0.0", cfg.listen_port)) {
        Ok(l) => l,
        Err(e) => {
            eprintln!(
                "solar-push-server: cannot bind 0.0.0.0:{}: {e}",
                cfg.listen_port
            );
            std::process::exit(1);
        }
    };
    println!(
        "[push] listening on 0.0.0.0:{} (board: PUSH_SERVER_HOST/PORT in secrets.h)",
        cfg.listen_port
    );
    if let Err(e) = serve(listener, state) {
        eprintln!("[push] accept loop failed: {e}");
        std::process::exit(1);
    }
}
