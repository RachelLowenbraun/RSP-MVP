package com.rsp.timing

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.rsp.timing.databinding.ActivityMainBinding

/**
 * Entry point for the M0 timing spike.
 *
 * Lifecycle:
 *   onCreate  → build UI, wait for surface
 *   surfaceCreated → run pre-flight, enable buttons if OK
 *   button press → SessionRunner.start(...)
 *   Stop / activity paused / surface destroyed → SessionRunner.stop() and shutdown
 *
 * Refuse-to-run posture: any pre-flight failure disables all Run buttons and
 * displays the reason. Re-tapping the surface re-runs pre-flight (letting the
 * user turn off Night Shift, unplug an HDMI cable, etc.).
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var gates: PreFlightGates
    private var runner: SessionRunner? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Keep screen on for the whole session.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        gates = PreFlightGates(this)

        binding.stimulusSurface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                doPreFlight()
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                runner?.stop()
                runner?.shutdown()
                runner = null
            }
        })

        binding.btnProbe.setOnClickListener {
            startSession(cfg = SessionRunner.Config(
                targetDurationMs = 0.0,   // 0 = probe-only, no visible stimulus
                count = 0,
                interEventFrames = 0,
                brightness = 0.5f
            ))
        }
        binding.btn33ms.setOnClickListener {
            startSession(cfg = SessionRunner.Config(
                targetDurationMs = 33.0,
                count = 200,
                interEventFrames = 60,      // ~500ms spacing at 120Hz
                brightness = 0.5f
            ))
        }
        binding.btn50ms.setOnClickListener {
            startSession(cfg = SessionRunner.Config(
                targetDurationMs = 50.0,
                count = 200,
                interEventFrames = 60,
                brightness = 0.5f
            ))
        }
        binding.btnStop.setOnClickListener {
            runner?.stop()
        }
    }

    private fun doPreFlight() {
        binding.status.text = getString(R.string.pre_flight_running)
        val res = gates.runAll()
        if (!res.ok) {
            binding.status.text = getString(R.string.fail_reason, res.reasons.joinToString("; "))
            enableRunButtons(false)
            return
        }

        // Lock refresh (raises reasonable-max) and brightness (fixed 0.5 for M0).
        val hz = gates.lockRefreshRate()
        gates.lockBrightness(0.5f)

        binding.status.text = getString(R.string.pre_flight_ok) +
            " (locked ${hz?.toInt() ?: "?"} Hz)"
        enableRunButtons(true)
    }

    private fun enableRunButtons(enable: Boolean) {
        binding.btnProbe.isEnabled = enable
        binding.btn33ms.isEnabled = enable
        binding.btn50ms.isEnabled = enable
        binding.btnStop.isEnabled = false
    }

    private fun startSession(cfg: SessionRunner.Config) {
        val surface = binding.stimulusSurface.holder.surface
        if (surface == null || !surface.isValid) {
            binding.status.text = getString(R.string.fail_reason, "surface_not_ready")
            return
        }
        enableRunButtons(false)
        binding.btnStop.isEnabled = true

        val r = SessionRunner(this, surface, useVulkan = true)
        runner = r
        r.start(lifecycleScope, cfg, object : SessionRunner.Listener {
            override fun onSessionStarted(logPath: String) {
                binding.status.text = "Running…"
                binding.logPath.text = getString(R.string.log_path_hint, logPath)
            }
            override fun onEventLogged(record: StimulusEventRecord, running: Int, total: Int) {
                binding.status.text = "Event $running / $total  status=${record.verificationStatus}  dev=${record.timingDeviationNs / 1000}μs"
            }
            override fun onSessionEnded(reason: String, totalEvents: Int, verifiedEvents: Int, logPath: String) {
                binding.status.text = "Done. reason=$reason  events=$totalEvents  verified=$verifiedEvents"
                binding.logPath.text = getString(R.string.log_path_hint, logPath)
                enableRunButtons(true)
                binding.btnStop.isEnabled = false
            }
            override fun onError(reason: String) {
                binding.status.text = getString(R.string.fail_reason, reason)
                enableRunButtons(true)
                binding.btnStop.isEnabled = false
            }
        })
    }

    override fun onPause() {
        super.onPause()
        // Spec §13.3 — phone backgrounded == immediate abort of active block.
        runner?.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        runner?.shutdown()
        runner = null
        gates.restoreBrightness()
    }
}
