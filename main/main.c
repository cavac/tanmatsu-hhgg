// Video Player for Tanmatsu Badge
// Plays interleaved MJPEG+MP3 AVI video streamed from SD card

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
#include "mjpeg_decoder.h"
#include "avi_parser.h"
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
static avi_parser_t avi_parser = {0};
static int current_frame = 0;
static bool video_ended = false;

// Video frame ring buffer in PSRAM (stores compressed MJPEG data)
#define VIDEO_BUFFER_FRAMES  16                // Max frames to buffer
#define VIDEO_FRAME_MAX_SIZE (64 * 1024)       // 64KB max per compressed frame
#define PRE_BUFFER_TIME_MS   300               // Pre-buffer 300ms of audio before starting

typedef struct {
    size_t size;
    int frame_index;    // Which frame number this is (for sync)
} buffered_frame_t;

static uint8_t* video_buffer_memory = NULL;    // VIDEO_BUFFER_FRAMES * VIDEO_FRAME_MAX_SIZE in PSRAM
static buffered_frame_t video_frames[VIDEO_BUFFER_FRAMES];
static int video_write_idx = 0;                // Next slot to write
static int video_read_idx = 0;                 // Next slot to read
static int video_buffered = 0;                 // Frames currently buffered
static int next_frame_index = 0;               // Frame counter for buffering
static bool end_of_file = false;               // True when AVI EOF reached

// Pending audio chunk (when queue was full and we need to retry)
static uint8_t pending_audio_data[4096];       // Copy of audio chunk data
static size_t pending_audio_size = 0;          // 0 = no pending chunk

// Video playback - FPS read from AVI file
static int video_fps = 30;
static int frame_duration_ms = 33;

// Forward declarations
static int prebuffer_chunks(void);
static bool process_video_frame(uint8_t* fb_pixels, int fb_stride, int fb_height);

// I2S buffer latency compensation (samples in DMA buffer not yet played)
// At 44.1kHz with ~2048 samples buffered, this is ~46ms
#define AUDIO_BUFFER_LATENCY_MS  50

// Playback timing
static int64_t playback_start_time_us = 0;
static int64_t audio_end_time_us = 0;      // When audio stopped (for wall clock fallback)
static uint32_t audio_end_position_ms = 0; // Audio position when it stopped

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

    // Build video file path
    char video_path[128];
    snprintf(video_path, sizeof(video_path), "/sd/at.cavac.hhgg/%s", entry->video_file);

    // Open AVI file for streaming (uses fastopen for optimal SD card performance)
    esp_err_t ret = avi_parser_open(&avi_parser, video_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open AVI file: %s", esp_err_to_name(ret));
        return ret;
    }

    const avi_info_t* avi_info = avi_parser_get_info(&avi_parser);

    // Set playback FPS from AVI file
    video_fps = avi_info->fps > 0 ? avi_info->fps : 30;
    frame_duration_ms = 1000 / video_fps;

    ESP_LOGI(TAG, "AVI: %lux%lu @ %d fps (%d ms/frame)",
             (unsigned long)avi_info->width, (unsigned long)avi_info->height,
             video_fps, frame_duration_ms);

    // Allocate video frame buffer in PSRAM
    if (!video_buffer_memory) {
        size_t buffer_size = VIDEO_BUFFER_FRAMES * VIDEO_FRAME_MAX_SIZE;
        video_buffer_memory = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
        if (!video_buffer_memory) {
            ESP_LOGE(TAG, "Failed to allocate video buffer (%zu bytes)", buffer_size);
            avi_parser_close(&avi_parser);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated video buffer: %zu bytes in PSRAM", buffer_size);
    }

    // Reset buffer state
    video_write_idx = 0;
    video_read_idx = 0;
    video_buffered = 0;
    next_frame_index = 0;
    end_of_file = false;
    current_frame = 0;
    video_ended = false;
    pending_audio_size = 0;

    // Initialize MJPEG decoder
    ret = mjpeg_decoder_init(avi_info->width, avi_info->height);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init MJPEG decoder");
        avi_parser_close(&avi_parser);
        return ret;
    }

    // Start audio player (creates queue and task)
    if (avi_info->has_audio) {
        ret = audio_player_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio start failed, continuing without audio");
        }
    }

    // Pre-buffer audio and video before starting playback timing
    // This ensures audio queue is filled and we have frames ready
    prebuffer_chunks();

    // Clear screen to black for letterbox bars
    pax_background(&fb, 0);
    blit();

    // Start timing AFTER pre-buffering so playback starts immediately
    playback_start_time_us = esp_timer_get_time();
    audio_end_time_us = 0;
    audio_end_position_ms = 0;

    ESP_LOGI(TAG, "Playback starting");
    return ESP_OK;
}

// Stop video playback
static void stop_playback(void) {
    ESP_LOGI(TAG, "Stopping playback");

    audio_player_stop();
    mjpeg_decoder_deinit();
    avi_parser_close(&avi_parser);

    // Reset buffer state (keep memory allocated for next video)
    video_write_idx = 0;
    video_read_idx = 0;
    video_buffered = 0;
    next_frame_index = 0;
    end_of_file = false;
    current_frame = 0;
    video_ended = false;
}

// Timing statistics for performance debugging
static uint32_t timing_decode_us = 0;
static uint32_t timing_copy_us = 0;
static uint32_t timing_frame_count = 0;

// Get pointer to video buffer slot
static inline uint8_t* video_buffer_slot(int idx) {
    return video_buffer_memory + (idx * VIDEO_FRAME_MAX_SIZE);
}

// Buffer one chunk from AVI file
// Returns: 0 = buffered audio or video, 1 = EOF, -1 = video buffer full (and audio pending)
static int buffer_one_chunk(void) {
    // First try to push any pending audio chunk
    if (pending_audio_size > 0) {
        if (audio_player_push_chunk(pending_audio_data, pending_audio_size) == ESP_OK) {
            pending_audio_size = 0;  // Success
        }
        // If push failed, keep pending and continue - we can still read video
    }

    // If video buffer is full and we have pending audio, we're truly stuck
    if (video_buffered >= VIDEO_BUFFER_FRAMES) {
        return -1;  // Video buffer full
    }

    avi_chunk_t chunk;
    esp_err_t ret = avi_parser_next_chunk(&avi_parser, &chunk);

    if (ret != ESP_OK || chunk.type == AVI_CHUNK_END) {
        end_of_file = true;
        return 1;  // EOF
    }

    if (chunk.type == AVI_CHUNK_AUDIO) {
        // Try to push to audio queue
        if (audio_player_push_chunk(chunk.data, chunk.size) == ESP_ERR_TIMEOUT) {
            // Queue full - save for retry (overwrite any existing pending)
            if (chunk.size <= sizeof(pending_audio_data)) {
                memcpy(pending_audio_data, chunk.data, chunk.size);
                pending_audio_size = chunk.size;
            }
        }
        // Always return 0 - we handled the audio (pushed or saved)
        // This allows us to continue reading video chunks
        return 0;
    }

    if (chunk.type == AVI_CHUNK_VIDEO) {
        // Copy to video ring buffer
        if (chunk.size > VIDEO_FRAME_MAX_SIZE) {
            ESP_LOGW(TAG, "Video frame too large: %zu > %d, skipping", chunk.size, VIDEO_FRAME_MAX_SIZE);
            next_frame_index++;
            return 0;
        }

        uint8_t* dest = video_buffer_slot(video_write_idx);
        memcpy(dest, chunk.data, chunk.size);
        video_frames[video_write_idx].size = chunk.size;
        video_frames[video_write_idx].frame_index = next_frame_index++;

        video_write_idx = (video_write_idx + 1) % VIDEO_BUFFER_FRAMES;
        video_buffered++;
        return 0;
    }

    // Unknown chunk type, skip
    return 0;
}

// Pre-buffer audio and video before starting playback
// Returns number of video frames buffered
static int prebuffer_chunks(void) {
    int target_frames = (PRE_BUFFER_TIME_MS * video_fps) / 1000;
    if (target_frames < 3) target_frames = 3;  // Minimum 3 frames
    if (target_frames > VIDEO_BUFFER_FRAMES - 2) target_frames = VIDEO_BUFFER_FRAMES - 2;

    ESP_LOGI(TAG, "Pre-buffering %d frames (%dms at %dfps)...", target_frames, PRE_BUFFER_TIME_MS, video_fps);

    while (video_buffered < target_frames && !end_of_file) {
        int result = buffer_one_chunk();
        if (result == 1) break;  // EOF
        // result == -1 shouldn't happen since we check video_buffered < target
    }

    ESP_LOGI(TAG, "Pre-buffered %d video frames", video_buffered);
    return video_buffered;
}

// Process video frame with wall clock sync
static bool process_video_frame(uint8_t* fb_pixels, int fb_stride, int fb_height) {
    // Read chunks to maintain buffers
    // Higher FPS needs more chunks per call to keep up
    int chunks_read = 0;
    int max_chunks = 8;  // Enough for ~2-3 video frames worth of data
    while (video_buffered < VIDEO_BUFFER_FRAMES && !end_of_file && chunks_read < max_chunks) {
        int result = buffer_one_chunk();
        if (result != 0) break;  // EOF or video buffer full
        chunks_read++;
    }

    // Check for end of video
    if (video_buffered == 0 && end_of_file) {
        audio_player_end_stream();
        ESP_LOGI(TAG, "=== VIDEO END: frame=%d ===", current_frame);
        return true;
    }

    // Calculate which frame should be displayed based on wall clock
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - playback_start_time_us) / 1000);
    int expected_frame = (int)(elapsed_ms / frame_duration_ms);

    // If we're ahead of schedule, wait
    if (current_frame > expected_frame) {
        return false;
    }

    // No frames buffered? Wait for more
    if (video_buffered == 0) {
        return false;
    }

    // Get the next buffered frame
    buffered_frame_t* frame = &video_frames[video_read_idx];
    uint8_t* frame_data = video_buffer_slot(video_read_idx);

    // Skip frames if we're behind (drop frames to catch up)
    while (frame->frame_index < expected_frame && video_buffered > 1) {
        // Drop this frame
        video_read_idx = (video_read_idx + 1) % VIDEO_BUFFER_FRAMES;
        video_buffered--;
        current_frame = frame->frame_index + 1;

        frame = &video_frames[video_read_idx];
        frame_data = video_buffer_slot(video_read_idx);
    }

    int64_t t0 = esp_timer_get_time();

    // Decode MJPEG frame
    uint8_t* bgr_out = NULL;
    int width = 0, height = 0;
    esp_err_t ret = mjpeg_decoder_decode(frame_data, frame->size, &bgr_out, &width, &height);

    // Consume the frame from buffer
    video_read_idx = (video_read_idx + 1) % VIDEO_BUFFER_FRAMES;
    video_buffered--;

    int64_t t1 = esp_timer_get_time();

    if (ret == ESP_OK && bgr_out) {
        current_frame = frame->frame_index + 1;

        // Copy to framebuffer
        mjpeg_copy_to_framebuffer(bgr_out, fb_pixels, width, height, 800);

        int64_t t2 = esp_timer_get_time();

        // Timing stats
        timing_decode_us += (t1 - t0);
        timing_copy_us += (t2 - t1);
        timing_frame_count++;

        if (timing_frame_count >= 30) {
            uint32_t audio_pos = audio_player_get_position_ms();
            int64_t video_pos_ms = current_frame * frame_duration_ms;

            ESP_LOGI(TAG, "Timing (avg 30): Decode=%.1fms Copy=%.1fms | Buf=%d",
                     timing_decode_us / 30000.0f, timing_copy_us / 30000.0f, video_buffered);
            ESP_LOGI(TAG, "Sync: wall=%lums audio=%lums video=%lldms frame=%d",
                     (unsigned long)elapsed_ms, (unsigned long)audio_pos, video_pos_ms, current_frame);
            timing_decode_us = 0;
            timing_copy_us = 0;
            timing_frame_count = 0;
        }
    }

    return false;
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

                if (video_ended) {
                    // Video finished - stop playback
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

        // Timing for vsync and blit (only during playback)
        static uint32_t timing_vsync_us = 0;
        static uint32_t timing_blit_us = 0;
        static uint32_t timing_loop_count = 0;

        int64_t tv0 = esp_timer_get_time();

        // Wait for vsync
        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
        }

        int64_t tv1 = esp_timer_get_time();

        // Blit to display
        blit();

        int64_t tv2 = esp_timer_get_time();

        // Log vsync/blit timing during playback
        if (app_state == APP_STATE_PLAYING) {
            timing_vsync_us += (tv1 - tv0);
            timing_blit_us += (tv2 - tv1);
            timing_loop_count++;

            if (timing_loop_count >= 100) {
                ESP_LOGI(TAG, "Loop timing (avg of 100): Vsync=%.1fms, Blit=%.1fms",
                         timing_vsync_us / 100000.0f,
                         timing_blit_us / 100000.0f);
                timing_vsync_us = 0;
                timing_blit_us = 0;
                timing_loop_count = 0;
            }
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
