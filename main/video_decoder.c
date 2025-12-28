// Video Decoder - H.264 decoding using esp_h264 component

#include "video_decoder.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "video_decoder";

// Frame dimensions (half resolution, will be 2x upscaled during display)
#define FRAME_WIDTH  300
#define FRAME_HEIGHT 240

// YUV420 buffer size: width * height * 1.5
#define YUV_BUFFER_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 3 / 2)

static esp_h264_dec_handle_t decoder = NULL;
static esp_h264_dec_param_handle_t param_handle = NULL;
static uint8_t* yuv_buffer = NULL;

esp_err_t video_decoder_init(void) {
    // Try to allocate YUV buffer in internal SRAM for speed
    yuv_buffer = heap_caps_malloc(YUV_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (yuv_buffer) {
        ESP_LOGI(TAG, "Allocated YUV buffer: %d bytes in internal SRAM", YUV_BUFFER_SIZE);
    } else {
        // Fallback to PSRAM
        yuv_buffer = heap_caps_malloc(YUV_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!yuv_buffer) {
            ESP_LOGE(TAG, "Failed to allocate YUV buffer (%d bytes)", YUV_BUFFER_SIZE);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated YUV buffer: %d bytes in PSRAM (SRAM unavailable)", YUV_BUFFER_SIZE);
    }
    memset(yuv_buffer, 0, YUV_BUFFER_SIZE);

    // Create software decoder
    esp_h264_dec_cfg_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_I420,
    };

    esp_h264_err_t ret = esp_h264_dec_sw_new(&cfg, &decoder);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "Failed to create H.264 decoder: %d", ret);
        heap_caps_free(yuv_buffer);
        yuv_buffer = NULL;
        return ESP_FAIL;
    }

    // Get parameter handle for resolution info
    ret = esp_h264_dec_sw_get_param_hd(decoder, &param_handle);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "Failed to get param handle: %d", ret);
        param_handle = NULL;
    }

    // Open the decoder
    ret = esp_h264_dec_open(decoder);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open H.264 decoder: %d", ret);
        esp_h264_dec_del(decoder);
        decoder = NULL;
        heap_caps_free(yuv_buffer);
        yuv_buffer = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "H.264 decoder initialized");
    return ESP_OK;
}

esp_err_t video_decoder_decode(uint8_t* nal_data, size_t nal_size,
                               uint8_t** yuv_out, int* width, int* height) {
    if (!decoder || !nal_data || nal_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Prepare input frame
    esp_h264_dec_in_frame_t in_frame = {
        .pts = 0,
        .raw_data = {
            .buffer = nal_data,
            .len = nal_size,
        },
        .consume = 0,
    };

    // Prepare output frame
    esp_h264_dec_out_frame_t out_frame = {0};

    // Decode
    esp_h264_err_t ret = esp_h264_dec_process(decoder, &in_frame, &out_frame);

    if (ret == ESP_H264_ERR_OK && out_frame.outbuf && out_frame.out_size > 0) {
        // Frame decoded successfully
        size_t copy_size = out_frame.out_size;
        if (copy_size > YUV_BUFFER_SIZE) {
            copy_size = YUV_BUFFER_SIZE;
        }
        memcpy(yuv_buffer, out_frame.outbuf, copy_size);

        // Get resolution if available
        if (param_handle) {
            esp_h264_resolution_t res = {0};
            if (esp_h264_dec_get_resolution(param_handle, &res) == ESP_H264_ERR_OK) {
                *width = res.width;
                *height = res.height;
            } else {
                // Default to expected size
                *width = FRAME_WIDTH;
                *height = FRAME_HEIGHT;
            }
        } else {
            *width = FRAME_WIDTH;
            *height = FRAME_HEIGHT;
        }

        *yuv_out = yuv_buffer;
        return ESP_OK;
    } else if (ret == ESP_H264_ERR_OK) {
        // Buffering - need more data
        return ESP_ERR_NOT_FINISHED;
    } else {
        ESP_LOGW(TAG, "Decode error: %d", ret);
        return ESP_FAIL;
    }
}

esp_err_t video_decoder_flush(void) {
    if (!decoder) {
        return ESP_ERR_INVALID_STATE;
    }

    // Flush by sending empty frame
    esp_h264_dec_in_frame_t in_frame = {
        .pts = 0,
        .raw_data = {
            .buffer = NULL,
            .len = 0,
        },
    };
    esp_h264_dec_out_frame_t out_frame = {0};

    esp_h264_err_t ret = esp_h264_dec_process(decoder, &in_frame, &out_frame);
    if (ret == ESP_H264_ERR_OK && out_frame.outbuf) {
        return ESP_OK;
    }

    return ESP_ERR_NOT_FINISHED;
}

void video_decoder_deinit(void) {
    if (decoder) {
        esp_h264_dec_close(decoder);
        esp_h264_dec_del(decoder);
        decoder = NULL;
        param_handle = NULL;
    }

    if (yuv_buffer) {
        heap_caps_free(yuv_buffer);
        yuv_buffer = NULL;
    }

    ESP_LOGI(TAG, "H.264 decoder deinitialized");
}

uint8_t* video_decoder_get_yuv_buffer(void) {
    return yuv_buffer;
}

size_t video_decoder_get_yuv_buffer_size(void) {
    return YUV_BUFFER_SIZE;
}
