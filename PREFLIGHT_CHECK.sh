#!/bin/bash
# RSP M0 Pre-Flight Check
# Run this before opening in Android Studio to verify environment

set -e

echo "════════════════════════════════════════════════════════"
echo "  RSP Milestone 0 — Pre-Flight Environment Check"
echo "════════════════════════════════════════════════════════"
echo ""

# Check 1: Java/JDK
echo "[1/6] Checking Java..."
if command -v java &> /dev/null; then
    JAVA_VERSION=$(java -version 2>&1 | head -n 1 | cut -d'"' -f2)
    echo "  ✓ Java found: $JAVA_VERSION"
    
    MAJOR=$(echo "$JAVA_VERSION" | cut -d'.' -f1)
    if [ "$MAJOR" -ge 17 ]; then
        echo "  ✓ Java 17+ requirement met"
    else
        echo "  ✗ Java 17+ required (found $MAJOR)"
        exit 1
    fi
else
    echo "  ✗ Java not found. Install JDK 17+ or Android Studio (bundles JDK)"
    exit 1
fi

# Check 2: Android SDK (optional, AS will install)
echo ""
echo "[2/6] Checking Android SDK..."
if [ -n "$ANDROID_HOME" ] || [ -n "$ANDROID_SDK_ROOT" ]; then
    SDK_PATH="${ANDROID_HOME:-$ANDROID_SDK_ROOT}"
    echo "  ✓ Android SDK: $SDK_PATH"
else
    echo "  ⚠ Android SDK not found (Android Studio will install)"
fi

# Check 3: adb
echo ""
echo "[3/6] Checking ADB..."
if command -v adb &> /dev/null; then
    ADB_VERSION=$(adb version | head -n 1)
    echo "  ✓ ADB found: $ADB_VERSION"
    
    DEVICES=$(adb devices | grep -v "List" | grep -v "^$" | wc -l)
    if [ "$DEVICES" -gt 0 ]; then
        echo "  ✓ $DEVICES device(s) connected:"
        adb devices | grep -v "List" | grep -v "^$" | sed 's/^/    /'
    else
        echo "  ⚠ No devices connected (connect via USB + enable debugging)"
    fi
else
    echo "  ⚠ ADB not found (will be available after Android Studio installs SDK)"
fi

# Check 4: Python (for audit analyzer)
echo ""
echo "[4/6] Checking Python..."
if command -v python3 &> /dev/null; then
    PYTHON_VERSION=$(python3 --version | cut -d' ' -f2)
    echo "  ✓ Python found: $PYTHON_VERSION"
    
    if python3 -c "import cv2" 2>/dev/null; then
        CV_VERSION=$(python3 -c "import cv2; print(cv2.__version__)")
        echo "  ✓ OpenCV installed: $CV_VERSION"
    else
        echo "  ⚠ OpenCV not found (needed for Phase 6 audit)"
        echo "    Install: pip3 install opencv-python-headless"
    fi
else
    echo "  ✗ Python 3 not found. Install Python 3.10+ for audit analyzer."
fi

# Check 5: Project structure
echo ""
echo "[5/6] Checking project files..."
REQUIRED_FILES=(
    "app/build.gradle.kts"
    "app/src/main/AndroidManifest.xml"
    "app/src/main/java/com/rsp/timing/MainActivity.kt"
    "app/src/main/cpp/CMakeLists.txt"
    "app/src/main/cpp/renderer/VulkanRenderer.cpp"
    "scripts/audit_analyzer.py"
)

ALL_PRESENT=true
for FILE in "${REQUIRED_FILES[@]}"; do
    if [ -f "$FILE" ]; then
        echo "  ✓ $FILE"
    else
        echo "  ✗ $FILE MISSING"
        ALL_PRESENT=false
    fi
done

if [ "$ALL_PRESENT" = false ]; then
    echo ""
    echo "  ERROR: Some required files missing. Re-extract rsp-m0-android-v2.zip"
    exit 1
fi

# Check 6: Disk space
echo ""
echo "[6/6] Checking disk space..."
AVAILABLE=$(df -h . | tail -1 | awk '{print $4}')
echo "  Available: $AVAILABLE"
echo "  Required: ~5 GB (for Gradle cache + NDK + build artifacts)"

echo ""
echo "════════════════════════════════════════════════════════"
echo "  Pre-Flight Check: COMPLETE"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Next steps:"
echo "  1. Open Android Studio"
echo "  2. File → Open → $(pwd)"
echo "  3. Wait for Gradle sync"
echo "  4. See QUICKSTART.md for full guide"
echo ""
