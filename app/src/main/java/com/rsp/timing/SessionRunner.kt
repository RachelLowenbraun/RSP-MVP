package com.rsp.timing

import android.content.Context
import android.util.Log
import android.view.Surface
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Drives the C++ render thread through a scripted sequence and drains
 * completed [StimulusEventRecord]s into the evidence log.
 *
 * This class is deliberately thin. It does NOT participate in per-frame execution
 * (that's entirely in C++). It only:
 *   1. Configures the native sequence.
 *   2. Starts it.
 *   3. Polls native for completed events on a background dispatcher.
 *   4. Writes each event to the JSONL evidence log.
 *   5. Handles user-initiated stop.
 */
class SessionRunner(
    private val context: Context,
    private val surface: Surface,
    private val useVulkan: Boolean = true
) {
    companion object {
        private const val TAG = "SessionRunner"
        private const val DRAIN_INTERVAL_MS = 100L
    }

    private var job: Job? = null
    private var log: EvidenceLog? = null

    data class Config(
        val targetDurationMs: Double,
        val count: Int,
        val interEventFrames: Int,
        val brightness: Float
    )

    interface Listener {
        fun onSessionStarted(logPath: String)
        fun onEventLogged(record: StimulusEventRecord, running: Int, total: Int)
        fun onSessionEnded(reason: String, totalEvents: Int, verifiedEvents: Int, logPath: String)
        fun onError(reason: String)
    }

    /**
     * Runs a scripted sequence to completion or until [stop] is called.
     * Blocks the coroutine but not the caller thread.
     */
    fun start(scope: CoroutineScope, cfg: Config, listener: Listener) {
        job?.cancel()
        job = scope.launch(Dispatchers.Default) {
            try {
                // 1. Init native engine with the chosen render path.
                val initRc = NativeBridge.nativeInit(useVulkan)
                if (initRc != 0) {
                    listener.onError("native_init_failed_rc_$initRc")
                    return@launch
                }

                // 2. Bind surface.
                NativeBridge.nativeSetSurface(surface)

                // 3. Refresh probe.
                val probe = NativeBridge.nativeProbeRefresh(300)
                val measuredHz = probe[0]
                val jitterP99Ns = probe[1].toLong()
                Log.i(TAG, "Refresh probe: hz=$measuredHz jitter_p99=$jitterP99Ns")

                // 4. Prepare log; write header.
                val evidence = EvidenceLog(context)
                log = evidence
                evidence.writeHeader(
                    targetDurationMs = cfg.targetDurationMs,
                    eventCount = cfg.count,
                    measuredHz = measuredHz,
                    jitterP99Ns = jitterP99Ns,
                    lockedHz = measuredHz.toFloat(),  // in production, compare vs requested
                    brightness = cfg.brightness
                )
                withContext(Dispatchers.Main) {
                    listener.onSessionStarted(evidence.path)
                }

                // 5. Configure and start the native sequence.
                NativeBridge.nativeConfigureSequence(
                    targetDurationMs = cfg.targetDurationMs,
                    count = cfg.count,
                    interEventFrames = cfg.interEventFrames,
                    sessionNonceHex = evidence.nonce
                )
                NativeBridge.nativeStart()

                // 6. Drain loop.
                var loggedCount = 0
                var verifiedCount = 0
                while (NativeBridge.nativeIsRunning()) {
                    delay(DRAIN_INTERVAL_MS)
                    val batch = NativeBridge.nativeDrainCompletedEvents()
                    for (rec in batch) {
                        evidence.writeEvent(rec)
                        loggedCount++
                        if (rec.verificationStatus == "verified") verifiedCount++
                        withContext(Dispatchers.Main) {
                            listener.onEventLogged(rec, loggedCount, cfg.count)
                        }
                    }
                }

                // 7. Final drain after the run ends (may have residual records).
                val tail = NativeBridge.nativeDrainCompletedEvents()
                for (rec in tail) {
                    evidence.writeEvent(rec)
                    loggedCount++
                    if (rec.verificationStatus == "verified") verifiedCount++
                }

                evidence.writeFooter("completed", loggedCount, verifiedCount)
                evidence.close()
                withContext(Dispatchers.Main) {
                    listener.onSessionEnded("completed", loggedCount, verifiedCount, evidence.path)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "Session error", t)
                log?.let {
                    it.writeFooter("system_error", -1, -1)
                    it.close()
                }
                withContext(Dispatchers.Main) {
                    listener.onError(t.message ?: "unknown")
                }
            }
        }
    }

    fun stop() {
        NativeBridge.nativeStop()
    }

    fun shutdown() {
        job?.cancel()
        try { NativeBridge.nativeShutdown() } catch (_: Throwable) {}
        try { log?.close() } catch (_: Throwable) {}
    }
}
