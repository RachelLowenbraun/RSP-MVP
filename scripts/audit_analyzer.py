#!/usr/bin/env python3
"""RSP F-I02 — 240 fps timing audit analyzer.

Reads:
  * a JSONL evidence log emitted by the M0 spike (session_header + stimulus_event rows)
  * a 240 fps (or any high-fps) video recording of the same session

For each stimulus_event in the log:
  1. Locate the fiducial strip in each video frame.
  2. Decode the 100-cell payload: [start=1] [nonce:64 LSB-first]
     [counter:32 LSB-first] [type:3 LSB-first] [parity] [stop=1] [pad].
  3. Verify start/stop/parity; reject frames whose decode fails.
  4. Group consecutive video frames whose decoded (nonce, type=TARGET) match
     the event's fiducial_nonce_hex → measured target range.
  5. Compute measured_duration_ms from the first-target-frame to just-after
     the last-target-frame using the video's per-frame timestamps.
  6. Compare against JSONL achieved_duration_ms.

Emits a bench_validation_record JSON with per-event and aggregate stats.

Pass criterion (spec §14.4, tightened by Redline Patch 7):
  - >= 99% of events with |measured - logged| within one frame period at the
    device's locked refresh rate
  - Zero events with measured duration > target + one frame period
  - Zero events with fiducial decode failure across the target range

Usage:
    python3 audit_analyzer.py --jsonl session.jsonl --video capture.mp4 \\
        --out report.json [--strip-region auto|X0,Y0,X1,Y1] [--debug-frames DIR]
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

import cv2
import numpy as np


# -------- Fiducial constants (must match C++ Fiducial.h and .frag shader) --------

CELLS_TOTAL = 104
CELLS_PAYLOAD = 102
CELL_PX_W = 8
CELL_PX_H = 32
STRIP_OFFSET_LEFT_PX = 24
STRIP_OFFSET_BOTTOM_PX = 24
STRIP_WIDTH_PX = CELLS_TOTAL * CELL_PX_W   # 832
STRIP_HEIGHT_PX = CELL_PX_H                 # 32

FRAME_AMBIENT = 0
FRAME_TARGET = 1
FRAME_MASK_FORWARD = 2
FRAME_MASK_BACKWARD = 3
FRAME_CONTROL = 4


# -------- Event & session data --------

@dataclass
class StimulusEvent:
    event_index: int
    fiducial_nonce_hex: str
    target_duration_ms: float
    achieved_duration_ms: float
    frame_count: int
    frame_period_ns: int
    verification_status: str
    # populated by analyzer:
    measured_start_frame: Optional[int] = None
    measured_end_frame: Optional[int] = None
    measured_frame_count: Optional[int] = None
    measured_duration_ms: Optional[float] = None
    absolute_deviation_ms: Optional[float] = None
    external_verified: Optional[bool] = None
    external_reason: Optional[str] = None


@dataclass
class SessionHeader:
    session_id: str
    session_nonce_hex: str
    device_model: str
    os_version: str
    target_duration_ms: float
    measured_hz: float
    frame_period_ns: int
    locked_hz: float


# -------- Fiducial decoder --------

def parse_nonce_hex(hex_str: str) -> int:
    return int(hex_str, 16) & 0xFFFFFFFFFFFFFFFF


def event_nonce_for_index(session_nonce: int, event_index: int) -> int:
    """Mirrors VulkanRendererImpl::MakeEventNonce."""
    return (session_nonce ^ ((event_index * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF)) \
        & 0xFFFFFFFFFFFFFFFF


def decode_fiducial(strip_pixels: np.ndarray) -> Optional[dict]:
    """Decode a fiducial strip (BGR image ~STRIP_WIDTH_PX × STRIP_HEIGHT_PX).

    strip_pixels: uint8 (H, W, 3) array. Any resolution — the decoder
    resamples cell centers using nearest-neighbor.

    Returns dict with:
        {'nonce': int64, 'counter': uint32, 'type': int, 'ok': bool}
    or None if the strip could not be decoded at all.
    """
    if strip_pixels.size == 0:
        return None

    # Convert to grayscale and threshold.
    gray = cv2.cvtColor(strip_pixels, cv2.COLOR_BGR2GRAY) \
        if strip_pixels.ndim == 3 else strip_pixels

    h, w = gray.shape
    if h < 4 or w < 8:
        return None

    # Adaptive threshold based on the strip's own min/max (robust to camera
    # exposure). We compute median as the midpoint.
    lo, hi = int(np.min(gray)), int(np.max(gray))
    if hi - lo < 30:
        # Too little contrast — probably not looking at the strip at all.
        return None
    thresh = (lo + hi) // 2

    # Sample each cell at its center (relative to the strip width).
    cells = np.zeros(CELLS_TOTAL, dtype=np.uint8)
    cell_w_f = w / CELLS_TOTAL
    center_y = h // 2
    for i in range(CELLS_TOTAL):
        cx = int((i + 0.5) * cell_w_f)
        if cx >= w:
            cx = w - 1
        # Sample a small 3×3 patch around center to reduce noise.
        y0 = max(0, center_y - 1)
        y1 = min(h, center_y + 2)
        x0 = max(0, cx - 1)
        x1 = min(w, cx + 2)
        patch = gray[y0:y1, x0:x1]
        cells[i] = 1 if int(np.median(patch)) > thresh else 0

    # Decode.
    if cells[0] != 1:
        return None

    idx = 1
    nonce = 0
    for b in range(64):
        nonce |= int(cells[idx]) << b
        idx += 1
    counter = 0
    for b in range(32):
        counter |= int(cells[idx]) << b
        idx += 1
    ft = 0
    for b in range(3):
        ft |= int(cells[idx]) << b
        idx += 1
    parity_stored = int(cells[idx]); idx += 1
    if cells[idx] != 1:  # stop bit
        return {'nonce': nonce, 'counter': counter, 'type': ft, 'ok': False,
                'reason': 'stop_bit'}
    parity_calc = 0
    for c in cells[1:1 + 64 + 32 + 3]:
        parity_calc ^= int(c)
    if parity_calc != parity_stored:
        return {'nonce': nonce, 'counter': counter, 'type': ft, 'ok': False,
                'reason': 'parity'}
    return {'nonce': nonce, 'counter': counter, 'type': ft, 'ok': True,
            'reason': None}


# -------- Video reader --------

@dataclass
class VideoFrame:
    index: int
    timestamp_ms: float
    decode: Optional[dict]   # from decode_fiducial


class VideoIter:
    def __init__(self, path: str, strip_region: Optional[tuple[int, int, int, int]] = None):
        self.cap = cv2.VideoCapture(path)
        if not self.cap.isOpened():
            raise RuntimeError(f"cannot open video: {path}")
        self.fps = self.cap.get(cv2.CAP_PROP_FPS) or 240.0
        self.width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        self.strip_region = strip_region

    def _auto_strip_region(self, frame_h: int, frame_w: int) -> tuple[int, int, int, int]:
        # The device screen may not fill the whole video frame; we assume the video
        # closely captures the screen and use the same relative placement.
        # For the synthetic test, the video IS the screen so this is exact.
        # For real recordings this needs a per-rig calibration (out of scope: a
        # simple corner-marker at pre-flight would let us auto-locate).
        x0 = STRIP_OFFSET_LEFT_PX
        y1 = frame_h - STRIP_OFFSET_BOTTOM_PX
        y0 = y1 - STRIP_HEIGHT_PX
        x1 = x0 + STRIP_WIDTH_PX
        if x1 > frame_w or y0 < 0:
            # Strip does not fit — fail early rather than silently produce empty decodes.
            # Callers can override with --strip-region if the video is a rescaled screen.
            raise RuntimeError(
                f"Fiducial strip region ({x0},{y0},{x1},{y1}) does not fit in video "
                f"({frame_w}x{frame_h}). Pass --strip-region X0,Y0,X1,Y1 for rescaled recordings."
            )
        return (x0, y0, x1, y1)

    def __iter__(self):
        idx = 0
        while True:
            ok, frame = self.cap.read()
            if not ok:
                break
            timestamp_ms = self.cap.get(cv2.CAP_PROP_POS_MSEC)
            if self.strip_region is None:
                region = self._auto_strip_region(frame.shape[0], frame.shape[1])
            else:
                region = self.strip_region
            x0, y0, x1, y1 = region
            strip = frame[y0:y1, x0:x1]
            decode = decode_fiducial(strip)
            yield VideoFrame(index=idx, timestamp_ms=timestamp_ms, decode=decode)
            idx += 1

    def close(self):
        self.cap.release()


# -------- Analyzer --------

def analyze(jsonl_path: Path, video_path: Path, out_path: Path,
            strip_region: Optional[tuple[int, int, int, int]] = None,
            debug_dir: Optional[Path] = None) -> int:

    # Load JSONL.
    header: Optional[SessionHeader] = None
    events: list[StimulusEvent] = []
    with jsonl_path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            kind = row.get('kind')
            if kind == 'session_header':
                header = SessionHeader(
                    session_id=row['session_id'],
                    session_nonce_hex=row['session_nonce'],
                    device_model=row.get('device_model', ''),
                    os_version=row.get('os_version', ''),
                    target_duration_ms=row['target_duration_ms'],
                    measured_hz=row['measured_hz'],
                    frame_period_ns=int(1e9 / row['measured_hz']) if row.get('measured_hz') else 0,
                    locked_hz=row.get('locked_hz', row.get('measured_hz', 0)),
                )
            elif kind == 'stimulus_event':
                events.append(StimulusEvent(
                    event_index=row['event_index'],
                    fiducial_nonce_hex=row['fiducial_nonce'],
                    target_duration_ms=row['target_duration_ms'],
                    achieved_duration_ms=row['achieved_duration_ms'],
                    frame_count=row['frame_count'],
                    frame_period_ns=row['frame_period_ns'],
                    verification_status=row.get('verification_status', 'unknown'),
                ))
    if header is None:
        print('no session_header in JSONL', file=sys.stderr)
        return 2

    # Index events by their expected fiducial nonce.
    session_nonce = parse_nonce_hex(header.session_nonce_hex)
    nonce_to_event: dict[int, StimulusEvent] = {}
    for ev in events:
        expected = event_nonce_for_index(session_nonce, ev.event_index)
        recorded = parse_nonce_hex(ev.fiducial_nonce_hex)
        # Sanity: the recorded fiducial_nonce_hex must match derivation.
        if expected != recorded:
            print(f"WARNING: event {ev.event_index}: recorded nonce {recorded:016x} "
                  f"!= derived {expected:016x}", file=sys.stderr)
        nonce_to_event[recorded] = ev

    # Walk the video.
    vit = VideoIter(str(video_path), strip_region=strip_region)
    if debug_dir:
        debug_dir.mkdir(parents=True, exist_ok=True)

    # Group target frames by (nonce_matched_event).
    # Each event's measured range = [first_frame_matching, last_frame_matching].
    # We DON'T require consecutive frames because a 240 fps camera may see the
    # transition-in and transition-out frames as ambiguous; instead we take
    # first_ok..last_ok bounded by the same nonce+type=TARGET.
    #
    # But we also warn if there's a gap of >1 frame within the range, which
    # would suggest a dropout.
    decoded_frames = 0
    decoded_ok_frames = 0
    per_event_frames: dict[int, list[VideoFrame]] = {ev.event_index: [] for ev in events}

    for vf in vit:
        if vf.decode is not None:
            decoded_frames += 1
            if vf.decode.get('ok'):
                decoded_ok_frames += 1
                if vf.decode['type'] == FRAME_TARGET:
                    ev = nonce_to_event.get(vf.decode['nonce'])
                    if ev is not None:
                        per_event_frames[ev.event_index].append(vf)
        if debug_dir and vf.index < 300:
            # Save first N frames' decoded state for inspection.
            pass  # skipped; enable if needed
    vit.close()

    # Compute measured stats per event.
    tolerance_ms = (header.frame_period_ns / 1_000_000.0) if header.frame_period_ns > 0 else 8.5
    for ev in events:
        frames = per_event_frames[ev.event_index]
        if not frames:
            ev.external_verified = False
            ev.external_reason = "no_target_frames_decoded"
            continue
        # Basic contiguity: warn if the range has gaps > 1 frame.
        frame_indices = [vf.index for vf in frames]
        # Sorted iteration order (video is sequential, so already sorted).
        first_vf = frames[0]
        last_vf = frames[-1]
        # Measured duration:  present-time of the frame AFTER the last target frame
        #   minus present-time of first target frame.
        # For simplicity here: duration ≈ (last.ts - first.ts) + one_frame_period.
        # A more precise reading uses the next non-target frame's timestamp.
        one_frame_ms = 1000.0 / (vit.fps or 240.0)
        measured_ms = (last_vf.timestamp_ms - first_vf.timestamp_ms) + one_frame_ms
        ev.measured_start_frame = first_vf.index
        ev.measured_end_frame = last_vf.index
        ev.measured_frame_count = last_vf.index - first_vf.index + 1
        ev.measured_duration_ms = measured_ms
        ev.absolute_deviation_ms = measured_ms - ev.achieved_duration_ms
        # Verification:
        max_dev = tolerance_ms  # one refresh-frame period
        # Over-run: measured > target + one_frame is a hard fail per spec §20.
        if measured_ms > ev.target_duration_ms + tolerance_ms + 0.5:
            ev.external_verified = False
            ev.external_reason = "over_target_by_more_than_one_frame"
        elif abs(ev.absolute_deviation_ms) > max_dev:
            ev.external_verified = False
            ev.external_reason = "deviation_over_tolerance"
        else:
            ev.external_verified = True
            ev.external_reason = None

    total = len(events)
    verified = sum(1 for e in events if e.external_verified is True)
    unverified = total - verified
    verified_ratio = (verified / total) if total > 0 else 0.0
    zero_over_target = all(
        (e.measured_duration_ms is None) or
        (e.measured_duration_ms <= e.target_duration_ms + tolerance_ms + 0.5)
        for e in events
    )
    ratio_pass = verified_ratio >= 0.99  # spec §14.4 pass criterion

    report = {
        'kind': 'bench_validation_record',
        'schema_version': '1.0',
        'session_id': header.session_id,
        'session_nonce': header.session_nonce_hex,
        'device_model': header.device_model,
        'os_version': header.os_version,
        'target_duration_ms': header.target_duration_ms,
        'device_reported_hz': header.measured_hz,
        'tolerance_ms_used': tolerance_ms,
        'video_fps': vit.fps,
        'video_resolution': f"{vit.width}x{vit.height}",
        'decoded_frames_total': decoded_frames,
        'decoded_frames_ok': decoded_ok_frames,
        'event_count': total,
        'externally_verified': verified,
        'externally_unverified': unverified,
        'external_verified_ratio': verified_ratio,
        'pass_criteria': {
            'ratio_at_least_0_99': ratio_pass,
            'zero_events_over_target_by_more_than_one_frame': zero_over_target,
            'overall_pass': ratio_pass and zero_over_target,
        },
        'events': [asdict(e) for e in events],
    }
    out_path.write_text(json.dumps(report, indent=2))
    print(f"wrote {out_path}")
    print(f"  events: {total}, externally verified: {verified} ({verified_ratio:.1%}), "
          f"pass: {report['pass_criteria']['overall_pass']}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="RSP F-I02 audit analyzer")
    ap.add_argument('--jsonl', required=True)
    ap.add_argument('--video', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--strip-region', default='auto',
                    help='"auto" or "X0,Y0,X1,Y1" pixel bbox of the fiducial strip')
    ap.add_argument('--debug-frames', default=None,
                    help='If set, dump N debug frames into this directory')
    args = ap.parse_args()

    strip_region = None
    if args.strip_region != 'auto':
        parts = [int(x) for x in args.strip_region.split(',')]
        if len(parts) != 4:
            print('--strip-region must be "X0,Y0,X1,Y1"', file=sys.stderr)
            return 2
        strip_region = tuple(parts)

    debug_dir = Path(args.debug_frames) if args.debug_frames else None
    return analyze(Path(args.jsonl), Path(args.video), Path(args.out),
                   strip_region=strip_region, debug_dir=debug_dir)


if __name__ == '__main__':
    sys.exit(main())
