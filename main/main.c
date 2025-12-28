// Video Player for Tanmatsu Badge
// Plays pre-extracted H.264/AAC video files from SD card

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "pax_gfx.h"
#include "usb_device.h"
#include "hershey_font.h"
#include "sdcard.h"

// Video player modules
#include "ui_draw.h"
#include "ui_menu.h"
#include "media_loader.h"
#include "video_decoder.h"
#include "yuv_convert.h"
#include "audio_player.h"

static const char* TAG = "video_player";

// Application states
typedef enum {
    APP_STATE_LOADING,      // Loading playlist
    APP_STATE_MENU,         // Video selection menu
    APP_STATE_PRELOADING,   // Preloading video to PSRAM
    APP_STATE_PLAYING,      // Video playback
    APP_STATE_ERROR,        // Error state
} app_state_t;

// Global display variables
static size_t display_h_res = 0;
static size_t display_v_res = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t display_data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t fb = {0};
static QueueHandle_t input_event_queue = NULL;
static SemaphoreHandle_t vsync_sem = NULL;

// Video player state
static app_state_t app_state = APP_STATE_LOADING;
static playlist_t playlist = {0};
static ui_menu_state_t menu_state = {0};
static preloaded_media_t current_media = {0};
static int current_frame = 0;
static bool video_ended = false;

// Video playback configuration
#define VIDEO_FPS           10
#define FRAME_DURATION_MS   (1000 / VIDEO_FPS)

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// Draw loading screen
static void draw_loading_screen(uint8_t* fb_pixels, int stride, int height, const char* message) {
    ui_clear(fb_pixels, stride, height, COLOR_BG);

    // Draw LCARS-style header
    ui_draw_lcars_bar(fb_pixels, stride, height, 0, 0, 800, 60, COLOR_ACCENT1);
    hershey_draw_string(fb_pixels, stride, height, 100, 30, "HITCHHIKER'S GUIDE", 28, 0, 0, 0);

    // Draw loading message centered
    int msg_len = strlen(message);
    int msg_x = 100; // (800 - msg_len * 12) / 2;
    hershey_draw_string(fb_pixels, stride, height, msg_x, 250, message, 24, 255, 255, 255);
}

// Draw error screen
static void draw_error_screen(uint8_t* fb_pixels, int stride, int height, const char* error) {
    ui_clear(fb_pixels, stride, height, COLOR_BG);

    // Draw LCARS-style header in red for error
    ui_draw_lcars_bar(fb_pixels, stride, height, 0, 0, 800, 60, 0xFF0000);
    hershey_draw_string(fb_pixels, stride, height, 100, 35, "ERROR", 28, 255, 255, 255);

    // Draw error message
    hershey_draw_string(fb_pixels, stride, height, 50, 200, error, 18, 255, 100, 100);
    hershey_draw_string(fb_pixels, stride, height, 50, 350, "Press ESC to return to launcher", 16, 200, 200, 200);
}

// Start video playback
static esp_err_t start_playback(video_entry_t* entry) {
    ESP_LOGI(TAG, "Starting playback: %s", entry->display_name);

    // Preload video and audio to PSRAM
    esp_err_t ret = media_preload("/sd/at.cavac.hhgg", entry, &current_media);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to preload media: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Media preloaded: video=%zu bytes, audio=%zu bytes",
             current_media.video_size, current_media.audio_size);

    // Initialize video decoder
    ret = video_decoder_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init video decoder");
        media_unload(&current_media);
        return ret;
    }

    // Start audio playback
    ret = audio_player_start(&current_media);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio start failed, continuing without audio");
    }

    current_frame = 0;
    video_ended = false;

    return ESP_OK;
}

// Stop video playback
static void stop_playback(void) {
    ESP_LOGI(TAG, "Stopping playback");

    audio_player_stop();
    video_decoder_deinit();
    media_unload(&current_media);

    current_frame = 0;
    video_ended = false;
}

// Process one video frame with A/V sync
static bool process_video_frame(uint8_t* fb_pixels, int fb_stride, int fb_height) {
    // Get audio position for A/V sync
    uint32_t audio_ms = audio_player_get_position_ms();
    int expected_frame = audio_ms / FRAME_DURATION_MS;

    // If we're ahead of audio, don't decode a new frame yet
    if (current_frame > expected_frame) {
        return false;  // Wait for audio to catch up
    }

    // We need to decode frame(s) to catch up to audio
    // Skip frames if way behind (more than 1 frame)
    while (current_frame < expected_frame) {
        size_t nal_size = 0;
        uint8_t* nal_data = stream_next_video_nal(&current_media, &nal_size);
        if (!nal_data) {
            return true;  // End of video
        }

        // Decode frame
        uint8_t* yuv_out = NULL;
        int width = 0, height = 0;
        esp_err_t ret = video_decoder_decode(nal_data, nal_size, &yuv_out, &width, &height);

        current_frame++;

        // Only display the last frame we decode (the one that catches us up)
        if (current_frame >= expected_frame && ret == ESP_OK && yuv_out) {
            // Convert YUV to BGR and write to framebuffer (includes 270° rotation)
            yuv_to_bgr(yuv_out, fb_pixels, width, height);
        }
    }

    return false;  // Continue playing
}

void app_main(void) {
    // Initialize USB debug console
    usb_initialize();

    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    esp_err_t res;

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = display_color_format,
            .num_fbs = 1,
        },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    // Turn off LEDs
    uint8_t led_data[18] = {0};
    bsp_led_write(led_data, sizeof(led_data));

    // Get display parameters
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();

    // Convert to PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    if (display_color_format == LCD_COLOR_PIXEL_FORMAT_RGB565) {
        format = PAX_BUF_16_565RGB;
    }

    // Convert rotation
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        default: break;
    }

    // Initialize graphics
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    // Get input queue
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Enable vsync
    esp_err_t te_err = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&vsync_sem);
    }
    if (te_err != ESP_OK || vsync_sem == NULL) {
        ESP_LOGW(TAG, "Vsync not available - playback may stutter");
        vsync_sem = NULL;
    }

    // Get framebuffer info (use _rw for writable access)
    // Note: stride is in pixels, not bytes (Hershey font multiplies by 3 internally)
    uint8_t* fb_pixels = (uint8_t*)pax_buf_get_pixels_rw(&fb);
    int fb_stride = display_h_res;  // Width in pixels
    int fb_height = display_v_res;  // Height in pixels

    // Draw initial loading screen
    draw_loading_screen(fb_pixels, fb_stride, fb_height, "Initializing...");
    blit();

    // Initialize SD card
    res = sdcard_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed");
        app_state = APP_STATE_ERROR;
        draw_error_screen(fb_pixels, fb_stride, fb_height, "SD card not found");
        blit();
    }

    // Initialize YUV converter
    if (app_state != APP_STATE_ERROR) {
        res = yuv_convert_init();
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "YUV converter init warning (software fallback available)");
        }
    }

    // Initialize audio player
    if (app_state != APP_STATE_ERROR) {
        res = audio_player_init();
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Audio init failed - videos will play without sound");
        }
    }

    // Load playlist
    if (app_state != APP_STATE_ERROR) {
        draw_loading_screen(fb_pixels, fb_stride, fb_height, "Loading playlist...");
        blit();

        res = playlist_load("/sd/at.cavac.hhgg/playlist.json", &playlist);
        if (res != ESP_OK || playlist.video_count == 0) {
            ESP_LOGE(TAG, "Failed to load playlist");
            app_state = APP_STATE_ERROR;
            draw_error_screen(fb_pixels, fb_stride, fb_height, "No videos found on SD card");
            blit();
        } else {
            ESP_LOGI(TAG, "Loaded playlist: %s (%d videos)", playlist.title, playlist.video_count);
            ui_menu_init(&menu_state, &playlist);
            app_state = APP_STATE_MENU;
        }
    }

    // Input state tracking
    bool key_up_pressed = false;
    bool key_down_pressed = false;
    bool key_enter_pressed = false;
    bool key_esc_pressed = false;

    // Main loop
    while (1) {
        // Process input events
        bsp_input_event_t event;
        while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            if (event.type == INPUT_EVENT_TYPE_SCANCODE) {
                bsp_input_scancode_t scancode = event.args_scancode.scancode;
                // Check if it's a release event (release modifier is 0x80)
                bool released = (scancode & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0;
                // Get base scancode without release modifier
                bsp_input_scancode_t base_scancode = scancode & ~BSP_INPUT_SCANCODE_RELEASE_MODIFIER;

                switch (base_scancode) {
                    case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP:
                        key_up_pressed = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN:
                        key_down_pressed = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ENTER:
                        key_enter_pressed = !released;
                        break;
                    case BSP_INPUT_SCANCODE_ESC:
                        key_esc_pressed = !released;
                        break;
                    default:
                        break;
                }
            } else if (event.type == INPUT_EVENT_TYPE_ACTION) {
                // Power button returns to launcher
                bsp_device_restart_to_launcher();
            }
        }

        // State machine
        switch (app_state) {
            case APP_STATE_LOADING:
                // Already handled during init
                break;

            case APP_STATE_MENU: {
                // Handle ESC in menu - return to launcher
                if (key_esc_pressed) {
                    key_esc_pressed = false;
                    bsp_device_restart_to_launcher();
                }

                // Handle menu input
                video_entry_t* selected = NULL;
                bool selection_made = ui_menu_handle_input(&menu_state,
                    key_up_pressed, key_down_pressed, key_enter_pressed, &selected);

                // Clear pressed state after handling
                key_up_pressed = false;
                key_down_pressed = false;
                key_enter_pressed = false;

                if (selection_made && selected) {
                    // Start preloading
                    app_state = APP_STATE_PRELOADING;

                    char msg[80];
                    snprintf(msg, sizeof(msg), "Pfrimmelizing  %s", selected->display_name);
                    draw_loading_screen(fb_pixels, fb_stride, fb_height, msg);
                    blit();

                    res = start_playback(selected);
                    if (res == ESP_OK) {
                        app_state = APP_STATE_PLAYING;
                    } else {
                        app_state = APP_STATE_MENU;
                        // Continue to menu
                    }
                } else {
                    // Draw menu
                    ui_menu_draw(&menu_state, fb_pixels, fb_stride, fb_height);
                }
                break;
            }

            case APP_STATE_PRELOADING:
                // Handled in APP_STATE_MENU transition
                break;

            case APP_STATE_PLAYING: {
                // Handle ESC - return to menu
                if (key_esc_pressed) {
                    key_esc_pressed = false;
                    stop_playback();
                    app_state = APP_STATE_MENU;
                    break;
                }

                // Process video frame
                video_ended = process_video_frame(fb_pixels, fb_stride, fb_height);

                if (video_ended || !audio_player_is_playing()) {
                    // Video finished
                    stop_playback();
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_ERROR:
                // Handle ESC in error state
                if (key_esc_pressed) {
                    bsp_device_restart_to_launcher();
                }
                break;
        }

        // Wait for vsync
        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
        }

        // Blit to display
        blit();

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
