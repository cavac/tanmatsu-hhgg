// YUV to BGR Conversion - Using PPA hardware acceleration with software fallback

#include "yuv_convert.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/ppa.h"
#include <string.h>

// ESP32-P4 cache line size
#define CACHE_LINE_SIZE 64

static const char* TAG = "yuv_convert";

static ppa_client_handle_t ppa_client = NULL;
static bool ppa_available = false;

// Cache-aligned intermediate buffer for PPA output
// 800x480x3 = 1,152,000 bytes
#define PPA_BUFFER_SIZE (800 * 480 * 3)
static uint8_t* ppa_aligned_buffer = NULL;

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

esp_err_t yuv_convert_init(void) {
    // Allocate cache-aligned buffer for PPA output (in PSRAM)
    ppa_aligned_buffer = heap_caps_aligned_alloc(CACHE_LINE_SIZE, PPA_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!ppa_aligned_buffer) {
        ESP_LOGW(TAG, "Failed to allocate PPA buffer, using software conversion only");
        ppa_available = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Allocated %d byte PPA buffer (aligned to %d)", PPA_BUFFER_SIZE, CACHE_LINE_SIZE);

    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };

    esp_err_t ret = ppa_register_client(&cfg, &ppa_client);
    if (ret == ESP_OK) {
        // TODO: PPA YUV420->RGB888 with rotation has stride issues, disable for now
        ppa_available = false;
        ESP_LOGI(TAG, "PPA disabled - using optimized software conversion");
    } else {
        ppa_available = false;
        ESP_LOGW(TAG, "PPA not available, using software conversion");
    }

    return ESP_OK;  // Always return OK, we have software fallback
}

esp_err_t yuv_to_bgr(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height) {
    if (!yuv_in || !bgr_out) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t num_pixels = width * height;

    if (ppa_available && ppa_client && ppa_aligned_buffer) {
        // Use PPA hardware conversion to aligned buffer
        // Note: YUV420 doesn't support rgb_swap, so we output RGB888 and swap in copy
        ppa_srm_oper_config_t srm_cfg = {
            .in = {
                .buffer = yuv_in,
                .pic_w = width,
                .pic_h = height,
                .block_w = width,
                .block_h = height,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            },
            .out = {
                .buffer = ppa_aligned_buffer,
                .buffer_size = PPA_BUFFER_SIZE,
                .pic_w = height,  // Swapped due to rotation
                .pic_h = width,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
            .scale_x = 1.0,
            .scale_y = 1.0,
            .rgb_swap = 0,  // YUV420 doesn't support this, do swap in copy
            .byte_swap = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };

        esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_client, &srm_cfg);
        if (ret == ESP_OK) {
            // Copy from aligned buffer to framebuffer, swapping R and B
            uint8_t* src = ppa_aligned_buffer;
            uint8_t* dst = bgr_out;
            for (size_t i = 0; i < num_pixels; i++) {
                dst[0] = src[2];  // B <- R
                dst[1] = src[1];  // G <- G
                dst[2] = src[0];  // R <- B
                src += 3;
                dst += 3;
            }
            return ESP_OK;
        }

        ESP_LOGW(TAG, "PPA conversion failed: %s, falling back to software", esp_err_to_name(ret));
    }

    // Software fallback
    yuv_to_bgr_software(yuv_in, bgr_out, width, height);
    return ESP_OK;
}

// Clamp table: maps [-256..511] to [0..255]
// Index with (value + 256) to handle negative values
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

// Letterbox offset: center 600px video on 800px display
#define LETTERBOX_OFFSET 100

// Software YUV420 to BGR888 conversion with 270-degree rotation
// Row-by-row reads for better PSRAM cache, clamp table for speed
// Outputs to center of 800-pixel wide display with letterboxing
void yuv_to_bgr_software(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height) {
    init_clamp_table();

    uint8_t* y_plane = yuv_in;
    uint8_t* u_plane = yuv_in + width * height;
    uint8_t* v_plane = u_plane + (width * height / 4);

    int uv_stride = width >> 1;
    int out_stride = height * 3;  // Output row stride (480 pixels * 3 bytes)

    // Process row-by-row for sequential PSRAM reads
    for (int src_y = 0; src_y < height; src_y++) {
        uint8_t* y_row = y_plane + src_y * width;
        uint8_t* u_row = u_plane + (src_y >> 1) * uv_stride;
        uint8_t* v_row = v_plane + (src_y >> 1) * uv_stride;

        // Pre-compute destination X for this source row
        int dst_x = height - 1 - src_y;

        for (int src_x = 0; src_x < width; src_x++) {
            int y_val = y_row[src_x];
            int u_val = u_row[src_x >> 1] - 128;
            int v_val = v_row[src_x >> 1] - 128;

            // YUV to RGB (BT.601)
            int c = yuv_y_table[y_val];
            int r = c + ((v_val * 359) >> 8);
            int g = c - ((u_val * 88 + v_val * 183) >> 8);
            int b = c + ((u_val * 454) >> 8);

            // 270° rotation with letterbox offset: dst_y = src_x + offset
            uint8_t* dst = bgr_out + (src_x + LETTERBOX_OFFSET) * out_stride + dst_x * 3;
            dst[0] = clamp_table[b + 256];
            dst[1] = clamp_table[g + 256];
            dst[2] = clamp_table[r + 256];
        }
    }
}

void yuv_convert_deinit(void) {
    if (ppa_client) {
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
    }
    if (ppa_aligned_buffer) {
        heap_caps_free(ppa_aligned_buffer);
        ppa_aligned_buffer = NULL;
    }
    ppa_available = false;
    ESP_LOGI(TAG, "YUV converter deinitialized");
}
