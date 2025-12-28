// YUV to BGR Conversion - Using PPA hardware acceleration
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize the YUV converter (PPA client)
esp_err_t yuv_convert_init(void);

// Convert YUV420 (I420) to BGR888 with 270-degree rotation
// yuv_in: YUV420 planar data (Y plane, then U plane, then V plane)
// bgr_out: Output BGR888 buffer (must be width * height * 3 bytes)
// width, height: Frame dimensions
esp_err_t yuv_to_bgr(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height);

// Software fallback for YUV to BGR conversion (if PPA not available)
void yuv_to_bgr_software(uint8_t* yuv_in, uint8_t* bgr_out, int width, int height);

// Deinitialize the converter
void yuv_convert_deinit(void);
