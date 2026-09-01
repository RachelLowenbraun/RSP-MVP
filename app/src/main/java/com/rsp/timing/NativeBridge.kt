package com.rsp.timing

/**
 * All JNI entry points into the C++ render/timing core.
 *
 * Contract:
 *   - The C++ core owns the render thread, the render loop, and the frame scheduler.
 *   - Kotlin calls in to configure and to poll evidence records; C++ never calls back
 *     during a stimulus event (per Redline Patch 7 rule: zero JNI per frame during
 *     a flash sequence).
 *   - The only callback path is [Callback], invoked ONLY between events or after
 *     the sequence completes, to hand back completed event records for JSONL serialization.
 */
object NativeBridge {

    init {
        System.loadLibrary("rsp_timing")
    }

    /** One-time engine init. Returns 0 on success, negative errno-style code on failure. */
    external fun nativeInit(useVulkan: Boolean): Int

    /** Bind the SurfaceView's underlying [android.view.Surface]. */
    external fun nativeSetSurface(surface: Any?)

    /** Query the display refresh probe: measured Hz over N frames, jitter p99 ns. */
    external fun nativeProbeRefresh(sampleFrames: Int): DoubleArray  // [measuredHz, jitterP99Ns]

    /**
     * Configure a scripted stimulus sequence.
     * @param targetDurationMs intended target duration per event
     * @param count number of events
     * @param interEventFrames spacing between events (in locked-refresh frames)
     * @param sessionNonceHex 64-bit session nonce (also embedded in fiducial)
     */
    external fun nativeConfigureSequence(
        targetDurationMs: Double,
        count: Int,
        interEventFrames: Int,
        sessionNonceHex: String
    )

    /** Start rendering the configured sequence. Non-blocking; check [nativeIsRunning]. */
    external fun nativeStart()

    /** Signal the render thread to stop at the next safe boundary. */
    external fun nativeStop()

    external fun nativeIsRunning(): Boolean

    /**
     * Drain any completed [StimulusEventRecord]s from the C++-side ring buffer.
     * Called on a non-critical thread; C++ does no allocation during a running event,
     * so records are staged and drained here after each event completes.
     */
    external fun nativeDrainCompletedEvents(): Array<StimulusEventRecord>

    /**
     * Ingest actual presentation timestamps from Surface.getFrameTimestamps.
     * Correlated by frame number. C++ uses these to close out event records with
     * verification_status.
     */
    external fun nativeIngestPresentTimestamp(
        frameNumber: Long,
        actualPresentNs: Long,
        source: Int  // 0=frame_timeline, 1=egl_pt, 2=vk_display_timing, 3=missing
    )

    external fun nativeShutdown()
}

/**
 * Mirror of C++ StimulusEventRecord. Field names match spec §9.2 stimulus_event
 * so an audit tool can consume the JSONL directly.
 */
data class StimulusEventRecord(
    val eventIndex: Int,
    val fiducialNonceHex: String,
    val targetDurationMs: Double,
    val frameCount: Int,
    val framePeriodNs: Long,
    val achievedDurationMs: Double,
    val scheduledFrameNumber: Long,
    val perFrameIntendedNs: LongArray,
    val perFrameActualNs: LongArray,
    val perFrameSource: IntArray,  // 0..3 per nativeIngestPresentTimestamp
    val timingDeviationNs: Long,
    val refreshHzAtEvent: Double,
    val brightnessAtEvent: Float,
    val verificationStatus: String  // "verified" | "deviation" | "failed" | "missing_pt"
)
