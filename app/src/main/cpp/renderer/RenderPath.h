// RenderPath.h — abstract interface for a stimulus render path.
//
// The M0 spike has two concrete implementations:
//   VulkanRenderer  — primary, uses VK_GOOGLE_display_timing
//   GlesRenderer    — fallback, uses EGL_ANDROID_presentation_time + ANativeWindow_getFrameTimestamps
//
// Contract:
//   Init()                       — one-shot; queries surface caps, verifies extensions
//   PreflightOK()                — returns true if the surface + extensions meet clinical criteria
//   SetSurface(window)           — bind the ANativeWindow
//   ProbeRefresh(N)              — measure inter-frame timing over N frames; returns Hz + jitter
//   ConfigureSequence(...)       — set the parameters of the scripted sequence
//   Start()                      — non-blocking; spawns render thread if not already
//   Stop()                       — signals render thread to unwind at next safe boundary
//   IsRunning()                  — thread-safe
//   DrainEvents(out, max)        — copy completed event records out of the ring
//
// Everything below the JNI is single-threaded per render path except the ring buffer
// in TimingLog which is SPSC.

#pragma once
#include <cstdint>
#include <atomic>
#include "../timing/TimingLog.h"
#include "../fiducial/Fiducial.h"

struct ANativeWindow;

namespace rsp::render {

struct RefreshProbe {
    double measured_hz = 0.0;
    int64_t jitter_p99_ns = 0;
    int64_t frame_period_ns_median = 0;
};

struct SequenceConfig {
    double target_duration_ms = 0.0;
    int count = 0;
    int inter_event_frames = 60;
    uint64_t session_nonce = 0;
    float brightness_at_event = 0.5f;  // recorded, not commanded (window brightness set in Kotlin)
    // Tolerance applied to per-event verification (spec §5.2.2 default 2ms).
    double tolerance_ms = 2.0;
};

class RenderPath {
public:
    virtual ~RenderPath() = default;

    virtual int Init() = 0;                                    // returns 0 on success
    virtual bool PreflightOK() = 0;                            // extensions + display caps
    virtual void SetSurface(ANativeWindow* window) = 0;
    virtual RefreshProbe ProbeRefresh(int sample_frames) = 0;

    virtual void ConfigureSequence(const SequenceConfig& cfg) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;

    virtual int DrainEvents(timing::StimulusEventRecord* out, int max) = 0;

    virtual void Shutdown() = 0;
};

// Factory
RenderPath* CreateVulkanRenderer();
RenderPath* CreateGlesRenderer();

}  // namespace rsp::render
