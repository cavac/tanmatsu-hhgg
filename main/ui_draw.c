// UI Drawing primitives for LCARS-style interface
// Handles 270-degree display rotation automatically

#include "ui_draw.h"
#include <string.h>

// Set a single pixel with 270-degree rotation handling
// The display is rotated 270 degrees, so:
// - Screen X (0-799) maps to buffer Y (row)
// - Screen Y (0-479) maps to buffer X (column), inverted
// fb_stride is in PIXELS (not bytes)
void ui_set_pixel(uint8_t* fb, int fb_stride, int fb_height,
                  int screen_x, int screen_y, uint32_t color) {
    // Convert screen coordinates to buffer coordinates
    // For 270-degree rotation: buf_x = fb_stride - 1 - screen_y, buf_y = screen_x
    int buf_x = fb_stride - 1 - screen_y;
    int buf_y = screen_x;

    // Bounds check
    if (buf_x < 0 || buf_x >= fb_stride || buf_y < 0 || buf_y >= fb_height) {
        return;
    }

    // Write pixel in BGR format (stride * 3 for bytes per row)
    int idx = buf_y * fb_stride * 3 + buf_x * 3;
    fb[idx + 0] = COLOR_B(color);  // Blue
    fb[idx + 1] = COLOR_G(color);  // Green
    fb[idx + 2] = COLOR_R(color);  // Red
}

// Draw a horizontal line (fast path using rotation-aware memset-like approach)
void ui_draw_hline(uint8_t* fb, int fb_stride, int fb_height,
                   int x, int y, int len, uint32_t color) {
    for (int i = 0; i < len; i++) {
        ui_set_pixel(fb, fb_stride, fb_height, x + i, y, color);
    }
}

// Draw a vertical line
void ui_draw_vline(uint8_t* fb, int fb_stride, int fb_height,
                   int x, int y, int len, uint32_t color) {
    for (int i = 0; i < len; i++) {
        ui_set_pixel(fb, fb_stride, fb_height, x, y + i, color);
    }
}

// Draw a filled rectangle
void ui_fill_rect(uint8_t* fb, int fb_stride, int fb_height,
                  int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        ui_draw_hline(fb, fb_stride, fb_height, x, y + dy, w, color);
    }
}

// Draw a rectangle outline
void ui_draw_rect(uint8_t* fb, int fb_stride, int fb_height,
                  int x, int y, int w, int h, uint32_t color) {
    ui_draw_hline(fb, fb_stride, fb_height, x, y, w, color);           // Top
    ui_draw_hline(fb, fb_stride, fb_height, x, y + h - 1, w, color);   // Bottom
    ui_draw_vline(fb, fb_stride, fb_height, x, y, h, color);           // Left
    ui_draw_vline(fb, fb_stride, fb_height, x + w - 1, y, h, color);   // Right
}

// Helper: Draw a filled circle quadrant for rounded corners
static void fill_circle_quadrant(uint8_t* fb, int fb_stride, int fb_height,
                                  int cx, int cy, int r,
                                  int quadrant, uint32_t color) {
    // quadrant: 0=top-left, 1=top-right, 2=bottom-right, 3=bottom-left
    for (int dy = 0; dy <= r; dy++) {
        for (int dx = 0; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                int px, py;
                switch (quadrant) {
                    case 0: px = cx - dx; py = cy - dy; break;  // top-left
                    case 1: px = cx + dx; py = cy - dy; break;  // top-right
                    case 2: px = cx + dx; py = cy + dy; break;  // bottom-right
                    case 3: px = cx - dx; py = cy + dy; break;  // bottom-left
                    default: return;
                }
                ui_set_pixel(fb, fb_stride, fb_height, px, py, color);
            }
        }
    }
}

// Draw a filled rounded rectangle
void ui_fill_rounded_rect(uint8_t* fb, int fb_stride, int fb_height,
                          int x, int y, int w, int h, int radius, uint32_t color) {
    // Clamp radius to half of the smaller dimension
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    // Draw the main body (three rectangles)
    // Center rectangle (full width, reduced height)
    ui_fill_rect(fb, fb_stride, fb_height, x, y + radius, w, h - 2 * radius, color);
    // Top rectangle (reduced width)
    ui_fill_rect(fb, fb_stride, fb_height, x + radius, y, w - 2 * radius, radius, color);
    // Bottom rectangle (reduced width)
    ui_fill_rect(fb, fb_stride, fb_height, x + radius, y + h - radius, w - 2 * radius, radius, color);

    // Draw the four corners
    fill_circle_quadrant(fb, fb_stride, fb_height, x + radius, y + radius, radius, 0, color);           // top-left
    fill_circle_quadrant(fb, fb_stride, fb_height, x + w - 1 - radius, y + radius, radius, 1, color);   // top-right
    fill_circle_quadrant(fb, fb_stride, fb_height, x + w - 1 - radius, y + h - 1 - radius, radius, 2, color);  // bottom-right
    fill_circle_quadrant(fb, fb_stride, fb_height, x + radius, y + h - 1 - radius, radius, 3, color);   // bottom-left
}

// Draw an LCARS-style bar (rounded on left, straight on right)
void ui_draw_lcars_bar(uint8_t* fb, int fb_stride, int fb_height,
                       int x, int y, int w, int h, uint32_t color) {
    int radius = h / 2;
    if (radius > w / 2) radius = w / 2;

    // Main rectangle (excluding left rounded part)
    ui_fill_rect(fb, fb_stride, fb_height, x + radius, y, w - radius, h, color);

    // Left rounded end (semi-circle)
    fill_circle_quadrant(fb, fb_stride, fb_height, x + radius, y + radius, radius, 0, color);  // top-left
    fill_circle_quadrant(fb, fb_stride, fb_height, x + radius, y + h - 1 - radius, radius, 3, color);  // bottom-left

    // Fill the gap between the two quadrants
    ui_fill_rect(fb, fb_stride, fb_height, x, y + radius, radius, h - 2 * radius, color);
}

// Draw an LCARS-style elbow (corner piece)
void ui_draw_lcars_elbow(uint8_t* fb, int fb_stride, int fb_height,
                         int x, int y, int w, int h, int corner_radius,
                         bool top_left, uint32_t color) {
    if (top_left) {
        // Top-left elbow: vertical bar on left, horizontal on top, rounded outer corner
        int bar_width = corner_radius / 2;
        if (bar_width < 10) bar_width = 10;

        // Vertical bar
        ui_fill_rect(fb, fb_stride, fb_height, x, y, bar_width, h, color);
        // Horizontal bar
        ui_fill_rect(fb, fb_stride, fb_height, x, y, w, bar_width, color);
        // Outer corner fill
        fill_circle_quadrant(fb, fb_stride, fb_height, x + bar_width + corner_radius,
                            y + bar_width + corner_radius, corner_radius, 0, color);
    } else {
        // Other elbow orientations can be added as needed
        // For now, just draw a simple corner
        int bar_width = corner_radius / 2;
        if (bar_width < 10) bar_width = 10;
        ui_fill_rect(fb, fb_stride, fb_height, x, y, bar_width, h, color);
        ui_fill_rect(fb, fb_stride, fb_height, x, y + h - bar_width, w, bar_width, color);
    }
}

// Clear screen to a color
// fb_stride is in PIXELS (not bytes)
void ui_clear(uint8_t* fb, int fb_stride, int fb_height, uint32_t color) {
    uint8_t r = COLOR_R(color);
    uint8_t g = COLOR_G(color);
    uint8_t b = COLOR_B(color);

    // Total buffer size in bytes
    size_t buffer_size = fb_stride * fb_height * 3;

    // For black, use fast memset
    if (color == 0) {
        memset(fb, 0, buffer_size);
        return;
    }

    // For other colors, fill pixel by pixel (BGR order)
    for (int row = 0; row < fb_height; row++) {
        uint8_t* row_ptr = fb + row * fb_stride * 3;
        for (int col = 0; col < fb_stride; col++) {
            row_ptr[col * 3 + 0] = b;
            row_ptr[col * 3 + 1] = g;
            row_ptr[col * 3 + 2] = r;
        }
    }
}
