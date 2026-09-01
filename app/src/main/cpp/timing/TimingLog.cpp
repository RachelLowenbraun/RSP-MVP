#include "TimingLog.h"
#include <algorithm>

namespace rsp::timing {

void TimingLog::PushCompletedEvent(const StimulusEventRecord& rec) {
    // Single-producer / single-consumer ring. Producer (render thread) advances head_;
    // consumer (drain thread) advances tail_. Both loads/stores are relaxed except
    // the head store, which is release so the consumer sees the record fully.
    uint32_t head = head_.load(std::memory_order_relaxed);
    uint32_t tail = tail_.load(std::memory_order_acquire);
    uint32_t next = (head + 1) % kRingCapacity;
    if (next == tail) {
        // Ring full — overwrite oldest. Advance tail so consumer sees a fresh window.
        tail_.store((tail + 1) % kRingCapacity, std::memory_order_release);
        records_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    ring_[head] = rec;
    head_.store(next, std::memory_order_release);
}

int TimingLog::Drain(StimulusEventRecord* out, int max_out) {
    uint32_t head = head_.load(std::memory_order_acquire);
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    int copied = 0;
    while (tail != head && copied < max_out) {
        out[copied++] = ring_[tail];
        tail = (tail + 1) % kRingCapacity;
    }
    tail_.store(tail, std::memory_order_release);
    return copied;
}

bool TimingLog::BindPresentTimestamp(int64_t frame_number, int64_t actual_present_ns, int source) {
    // Bind to the currently-building event only. In the M0 spike we don't retain
    // multiple in-flight events; events are strictly sequential with inter-event gaps.
    auto& b = building_;
    if (b.frame_count == 0) return false;
    int64_t base = b.scheduled_frame_number;
    int idx = static_cast<int>(frame_number - base);
    if (idx < 0 || idx >= b.frame_count) return false;
    b.per_frame_actual_ns[idx] = actual_present_ns;
    b.per_frame_source[idx] = source;
    return true;
}

}  // namespace rsp::timing
