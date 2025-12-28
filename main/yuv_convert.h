// YUV to BGR Conversion with 2x upscaling
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize the YUV converter
esp_err_t yuv_convert_init(void);

// Convert YUV420 to BGR888 with 270-degree rotation and 2x upscaling
// Input: 300x240 YUV420, Output: 600x480 BGR888 (centered on 800x480 display)
esp_err_t yuv_to_bgr_2x(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height);

// Deinitialize the converter
void yuv_convert_deinit(void);
