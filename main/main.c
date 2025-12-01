#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
// #include "nvs_flash.h"  // Disabled to save memory
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "renderer.h"

// External ST7701 color format function (from esp32-component-mipi-dsi-abstraction)
extern esp_err_t st7701_set_color_format(lcd_color_rgb_pixel_format_t format);

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
static QueueHandle_t                input_event_queue    = NULL;

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    esp_err_t res;

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = display_color_format,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    uint8_t led_data[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    bsp_led_write(led_data, sizeof(led_data));

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);  // Check that the display parameters have been initialized
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();

    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    // Convert BSP display rotation format into PAX orientation type
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

    // Initialize graphics stack
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    format = PAX_BUF_2_PAL;
#endif
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
#if defined(CONFIG_BSP_TARGET_KAMI)
    // Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
    fb.palette      = palette;
    fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
#endif
    pax_buf_set_orientation(&fb, orientation);

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Initialize 3D cube renderer
    renderer_init();
    static int frame_number = 0;
    // Allocate cube buffer from PSRAM to save internal RAM
    uint8_t* cube_buffer = (uint8_t*)heap_caps_malloc(480 * 480 * 3, MALLOC_CAP_SPIRAM);
    if (cube_buffer == NULL) {
        // Fallback to regular malloc if PSRAM not available
        cube_buffer = (uint8_t*)malloc(480 * 480 * 3);
    }

    uint32_t delay = pdMS_TO_TICKS(1);  // 1ms timeout for responsive input
    // Draw black background
    pax_background(&fb, BLACK);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 50, 490, 20, "The");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 50, 490, 80, "Cube");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 160, "3D render demo");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 10, 490, 180, "by Rene 'cavac' Schickbauer");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 220, "Loosely based on");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 240, "the 'tinyrenderer'");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 260, "project.");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 10, 490, 280, "https://haqr.eu/tinyrenderer/");

    while(1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, delay) == pdTRUE) {
            bsp_device_restart_to_launcher();
        }

        // DRAW 3D CUBE INTO SCREEN BUFFER
        renderer_render_frame(cube_buffer, frame_number++);

        // Copy rendered cube to screen buffer (centered with black bars)
        int x_offset = (display_h_res - 480) / 2;
        uint8_t* fb_pixels = (uint8_t*)pax_buf_get_pixels(&fb);
        int fb_stride = display_h_res * 3;  // RGB888 = 3 bytes per pixel

        for (int y = 0; y < 480; y++) {
            uint8_t* src = cube_buffer + y * 480 * 3;
            uint8_t* dst = fb_pixels + y * fb_stride + x_offset * 3;
            memcpy(dst, src, 480 * 3);
        }

        blit();
    }
}
