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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
// #include "nvs_flash.h"  // Disabled to save memory
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "renderer.h"
#include "usb_device.h"
#include "simple_font.h"
#include "hershey_font.h"

//#define CAVAC_DEBUG

#ifdef CAVAC_DEBUG
#include "sdcard.h"
#endif

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

static const char* TAG = "cube";

#ifdef CAVAC_DEBUG
static int screenshot_counter = 0;
static int64_t last_screenshot_time = 0;
#define SCREENSHOT_DEBOUNCE_MS 500

// Save framebuffer as PPM image to SD card
// PPM is a simple format: "P6\nwidth height\n255\n" followed by raw RGB data
static void save_screenshot(void) {
    // Debounce - ignore if called too soon after last screenshot
    int64_t now = esp_timer_get_time();
    if (now - last_screenshot_time < SCREENSHOT_DEBOUNCE_MS * 1000) {
        return;
    }
    last_screenshot_time = now;

    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, cannot save screenshot");
        return;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "/sd/cube_screenshot_%03d.ppm", screenshot_counter++);

    FILE* f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return;
    }

    // Write PPM header
    fprintf(f, "P6\n%zu %zu\n255\n", display_h_res, display_v_res);

    // Get framebuffer pointer
    uint8_t* pixels = (uint8_t*)pax_buf_get_pixels(&fb);

    // Write pixels (convert BGR to RGB for PPM format)
    for (size_t y = 0; y < display_v_res; y++) {
        for (size_t x = 0; x < display_h_res; x++) {
            int idx = (y * display_h_res + x) * 3;
            uint8_t b = pixels[idx + 0];
            uint8_t g = pixels[idx + 1];
            uint8_t r = pixels[idx + 2];
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Screenshot saved: %s", filename);
}
#endif

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000

#define PERF_SAMPLE_FRAMES 50

// Fast direct buffer clear for right side black bar (screen x >= 480)
// Screen is landscape 800x480, cube is 480x480 on the left
// Buffer format: 800x480 RGB888, stride = 2400 bytes
static void clear_right_bar(pax_buf_t* buf) {
    uint8_t* pixels = (uint8_t*)pax_buf_get_pixels(buf);
    int stride = display_h_res * 3;  // 800 * 3 = 2400 bytes per row
    int linecount = display_v_res - 480 - 1;

    // Clear screen columns 480-799 (right 320 pixels)
    // Direct buffer access: column X at row Y = pixels + Y*stride + X*3
    // This is a contiguous region of memory, so we can use a single memset, no need for a loop
    /*
    for (int x = 480; x < display_v_res; x++) {
        memset(pixels + x * stride, 0, stride);
    }
    */
    memset(pixels + 480 * stride, 0, linecount * stride);

}

void app_main(void) {
    // Initialize USB debug console
    usb_initialize();

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

#ifdef CAVAC_DEBUG
    // Initialize SD card for screenshots
    res = sdcard_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed - screenshots disabled");
    }
#endif

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

    // Calculate framebuffer offset and stride for direct rendering
    int x_offset = (display_h_res - 480) / 2;
    int fb_stride = display_h_res * 3;  // RGB888 = 3 bytes per pixel

    // Enable tearing effect (vsync) mode and get semaphore for smooth animation
    SemaphoreHandle_t vsync_sem = NULL;
    esp_err_t te_err = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&vsync_sem);
    }
    if (te_err == ESP_OK && vsync_sem != NULL) {
        ESP_LOGI(TAG, "Vsync synchronization enabled (TE_V_BLANKING mode)");
    } else {
        ESP_LOGW(TAG, "Vsync not available (err=%d) - animation may stutter", te_err);
        vsync_sem = NULL;  // Disable vsync waiting
    }

    uint32_t delay = pdMS_TO_TICKS(0);  // No delay - vsync provides timing
    // Draw black background
    /*
    pax_background(&fb, BLACK);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 50, 490, 20, "The");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 50, 490, 80, "Cube");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 160, "3D render demo");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 10, 490, 180, "by Rene 'cavac' Schickbauer");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 220, "Loosely based on");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 240, "the 'tinyrenderer'");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 490, 260, "project.");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 10, 490, 280, "https://haqr.eu/tinyrenderer/");
    blit();
    */

    // Performance measurement variables
    int64_t render_time_sum = 0;
    int64_t vsync_time_sum = 0;
    int64_t blit_time_sum = 0;
    int64_t frame_time_sum = 0;
    int64_t frame_start_time = esp_timer_get_time();
    int perf_frame_count = 0;
    float current_fps = 0.0f;  // For on-screen display

    while(1) {
        // Measure total frame time (from end of last frame to end of this frame)
        int64_t now = esp_timer_get_time();
        int64_t frame_time = now - frame_start_time;
        frame_start_time = now;
        frame_time_sum += frame_time;
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, delay) == pdTRUE) {
#ifdef CAVAC_DEBUG
            // KEYBOARD events for screenshot (fires once per character)
            if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
                if (event.args_keyboard.ascii == ' ') {
                    save_screenshot();
                }
            } else
#endif
            if (event.type == INPUT_EVENT_TYPE_SCANCODE) {
                // ESC via scancode returns to launcher
                if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_ESC) {
                    bsp_device_restart_to_launcher();
                }
            } else if (event.type == INPUT_EVENT_TYPE_ACTION) {
                // Action events (like power button) return to launcher
                bsp_device_restart_to_launcher();
            }
        }

        int64_t t_start, t_end;

        // Get pointer to PAX framebuffer with offset for centered cube
        uint8_t* fb_pixels = (uint8_t*)pax_buf_get_pixels(&fb);

        // Clear the right black bar area
        clear_right_bar(&fb);

        // DEBUG: Test with simple coordinates to verify rotation transform
        // Screen coords: x=100, y=100 should appear near upper-left area
        hershey_draw_string(fb_pixels, display_h_res, display_v_res, 100, 100, "TEST", 3.0f, 255, 255, 255);
        uint8_t* render_target = fb_pixels + x_offset * 3;

        // DRAW 3D CUBE DIRECTLY INTO SCREEN BUFFER
        t_start = esp_timer_get_time();
        //renderer_render_frame(render_target, fb_stride, frame_number++);

        // Draw FPS counter using fast bitmap font (within the 480x480 render area)
        // Uses 270° rotated drawing to match display orientation
        if (current_fps > 0) {
            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "%.1f fps", current_fps);
            font_draw_string_270(render_target, 480, 480, 12, 12, fps_str, 0, 0, 0);  // Black text
        }

        t_end = esp_timer_get_time();
        render_time_sum += (t_end - t_start);

        // Wait for vsync to avoid tearing and ensure smooth animation
        t_start = esp_timer_get_time();
        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));  // Wait up to 50ms for vsync
        }
        t_end = esp_timer_get_time();
        vsync_time_sum += (t_end - t_start);

        // Blit to display
        t_start = esp_timer_get_time();
        //bsp_display_blit(0, 0, 480, 480, pax_buf_get_pixels(&fb));
        blit();
        t_end = esp_timer_get_time();
        blit_time_sum += (t_end - t_start);

        perf_frame_count++;

        // Log performance stats every PERF_SAMPLE_FRAMES frames
        if (perf_frame_count >= PERF_SAMPLE_FRAMES) {
            int64_t avg_render = render_time_sum / PERF_SAMPLE_FRAMES;
            int64_t avg_vsync = vsync_time_sum / PERF_SAMPLE_FRAMES;
            int64_t avg_blit = blit_time_sum / PERF_SAMPLE_FRAMES;
            int64_t avg_frame = frame_time_sum / PERF_SAMPLE_FRAMES;

            current_fps = 1000000.0f / avg_frame;
            ESP_LOGI(TAG, "Perf (avg %d frames): render=%lldus, vsync=%lldus, blit=%lldus, frame=%lldus (%.1f fps)",
                     PERF_SAMPLE_FRAMES, avg_render, avg_vsync, avg_blit, avg_frame,
                     current_fps);

            // Reset counters
            render_time_sum = 0;
            vsync_time_sum = 0;
            blit_time_sum = 0;
            frame_time_sum = 0;
            perf_frame_count = 0;
        }
    }
}
