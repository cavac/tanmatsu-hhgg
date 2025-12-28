// Video Decoder - H.264 decoding using esp_h264 component
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Initialize the H.264 decoder
// Allocates YUV buffer in PSRAM
esp_err_t video_decoder_init(void);

// Decode a NAL unit to YUV420 frame
// nal_data: pointer to NAL unit (with start code)
// nal_size: size of NAL unit
// yuv_out: pointer to decoded YUV420 data (in PSRAM)
// width, height: output frame dimensions
// Returns ESP_OK on success, ESP_ERR_NOT_FINISHED if frame not ready yet
esp_err_t video_decoder_decode(uint8_t* nal_data, size_t nal_size,
                               uint8_t** yuv_out, int* width, int* height);

// Flush decoder (get remaining frames)
esp_err_t video_decoder_flush(void);

// Deinitialize decoder and free resources
void video_decoder_deinit(void);

// Get YUV buffer pointer (for direct access)
uint8_t* video_decoder_get_yuv_buffer(void);

// Get YUV buffer size
size_t video_decoder_get_yuv_buffer_size(void);
