# RSP — Milestone 0: Android Timing Spike

**Purpose:** the highest-risk-first spike required by RSP MVP spec §19.3. This project answers *one question, definitively*: on a candidate Android device, can we present a target for a specified integer number of frames, with per-frame presentation timestamps captured, and prove the on-screen result to an external 240 fps camera via a fiducial marker?

If this passes on the intended clinical-tier device allowlist, the frame-timing thesis is implementable and the rest of the RSP clinical app can be built on top. If it fails, no downstream work has meaning until the hardware is understood.

**Scope constraint (read this first):** this is a *spike*, not a product. It contains:
- A single Activity that runs a scripted stimulus sequence at a locked refresh rate.
- An NDK C++ render path (Vulkan primary, GLES fallback) that submits frames with explicit desired-present-time bindings.
- A fiducial patch encoding frame counter + session nonce, drawn in the same frame submission as the target.
- Per-frame presentation-timestamp capture via `vkGetPastPresentationTimingGOOGLE` (Vulkan) or `ANativeWindow_getFrameTimestamps` (GLES / NDK).
- A JSONL evidence log matching the schema an eventual `stimulus_event` row would use (§9.2 of the spec).
- Pre-flight gates for: refresh-rate lock, brightness lock, Low Power Mode, thermal throttle, Night Shift / adaptive color, mirroring / cast, split-screen / PiP.
- A **real F-I02 audit analyzer** (Python + OpenCV) that decodes the fiducial from a 240 fps external recording and produces a bench-validation record per spec §14.4.
- A **synthetic session generator** so the analyzer can be tested end-to-end without a device.

**What it does NOT contain (per Milestone 0 scope):**
- No breath sensing.
- No calibration engine.
- No safety subsystem beyond the pre-flight gates.
- No signing / hash chain (the JSONL is unsigned; production evidence log adds signing per spec §14.3).
- No masks (the target is a plain-colored patch).
- No participant UI, no consent, no session state machine.

## Minimum device requirements (matches spec §5.2.3, tightened per Redline Patch 7)

- **Android 13 (API 33)** or newer
- Native display refresh ≥ 90 Hz sustained and lockable
- Vulkan path requires `VK_GOOGLE_display_timing` extension present
- GLES path requires `EGL_ANDROID_presentation_time` + `ANativeWindow_getFrameTimestamps` (present on API 24+)
- Programmatic brightness control

The app will refuse to start on a device that fails any of these checks and will report which check failed with a specific reason.

## What the spike measures

For each stimulus event in a scripted sequence, it records:

| Field | Source | Purpose |
|---|---|---|
| `target_duration_ms` | script | intended value |
| `frame_count` | computed from `frame_period_ns` | integer frames at locked refresh |
| `achieved_duration_ms` | `frame_count × frame_period_ns / 1e6` | theoretical achieved |
| `intended_present_ns` per frame | frame timeline | what we asked for |
| `actual_present_ns` per frame | `Surface.getFrameTimestamps` / `vkGetPastPresentationTimingGOOGLE` | what the compositor did |
| `presentation_deviation_ns` per frame | computed | drift |
| `verification_status` | derived | `verified` / `deviation` / `failed` / `missing_pt` |
| `fiducial_nonce` | random per event | binds an external recording to this event |
| `refresh_hz_at_event` | probed | for drift detection |
| `brightness_at_event` | probed | for luminance stability |

Every event with any `missing_pt` frame is demoted to `deviation` regardless of the derived numbers — per Redline Patch 7 rule that missing presentation timestamps are themselves a failure mode.

## Directory layout

```
app/
├── build.gradle.kts                    Module build config (min API 33, arm64-v8a)
├── src/main/
│   ├── AndroidManifest.xml
│   ├── java/com/rsp/timing/
│   │   ├── MainActivity.kt             Entry point, orchestration
│   │   ├── PreFlightGates.kt           All refuse-to-run checks
│   │   ├── SessionRunner.kt            Coroutine driver + drain loop
│   │   ├── EvidenceLog.kt              JSONL emitter (§9.2 schema)
│   │   ├── FrameTimestampBridge.kt     Kotlin-side timestamp bridge (GLES fallback)
│   │   └── NativeBridge.kt             JNI declarations
│   ├── cpp/
│   │   ├── CMakeLists.txt              -Werror, -fno-exceptions, -fno-rtti
│   │   ├── renderer/
│   │   │   ├── VulkanRenderer.cpp/h    Vulkan + VK_GOOGLE_display_timing
│   │   │   ├── GlesRenderer.cpp/h      GLES3 + EGL + ANativeWindow_getFrameTimestamps
│   │   │   ├── RenderPath.cpp/h        Abstract interface + factory
│   │   │   ├── StimulusShadersSpv.h    Auto-generated SPIR-V (from shaders/)
│   │   │   └── shaders/
│   │   │       ├── stimulus.vert       Fullscreen triangle
│   │   │       └── stimulus.frag       Target + fiducial rendering
│   │   ├── fiducial/
│   │   │   └── Fiducial.cpp/h          104-cell encoder (spec §5.2.4)
│   │   ├── timing/
│   │   │   ├── FrameScheduler.cpp/h    Deterministic frame-count math
│   │   │   └── TimingLog.cpp/h         SPSC lock-free ring buffer
│   │   └── jni/
│   │       └── JniBridge.cpp           Kotlin ↔ C++ glue
│   └── res/
│       ├── layout/activity_main.xml
│       └── values/{strings,themes}.xml
├── build.gradle.kts, settings.gradle.kts
└── scripts/
    ├── embed_shaders.py                Compiles GLSL → SPIR-V → C++ header
    ├── gen_synthetic_session.py        Emits synthetic JSONL + video for analyzer testing
    ├── audit_analyzer.py               ★ Real F-I02 external timing audit (spec §14.4)
    ├── audit_analyzer_stub.py          DEPRECATED (points to audit_analyzer.py)
    └── run_scripted_sequence.md        On-device instructions
```

## Verification status

Everything below runs on this host, verified. On-device runtime is the only thing that requires the physical hardware.

| Component | Status | Verification method |
|---|---|---|
| `FrameScheduler` frame-count math | **VERIFIED** | 5/5 rows of spec §5.2.1 timing table match; scheduling produces correct intended-present intervals |
| `Fiducial` encode/decode + parity | **VERIFIED** | 4/4 encode-decode round-trips pass; single-bit-flip parity detection works; **AddressSanitizer confirms no out-of-bounds writes** (this caught a real bug — see below) |
| `TimingLog` SPSC ring | **VERIFIED** | 2000-record concurrent stress: 0 dropped, 0 order failures; slow-consumer test correctly drops 729/1000 with monotonic indices |
| Vulkan renderer C++ | **BUILDS clean with -Werror** | Compiles on host g++ against real Vulkan headers with `VK_USE_PLATFORM_ANDROID_KHR`; links into shared library; all undefined symbols resolve to `libvulkan.so` / Android NDK on-device |
| GLES renderer C++ | **BUILDS clean with -Werror** | Same as above with real EGL + GLES3 headers |
| JNI bridge | **BUILDS clean with -Werror** | Correct JNI type mapping against real `jni.h` from OpenJDK 21 |
| Full `librsp_timing.so` link | **LINKS clean** | 118 KB shared object; all inter-file symbols resolve |
| SPIR-V shaders | **VERIFIED** | Compiled with `glslangValidator`; embedded in header via `embed_shaders.py` |
| Audit analyzer positive test | **VERIFIED** | 10/10 events externally verified; measured duration matches within microseconds |
| Audit analyzer negative test 1 (lying JSONL) | **VERIFIED** | Analyzer catches a JSONL claiming 50 ms while video shows 33 ms; flags `deviation_over_tolerance` |
| Audit analyzer negative test 2 (missing event) | **VERIFIED** | Analyzer catches an event with no matching fiducial nonce in the video |
| Audit analyzer negative test 3 (overrun) | **VERIFIED** | Analyzer catches video showing 66 ms while JSONL claims 33 ms target; flags `over_target_by_more_than_one_frame` |
| Pre-flight gate logic (Kotlin) | **LIKELY** | Reads real system state; needs a device to run against — can't be host-compiled to a jar and executed |
| On-device runtime end-to-end | **UNVERIFIED** | Requires a physical Android 13+ device |

### Real bug caught during verification

**Fiducial encoder off-by-one** (originally kCells=100, but writes 102 cells: start+64+32+3+parity+stop). AddressSanitizer flagged this as a stack out-of-bounds write. Fixed by bumping `kCells` to 104 (with `kPayloadCells=102`) in `Fiducial.h`, the fragment shader, and the GLES inline shader. Re-verified with ASan — no memory errors.

## Build & run

```
# Prerequisites:
# - Android Studio Hedgehog or newer
# - Android NDK 26+ installed
# - Physical Android 13+ device with USB debugging

# 1. Open this directory in Android Studio.
# 2. Let Gradle sync; it will prompt to install NDK/CMake if missing.
# 3. Connect an eligible device.
# 4. Run 'app' in Release configuration.
# 5. On device: tap "Run 33ms x 200" or a similar preset.
# 6. Point a 240 fps camera at the screen with the fiducial in frame.
# 7. When the sequence completes, the JSONL log path is displayed on-screen.
#    Pull it via `adb pull /sdcard/Android/data/com.rsp.timing/files/logs/`.
# 8. Feed the JSONL + video to the audit analyzer:
#      python3 scripts/audit_analyzer.py \
#          --jsonl session.jsonl --video capture.mp4 --out report.json
```

## Testing without a device

```
# Generate a synthetic session (video + JSONL that mimic what the device produces):
python3 scripts/gen_synthetic_session.py --out-dir /tmp/synth \
    --target-ms 33 --events 10 --refresh-hz 120 --video-fps 240 \
    --width 1080 --height 2400

# Run the analyzer:
python3 scripts/audit_analyzer.py --jsonl /tmp/synth/session.jsonl \
    --video /tmp/synth/capture.mp4 --out /tmp/synth/report.json
```

Requirements: `pip install opencv-python-headless numpy`.

## Interpreting results

- **Pass:** ≥99% of events with `verification_status = 'verified'`, zero events exceeding target by more than 1 frame period, all presentation timestamps populated (no `missing_pt` frames).
- **Partial pass:** all events populated but deviation histogram has a tail — investigate whether it correlates with thermal state, other running apps, or refresh drift.
- **Fail:** any missing presentation timestamps, any refresh-lock drift, any brightness override — device is not eligible for the clinical tier. Not a "we'll come back to it later" situation; it's a hard gate.

## What next after this spike passes on real hardware

Immediate:
1. Bench validation harness (spec F-I01) using this codebase as the driven-device fixture.
2. Milestone 1 — calibration engine (2AFC staircase). Reuses the render path.

Downstream (Milestones 2+):
3. Breath fusion, session engine, safety subsystem, evidence signing + hash chain, backend, safety console, clinician console.

## Correspondence to spec

| Spec section | Implementation here |
|---|---|
| §5.2.1 timing math | `FrameScheduler.h` — `round(target_ms / frame_period_ms)` and achieved computation |
| §5.2.2 rule 1 — lock refresh | `PreFlightGates.kt` — `Surface.setFrameRate` + `Window.setPreferredDisplayModeId` |
| §5.2.2 rule 3 — pre-upload | Pipeline + shaders built once in `SetSurface()`; no allocation in render loop |
| §5.2.2 rule 4 — no GC | Render path is NDK C++ with `-fno-exceptions`; zero JNI per frame; render thread at `THREAD_PRIORITY_URGENT_AUDIO` |
| §5.2.2 rule 5 — explicit timing | `VulkanRenderer::RenderAndPresent` — `VkPresentTimesInfoGOOGLE` with `desiredPresentTime`; `GlesRenderer::PresentOneFrame` — `eglPresentationTimeANDROID` |
| §5.2.2 rule 6 — verify presentation | `VulkanRenderer::RetrievePastPresentationTiming` — `vkGetPastPresentationTimingGOOGLE`; `GlesRenderer` — `ANativeWindow_getFrameTimestamps` |
| §5.2.2 rule 7 — brightness lock | `PreFlightGates::lockBrightness` (window-scoped) |
| §5.2.2 rule 8 — adaptive color | `PreFlightGates::checkDisplayTransforms` — Night Shift / invert / daltonizer |
| §5.2.2 rule 9 — no overlays / mirror | `PreFlightGates::checkCaptureEnvironment` — multi-window, PiP, external display |
| §5.2.2 rule 10 — thermal + power | `PreFlightGates::checkPower` / `checkThermal` |
| §5.2.3 hardware eligibility | `PreFlightGates` full sequence + `VulkanRenderer::PreflightOK` (extension query) |
| §5.2.4 fiducial | `Fiducial.cpp` — same submission as target via shader push-constants |
| §9.2 stimulus_event schema | `EvidenceLog.kt` — JSONL rows match field names |
| §14.4 external timing audit | `scripts/audit_analyzer.py` — real OpenCV-based decoder |

Everything else is out of scope for M0.

## License

Internal, unreleased. Do not distribute.
