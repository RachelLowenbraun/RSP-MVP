# 🚀 RSP M0 — Run This First!

## Prerequisites (5 min)

### Required Hardware
- **Android phone:** Android 13+ with 120 Hz display
  - ✅ Google Pixel 7 Pro, 8, 8 Pro (recommended)
  - ✅ Samsung Galaxy S23+, S24
  - ✅ OnePlus 11, 12
- **USB cable** (USB-C recommended)
- **Computer:** Windows/Mac/Linux with Android Studio

### Required Software
- **Android Studio Hedgehog (2023.1.1+)**
  - Download: https://developer.android.com/studio
  - Includes bundled JDK 17
- **~5 GB free disk space** (Gradle + NDK + build artifacts)

---

## Step 1: Prepare Your Phone (2 min)

### Enable Developer Mode
1. **Settings → About phone**
2. Tap **Build number** 7 times
3. Enter PIN/password when prompted
4. You'll see: "You are now a developer!"

### Enable USB Debugging
1. **Settings → System → Developer options**
2. Toggle **USB debugging** → ON
3. Toggle **Stay awake** → ON (prevents sleep during testing)

### Disable Power-Saving Features
1. **Settings → Battery**
2. **Battery saver** → OFF
3. **Adaptive battery** → OFF (optional, prevents throttling)

### Connect Phone to Computer
1. Plug in USB cable
2. On phone, tap **Allow USB debugging** popup
3. Check "Always allow from this computer" → **OK**

---

## Step 2: Open Project in Android Studio (3 min)

### Launch Android Studio
1. If first time: complete initial setup wizard
2. Main window shows "Welcome to Android Studio"

### Open This Project
1. Click **Open** (or File → Open)
2. Navigate to this folder: `milestone-0-android/`
3. Click **OK**

### Wait for Gradle Sync
Android Studio will automatically:
- Download Gradle dependencies (~1-2 min)
- Install Android SDK 33 (if missing)
- Install NDK 25.1.8937393 (required for C++)
- Install CMake 3.22.1 (required for native build)

**Watch the bottom status bar:**
- "Gradle sync in progress..." → wait
- ✅ "Gradle sync completed successfully" → proceed

**If you see errors:**
- "SDK location not found"
  - **Tools → SDK Manager**
  - Install **Android 13.0 (API 33)** → Apply
- "NDK not configured"
  - **Tools → SDK Manager → SDK Tools** tab
  - Check ☑ **NDK (Side by side)** → Apply
- Other errors → run `./PREFLIGHT_CHECK.sh` in terminal

---

## Step 3: Verify Device Connection (30 sec)

### Check Device Appears
Top toolbar should show your device name:
- Example: "Pixel 8 Pro" or "Samsung SM-S918B"

**If device not visible:**
```bash
# In Android Studio terminal (Alt+F12 / Ctrl+` ):
adb devices

# Should output:
# List of devices attached
# 1A2B3C4D5E6F    device

# If shows "unauthorized" → check phone for popup
# If empty → try different USB port or cable
```

---

## Step 4: Build the App (1 min)

### Select Build Variant
1. **View → Tool Windows → Build Variants**
   - Or click "Build Variants" tab at bottom-left
2. Change **app** from `debug` to **`release`**
   - (Release = optimized, no debug overhead)

### Build Project
- Click **Build → Make Project**
- Or press `Ctrl+F9` (Windows/Linux) / `Cmd+F9` (Mac)

**Expected output in Build window:**
```
BUILD SUCCESSFUL in 45s
42 actionable tasks: 42 executed
```

**If build fails:**
- Check error messages in "Build" panel
- Common fix: **Build → Clean Project** → retry
- Still failing? → Paste error here and I'll help

---

## Step 5: Deploy & Run (1 min)

### Deploy to Device
1. Click green **▶ Run** button (top-right toolbar)
2. Or press `Shift+F10` (Windows/Linux) / `Ctrl+R` (Mac)

Android Studio will:
- Install APK to device (~5-10 sec)
- Launch app automatically

**On your phone, you should now see:**
```
╔═══════════════════════════════╗
║   RSP Timing Spike            ║
║                               ║
║  Status: IDLE                 ║
║  Device: Pixel 8 Pro          ║
║  Refresh: 120.0 Hz            ║
║                               ║
║  ┌─────────────────────────┐ ║
║  │   Run Timing Test       │ ║
║  └─────────────────────────┘ ║
║                               ║
║  [View Logs]   [Export]      ║
║                               ║
╚═══════════════════════════════╝
```

**If app crashes or doesn't launch:**
- Check Android Studio "Logcat" window for errors
- Filter: `RSP_TIMING`
- Paste crash logs and I'll debug

---

## Step 6: Run First Test (30 sec)

### On Your Phone
1. Tap **[Run Timing Test]** button
2. Screen goes **fullscreen black**
3. **Look at bottom-left corner** — you should see:
   - A thin white/black flickering strip (about 1 cm wide)
   - This is the "fiducial" — timing reference for audit
4. After ~10 seconds, screen returns to main view
5. Status updates to: `COMPLETED (20 events)`

### Check Android Studio Logcat
In Android Studio:
1. **View → Tool Windows → Logcat**
2. In filter box, type: `RSP_TIMING`

**You should see ~20 lines like this:**
```
I/VulkanRenderer: VK_GOOGLE_display_timing: SUPPORTED
I/VulkanRenderer: Swapchain created: 1080x2400, format=50
I/VulkanRenderer: Refresh rate locked: 120.000000 Hz
I/FrameScheduler: Target 33.0 ms → 4 frames → 33.333 ms
I/SessionRunner: ═══ Session Start (nonce: a3f8e942...) ═══
I/SessionRunner: Event  0: frames=4, achieved=33.333 ms ✓ verified
I/SessionRunner: Event  1: frames=4, achieved=33.333 ms ✓ verified
I/SessionRunner: Event  2: frames=4, achieved=33.333 ms ✓ verified
...
I/SessionRunner: Event 19: frames=4, achieved=33.333 ms ✓ verified
I/SessionRunner: ═══ Session Complete: 20/20 verified ═══
```

---

## ✅ Success Checklist

**You've successfully completed M0 if:**

- [x] App builds without errors
- [x] App runs on device without crashing
- [x] Status shows `Refresh: 120.0 Hz` (or 90/144 Hz)
- [x] Test completes (~10 sec) and returns to main screen
- [x] Status shows `COMPLETED (20 events)`
- [x] Logcat shows: `20/20 verified` ✓
- [x] Logcat shows: `VK_GOOGLE_display_timing: SUPPORTED` ✓

**All checked? → MILESTONE 0 PASS! 🎉**

---

## Step 7: Extract Evidence (1 min)

### Pull the Log File
In Android Studio terminal (or system terminal):
```bash
# List evidence files
adb shell "ls -lh /sdcard/Android/data/com.rsp.timing/files/evidence/"

# Pull most recent session
adb pull /sdcard/Android/data/com.rsp.timing/files/evidence/session_*.jsonl ~/Desktop/
```

### Validate the Log
```bash
# Count verified events (should output: 20)
grep "verification_status" ~/Desktop/session_*.jsonl | grep -c "verified"

# View first event
head -30 ~/Desktop/session_*.jsonl
```

**Expected structure:**
```json
{"header":{"session_id":"...","device_model":"Pixel 8 Pro","refresh_rate_hz":120.0,...}}
{"event":{"event_index":0,"frame_count":4,"achieved_duration_ms":33.333332,"presented_timestamps_ns":[...],"verification_status":"verified"}}
{"event":{"event_index":1,...}}
...
{"footer":{"end_reason":"completed","total_events":20,"verified_events":20}}
```

---

## 📊 What to Report

**Please share:**
1. ✅ Device model + Android version
   - Example: "Pixel 8 Pro, Android 14"
2. ✅ Screenshot of status screen showing `COMPLETED (20 events)`
3. ✅ Paste Logcat output (filter: `RSP_TIMING`, ~20 lines)
4. ✅ Output of: `grep "verified" session_*.jsonl | wc -l`

**This proves:**
- Frame-exact timing works on your device
- Vulkan renderer meets spec §5.2 requirements
- Device is eligible for RSP clinical study

---

## 🐛 Troubleshooting

### "Device not detected"
- Check USB cable (try different port)
- Phone shows "Allow USB debugging?" → tap **OK**
- Run: `adb devices` (should list one device)

### "Build failed"
- **Tools → SDK Manager** → install Android SDK 33 + NDK
- **Build → Clean Project** → rebuild
- Paste error and I'll help

### "App crashes on launch"
- Check Logcat for `FATAL EXCEPTION`
- Device might lack `VK_GOOGLE_display_timing` (incompatible)
- Try different device or paste crash log

### "VK_GOOGLE_display_timing: NOT SUPPORTED"
- Device doesn't support required Vulkan extension
- Try newer Pixel/Samsung/OnePlus model
- Or report device model → we'll check compatibility

### "Fiducial strip invisible"
- Max screen brightness (Settings → Display)
- Disable adaptive brightness
- Disable Night Light / blue light filter

### "Test stuck on black screen"
- Force-stop app (phone Settings → Apps)
- Disable battery saver
- Retry

---

## 🎯 What's Next?

After confirming M0 pass:
1. **Thermal test** — Run 5 consecutive sessions, check for throttling
2. **Device allowlist** — Add validated model to spec §5.2.3
3. **Milestone 1** — Build calibration engine (2AFC staircase)
4. **External audit** — 240 fps camera validation (optional, see QUICKSTART.md Phase 6)

---

## 📚 More Documentation

- **QUICKSTART.md** — Expanded guide with Phase 6 external audit
- **README.md** — Architecture & implementation details
- **PREFLIGHT_CHECK.sh** — Automated environment validator
- **scripts/audit_analyzer.py** — 240 fps external verification tool

---

**Questions?** Paste errors/logs and I'll help debug!

**Success?** Report back with device info + screenshot! 🚀
