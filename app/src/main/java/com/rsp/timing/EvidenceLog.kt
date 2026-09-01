package com.rsp.timing

import android.content.Context
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

/**
 * JSONL evidence log for the M0 spike.
 *
 * Field names match spec §9.2 stimulus_event so an audit consumer can process this
 * without translation. First line is a session-header record with device / display
 * probe metadata; each subsequent line is one stimulus event.
 *
 * NOT SIGNED. The production evidence log adds a device-held-keypair signature and a
 * hash chain (§14.3). Adding those to M0 is 100 lines of scaffolding that doesn't
 * exercise the timing question this spike is meant to answer.
 */
class EvidenceLog(context: Context) {

    private val file: File
    private val sessionId: String = UUID.randomUUID().toString()
    private val sessionNonceHex: String
    private val out: FileOutputStream

    init {
        // Files dir is scoped to the app; adb pull is straightforward.
        val dir = File(context.getExternalFilesDir(null), "logs").apply { mkdirs() }
        val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        file = File(dir, "m0-session-$ts.jsonl")
        out = FileOutputStream(file)
        sessionNonceHex = generateNonce()
    }

    val path: String get() = file.absolutePath
    val nonce: String get() = sessionNonceHex

    fun writeHeader(
        targetDurationMs: Double,
        eventCount: Int,
        measuredHz: Double,
        jitterP99Ns: Long,
        lockedHz: Float,
        brightness: Float
    ) {
        val row = JSONObject().apply {
            put("kind", "session_header")
            put("session_id", sessionId)
            put("session_nonce", sessionNonceHex)
            put("device_model", Build.MODEL)
            put("device_manufacturer", Build.MANUFACTURER)
            put("os_version", Build.VERSION.RELEASE)
            put("api_level", Build.VERSION.SDK_INT)
            put("target_duration_ms", targetDurationMs)
            put("event_count", eventCount)
            put("locked_hz", lockedHz.toDouble())
            put("measured_hz", measuredHz)
            put("jitter_p99_ns", jitterP99Ns)
            put("brightness", brightness.toDouble())
            put("timestamp_ms", System.currentTimeMillis())
            put("spec_version", "1.0")
            put("m0_build", "0.1-m0")
        }
        writeln(row.toString())
    }

    fun writeEvent(rec: StimulusEventRecord) {
        val row = JSONObject().apply {
            put("kind", "stimulus_event")
            put("session_id", sessionId)
            put("event_index", rec.eventIndex)
            put("fiducial_nonce", rec.fiducialNonceHex)
            put("target_duration_ms", rec.targetDurationMs)
            put("frame_count", rec.frameCount)
            put("frame_period_ns", rec.framePeriodNs)
            put("achieved_duration_ms", rec.achievedDurationMs)
            put("scheduled_frame_number", rec.scheduledFrameNumber)
            put("intended_present_ns", rec.perFrameIntendedNs.toJsonArray())
            put("actual_present_ns", rec.perFrameActualNs.toJsonArray())
            put("present_source_per_frame", rec.perFrameSource.toJsonArray())
            put("timing_deviation_ns", rec.timingDeviationNs)
            put("refresh_hz_at_event", rec.refreshHzAtEvent)
            put("brightness_at_event", rec.brightnessAtEvent.toDouble())
            put("verification_status", rec.verificationStatus)
        }
        writeln(row.toString())
    }

    fun writeFooter(endReason: String, totalEvents: Int, verifiedEvents: Int) {
        val row = JSONObject().apply {
            put("kind", "session_footer")
            put("session_id", sessionId)
            put("end_reason", endReason)
            put("total_events", totalEvents)
            put("verified_events", verifiedEvents)
            put("timestamp_ms", System.currentTimeMillis())
        }
        writeln(row.toString())
    }

    private fun writeln(s: String) {
        out.write(s.toByteArray(Charsets.UTF_8))
        out.write("\n".toByteArray())
    }

    fun close() {
        out.flush()
        out.close()
    }

    private fun generateNonce(): String {
        val bytes = ByteArray(8)
        java.security.SecureRandom().nextBytes(bytes)
        return bytes.joinToString("") { "%02x".format(it) }
    }

    private fun LongArray.toJsonArray(): JSONArray {
        val arr = JSONArray()
        forEach { arr.put(it) }
        return arr
    }

    private fun IntArray.toJsonArray(): JSONArray {
        val arr = JSONArray()
        forEach { arr.put(it) }
        return arr
    }
}
