/**
 * push_client.h — the board's entire data layer: a thin socket client.
 *
 * The board is a pure renderer (see spec.md): it never touches the
 * Envoy, TLS, bucketing, or NVS. It connects to the Rust push server
 * on the LAN (server/), reads newline-delimited JSON lines, and hands
 * complete screen state to the existing renderer. The pushed `epoch`
 * also drives the wall clock and the chart's now-line — the board no
 * longer seeds or syncs its own clock by any other means.
 */
#pragma once

/** Wire up Wi-Fi listeners (radio-quiet connect profile). Non-blocking. */
void push_client_init(void);

/**
 * Pump the connect/read state machine. Call from loop() as often as
 * possible; every call is non-blocking (millisecond-scale socket reads
 * only). If the push stream stops, the display simply keeps its last
 * rendered frame (reconnect/stale handling is out of scope this version).
 */
void push_client_tick(void);
