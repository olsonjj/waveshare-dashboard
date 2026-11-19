#!/usr/bin/env python3
"""glitch_watch.py — host-side panel-glitch watcher for the solar dashboard.

Owns the CH343 UART, mirrors all text traffic (timestamped) to a log, and
periodically asks the board for binary frame dumps:

  SNAPSHOT  -> both RGB panel framebuffers (B0/B1, what the panel DMA feeds)
  LVSNAP    -> LVGL's own render of the active screen (what the firmware
               *thinks* is on screen)

Frames are decoded (12-byte header + RGB565, same layout hostsim/raw2png.py
reads), converted to PNGs, diffed against the previous capture of the same
tag, and row-level anomalies are logged with wall-clock timestamps. The
serial.log text stream runs alongside, so every anomaly carries the board's
radio/poll/heap context at that moment.

Triage matrix (requires a user photo/eyeball at the same moment):
  B* clean, LV clean, panel shows ghosts -> physical display path (scan/
     ribbon/EMI) - software exonerated.
  B* ghosted, LV clean                   -> firmware write path (blit/DMA).
  LV ghosted, B* ghosted                 -> LVGL/draw-state corruption.

Usage: python3 glitch_watch.py [port] [interval_s]   (defaults in main())
"""
import glob
import os
import struct
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.environ.get("GLITCHWATCH_DIR", "/Users/johnolson/.glitchwatch")
TEXT_LOG = os.path.join(LOG_DIR, "serial.log")
EVENT_LOG = os.path.join(LOG_DIR, "events.log")
SNAP_DIR = os.path.join(LOG_DIR, "snaps")
POLL_INTERVAL_S = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0
LV_INTERVAL_S = 300.0

MASK = (560, 70)  # x >= MASK[0] and y < MASK[1] = header/clock digits: benign diffs
# LVGL perf-monitor overlay (LV_USE_PERF_MONITOR=1, LV_ALIGN_BOTTOM_RIGHT
# in lv_conf.h): redraws FPS/CPU text every second at the bottom-right
# corner. Benign, but it flags the bottom rows of every otherwise-identical
# capture. Nothing else renders at x >= 720 on rows >= 440 (the corner is
# already covered by the overlay itself).
PM_MASK = (720, 440)  # x >= PM_MASK[0] and y >= PM_MASK[1]: perf-monitor text
CAM_TRIGGER = os.path.join(os.path.dirname(EVENT_LOG), "trig_queue")


def log(msg):
    line = "%s %s" % (time.strftime("%m-%d %H:%M:%S"), msg)
    with open(EVENT_LOG, "a") as f:
        f.write(line + "\n")
    print(line, flush=True)


def rgb565_to_png(src, png_path):
    data = src if isinstance(src, (bytes, bytearray)) else open(src, "rb").read()
    w, h = struct.unpack_from("<ii", data, 4)
    px = data[12:12 + w * h * 2]

    def chunk(tag, payload):
        import zlib
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    import zlib
    raw = bytearray()
    for y in range(h):
        row = bytearray()
        base = y * w * 2
        for i in range(0, w * 2, 2):
            v = px[base + i] | (px[base + i + 1] << 8)
            row += bytes((((v >> 11) & 0x1F) << 3,
                          ((v >> 5) & 0x3F) << 2,
                          (v & 0x1F) << 3))
        raw += b"\x00" + bytes(row)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    open(png_path, "wb").write(png)


def diff_stats(cur_raw, prev_raw):
    """Row-wise diff summary. Returns (rows_out, changed_rows, px_total,
    px_out_mask) where px_out_mask counts differing pixels OUTSIDE the
    benign header/clock region (MASK)."""
    n = min(len(cur_raw), len(prev_raw))
    w, h = struct.unpack_from("<ii", cur_raw, 4)
    px_cur, px_prev = cur_raw[12:n], prev_raw[12:n]
    rows, rows_out = [], 0
    px_out = 0
    total_px = 0
    per_row = (len(px_cur) // h)
    for y in range(h):
        row = px_cur[y * per_row:(y + 1) * per_row]
        prow = px_prev[y * per_row:(y + 1) * per_row]
        if row == prow:
            continue
        rows.append(y)
        # count differing pixels in the row
        d = [i for i in range(len(row)) if row[i] != prow[i]]
        if y < MASK[1]:
            # header row: the clock digits (x >= MASK[0]) tick every second,
            # so they are the BENIGN part and must be masked out; anything
            # else differing on a header row counts as anomalous
            d_clock = [i for i in d if (i // 2) >= MASK[0]]
            out_px = len(d) - len(d_clock)
            px_out += out_px
            rows_out += 1 if out_px > 0 else 0
        elif y >= PM_MASK[1]:
            # perf-monitor rows: the bottom-right FPS/CPU overlay redraws
            # every second; only differences OUTSIDE it are anomalous
            d_pm = [i for i in d if (i // 2) >= PM_MASK[0]]
            out_px = len(d) - len(d_pm)
            px_out += out_px
            rows_out += 1 if out_px > 0 else 0
        else:
            px_out += len(d)
            rows_out += 1
        total_px += len(d)
    if len(rows) > 8:
        rows = rows[:4] + ["..."] + rows[-4:]
    return rows_out, rows, total_px, px_out


def fnv1a(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


class Watcher:
    def __init__(self, port, baud=921600):
        self.port = port
        self.baud = baud
        self.ser = None
        self.text_log = open(TEXT_LOG, "a")
        self.last_raw = {}      # tag -> bytes
        self.last_snap_t = 0.0
        self.last_lv_t = 0.0

    def open_port(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.5)
        log("--- port opened %s" % self.port)

    def ensure_open(self):
        if self.ser is None:
            time.sleep(1.0)
            self.open_port()
            return False
        return True

    def write_text(self, data):
        for line in data.split(b"\n"):
            if line:
                ts = time.strftime("%H:%M:%S")
                self.text_log.write("[%s] %s\n" % (ts, line.decode("utf-8", "replace")))
                self.text_log.flush()

    def save_frame(self, tag, payload):
        png = os.path.join(SNAP_DIR, "latest_%s.png" % tag)
        try:
            rgb565_to_png(payload, png)
        except Exception as e:
            log("SNAP png fail %s: %s" % (tag, e))
        # compare with previous of same tag
        prev = self.last_raw.get(tag)
        self.last_raw[tag] = payload
        if prev is None:
            log("SNAP %s ok (baseline) %d bytes" % (tag, len(payload)))
        else:
            rows, ys, px, px_out = diff_stats(payload, prev)
            if rows == 0:
                log("SNAP %s ok identical" % tag)
            else:
                log("SNAP %s CHANGED rows=%s px=%d (outside benign masks: rows=%d px=%d)"
                    % (tag, ys, px, rows, px_out))
        # keep raw files pruned (keep latest 6 per tag)
        for f in sorted(glob.glob(os.path.join(SNAP_DIR, "*_%s.raw" % tag)))[0:-6]:
            os.unlink(f)

    def collect_frame(self, header, rest):
        """header: '<tag> <w> <h> <len> <fnv>'; rest: bytes after marker line.
        Body is base64 in newline-terminated chunks; rebuilds the 12-byte
        raw2png header host-side (wire is not binary-safe)."""
        parts = header.split()
        tag, w, h, ln, fnv = parts[0], int(parts[1]), int(parts[2]), int(parts[3]), parts[4]
        ts = time.strftime("%H%M%S")
        path = os.path.join(SNAP_DIR, "%s_%s.raw" % (ts, tag))
        need_b64 = ((ln + 2) // 3) * 4
        b64 = bytearray(rest)
        got = sum(1 for c in b64 if c not in (10, 13))
        while got < need_b64:
            chunk = self.ser.readline()
            if not chunk:
                log("SNAP %s TIMEOUT (%d/%d b64chars)" % (tag, got, need_b64))
                return
            b64 += chunk
            got += sum(1 for c in chunk if c not in (10, 13))
        tail = b""
        while not tail.endswith(b"\n"):
            c = self.ser.read(1)
            if not c:
                log("SNAP %s TIMEOUT waiting for END" % tag)
                return
            tail += c
        if b"###SNAPEND" not in tail:
            log("SNAP %s missing SNAPEND; tail=%r" % (tag, tail[:40]))
            return
        import base64 as _b64
        try:
            px = _b64.b64decode(bytes(bytearray(c for c in b64 if c not in (10, 13))))
        except Exception as e:
            log("SNAP %s b64decode fail: %s" % (tag, e))
            return
        if len(px) != ln:
            log("SNAP %s decoded %d bytes, expected %d" % (tag, len(px), ln))
            return
        if "%08x" % fnv1a(px) != fnv:
            log("SNAP %s CRC MISMATCH (wire corruption)" % tag)
            return
        hdr = b"SNAP" + struct.pack("<ii", w, h)
        data = hdr + px
        open(path, "wb").write(data)
        self.save_frame(tag, data)
    def run(self):
        buf = b""
        while True:
            now = time.time()
            if os.path.exists(CAM_TRIGGER):
                os.unlink(CAM_TRIGGER)
                log("cam trigger -> immediate SNAPSHOT"
                    " (screen freezed by dump; panel-side ghosts will never show here)")
                self.last_snap_t = now
                self.last_lv_t = now
                self.ser.write(b"SNAPSHOT\nLVSNAP\n")
                self.ser.flush()
            if now - self.last_snap_t >= POLL_INTERVAL_S:
                self.last_snap_t = now
                self.ser.write(b"SNAPSHOT\n")
                self.ser.flush()
            if now - self.last_lv_t >= LV_INTERVAL_S:
                self.last_lv_t = now
                self.ser.write(b"LVSNAP\n")
                self.ser.flush()
            data = self.ser.read(4096)
            if not data:
                continue
            buf += data
            while True:
                i = buf.find(b"###SNAP")
                nl = buf.find(b"\n", i) if i >= 0 else -1
                if i < 0:
                    # no marker: text traffic (up to last newline)
                    last_nl = buf.rfind(b"\n")
                    if last_nl >= 0:
                        self.write_text(buf[:last_nl])
                        buf = buf[last_nl + 1:]
                    break
                line = buf[i:nl]
                # text before marker
                self.write_text(buf[:i])
                header = line[7:].decode("ascii", "replace").strip()
                if header.startswith("END"):
                    buf = buf[nl + 1:]
                    continue
                if header.startswith("err"):
                    log("SNAP error: %s" % header)
                    buf = buf[nl + 1:]
                    continue
                rest = buf[nl + 1:]
                self.collect_frame(header, rest)
                buf = b""


def main():
    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(SNAP_DIR, exist_ok=True)
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem5B901265491"
    w = Watcher(port)
    while True:
        try:
            w.ensure_open()
            w.run()
        except serial.SerialException as e:
            log("--- serial error: %s; reopening" % e)
            try:
                if w.ser:
                    w.ser.close()
            except Exception:
                pass
            w.ser = None
            time.sleep(2)


if __name__ == "__main__":
    main()
