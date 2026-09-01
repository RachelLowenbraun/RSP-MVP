// FrameScheduler.h — deterministic frame-count math for the timing spike.
//
// This is the heart of the M0 spike. It computes:
//   - frame_count = round(target_ms / frame_period_ms)  [spec §5.2.1]
//   - achieved_duration_ms = frame_count * frame_period_ms
//   - the deviation from target
//   - the intended presentation time for each frame of a stimulus event,
//     scheduled to begin at a future frame boundary at least 2 frames ahead of
//     "now" in vsync-native time.
//
// It does NOT touch any graphics API. Every dependency here is arithmetic.
// This makes the file 100% unit-testable and also 100% shared with the iOS side
// (via a KMP-friendly pure-C++ port if we go that route — but we won't per
// Redline Patch 8; we duplicate + conformance-test the renderer, and shared code
// stays in this arithmetic layer only).
//
// Timekeeping unit: nanoseconds throughout. int64_t everywhere. No floats in the
// scheduling path. Floats appear only when a `double` return is genuinely
// user-facing (achieved_duration_ms, refresh_hz).

#pragma once
#include <cstdint>
#include <cmath>

namespace rsp::timing {

struct FramePlan {
    // Number of frames the stimulus target is presented for.
    int frame_count = 0;
    // Frame period at the locked refresh rate.
    int64_t frame_period_ns = 0;
    // Theoretical achieved duration (frame_count * frame_period).
    double achieved_duration_ms = 0.0;
    // How far achieved is from target. Signed.
    double deviation_from_target_ms = 0.0;
    // Whether this plan is within the tolerance for a given target.
    bool within_tolerance = false;

    // Presentation-time budget for each frame (intended_present_ns per frame,
    // frame index 0..frame_count-1). The first entry is the intended present
    // time of the first target frame; each successive entry is +frame_period_ns.
    // Populated by `Schedule()`.
    int64_t intended_present_ns[16] = {0};   // supports up to 16-frame events
    int64_t scheduled_frame_number = 0;      // frame number of the first frame
};

class FrameScheduler {
public:
    // Compute the frame plan for a target duration, given the locked frame period.
    // Does NOT populate presentation times — call Schedule() with a reference clock
    // for that.
    static FramePlan Plan(double target_ms, int64_t frame_period_ns, double tolerance_ms) {
        FramePlan p{};
        p.frame_period_ns = frame_period_ns;
        if (frame_period_ns <= 0 || target_ms < 0) {
            return p;  // invalid — frame_count=0
        }
        double frame_period_ms = frame_period_ns / 1'000'000.0;
        // spec §5.2.1: round to nearest whole frame
        p.frame_count = static_cast<int>(std::llround(target_ms / frame_period_ms));
        if (p.frame_count < 1) p.frame_count = 1;
        if (p.frame_count > 16) p.frame_count = 16;  // spike cap
        p.achieved_duration_ms = p.frame_count * frame_period_ms;
        p.deviation_from_target_ms = p.achieved_duration_ms - target_ms;
        p.within_tolerance = std::abs(p.deviation_from_target_ms) <= tolerance_ms;
        return p;
    }

    // Populate p.intended_present_ns[] and p.scheduled_frame_number given a
    // starting future frame boundary (at least 2 frames ahead of "now") and its
    // corresponding present time in the display clock's nanosecond domain.
    //
    // begin_frame_number: display frame counter value at which the first target
    //                     frame should be presented.
    // begin_present_ns:   the display clock's intended present time for that
    //                     frame number.
    static void Schedule(FramePlan& p, int64_t begin_frame_number, int64_t begin_present_ns) {
        p.scheduled_frame_number = begin_frame_number;
        for (int i = 0; i < p.frame_count; ++i) {
            p.intended_present_ns[i] = begin_present_ns + int64_t(i) * p.frame_period_ns;
        }
    }
};

}  // namespace rsp::timing
