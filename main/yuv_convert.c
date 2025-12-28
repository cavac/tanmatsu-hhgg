// YUV to BGR Conversion with 2x upscaling

#include "yuv_convert.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char* TAG = "yuv_convert";

// YUV to RGB conversion coefficients (BT.601)
// Placed in DRAM for fast access
static DRAM_ATTR const int16_t yuv_y_table[256] = {
    // Y coefficient table (Y - 16) * 1.164
    -19, -18, -16, -15, -14, -13, -12, -10, -9, -8, -7, -6, -4, -3, -2, -1,
    0, 1, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 15, 16, 17, 18,
    19, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37,
    39, 40, 41, 42, 43, 45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 57,
    58, 59, 60, 61, 63, 64, 65, 66, 67, 69, 70, 71, 72, 73, 75, 76,
    77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90, 91, 93, 94, 95,
    96, 97, 99, 100, 101, 102, 103, 105, 106, 107, 108, 109, 111, 112, 113, 114,
    115, 117, 118, 119, 120, 121, 123, 124, 125, 126, 127, 129, 130, 131, 132, 133,
    135, 136, 137, 138, 139, 141, 142, 143, 144, 145, 147, 148, 149, 150, 151, 153,
    154, 155, 156, 157, 159, 160, 161, 162, 163, 165, 166, 167, 168, 169, 171, 172,
    173, 174, 175, 177, 178, 179, 180, 181, 183, 184, 185, 186, 187, 189, 190, 191,
    192, 193, 195, 196, 197, 198, 199, 201, 202, 203, 204, 205, 207, 208, 209, 210,
    211, 213, 214, 215, 216, 217, 219, 220, 221, 222, 223, 225, 226, 227, 228, 229,
    231, 232, 233, 234, 235, 237, 238, 239, 240, 241, 243, 244, 245, 246, 247, 249,
    250, 251, 252, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
};

// Clamp table: maps [-256..511] to [0..255]
static DRAM_ATTR uint8_t clamp_table[768];
static bool clamp_table_init = false;

static void init_clamp_table(void) {
    if (clamp_table_init) return;
    for (int i = 0; i < 768; i++) {
        int val = i - 256;
        if (val < 0) clamp_table[i] = 0;
        else if (val > 255) clamp_table[i] = 255;
        else clamp_table[i] = val;
    }
    clamp_table_init = true;
}

esp_err_t yuv_convert_init(void) {
    init_clamp_table();
    ESP_LOGI(TAG, "YUV converter initialized (2x upscaling mode)");
    return ESP_OK;
}

// Letterbox offset: center 600px video on 800px display
#define LETTERBOX_OFFSET 100

// Visible video dimensions (before 2x upscale)
#define VISIBLE_WIDTH  300
#define VISIBLE_HEIGHT 240

// Convert YUV420 to BGR888 with 270-degree rotation and 2x upscaling
// Input: YUV420 with macroblock-aligned stride (e.g., 304x240)
// Output: 600x480 BGR888 on 800x480 display (only visible 300x240 rendered)
esp_err_t yuv_to_bgr_2x(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height) {
    if (!yuv_in || !bgr_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Use actual decoded dimensions for plane layout (may be macroblock-aligned)
    // e.g., width=304, height=240 for 300x240 video
    uint8_t* y_plane = yuv_in;
    uint8_t* u_plane = yuv_in + width * height;
    uint8_t* v_plane = u_plane + (width * height / 4);

    // Stride for UV planes (half of luma width)
    int uv_stride = width >> 1;

    // Output dimensions after 2x upscale of VISIBLE content
    int out_height = VISIBLE_HEIGHT * 2; // 480
    int out_stride = out_height * 3;     // 480 pixels * 3 bytes (due to 270° rotation)

    // Process row-by-row for sequential PSRAM reads
    // Only process VISIBLE_HEIGHT rows, using 'width' as stride for plane access
    for (int src_y = 0; src_y < VISIBLE_HEIGHT; src_y++) {
        uint8_t* y_row = y_plane + src_y * width;  // Use actual stride (304)
        uint8_t* u_row = u_plane + (src_y >> 1) * uv_stride;
        uint8_t* v_row = v_plane + (src_y >> 1) * uv_stride;

        // Each source row produces 2 destination rows (2x upscale)
        // With 270° rotation: src_y -> dst_x, so 2 adjacent dst_x columns
        int dst_x_base = out_height - 1 - (src_y * 2);

        // Only process VISIBLE_WIDTH columns (300), ignoring padding (cols 300-303)
        for (int src_x = 0; src_x < VISIBLE_WIDTH; src_x++) {
            int y_val = y_row[src_x];
            int u_val = u_row[src_x >> 1] - 128;
            int v_val = v_row[src_x >> 1] - 128;

            // YUV to RGB (BT.601)
            int c = yuv_y_table[y_val];
            int r = c + ((v_val * 359) >> 8);
            int g = c - ((u_val * 88 + v_val * 183) >> 8);
            int b = c + ((u_val * 454) >> 8);

            uint8_t b_clamped = clamp_table[b + 256];
            uint8_t g_clamped = clamp_table[g + 256];
            uint8_t r_clamped = clamp_table[r + 256];

            // 2x upscale: each source pixel becomes a 2x2 block
            // With 270° rotation and letterbox offset
            int dst_y_base = (src_x * 2) + LETTERBOX_OFFSET;

            // Write 2x2 block (4 pixels)
            // Due to rotation, adjacent Y in output = adjacent X in source
            // dst_x = out_height - 1 - src_y*2, dst_x-1 = out_height - 2 - src_y*2

            // Pixel (0,0) of 2x2 block
            uint8_t* dst = bgr_out + dst_y_base * out_stride + dst_x_base * 3;
            dst[0] = b_clamped;
            dst[1] = g_clamped;
            dst[2] = r_clamped;

            // Pixel (1,0) of 2x2 block
            dst = bgr_out + (dst_y_base + 1) * out_stride + dst_x_base * 3;
            dst[0] = b_clamped;
            dst[1] = g_clamped;
            dst[2] = r_clamped;

            // Pixel (0,1) of 2x2 block
            dst = bgr_out + dst_y_base * out_stride + (dst_x_base - 1) * 3;
            dst[0] = b_clamped;
            dst[1] = g_clamped;
            dst[2] = r_clamped;

            // Pixel (1,1) of 2x2 block
            dst = bgr_out + (dst_y_base + 1) * out_stride + (dst_x_base - 1) * 3;
            dst[0] = b_clamped;
            dst[1] = g_clamped;
            dst[2] = r_clamped;
        }
    }

    return ESP_OK;
}

void yuv_convert_deinit(void) {
    ESP_LOGI(TAG, "YUV converter deinitialized");
}

// Copy BGR888 to framebuffer with 270-degree rotation
// For 270° rotation: src(x,y) -> dst(height-1-y, x)
// With letterboxing: centered horizontally on display
esp_err_t bgr_rotate_270(uint8_t* bgr_in, uint8_t* fb_out,
                          int src_width, int src_height, int display_width) {
    if (!bgr_in || !fb_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate letterbox offset to center content
    // For 600x480 on 800px wide display: offset = (800 - 600) / 2 = 100
    int letterbox_offset = (display_width - src_width) / 2;
    if (letterbox_offset < 0) letterbox_offset = 0;

    // Output dimensions after rotation
    // src 600x480 becomes dst 480x600 (width becomes height)
    int out_height = src_width;  // 600 becomes vertical dimension
    int out_stride = src_height * 3;  // 480 pixels * 3 bytes per row

    // Process row by row for sequential memory access on source
    for (int src_y = 0; src_y < src_height; src_y++) {
        uint8_t* src_row = bgr_in + src_y * src_width * 3;

        // After 270° rotation: src_y maps to dst_x (column in output)
        // dst_x goes from (height-1) down to 0 as src_y increases
        int dst_x = src_height - 1 - src_y;

        for (int src_x = 0; src_x < src_width; src_x++) {
            // After 270° rotation: src_x maps to dst_y (row in output)
            // With letterbox offset applied
            int dst_y = src_x + letterbox_offset;

            // Calculate destination offset
            // dst layout: row-major, each row is out_stride bytes
            uint8_t* dst = fb_out + dst_y * out_stride + dst_x * 3;

            // Copy BGR pixel
            dst[0] = src_row[src_x * 3 + 0];  // B
            dst[1] = src_row[src_x * 3 + 1];  // G
            dst[2] = src_row[src_x * 3 + 2];  // R
        }
    }

    return ESP_OK;
}
