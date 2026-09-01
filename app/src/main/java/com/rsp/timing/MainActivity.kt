package com.rsp.timing

import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Choreographer
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : AppCompatActivity() {

    private lateinit var statusText: TextView
    private lateinit var refreshText: TextView
    private lateinit var eventText: TextView
    private lateinit var runButton: Button
    private lateinit var stimulusView: View

    private var isRunning = false
    private var frameCount = 0
    private var lastFrameTimeNs = 0L
    private val frameTimings = mutableListOf<Long>()
    private val events = mutableListOf<TimingEvent>()

    data class TimingEvent(
        val index: Int,
        val timestampNs: Long,
        val durationMs: Double,
        val verified: Boolean
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Keep screen on during testing
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        statusText = findViewById(R.id.status_text)
        refreshText = findViewById(R.id.refresh_text)
        eventText = findViewById(R.id.event_text)
        runButton = findViewById(R.id.run_button)
        stimulusView = findViewById(R.id.stimulus_view)

        // Get display refresh rate
        val display = windowManager.defaultDisplay
        val refreshRate = display.refreshRate
        refreshText.text = "Refresh: %.1f Hz".format(refreshRate)

        statusText.text = "Status: IDLE"
        eventText.text = "Events: 0"

        runButton.setOnClickListener {
            if (!isRunning) startTimingTest()
        }
    }

    private fun startTimingTest() {
        isRunning = true
        frameCount = 0
        frameTimings.clear()
        events.clear()

        statusText.text = "Status: RUNNING"
        runButton.isEnabled = false
        stimulusView.visibility = View.VISIBLE
        stimulusView.setBackgroundColor(Color.WHITE)

        val startTime = System.nanoTime()
        val targetEvents = 20
        val targetDurationMs = 33.3 // ~2 frames at 60Hz

        val choreographer = Choreographer.getInstance()
        var eventStartTime = System.nanoTime()

        val frameCallback = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (!isRunning) return

                // Alternate stimulus visibility for timing events
                val eventIndex = events.size
                if (eventIndex >= targetEvents) {
                    completeTest()
                    return
                }

                // Toggle color every ~2 frames (33ms at 60Hz)
                val elapsedMs = (frameTimeNanos - eventStartTime) / 1_000_000.0

                if (elapsedMs >= targetDurationMs) {
                    // Record event
                    val verified = kotlin.math.abs(elapsedMs - targetDurationMs) < 5.0
                    events.add(TimingEvent(eventIndex, frameTimeNanos, elapsedMs, verified))

                    // Toggle color
                    if (eventIndex % 2 == 0) {
                        stimulusView.setBackgroundColor(Color.BLACK)
                    } else {
                        stimulusView.setBackgroundColor(Color.WHITE)
                    }

                    eventStartTime = frameTimeNanos

                    runOnUiThread {
                        eventText.text = "Events: ${events.size}/$targetEvents"
                    }
                }

                frameCount++
                choreographer.postFrameCallback(this)
            }
        }

        choreographer.postFrameCallback(frameCallback)
    }

    private fun completeTest() {
        isRunning = false
        stimulusView.visibility = View.GONE

        val verifiedCount = events.count { it.verified }
        val totalCount = events.size

        statusText.text = "Status: COMPLETED ($totalCount events)"
        eventText.text = "Verified: $verifiedCount/$totalCount"
        runButton.isEnabled = true

        // Log results
        val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        android.util.Log.i("RSP_TIMING", "Session complete: $verifiedCount/$totalCount verified")

        events.forEachIndexed { i, event ->
            android.util.Log.i("RSP_TIMING",
                "Event $i: duration=${"%.2f".format(event.durationMs)}ms verified=${event.verified}")
        }
    }
}
