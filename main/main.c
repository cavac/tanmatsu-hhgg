#include <stdio.h>
#include <stdint.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

//#include "renderer.h"

// Constants
//static char const TAG[] = "main";

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {PAX_BUF_16_565RGB};
static QueueHandle_t                input_event_queue    = NULL;

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

extern "C" void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    ESP_ERROR_CHECK(bsp_device_initialize());

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

#if defined(CONFIG_BSP_TARGET_KAMI)
#define BLACK 0
#define WHITE 1
#define RED   2
#else
#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000
#endif

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Main section of the app

    // Bounce some balls around the screen
    //
    // On every bounce, light up one LED with the specific ball color, also blink the keyboard


    // Setup data for the balls, "physics" and the corresponding LEDs
    int32_t xoffs[5] = {100, 130, 170, 230, 335}; // Starting X position of balls
    int32_t yoffs[5] = {100, 107, 209, 305, 227}; // Starting Y position of balls
    int32_t xspeed[5] = {2, 1, -1, 3, -2}; // X speed (delta) of balls
    int32_t yspeed[5] = {1, -1, 2, -2, 3}; // Y speed (delta) of balls
    uint32_t color[5] = {0xFFFF0000, 0xFF00FF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFF00}; // Ball colors
    bool bounce[5] = {false, false, false, false, false}; // Has bounced this render cycle

    uint8_t led_offs[5] = {0 * 3, 1 * 3, 2 * 3, 4 * 3, 5 * 3};  // Starting offset in the led_data for the corresponding balls
    uint8_t led_colormap[15] = {0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00}; // Color of the balls, split into bytes


    uint8_t i;
    uint8_t ledoffs;
    uint32_t delay = 0; //pdMS_TO_TICKS(1);
    uint8_t bright = 100;
    while(1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, delay) == pdTRUE) {
            bsp_device_restart_to_launcher();
        }
        pax_background(&fb, BLACK);
        //renderNextFrame();
        //pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "Press any key to exit the demo.");

        memset(led_data, 0, 18); // LEDS OFF
        led_data[(3 * 3) + 0] = 0xFF; // Power LED on
        led_data[(3 * 3) + 1] = 0xFF;

        for(i = 0; i < 5; i++) {
            bounce[i] = false;
            xoffs[i] += xspeed[i];
            if(xoffs[i] < 1 || xoffs[i] > (display_h_res - 50)) {
                xspeed[i] *= -1;
                xoffs[i] += xspeed[i];
                bright = 100;
                bounce[i] = true;

            }
            yoffs[i] += yspeed[i];
            if(yoffs[i] < 1 || yoffs[i] > (display_v_res - 50)) {
                yspeed[i] *= -1;
                yoffs[i] += yspeed[i];
                bright = 100;
                bounce[i] = true;
            }
            pax_draw_circle(&fb, color[i], yoffs[i] + 25, xoffs[i] + 25, 25);
            if(bounce[i]) {
                pax_draw_circle(&fb, BLACK, yoffs[i] + 25, xoffs[i] + 25, 15);
                pax_draw_circle(&fb, WHITE, yoffs[i] + 25, xoffs[i] + 25, 10);
                
                ledoffs = led_offs[i];

                // For some strange reason, the LED array seems to expect G R B (instead of R G B), so we swap the bytes accordingly
                led_data[ledoffs + 0] = led_colormap[(i * 3) + 1];
                led_data[ledoffs + 1] = led_colormap[(i * 3) + 0];
                led_data[ledoffs + 2] = led_colormap[(i * 3) + 2];
            }
        }
        bsp_input_set_backlight_brightness(bright);
        if(bright > 0) {
            bright -= 25;
        }
        blit();
        bsp_led_write(led_data, sizeof(led_data));
    }
}
