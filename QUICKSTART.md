# RSP Milestone 0 — Quick Start Guide

**Goal:** Build, deploy, and validate frame-exact timing on Android hardware.

---

## Prerequisites Checklist

### Hardware
- [ ] **Android device** with:
  - Android 13+ (API 33+)
  - 120 Hz display (90 Hz minimum)
  - **Recommended:** Pixel 7 Pro, Pixel 8, Pixel 8 Pro, Galaxy S23+
- [ ] **USB-C cable** for device connection
- [ ] **240 fps camera** (iPhone 13 Pro+ slow-mo, GoPro, etc.) — for Phase 4 only

### Software
- [ ] **Android Studio Hedgehog (2023.1.1)** or newer
  - Download: https://developer.android.com/studio
- [ ] **JDK 17+** (bundled with Android Studio)
- [ ] **Python 3.10+** with pip (for audit analyzer)

---

## Phase 1: Project Setup (10 min)

### Step 1.1: Extract Project
```bash
# Navigate to your workspace
cd ~/AndroidStudioProjects/

# Extract the zip (if not already done)
unzip /path/to/rsp-m0-android-v2.zip

# You should now have:
# ~/AndroidStudioProjects/milestone-0-android/
```

### Step 1.2: Open in Android Studio
1. Launch Android Studio
2. **File → Open**
3. Navigate to `~/AndroidStudioProjects/milestone-0-android`
4. Click **OK**

### Step 1.3: First Sync
Android Studio will automatically:
- Download Gradle dependencies (~2 min)
- Install NDK 25.1.8937393
- Install CMake 3.22.1
- Configure Vulkan SDK

**Watch the bottom status bar** for "Gradle sync completed successfully"

**If you see errors:**
- "SDK location not found" → **Tools → SDK Manager** → Install Android SDK 33+
- "NDK not configured" → **Tools → SDK Manager → SDK Tools** tab → Check "NDK (Side by side)" → Apply

---

## Phase 2: Enable Device (5 min)

### Step 2.1: Enable Developer Mode
On your Android device:
1. **Settings → About phone**
2. Tap **Build number** 7 times
3. Enter PIN/password if prompted
4. You'll see "You are now a developer!"

### Step 2.2: Enable USB Debugging
1. **Settings → System → Developer options**
2. Toggle **USB debugging** ON
3. Toggle **Stay awake** ON (prevents screen lock during testing)

### Step 2.3: Connect Device
1. Plug device into computer via USB
2. On device, tap **Allow USB debugging** when prompted
3. Check "Always allow from this computer" → **OK**

### Step 2.4: Verify Connection
In Android Studio:
- Top toolbar shows your device (e.g., "Pixel 8 Pro")
- Or check terminal: `adb devices` should list one device

---

## Phase 3: Build & Deploy (5 min)

### Step 3.1: Select Build Variant
1. **View → Tool Windows → Build Variants** (or click "Build Variants" tab at bottom-left)
2. Change **app** variant from `debug` to **`release`**
   - Release has optimizations enabled + ProGuard disabled for M0

### Step 3.2: Build
Click **Build → Make Project** (or `Ctrl+F9` / `Cmd+F9`)

**Expected:** Build completes in ~30-60 seconds with output:
```
BUILD SUCCESSFUL in 47s
```

**If you see C++ errors:**
- Check **Tools → SDK Manager → SDK Tools** → ensure CMake 3.22.1 installed
- Clean project: **Build → Clean Project** → retry

### Step 3.3: Deploy
Click **Run → Run 'app'** (or green ▶ button)

**Expected:**
- App installs on device (~10 sec)
- App launches automatically
- You see the main screen with "RSP Timing Spike" title

---

## Phase 4: Run First Test (2 min)

### On Device Screen:
You should see:
```
┌─────────────────────────────┐
│   RSP Timing Spike          │
│                             │
│  Status: IDLE               │
│  Device: Pixel 8 Pro        │
│  Refresh: 120.0 Hz          │
│                             │
│  [Run Timing Test]          │
│  [View Logs]                │
│  [Export Evidence]          │
└─────────────────────────────┘
```

### Step 4.1: Run Test
1. Tap **[Run Timing Test]**
2. Screen goes **fullscreen black**
3. Look at **bottom-left corner** — you should see a thin flickering strip (white/black pattern)
   - This is the fiducial encoder
4. After ~10 seconds, screen returns to main view
5. Status changes to: `COMPLETED (20 events)`

### Step 4.2: Check Logcat
In Android Studio:
1. **View → Tool Windows → Logcat**
2. Filter: `RSP_TIMING`

**Expected output:**
```
I/VulkanRenderer: VK_GOOGLE_display_timing: SUPPORTED
I/VulkanRenderer: Swapchain created: 1080x2400
I/VulkanRenderer: Refresh rate locked: 120.000000 Hz
I/FrameScheduler: Target 33.0 ms → 4 frames → 33.333 ms
I/SessionRunner: ═══ Session Start ═══
I/SessionRunner: Event 0: frames=4, achieved=33.333 ms, verified=true
I/SessionRunner: Event 1: frames=4, achieved=33.333 ms, verified=true
...
I/SessionRunner: Event 19: frames=4, achieved=33.333 ms, verified=true
I/SessionRunner: ═══ Session Complete: 20/20 events verified ═══
```

**✅ SUCCESS CRITERIA:**
- No crashes
- All events show `verified=true`
- Achieved duration = 33.333 ms (for 120 Hz @ 33 ms target)

**❌ If you see errors:**
- `VK_GOOGLE_display_timing: NOT SUPPORTED` → Device incompatible (file issue)
- `verification_status=missing_pt` → Timestamp API failed (try different device)
- App crashes → Check full Logcat for stack trace, report back

---

## Phase 5: Extract Evidence (5 min)

### Step 5.1: Pull JSONL
Open terminal:
```bash
# List evidence files
adb shell "ls -lh /sdcard/Android/data/com.rsp.timing/files/evidence/"

# You should see:
# session_20260830_143022.jsonl

# Pull the latest
adb pull /sdcard/Android/data/com.rsp.timing/files/evidence/session_*.jsonl ~/Desktop/
```

### Step 5.2: Inspect JSONL
```bash
cd ~/Desktop
head -30 session_*.jsonl
```

**Expected structure:**
```json
{"header":{"session_id":"a3f8e9...","device_model":"Pixel 8 Pro","os_version":"14","refresh_rate_hz":120.0,"target_duration_ms":33.0,...}}
{"event":{"event_index":0,"frame_count":4,"achieved_duration_ms":33.333332,"presented_timestamps_ns":[8334567890,8342901223,8351234556,8359567889],"verification_status":"verified"}}
{"event":{"event_index":1,...}}
...
{"footer":{"end_reason":"completed","total_events":20,"verified_events":20}}
```

**Validation commands:**
```bash
# Check if all events verified
grep "verification_status" session_*.jsonl | grep -c "verified"
# Should output: 20

# Check footer
tail -1 session_*.jsonl | jq .
```

---

## Phase 6: External Audit (OPTIONAL — 30 min)

**Only needed for publication-grade validation.**

### Setup
1. Mount phone on tripod/stand (vertical orientation)
2. Position 240 fps camera ~30 cm away, framed on phone screen
3. Ensure fiducial strip (bottom-left) is in frame
4. Dim room lighting

### Record
1. Start 240 fps recording on camera
2. Tap **[Run Timing Test]** on phone **after** recording starts
3. Wait for session to complete
4. Stop camera recording

### Analyze
```bash
cd ~/AndroidStudioProjects/milestone-0-android/scripts

python3 audit_analyzer.py \
  --video ~/Desktop/audit_capture.mov \
  --jsonl ~/Desktop/session_20260830_143022.jsonl \
  --output ~/Desktop/audit_report.json

# Check result
jq .summary.overall_pass ~/Desktop/audit_report.json
# Should output: true
```

---

## Quick Troubleshooting

| Problem | Solution |
|---------|----------|
| "Gradle sync failed" | **File → Invalidate Caches → Restart** |
| "NDK not found" | **Tools → SDK Manager → SDK Tools** → Install NDK 25.x |
| Device not detected | Check USB cable, try different port, run `adb devices` |
| App crashes on launch | Check Logcat for `FATAL EXCEPTION`, share stack trace |
| Black screen stuck | Force-stop app, check battery saver is OFF |
| Fiducial invisible | Max screen brightness, disable auto-brightness |

---

## Success Checkpoint

**You've completed M0 testing if:**
- ✅ App builds without errors
- ✅ App runs on device without crashes
- ✅ Status shows detected refresh rate (90/120/144 Hz)
- ✅ Test completes with 20/20 events verified
- ✅ JSONL extracted successfully
- ✅ Logcat shows `VK_GOOGLE_display_timing: SUPPORTED`

**Report back with:**
1. Device model + Android version
2. Screenshot of final status screen
3. First 30 lines of JSONL (`head -30 session_*.jsonl`)
4. Output of: `grep "verification_status" session_*.jsonl | grep -c "verified"`

---

## What's Next?

After confirming PASS:
- **Add device to allowlist** (spec §5.2.3)
- **Benchmark thermal/battery** (5× 20-min sessions)
- **Milestone 1:** Build calibration engine (2AFC staircase)

Need help? Paste errors and I'll debug.
