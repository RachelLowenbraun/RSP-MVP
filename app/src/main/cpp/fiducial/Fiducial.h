// Fiducial.h — the timing fiducial per spec §5.2.4.
//
// A small high-contrast patch drawn in the same frame submission as the target
// stimulus. Encodes:
//   - a 64-bit session nonce (binds a video recording to this session)
//   - a 32-bit frame counter (binds a video frame to a specific rendered frame)
//   - a 4-bit type tag (target / mask / ambient / control)
//
// Encoding: horizontal strip of black/white cells. Cell width `kCellPx`.
// Cell 0 is a start bit (always white). Cells 1..96 encode 96 payload bits
// (nonce 64 + counter 32) LSB-first, black=0 white=1. Cell 97 is a parity bit.
// Cell 98 is a stop bit (always white).
//
// The strip is drawn at a placement `[TBD-CLINICAL]` (bottom-left, outside
// central 30° region). M0 default: 24 px from the bottom, 24 px from the left,
// 100 cells × 8 px wide × 32 px tall.
//
// This is a spec description; actual pixel writes are done by the render path,
// which asks Fiducial to compute the cell colors for a given frame.

#pragma once
#include <cstdint>

namespace rsp::fiducial {

// Layout: 1 start + 64 nonce + 32 counter + 3 type + 1 parity + 1 stop = 102 payload cells.
// Rounded to 104 for a whole 832-pixel strip (kCells * kCellPxWidth = 832).
constexpr int kCells = 104;
constexpr int kPayloadCells = 102;
constexpr int kCellPxWidth = 8;
constexpr int kCellPxHeight = 32;
constexpr int kOffsetLeftPx = 24;
constexpr int kOffsetBottomPx = 24;

enum FrameType : uint8_t {
    FRAME_AMBIENT = 0,
    FRAME_TARGET = 1,
    FRAME_MASK_FORWARD = 2,
    FRAME_MASK_BACKWARD = 3,
    FRAME_CONTROL = 4,
};

/**
 * Compute the 100-cell state (0 or 1 per cell) for a specific frame.
 * @param session_nonce 64-bit nonce (constant within a session)
 * @param frame_counter 32-bit monotonic frame counter
 * @param frame_type    tag identifying what this frame is doing
 * @param out_cells     array of size kCells; caller fills renderer with these values
 */
void ComputeCells(uint64_t session_nonce,
                  uint32_t frame_counter,
                  FrameType frame_type,
                  uint8_t out_cells[kCells]);

/**
 * Convenience: parse a nonce hex string (16 chars = 8 bytes) into uint64_t.
 * Returns 0 on parse error.
 */
uint64_t ParseNonceHex(const char* hex);

}  // namespace rsp::fiducial
