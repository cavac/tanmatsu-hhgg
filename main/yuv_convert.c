// YUV to BGR Conversion - Using PPA hardware acceleration with software fallback

#include "yuv_convert.h"
#include "esp_log.h"
#include "driver/ppa.h"
#include <string.h>

static const char* TAG = "yuv_convert";

static ppa_client_handle_t ppa_client = NULL;
static bool ppa_available = false;

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
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };

    esp_err_t ret = ppa_register_client(&cfg, &ppa_client);
    if (ret == ESP_OK) {
        ppa_available = true;
        ESP_LOGI(TAG, "PPA client initialized for YUV conversion");
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

    if (ppa_available && ppa_client) {
        // Try PPA hardware conversion
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
                .buffer = bgr_out,
                .buffer_size = width * height * 3,
                .pic_w = height,  // Swapped due to rotation
                .pic_h = width,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
            .scale_x = 1.0,
            .scale_y = 1.0,
            .rgb_swap = 1,  // Swap R and B for BGR output
            .byte_swap = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };

        esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_client, &srm_cfg);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "PPA conversion failed, falling back to software");
    }

    // Software fallback
    yuv_to_bgr_software(yuv_in, bgr_out, width, height);
    return ESP_OK;
}

// Software YUV420 to BGR888 conversion with 270-degree rotation
void yuv_to_bgr_software(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height) {
    uint8_t* y_plane = yuv_in;
    uint8_t* u_plane = yuv_in + width * height;
    uint8_t* v_plane = u_plane + (width * height / 4);

    int uv_width = width / 2;

    // Output dimensions after 270-degree rotation
    int out_width = height;
    int out_stride = out_width * 3;
    (void)width;  // Output height = input width, not used directly

    for (int src_y = 0; src_y < height; src_y++) {
        for (int src_x = 0; src_x < width; src_x++) {
            // Get YUV values
            int y_idx = src_y * width + src_x;
            int uv_idx = (src_y / 2) * uv_width + (src_x / 2);

            int y = y_plane[y_idx];
            int u = u_plane[uv_idx] - 128;
            int v = v_plane[uv_idx] - 128;

            // YUV to RGB conversion (BT.601)
            int c = yuv_y_table[y];
            int r = c + ((v * 359) >> 8);
            int g = c - ((u * 88 + v * 183) >> 8);
            int b = c + ((u * 454) >> 8);

            // Clamp values
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;

            // 270-degree rotation: (x, y) -> (height - 1 - y, x)
            int dst_x = height - 1 - src_y;
            int dst_y = src_x;
            int dst_idx = dst_y * out_stride + dst_x * 3;

            // Write BGR
            bgr_out[dst_idx + 0] = b;
            bgr_out[dst_idx + 1] = g;
            bgr_out[dst_idx + 2] = r;
        }
    }
}

void yuv_convert_deinit(void) {
    if (ppa_client) {
        ppa_unregister_client(ppa_client);
        ppa_client = NULL;
    }
    ppa_available = false;
    ESP_LOGI(TAG, "YUV converter deinitialized");
}
