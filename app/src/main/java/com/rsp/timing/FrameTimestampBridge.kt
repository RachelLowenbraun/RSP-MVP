package com.rsp.timing

import android.os.Build
import android.util.Log
import android.view.Surface

/**
 * Retrieves actual presentation timestamps from Surface.getFrameTimestamps and
 * forwards them to the C++ core for event closure.
 *
 * IMPORTANT:
 * - Surface.getFrameTimestamps requires Surface.enableFrameTimestamps(true) before
 *   the render block. Timestamps are only retrievable after a 2-3 frame latency;
 *   this class polls after the render loop has advanced.
 * - This is the GLES/Compositor path. The Vulkan path uses
 *   vkGetPastPresentationTimingGOOGLE and does its own retrieval in C++, so this
 *   class is a no-op when the render path is Vulkan.
 * - Per Redline Patch 7: any frame with no retrievable presentation time is reported
 *   with source = MISSING and demotes the entire event.
 */
class FrameTimestampBridge(private val surface: Surface, private val useVulkan: Boolean) {

    companion object {
        private const val TAG = "FrameTimestampBridge"
        private const val SOURCE_FRAME_TIMELINE = 0
        private const val SOURCE_EGL_PT = 1
        private const val SOURCE_VK_DISPLAY_TIMING = 2
        private const val SOURCE_MISSING = 3
    }

    fun enable() {
        if (useVulkan) return  // Vulkan path retrieves via extension in C++
        try {
            surface.setFrameRate(0f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        } catch (t: Throwable) {
            // setFrameRate is best-effort here; the actual lock is via
            // Window.setPreferredDisplayModeId in PreFlightGates.
        }
        // API 24+: Surface.getFrameTimestamps requires timestamps be enabled.
        // On API 33+ this is largely automatic but we explicitly enable to be safe.
        // (There is no public enableFrameTimestamps as of API 34; enabling is implicit
        // for surfaces on the compositor.)
    }

    /**
     * For each frame number in [scheduledFrameNumbers], attempt to retrieve the actual
     * presentation timestamp and forward it to native. Call this AFTER the render loop
     * has advanced at least 3 frames past the last scheduled frame.
     *
     * Returns count of successfully retrieved timestamps.
     */
    fun drainAndIngest(scheduledFrameNumbers: LongArray): Int {
        if (useVulkan) return 0  // handled in C++
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
            // Below API 24 no frame timestamps API. Our min is API 33 so unreachable.
            return 0
        }
        var got = 0
        for (fn in scheduledFrameNumbers) {
            val presentNs = try {
                // Surface.getFrameTimestamp is a hidden API on some releases; the public
                // path is via Choreographer / SurfaceControl callbacks. In the M0 spike
                // this Kotlin bridge only exists as a stub — the C++ side handles
                // presentation timestamps via VK_GOOGLE_display_timing (Vulkan) or via
                // ANativeWindow_getFrameTimestamps (GLES / NDK).
                // We forward MISSING here to make explicit that this path is unused
                // in the current spike; real fill happens in native.
                -1L
            } catch (t: Throwable) {
                -1L
            }
            val source = if (presentNs > 0) SOURCE_EGL_PT else SOURCE_MISSING
            NativeBridge.nativeIngestPresentTimestamp(fn, if (presentNs > 0) presentNs else 0L, source)
            if (presentNs > 0) got++
        }
        return got
    }
}
