#version 450
// Push-constants: 3 uint32 encoding 100 fiducial cells (cell i => bit i)
// plus target/ambient intensity and viewport size for fiducial pixel positioning.
layout(push_constant) uniform PC {
    uint  cells0;         // cells 0..31   (bit 0 = cell 0)
    uint  cells1;         // cells 32..63
    uint  cells2;         // cells 64..99  (bits 64..99 => positions 0..35; only 0..35 used)
    uint  cells3;         // reserved/pad
    float target_intensity;   // 0.0..1.0
    float ambient_intensity;  // 0.0..1.0
    float viewport_w;
    float viewport_h;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// Fiducial placement: bottom-left, offset (24, 24) px from bottom-left corner,
// 100 cells * 8 px wide, 32 px tall.
const float kCellPxW = 8.0;
const float kCellPxH = 32.0;
const float kOffsetLeftPx = 24.0;
const float kOffsetBottomPx = 24.0;
const int kCells = 104;

void main() {
    // Screen-pixel coordinates. v_uv.y=0 is top, v_uv.y=1 is bottom for our setup.
    float px = v_uv.x * pc.viewport_w;
    float py_from_top = v_uv.y * pc.viewport_h;
    float py_from_bottom = pc.viewport_h - py_from_top;

    float strip_x0 = kOffsetLeftPx;
    float strip_x1 = kOffsetLeftPx + float(kCells) * kCellPxW;
    float strip_y0 = kOffsetBottomPx;              // from bottom
    float strip_y1 = kOffsetBottomPx + kCellPxH;   // from bottom

    // Default: full-field target/ambient depending on target_intensity.
    // Grayscale — R=G=B=intensity, A=1.
    float base_intensity = mix(pc.ambient_intensity, pc.target_intensity, pc.target_intensity);
    // Actually the mix() above is a no-op-ish because target_intensity is the selector.
    // Fix: use target_intensity when it's > ambient (a target frame), else ambient.
    // Simpler and unambiguous — the CPU sets target_intensity to the right value per frame.
    base_intensity = pc.target_intensity;

    vec3 color = vec3(base_intensity);

    // Are we inside the fiducial strip?
    if (px >= strip_x0 && px < strip_x1 &&
        py_from_bottom >= strip_y0 && py_from_bottom < strip_y1) {

        int cell_index = int(floor((px - strip_x0) / kCellPxW));
        // Clamp
        if (cell_index >= 0 && cell_index < kCells) {
            uint bit;
            if (cell_index < 32) {
                bit = (pc.cells0 >> uint(cell_index)) & 1u;
            } else if (cell_index < 64) {
                bit = (pc.cells1 >> uint(cell_index - 32)) & 1u;
            } else if (cell_index < 96) {
                bit = (pc.cells2 >> uint(cell_index - 64)) & 1u;
            } else {
                bit = (pc.cells3 >> uint(cell_index - 96)) & 1u;
            }
            // High contrast for the fiducial regardless of ambient/target.
            color = (bit == 1u) ? vec3(1.0) : vec3(0.0);
        }
    }

    fragColor = vec4(color, 1.0);
}
