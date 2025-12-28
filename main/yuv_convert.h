// YUV to BGR Conversion with 2x upscaling
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize the YUV converter
esp_err_t yuv_convert_init(void);

// Convert YUV420 to BGR888 with 270-degree rotation and 2x upscaling
// Input: 300x240 YUV420, Output: 600x480 BGR888 (centered on 800x480 display)
esp_err_t yuv_to_bgr_2x(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height);

// Copy BGR888 to framebuffer with 270-degree rotation (for MJPEG)
// Input: BGR888 from JPEG decoder (left-to-right, top-to-bottom)
// Output: Rotated BGR888 in framebuffer, centered for letterboxing
// src_width/height: source image dimensions
// display_width: total display width (800) for letterbox calculation
esp_err_t bgr_rotate_270(uint8_t* bgr_in, uint8_t* fb_out,
                          int src_width, int src_height, int display_width);

// Deinitialize the converter
void yuv_convert_deinit(void);
