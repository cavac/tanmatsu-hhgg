// UI Drawing primitives for LCARS-style interface
// Handles 270-degree display rotation automatically

#pragma once

#include <stdint.h>
#include <stdbool.h>

// LCARS Color palette (RGB888 format)
#define COLOR_BG        0x000000  // Black background
#define COLOR_ACCENT1   0xFF9900  // Orange (LCARS primary)
#define COLOR_ACCENT2   0xCC6699  // Mauve/pink
#define COLOR_ACCENT3   0x9999FF  // Periwinkle blue
#define COLOR_ACCENT4   0x99CCFF  // Light blue
#define COLOR_SELECTED  0xFFCC00  // Bright yellow (highlight)
#define COLOR_TEXT      0xFFFFFF  // White text
#define COLOR_DIM       0x666666  // Dimmed text

// Extract RGB components from 24-bit color
#define COLOR_R(c) (((c) >> 16) & 0xFF)
#define COLOR_G(c) (((c) >> 8) & 0xFF)
#define COLOR_B(c) ((c) & 0xFF)

// Set a single pixel (handles 270-degree rotation)
// screen_x, screen_y are in screen coordinates (0-799, 0-479)
void ui_set_pixel(uint8_t* fb, int fb_stride, int fb_height,
                  int screen_x, int screen_y, uint32_t color);

// Draw a filled rectangle
void ui_fill_rect(uint8_t* fb, int fb_stride, int fb_height,
                  int x, int y, int w, int h, uint32_t color);

// Draw a rectangle outline
void ui_draw_rect(uint8_t* fb, int fb_stride, int fb_height,
                  int x, int y, int w, int h, uint32_t color);

// Draw a filled rounded rectangle
void ui_fill_rounded_rect(uint8_t* fb, int fb_stride, int fb_height,
                          int x, int y, int w, int h, int radius, uint32_t color);

// Draw an LCARS-style bar (rounded on left, straight on right)
void ui_draw_lcars_bar(uint8_t* fb, int fb_stride, int fb_height,
                       int x, int y, int w, int h, uint32_t color);

// Draw an LCARS-style elbow (corner piece)
void ui_draw_lcars_elbow(uint8_t* fb, int fb_stride, int fb_height,
                         int x, int y, int w, int h, int corner_radius,
                         bool top_left, uint32_t color);

// Draw a horizontal line
void ui_draw_hline(uint8_t* fb, int fb_stride, int fb_height,
                   int x, int y, int len, uint32_t color);

// Draw a vertical line
void ui_draw_vline(uint8_t* fb, int fb_stride, int fb_height,
                   int x, int y, int len, uint32_t color);

// Clear screen to a color
void ui_clear(uint8_t* fb, int fb_stride, int fb_height, uint32_t color);
