// TimingLog.h — the in-flight event record buffer.
//
// The render thread writes completed events into this ring; the Kotlin drain
// thread reads them out and hands them to EvidenceLog for JSONL serialization.
// Reader and writer are on different threads and use lock-free primitives.
//
// Layout: fixed-capacity ring of StimulusEventRecord. Head advanced by writer
// on event completion; tail advanced by reader when Drain() consumes.
//
// The writer never blocks. If the ring is full (reader has fallen behind), the
// oldest unread record is overwritten and a "records_dropped" counter is
// incremented — this counter is surfaced in the session footer so we know if
// the drain thread was too slow. For M0 with drain every 100 ms and events
// ~1/second, this should never fire.

#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace rsp::timing {

constexpr int kMaxFramesPerEvent = 16;
constexpr int kRingCapacity = 256;

// One completed stimulus event. Mirrors the Kotlin StimulusEventRecord.
struct StimulusEventRecord {
    int event_index = -1;
    char fiducial_nonce_hex[17] = {0};   // 8 bytes hex + null
    double target_duration_ms = 0.0;
    int frame_count = 0;
    int64_t frame_period_ns = 0;
    double achieved_duration_ms = 0.0;
    int64_t scheduled_frame_number = 0;
    int64_t per_frame_intended_ns[kMaxFramesPerEvent] = {0};
    int64_t per_frame_actual_ns[kMaxFramesPerEvent] = {0};
    int per_frame_source[kMaxFramesPerEvent] = {3};   // default MISSING
    int64_t timing_deviation_ns = 0;
    double refresh_hz_at_event = 0.0;
    float brightness_at_event = 0.0f;
    // "verified" | "deviation" | "failed" | "missing_pt"
    char verification_status[16] = "unknown";
};

class TimingLog {
public:
    // Called from render thread when an event completes. Non-blocking.
    void PushCompletedEvent(const StimulusEventRecord& rec);

    // Called from drain thread. Copies out up to `max_out` records; returns count.
    int Drain(StimulusEventRecord* out, int max_out);

    // Called from render thread when an ingested presentation timestamp arrives
    // for a still-in-flight event's frame. Finds the event by scheduled_frame_number
    // range and sets per_frame_actual_ns/source. Returns true if bound to an event.
    // For M0 the render thread pulls presentation timestamps itself via
    // vkGetPastPresentationTimingGOOGLE (Vulkan) or ANativeWindow_getFrameTimestamps
    // (GLES) — this method exists to also accept ingest from Kotlin as a fallback.
    bool BindPresentTimestamp(int64_t frame_number, int64_t actual_present_ns, int source);

    // Set on the currently-building event. Called by the render thread between frames.
    // Only usable before PushCompletedEvent for this event.
    void SetBuildingEvent(const StimulusEventRecord& rec) { building_ = rec; }
    StimulusEventRecord& Building() { return building_; }

    int RecordsDropped() const { return records_dropped_.load(); }

private:
    StimulusEventRecord ring_[kRingCapacity];
    std::atomic<uint32_t> head_{0};  // next write index
    std::atomic<uint32_t> tail_{0};  // next read index
    std::atomic<int> records_dropped_{0};

    // Event currently being built by the render thread. Not thread-shared.
    StimulusEventRecord building_{};
};

}  // namespace rsp::timing
