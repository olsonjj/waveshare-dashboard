//! Integration test: a fake board (plain TCP client) connects to the
//! push server and asserts it receives well-formed JSON lines at ~1 s
//! cadence, and that reconnects resume the stream.

use std::io::{BufRead, BufReader};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use solar_push_server::buckets::DayState;
use solar_push_server::push::serve;

fn spawn_server() -> (u16, Arc<Mutex<DayState>>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind ephemeral port");
    let port = listener.local_addr().unwrap().port();
    // use the REAL current local date so the pusher's midnight-rollover
    // check does not reset the state on its first tick
    let today = solar_push_server::clock::local_parts(solar_push_server::push::now_epoch()).date;
    let state = Arc::new(Mutex::new(DayState::new(&today)));
    let shared = state.clone();
    std::thread::spawn(move || serve(listener, shared).unwrap());
    (port, state)
}

fn read_one_line(reader: &mut BufReader<std::net::TcpStream>) -> String {
    let mut line = String::new();
    reader
        .read_line(&mut line)
        .expect("socket read succeeds within timeout");
    assert!(line.ends_with('\n'), "lines are newline-delimited");
    line
}

#[test]
fn fake_board_receives_wellformed_json_at_1s_cadence() {
    let (port, state) = spawn_server();
    {
        let mut st = state.lock().unwrap();
        st.kwh[42] = 1.5;
        st.watts_now = 3840;
        st.wh_today = 21651;
        st.peak_w = 5120;
        st.wh_7day = 987_650;
    }

    let conn = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
    conn.set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let mut reader = BufReader::new(conn);

    let t0 = Instant::now();
    let l1 = read_one_line(&mut reader);
    let first = Instant::now();
    let l2 = read_one_line(&mut reader);
    let dt = first.elapsed(); // time between line 1 and line 2 arriving

    // cadence: ~1 s apart (generous bounds to avoid CI flake)
    assert!(
        dt >= Duration::from_millis(700) && dt <= Duration::from_millis(2500),
        "second line arrived {dt:?} after the first; expected ~1 s"
    );
    assert!(t0.elapsed() < Duration::from_secs(10));

    // both lines are complete, well-formed contract payloads
    let mut prev_epoch = 0;
    for line in [l1, l2] {
        let v: serde_json::Value = serde_json::from_str(line.trim_end()).unwrap();
        assert_eq!(v["v"], 1);
        assert_eq!(v["watts_now"], 3840);
        assert_eq!(v["wh_today"], 21651);
        assert_eq!(v["watts_peak_today"], 5120);
        assert_eq!(v["wh_7day"], 987650);
        assert_eq!(v["kwh_15m"].as_array().unwrap().len(), 96);
        assert!((v["kwh_15m"][42].as_f64().unwrap() - 1.5).abs() < 1e-6);
        let epoch = v["epoch"].as_i64().unwrap();
        assert!(epoch >= prev_epoch, "epochs are non-decreasing");
        prev_epoch = epoch;
    }
}

#[test]
fn reconnect_resumes_stream_and_replaces_old_client() {
    let (port, _state) = spawn_server();

    // first client connects and gets data
    let c1 = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
    c1.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    let mut r1 = BufReader::new(c1);
    read_one_line(&mut r1);

    // a new client replaces it; the old stream is closed by the server
    let c2 = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
    c2.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    let mut r2 = BufReader::new(c2);
    let t = Instant::now();
    read_one_line(&mut r2);
    assert!(
        t.elapsed() < Duration::from_secs(5),
        "replacement client served promptly"
    );

    // the old client sees EOF once the server drops its stream
    let mut sink = String::new();
    let n = r1
        .read_line(&mut sink)
        .expect("old client read completes (EOF ok)");
    assert_eq!(n, 0, "old client connection was closed");
}
