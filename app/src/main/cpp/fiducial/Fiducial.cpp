#include "Fiducial.h"

namespace rsp::fiducial {

static uint8_t ComputeParity(const uint8_t* cells, int n) {
    uint8_t p = 0;
    for (int i = 0; i < n; ++i) p ^= (cells[i] & 1);
    return p;
}

void ComputeCells(uint64_t session_nonce,
                  uint32_t frame_counter,
                  FrameType frame_type,
                  uint8_t out_cells[kCells]) {
    int i = 0;
    // Start bit
    out_cells[i++] = 1;
    // 64-bit nonce, LSB-first
    for (int b = 0; b < 64; ++b) {
        out_cells[i++] = static_cast<uint8_t>((session_nonce >> b) & 1);
    }
    // 32-bit frame counter, LSB-first
    for (int b = 0; b < 32; ++b) {
        out_cells[i++] = static_cast<uint8_t>((frame_counter >> b) & 1);
    }
    // 3-bit frame type (masked into low 3 bits)
    uint8_t t = static_cast<uint8_t>(frame_type) & 0x07;
    for (int b = 0; b < 3; ++b) {
        out_cells[i++] = (t >> b) & 1;
    }
    // Parity over payload cells (indices 1..i-1)
    uint8_t parity = ComputeParity(out_cells + 1, i - 1);
    out_cells[i++] = parity;
    // Stop bit
    out_cells[i++] = 1;
    // Pad remaining cells to 0 (should be nothing; check)
    while (i < kCells) out_cells[i++] = 0;
}

uint64_t ParseNonceHex(const char* hex) {
    if (!hex) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 16; ++i) {
        char c = hex[i];
        if (c == 0) return 0;
        uint64_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

}  // namespace rsp::fiducial
