# Running a scripted timing sequence

Once the M0 app is built and installed on a candidate device:

## 1. Confirm the device is a candidate

Required:
- Android 13 (API 33) or newer
- Native display refresh ≥ 90 Hz — check under `Settings › Display › Refresh Rate` or run `dumpsys display | grep "refresh"`
- Vulkan 1.3 support (most 2021+ flagships)
- Do NOT run this test on a device running under a battery-saver mode; the app will refuse

## 2. Set up the external timing audit rig

- A second phone or camera with ≥240 fps video capture (recent iPhones: slo-mo; recent Sony/Pixel: slow-motion)
- A stable stand or tripod
- Room lighting steady; the fiducial strip must be clearly visible without saturation

Position: point at the M0 app's screen so the entire fiducial strip is captured. The strip is at the bottom-left of the display, 24 px in from each edge, 100 cells × 8 px × 32 px. On a 1080-pixel-wide display that's roughly 800 × 32 px in the corner.

Start the 240 fps recording BEFORE you tap "Run" in the app; stop it AFTER the on-screen status shows "Done."

## 3. Run a sequence

Launch the app. Wait for status = "Pre-flight OK. Ready. (locked ??? Hz)".

Tap one of:
- **Refresh-lock probe (300 frames)** — sanity check; produces a session with 0 events and one probe result. Confirms the refresh probe returns sensible values on this device.
- **Run 33 ms × 200 events** — the primary test. 200 events at 33ms target.
- **Run 50 ms × 200 events** — comparison; produces different-duration data.

Watch the status line update per event. When it shows "Done", note the `log path` on the second line.

## 4. Pull the log

```
adb shell "ls /sdcard/Android/data/com.rsp.timing/files/logs/"
adb pull /sdcard/Android/data/com.rsp.timing/files/logs/m0-session-<ts>.jsonl ./
```

## 5. Analyze

Feed the JSONL and the 240 fps video to the audit analyzer (F-I02, `audit_analyzer_stub.py` in this same directory — placeholder). Pass criterion per spec §14.4:

- Measured on-screen duration (from decoded fiducial) within ±1 frame period of logged `achieved_duration_ms` for **≥99%** of events
- Zero events exceeding target by more than 1 frame period

## 6. Report

For each device model tested, produce a bench-validation record with:
- Device model, OS version, Vulkan driver version
- Refresh probe results (measured Hz, jitter p99)
- Event count, verified count, deviation histogram
- Any events with `verification_status = 'failed'` or `'missing_pt'`
- The 240 fps rig analyzer output

A device model that passes both the software-side stats (from the JSONL) and the external-audit stats (from the fiducial decode) advances to full §18.2 bench validation. A device that fails either does not enter the clinical-tier allowlist.
