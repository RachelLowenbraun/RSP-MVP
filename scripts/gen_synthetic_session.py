#!/usr/bin/env python3
"""Generate a synthetic session for testing the F-I02 audit analyzer.

Produces:
  1. A JSONL evidence log in the same format the M0 spike emits on-device.
  2. A high-fps MP4 video in which every video frame is a rendering of the
     device screen at that instant. The fiducial encodes the same bits the C++
     Fiducial::ComputeCells would produce.

This is DEV-ONLY. It bypasses the actual render path and is used strictly to
confirm end-to-end the analyzer's decode + verification math.

Usage:
    python3 gen_synthetic_session.py --out-dir /tmp/synth \\
        --target-ms 33 --events 20 --refresh-hz 120 --video-fps 240

The generator is faithful to the C++ Fiducial encoder — nonce and frame counter
are LSB-first, parity is XOR over payload, start=stop=1.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import secrets
import sys
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


CELLS_TOTAL = 104
CELLS_PAYLOAD = 102
CELL_PX_W = 8
CELL_PX_H = 32
STRIP_OFFSET_LEFT_PX = 24
STRIP_OFFSET_BOTTOM_PX = 24
STRIP_WIDTH_PX = CELLS_TOTAL * CELL_PX_W
STRIP_HEIGHT_PX = CELL_PX_H

FRAME_AMBIENT = 0
FRAME_TARGET = 1


def compute_cells(session_nonce: int, frame_counter: int, frame_type: int) -> np.ndarray:
    """Mirror of C++ ComputeCells."""
    cells = np.zeros(CELLS_TOTAL, dtype=np.uint8)
    cells[0] = 1  # start
    idx = 1
    for b in range(64):
        cells[idx] = (session_nonce >> b) & 1
        idx += 1
    for b in range(32):
        cells[idx] = (frame_counter >> b) & 1
        idx += 1
    for b in range(3):
        cells[idx] = (frame_type >> b) & 1
        idx += 1
    # Parity over cells 1..idx-1 (that's 64+32+3 = 99 payload bits)
    parity = 0
    for c in cells[1:idx]:
        parity ^= int(c)
    cells[idx] = parity
    idx += 1
    cells[idx] = 1  # stop
    return cells


def event_nonce_for_index(session_nonce: int, event_index: int) -> int:
    return (session_nonce ^ ((event_index * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF)) \
        & 0xFFFFFFFFFFFFFFFF


def render_frame(width: int, height: int, kind: str, cells: np.ndarray) -> np.ndarray:
    """Render a frame with the fiducial strip drawn per `cells`."""
    if kind == 'target':
        base_gray = int(0.7 * 255)
    else:
        base_gray = int(0.05 * 255)
    frame = np.full((height, width, 3), base_gray, dtype=np.uint8)
    # Fiducial strip.
    y1 = height - STRIP_OFFSET_BOTTOM_PX
    y0 = y1 - STRIP_HEIGHT_PX
    x0 = STRIP_OFFSET_LEFT_PX
    for i in range(CELLS_TOTAL):
        cx0 = x0 + i * CELL_PX_W
        cx1 = cx0 + CELL_PX_W
        color = 255 if cells[i] == 1 else 0
        frame[y0:y1, cx0:cx1] = (color, color, color)
    return frame


@dataclass
class SessionSpec:
    target_ms: float
    events: int
    refresh_hz: float
    video_fps: float
    width: int = 1080
    height: int = 2400
    inter_event_frames: int = 60
    ramp_frames: int = 10


def generate(spec: SessionSpec, out_dir: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    # Guardrail: the fiducial strip must fit horizontally with margin.
    required_w = STRIP_OFFSET_LEFT_PX + STRIP_WIDTH_PX + 16
    if spec.width < required_w:
        raise ValueError(
            f"video width {spec.width} px is smaller than required {required_w} px "
            f"for the fiducial strip ({STRIP_WIDTH_PX} px + {STRIP_OFFSET_LEFT_PX} px offset + margin). "
            "Use --width >= {required_w} or reduce CELLS_TOTAL / CELL_PX_W."
        )
    required_h = STRIP_OFFSET_BOTTOM_PX + STRIP_HEIGHT_PX + 16
    if spec.height < required_h:
        raise ValueError(
            f"video height {spec.height} px is smaller than required {required_h} px."
        )
    jsonl_path = out_dir / 'session.jsonl'
    video_path = out_dir / 'capture.mp4'

    frame_period_ns = int(round(1e9 / spec.refresh_hz))
    plan_frame_count = max(1, int(round(spec.target_ms / (frame_period_ns / 1e6))))
    achieved_ms = plan_frame_count * (frame_period_ns / 1e6)

    session_nonce = secrets.randbits(64)
    session_nonce_hex = f'{session_nonce:016x}'
    print(f"session_nonce = {session_nonce_hex}")

    # Build the sequence of DEVICE FRAMES (one per refresh-frame interval).
    # Each device frame corresponds to (video_fps / refresh_hz) video frames.
    # That's not an integer in general; we accumulate fractional error and emit
    # ceil/floor as appropriate.
    video_fps = spec.video_fps
    device_frame_period_ns = frame_period_ns
    video_frame_period_ns = int(round(1e9 / video_fps))
    # Ratio of video frames per device frame.
    vpd = video_fps / spec.refresh_hz

    # Emit JSONL header.
    with jsonl_path.open('w') as jf:
        jf.write(json.dumps({
            'kind': 'session_header',
            'session_id': 'synth-' + session_nonce_hex[:8],
            'session_nonce': session_nonce_hex,
            'device_model': 'SYNTH-Pixel-N',
            'device_manufacturer': 'RSP-SYNTH',
            'os_version': '13',
            'api_level': 33,
            'target_duration_ms': spec.target_ms,
            'event_count': spec.events,
            'locked_hz': spec.refresh_hz,
            'measured_hz': spec.refresh_hz,
            'jitter_p99_ns': 200_000,
            'brightness': 0.5,
            'timestamp_ms': 0,
            'spec_version': '1.0',
            'm0_build': 'synth',
        }) + '\n')

        # Build the list of device frames.
        # Sequence: ramp | (inter_event_ambient | target × N | tail_ambient) × events | ramp
        device_frames: list[tuple[str, int, int]] = []  # (kind, counter, event_index_if_target)
        counter = 0
        for _ in range(spec.ramp_frames):
            device_frames.append(('ambient', counter, -1))
            counter += 1
        target_ranges: list[tuple[int, int, int]] = []  # (event_index, first_df, last_df)
        for ev_idx in range(spec.events):
            for _ in range(spec.inter_event_frames):
                device_frames.append(('ambient', counter, -1))
                counter += 1
            first_df = len(device_frames)
            for _ in range(plan_frame_count):
                device_frames.append(('target', counter, ev_idx))
                counter += 1
            last_df = len(device_frames) - 1
            for _ in range(4):
                device_frames.append(('ambient', counter, -1))
                counter += 1
            target_ranges.append((ev_idx, first_df, last_df))

        # Video writer. Use mp4v (broadly supported by OpenCV builds).
        fourcc = cv2.VideoWriter_fourcc(*'mp4v')
        vw = cv2.VideoWriter(str(video_path), fourcc, spec.video_fps,
                             (spec.width, spec.height))
        if not vw.isOpened():
            raise RuntimeError(f"Failed to open video writer for {video_path}")

        # Walk device frames, emit video frames at video_fps.
        # We keep a running mapping (device_frame_index -> [video_frame_indices]).
        total_video_frames = int(math.ceil(len(device_frames) * vpd))
        df_to_vfs: list[list[int]] = [[] for _ in device_frames]
        # Time cursor in nanoseconds.
        t_ns = 0
        video_t_ns = 0
        cur_df = 0
        cur_df_end_ns = device_frame_period_ns
        for vfi in range(total_video_frames):
            # Advance device-frame index if the current time exceeds its end.
            while video_t_ns >= cur_df_end_ns and cur_df + 1 < len(device_frames):
                cur_df += 1
                cur_df_end_ns += device_frame_period_ns
            df_to_vfs[cur_df].append(vfi)
            kind, counter_val, ev_idx = device_frames[cur_df]
            ft = FRAME_TARGET if kind == 'target' else FRAME_AMBIENT
            # Use event nonce for target frames; ambient uses session_nonce as-is.
            active_nonce = event_nonce_for_index(session_nonce, ev_idx) if ft == FRAME_TARGET \
                else session_nonce
            cells = compute_cells(active_nonce, counter_val & 0xFFFFFFFF, ft)
            frame = render_frame(spec.width, spec.height, kind, cells)
            vw.write(frame)
            video_t_ns += video_frame_period_ns
        vw.release()

        # Emit one stimulus_event JSONL row per target range.
        for ev_idx, first_df, last_df in target_ranges:
            fiducial_nonce = event_nonce_for_index(session_nonce, ev_idx)
            # Intended present times: device-frame boundaries within the target range.
            intended = [(first_df + i) * device_frame_period_ns for i in range(plan_frame_count)]
            actual = intended[:]  # perfect simulation
            # Deviation: 0
            row = {
                'kind': 'stimulus_event',
                'session_id': 'synth-' + session_nonce_hex[:8],
                'event_index': ev_idx,
                'fiducial_nonce': f'{fiducial_nonce:016x}',
                'target_duration_ms': spec.target_ms,
                'frame_count': plan_frame_count,
                'frame_period_ns': device_frame_period_ns,
                'achieved_duration_ms': achieved_ms,
                'scheduled_frame_number': first_df,
                'intended_present_ns': intended,
                'actual_present_ns': actual,
                'present_source_per_frame': [2] * plan_frame_count,  # VK_GOOGLE_display_timing
                'timing_deviation_ns': int(round((achieved_ms - spec.target_ms) * 1e6)),
                'refresh_hz_at_event': spec.refresh_hz,
                'brightness_at_event': 0.5,
                'verification_status': 'verified',
            }
            jf.write(json.dumps(row) + '\n')

        jf.write(json.dumps({
            'kind': 'session_footer',
            'session_id': 'synth-' + session_nonce_hex[:8],
            'end_reason': 'completed',
            'total_events': spec.events,
            'verified_events': spec.events,
            'timestamp_ms': 0,
        }) + '\n')

    print(f"wrote {jsonl_path} and {video_path}")
    print(f"  target={spec.target_ms} ms, refresh={spec.refresh_hz} Hz, video={spec.video_fps} fps")
    print(f"  frame_count_per_event={plan_frame_count}, achieved={achieved_ms:.3f} ms")
    return jsonl_path, video_path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--target-ms', type=float, default=33.0)
    ap.add_argument('--events', type=int, default=20)
    ap.add_argument('--refresh-hz', type=float, default=120.0)
    ap.add_argument('--video-fps', type=float, default=240.0)
    ap.add_argument('--width', type=int, default=1080)
    ap.add_argument('--height', type=int, default=2400)
    ap.add_argument('--inter-event-frames', type=int, default=60)
    args = ap.parse_args()
    spec = SessionSpec(
        target_ms=args.target_ms,
        events=args.events,
        refresh_hz=args.refresh_hz,
        video_fps=args.video_fps,
        width=args.width,
        height=args.height,
        inter_event_frames=args.inter_event_frames,
    )
    generate(spec, Path(args.out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
