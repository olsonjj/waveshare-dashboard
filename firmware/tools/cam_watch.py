#!/usr/bin/env python3
"""cam_watch.py — fixed-webcam glitch watcher for the solar dashboard panel.

Captures a continuous low-fps grayscale stream from a USB webcam aimed at
the panel, normalizes for auto-exposure drift, and diffs successive frames.
Ghost rows (horizontal artifacts that flicker between frames while the rest
of the scene is static) light up as high-diff rows *outside* the UI's own
update areas.

On a trigger, saves a short frame burst around the event as PNGs under
/tmp/glitchwatch/cam/ (timestamped), with the trigger rows in the event log.
Also drops a "latest.png" periodically for eyeball review.

Run: python3 cam_watch.py [video_device_index]   (default 0 = eMeet C960)
"""
import glob
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

LOG_DIR = os.environ.get("GLITCHWATCH_DIR", "/Users/johnolson/.glitchwatch")
CAM_BURST_KEEP = 120    # keep newest N camera files (pruned after each trigger)
CAM_DIR = os.path.join(LOG_DIR, "cam")
CAM_EVENTS = os.path.join(LOG_DIR, "cam_events.log")
CAM_OUT = os.path.join(LOG_DIR, "cam_stream.log")
W, H = 1920, 1080
FPS = 30
DEV = sys.argv[1] if len(sys.argv) > 1 else "0"
ROW_TRIGGER = None      # set after calibration (adaptive)
TRIG_COOLDOWN_S = 10.0
BURST = 8               # frames kept around trigger
TRIGGER_FILE = os.path.join(LOG_DIR, "trig_queue")  # glitch_watch picks this up
LATEST_EVERY_S = 60.0


def elog(msg):
    line = "%s %s" % (time.strftime("%m-%d %H:%M:%S"), msg)
    with open(CAM_EVENTS, "a") as f:
        f.write(line + "\n")
    print(line, flush=True)


def ffmpeg_cmd():
    return [
        "ffmpeg", "-loglevel", "error",
        "-f", "avfoundation", "-framerate", "30", "-pixel_format", "uyvy422",
        "-video_device_index", DEV, "-i", "",
        "-vf", "fps=%d,scale=%d:%d" % (FPS, W, H),
        "-pix_fmt", "gray", "-an",
        "-f", "rawvideo", "pipe:1",
    ]


def save_png(gray, path):
    Image.fromarray(np.clip(gray, 0, 255).astype(np.uint8)).save(path)


def main():
    os.makedirs(CAM_DIR, exist_ok=True)
    # open pipe, wait for first frame
    p = subprocess.Popen(ffmpeg_cmd(), stdout=subprocess.PIPE,
                         stderr=open(CAM_OUT, "a"), bufsize=0)
    frame_bytes = W * H

    def read_frame():
        buf = bytearray()
        while len(buf) < frame_bytes:
            chunk = p.stdout.read(frame_bytes - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        return np.frombuffer(bytes(buf), dtype=np.uint8).reshape(H, W)

    warm = np.asarray(read_frame(), dtype=np.float32)
    if warm is None:
        elog("--- no frames from device %s" % DEV)
        return
    # skip 1 s for exposure to settle
    for _ in range(FPS):
        f = read_frame()
        if f is None:
            elog("--- stream ended early")
            return
    prev = np.asarray(read_frame(), dtype=np.float32)
    ring = [prev.copy()]
    noise = 3.0          # adaptive per-row diff noise floor (0-255 scale)
    seen = 0
    last_trig = 0.0
    last_latest = 0.0
    row_hist = []        # recent median row-diff magnitudes for calibration
    while True:
        f = read_frame()
        if f is None:
            elog("--- stream ended; restarting in 5 s")
            p.wait()
            time.sleep(5)
            return main()
        cur = np.asarray(f, dtype=np.float32)
        seen += 1
        # exposure normalization: per-frame gain+offset (proper gain ratio —
        # auto-exposure steps otherwise create full-frame false triggers)
        mg = float(np.median(cur)) or 1.0
        mp = float(np.median(prev)) or 1.0
        d = np.abs(cur * (mp / mg) - prev)
        row_mean = d.mean(axis=1)                       # H-vector
        med = float(np.median(row_mean))
        row_hist.append(med)
        if len(row_hist) > 500:
            row_hist.pop(0)
        noise = max(1.2, float(np.percentile(row_hist, 95)))
        thr = max(3 * noise, 4.0)
        hot = np.where(row_mean > thr)[0]
        t = time.time()
        if t - last_latest > LATEST_EVERY_S:
            last_latest = t
            save_png(cur, os.path.join(CAM_DIR, "latest.png"))
        if len(hot) and t - last_trig > TRIG_COOLDOWN_S:
            # candidate ghost rows — group into runs
            runs = []
            y0 = hot[0]
            for a, b in zip(hot[:-1], hot[1:]):
                if b != a + 1:
                    runs.append((int(y0), int(a)))
                    y0 = b
            runs.append((int(y0), int(hot[-1])))
            last_trig = t
            elog("CAM ghost? rows=%s thr=%.1f medrow=%.1f" % (runs, thr, med))
            with open(TRIGGER_FILE, "a") as tf:
                tf.write(time.strftime("%H:%M:%S\n"))
            # prune old camera files (keep newest CAM_BURST_KEEP)
            for old in sorted(glob.glob(os.path.join(CAM_DIR, "*_t-*.png")))[:-CAM_BURST_KEEP]:
                os.unlink(old)
            for k, gframe in enumerate(ring + [cur]):
                save_png(gframe, os.path.join(
                    CAM_DIR, "%s_t%+03d.png" % (time.strftime("%H%M%S"), k - len(hot))))
        ring.append(cur.copy())
        if len(ring) > BURST:
            ring.pop(0)
        prev = cur
        if seen % (FPS * 120) == 0:
            elog("CAM stats: noise=%.2f thr=%.1f medrow=%.1f" % (noise, thr, med))
            save_png(cur, os.path.join(CAM_DIR, "latest.png"))


if __name__ == "__main__":
    main()
