# GitHub Actions APK Build Setup

This guide will set up automated APK builds using GitHub Actions (free, no local Android SDK needed).

## Prerequisites

- GitHub account (free)
- Git installed on your computer

## Step-by-Step Setup (5 minutes)

### 1. Create GitHub Repository

```bash
# Option A: Via GitHub website
# 1. Go to https://github.com/new
# 2. Repository name: rsp-m0-android
# 3. Visibility: Public (or Private - both work)
# 4. Don't initialize with README
# 5. Click "Create repository"

# Option B: Via GitHub CLI (if installed)
gh repo create rsp-m0-android --public --source=. --remote=origin
```

### 2. Push Project to GitHub

```bash
# Navigate to project directory
cd ~/AndroidStudioProjects/milestone-0-android

# Initialize git (if not already done)
git init

# Add all files
git add .

# Commit
git commit -m "Initial commit - RSP M0 Android timing spike"

# Add remote (replace YOUR_USERNAME)
git remote add origin https://github.com/YOUR_USERNAME/rsp-m0-android.git

# Push to GitHub
git branch -M main
git push -u origin main
```

### 3. Trigger the Build

The GitHub Action will **automatically start building** as soon as you push!

**Monitor build progress:**
1. Go to your repository on GitHub
2. Click **"Actions"** tab
3. You'll see "Build RSP M0 Android APK" workflow running

**Build takes ~8-12 minutes** (GitHub provides free Ubuntu runner)

### 4. Download the APK

Once build completes (green checkmark ✅):

1. Click on the workflow run
2. Scroll down to **"Artifacts"** section
3. Download **"rsp-m0-android-apk"** (contains `rsp-m0-timing-spike.apk`)
4. Also download **"build-info"** for installation instructions

### 5. Install on Android Device

**Enable installation from unknown sources:**
1. Android Settings → Security → Unknown sources → Enable
   - Or: Settings → Apps → Special access → Install unknown apps → Chrome/Files → Allow

**Transfer APK to phone:**
```bash
# Option A: Via USB cable
adb install rsp-m0-timing-spike.apk

# Option B: Via cloud
# Upload APK to Google Drive/Dropbox
# Download on phone and tap to install

# Option C: Direct USB transfer
# Copy APK to phone's Download folder
# Use phone's file manager → tap APK → Install
```

---

## Manual Trigger (Re-build on Demand)

To trigger a new build without code changes:

1. Go to **Actions** tab on GitHub
2. Click **"Build RSP M0 Android APK"** workflow
3. Click **"Run workflow"** button (right side)
4. Select branch: `main`
5. Click green **"Run workflow"**

---

## Troubleshooting

### Build fails with "Gradle error"
**Fix:** Check `build.gradle.kts` syntax. The workflow logs show exact error line.

### Build fails with "NDK not found"
**Fix:** Workflow already installs NDK 25.1.8937393. If error persists, check `.github/workflows/build-apk.yml` line 23.

### APK won't install on phone
**Fix:** 
- Enable "Install from unknown sources" (see step 5 above)
- Check phone is Android 13+ (API 33+)
- Try: `adb install -r rsp-m0-timing-spike.apk` (force reinstall)

### Artifact expired (after 30 days)
**Fix:** Re-run workflow manually (see "Manual Trigger" above)

---

## Advanced: Build Locally (If You Have Android SDK)

If you already have Android Studio installed:

```bash
cd milestone-0-android

# Build release APK
./gradlew assembleRelease

# APK location:
# app/build/outputs/apk/release/app-release-unsigned.apk

# Sign it
keytool -genkey -v -keystore release.keystore -alias rsp -keyalg RSA -keysize 2048 -validity 10000
jarsigner -verbose -keystore release.keystore app/build/outputs/apk/release/app-release-unsigned.apk rsp

# Align (zipalign is in Android SDK build-tools/)
zipalign -v 4 app/build/outputs/apk/release/app-release-unsigned.apk rsp-m0.apk

# Install directly
adb install rsp-m0.apk
```

---

## What the GitHub Action Does

1. ✅ Sets up Ubuntu runner (free, fast)
2. ✅ Installs JDK 17
3. ✅ Installs Android SDK
4. ✅ Installs NDK 25.1.8937393 + CMake 3.22.1
5. ✅ Builds release APK with native C++ renderer
6. ✅ Signs APK with auto-generated keystore
7. ✅ Aligns APK (zipalign)
8. ✅ Uploads artifact (kept for 30 days)
9. ✅ Generates build info

**Total cost: $0** (GitHub Actions free tier: 2000 min/month)

---

## File Structure After Setup

```
milestone-0-android/
├─ .github/
│  └─ workflows/
│     └─ build-apk.yml          ← GitHub Actions config
├─ app/
│  └─ build.gradle.kts
├─ gradle/
│  └─ wrapper/
│     └─ gradle-wrapper.properties
├─ gradlew                       ← Gradle wrapper script
├─ GITHUB_APK_BUILD_SETUP.md    ← This file
└─ ... (rest of project)
```

---

## Next Steps After APK Install

1. Launch app on phone
2. Tap **[Run Timing Test]**
3. Observe flickering fiducial strip (bottom-left)
4. Check results: should show `COMPLETED (20 events)`
5. Report back: Device model + Android version + screenshot

---

## Questions?

- **Build failing?** Check Actions tab → click workflow → view logs
- **APK won't install?** Verify Android 13+ and unknown sources enabled
- **Need help?** Share the Actions build log (click failed step → copy text)

**Estimated end-to-end time:**
- Setup repo: 2 min
- Push code: 1 min  
- GitHub builds: 10 min
- Download + install: 2 min
- **Total: ~15 minutes to APK on phone** 🚀
