// Audio Player - AAC decode and I2S output
// Runs on Core 1 for parallel audio/video processing

#include "audio_player.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_aac_dec.h"
#include <string.h>

static const char* TAG = "audio_player";

// External declaration for bsp_audio_initialize (not in header but implemented for Tanmatsu)
extern esp_err_t bsp_audio_initialize(uint32_t rate);

// Audio task configuration
#define AUDIO_TASK_STACK_SIZE   (16 * 1024)
#define AUDIO_TASK_PRIORITY     6
#define AUDIO_TASK_CORE         1

// Sample rate (BSP actual rate is 44.1kHz, will be updated from decoder)
#define AUDIO_SAMPLE_RATE       44100

// Buffer sizes
#define PCM_BUFFER_SAMPLES      1024  // Per channel
#define PCM_BUFFER_SIZE         (PCM_BUFFER_SAMPLES * 2 * sizeof(int16_t))  // Stereo 16-bit

// Audio state
static i2s_chan_handle_t i2s_tx_handle = NULL;
static TaskHandle_t audio_task_handle = NULL;
static volatile bool audio_playing = false;
static volatile bool audio_stop_requested = false;
static volatile uint64_t samples_written = 0;
static preloaded_media_t* current_media = NULL;
static SemaphoreHandle_t audio_mutex = NULL;

// Timing for A/V sync debugging
static int64_t audio_start_time_us = 0;
static uint32_t i2s_timeout_count = 0;
static volatile uint32_t actual_sample_rate = AUDIO_SAMPLE_RATE;  // Updated from decoder

// PCM buffer in internal SRAM for DMA
static DRAM_ATTR int16_t pcm_buffer[PCM_BUFFER_SAMPLES * 2] __attribute__((aligned(16)));

// Forward declarations
static void audio_task(void* arg);

esp_err_t audio_player_init(void) {
    // Create mutex for thread safety
    audio_mutex = xSemaphoreCreateMutex();
    if (!audio_mutex) {
        ESP_LOGE(TAG, "Failed to create audio mutex");
        return ESP_ERR_NO_MEM;
    }

    // Try to get existing I2S handle first (BSP may have already initialized it)
    esp_err_t ret = bsp_audio_get_i2s_handle(&i2s_tx_handle);
    if (ret != ESP_OK || !i2s_tx_handle) {
        // Not initialized yet, try to initialize at 44.1kHz
        ESP_LOGI(TAG, "Initializing BSP audio at 44.1kHz");
        ret = bsp_audio_initialize(44100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize BSP audio: %s", esp_err_to_name(ret));
            return ret;
        }

        // Get I2S handle after init
        ret = bsp_audio_get_i2s_handle(&i2s_tx_handle);
        if (ret != ESP_OK || !i2s_tx_handle) {
            ESP_LOGE(TAG, "Failed to get I2S handle: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Using existing BSP audio I2S handle");
    }

    // Enable amplifier
    bsp_audio_set_amplifier(true);

    // Set hardware volume to max (software handles attenuation)
    bsp_audio_set_volume(100.0f);

    ESP_LOGI(TAG, "Audio player initialized");
    return ESP_OK;
}

esp_err_t audio_player_start(preloaded_media_t* media) {
    if (!media || !media->audio_data || media->audio_size == 0) {
        ESP_LOGE(TAG, "Invalid media for audio playback");
        return ESP_ERR_INVALID_ARG;
    }

    if (audio_playing) {
        ESP_LOGW(TAG, "Audio already playing, stopping first");
        audio_player_stop();
    }

    xSemaphoreTake(audio_mutex, portMAX_DELAY);

    current_media = media;
    media->audio_pos = 0;  // Reset audio position
    samples_written = 0;
    audio_stop_requested = false;
    audio_playing = true;
    audio_start_time_us = esp_timer_get_time();
    i2s_timeout_count = 0;

    xSemaphoreGive(audio_mutex);

    // Just enable amplifier - BSP manages I2S channel state
    bsp_audio_set_amplifier(true);

    // Create audio task on Core 1
    BaseType_t result = xTaskCreatePinnedToCore(
        audio_task,
        "audio_player",
        AUDIO_TASK_STACK_SIZE,
        media,
        AUDIO_TASK_PRIORITY,
        &audio_task_handle,
        AUDIO_TASK_CORE
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        audio_playing = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio playback started (size: %zu bytes)", media->audio_size);
    return ESP_OK;
}

void audio_player_stop(void) {
    // If task is still running, stop it
    if (audio_playing || audio_task_handle) {
        ESP_LOGI(TAG, "Stopping audio playback");

        // Signal task to stop
        audio_stop_requested = true;

        // Wait for task to finish (with timeout)
        int timeout_ms = 500;
        while (audio_playing && timeout_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout_ms -= 10;
        }

        // Force delete if still running
        if (audio_task_handle && audio_playing) {
            ESP_LOGW(TAG, "Force deleting audio task");
            vTaskDelete(audio_task_handle);
        }

        audio_task_handle = NULL;
        audio_playing = false;
        current_media = NULL;
    }

    // Always flush I2S with silence to stop any residual audio
    // (needed even if task already finished naturally)
    if (i2s_tx_handle) {
        memset(pcm_buffer, 0, PCM_BUFFER_SIZE);
        size_t bytes_written = 0;
        // Write silence multiple times to flush DMA buffer
        for (int i = 0; i < 4; i++) {
            i2s_channel_write(i2s_tx_handle, pcm_buffer, PCM_BUFFER_SIZE,
                              &bytes_written, pdMS_TO_TICKS(20));
        }
    }

    // Always turn off amplifier
    bsp_audio_set_amplifier(false);
}

bool audio_player_is_playing(void) {
    return audio_playing;
}

uint32_t audio_player_get_position_ms(void) {
    // Convert samples to milliseconds using actual decoder sample rate
    return (uint32_t)((samples_written * 1000ULL) / actual_sample_rate);
}

void audio_player_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    bsp_audio_set_volume((float)volume);
}

void audio_player_deinit(void) {
    // Stop any ongoing playback
    audio_player_stop();

    // Disable amplifier
    bsp_audio_set_amplifier(false);

    // Clean up mutex
    if (audio_mutex) {
        vSemaphoreDelete(audio_mutex);
        audio_mutex = NULL;
    }

    ESP_LOGI(TAG, "Audio player deinitialized");
}

// Audio decode and playback task
static void audio_task(void* arg) {
    preloaded_media_t* media = (preloaded_media_t*)arg;
    void* aac_decoder = NULL;
    esp_audio_err_t ret;

    ESP_LOGI(TAG, "Audio task started");

    // Create AAC decoder (NULL config for ADTS data)
    ret = esp_aac_dec_open(NULL, 0, &aac_decoder);
    if (ret != ESP_AUDIO_ERR_OK || !aac_decoder) {
        ESP_LOGE(TAG, "Failed to create AAC decoder: %d", ret);
        goto cleanup;
    }

    // Allocate output frame buffer
    uint8_t* frame_buffer = heap_caps_malloc(8192, MALLOC_CAP_DEFAULT);
    if (!frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        goto cleanup;
    }

    // Frame counting diagnostics
    uint32_t frames_parsed = 0;
    uint32_t frames_decoded = 0;
    uint32_t frames_errors = 0;
    uint32_t samples_lost = 0;  // From partial I2S writes

    // Main decode loop
    while (!audio_stop_requested) {
        // Get next AAC-ADTS frame from preloaded media
        size_t frame_size = 0;
        uint8_t* aac_frame = stream_next_audio_frame(media, &frame_size);

        if (!aac_frame || frame_size == 0) {
            // End of audio stream - log final timing and diagnostics
            int64_t elapsed_us = esp_timer_get_time() - audio_start_time_us;
            int64_t elapsed_ms = elapsed_us / 1000;
            int64_t audio_pos_ms = (samples_written * 1000) / actual_sample_rate;
            ESP_LOGI(TAG, "=== AUDIO END: elapsed=%lldms, audio_pos=%lldms, samples=%llu ===",
                     elapsed_ms, audio_pos_ms, (unsigned long long)samples_written);
            ESP_LOGI(TAG, "=== AUDIO STATS: parsed=%lu, decoded=%lu, errors=%lu, samples_lost=%lu ===",
                     (unsigned long)frames_parsed, (unsigned long)frames_decoded,
                     (unsigned long)frames_errors, (unsigned long)samples_lost);
            break;
        }

        frames_parsed++;

        // Prepare decode input
        esp_audio_dec_in_raw_t raw = {
            .buffer = aac_frame,
            .len = frame_size,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };

        // Prepare decode output
        esp_audio_dec_out_frame_t frame = {
            .buffer = frame_buffer,
            .len = 8192,
            .needed_size = 0,
            .decoded_size = 0,
        };

        // Decode AAC frame to PCM
        esp_audio_dec_info_t dec_info = {0};
        ret = esp_aac_dec_decode(aac_decoder, &raw, &frame, &dec_info);

        if (ret == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
            frames_decoded++;

            // Log format info once and capture actual sample rate
            static bool format_logged = false;
            if (!format_logged) {
                ESP_LOGI(TAG, "Audio format: %d Hz, %d ch, %d bits",
                         dec_info.sample_rate, dec_info.channel, dec_info.bits_per_sample);
                if (dec_info.sample_rate > 0) {
                    actual_sample_rate = dec_info.sample_rate;
                }
                format_logged = true;
            }

            // Copy to aligned PCM buffer with 80% software volume
            size_t copy_size = frame.decoded_size;
            if (copy_size > PCM_BUFFER_SIZE) {
                copy_size = PCM_BUFFER_SIZE;
            }

            // Apply 50% software volume (divide by 2)
            int16_t* src = (int16_t*)frame_buffer;
            int16_t* dst = pcm_buffer;
            size_t num_samples = copy_size / 2;
            for (size_t i = 0; i < num_samples; i++) {
                dst[i] = src[i] >> 1;
            }

            // Write PCM data to I2S with blocking wait
            // Use portMAX_DELAY to ensure all samples are written
            size_t bytes_written = 0;
            esp_err_t i2s_ret = i2s_channel_write(i2s_tx_handle, pcm_buffer, copy_size,
                                                   &bytes_written, portMAX_DELAY);

            samples_written += bytes_written / 4;

            // Track lost samples (should be 0 with portMAX_DELAY)
            if (bytes_written < copy_size) {
                samples_lost += (copy_size - bytes_written) / 4;
            }

            if (i2s_ret != ESP_OK) {
                i2s_timeout_count++;
            }

            // Log audio sync debug info every ~3 seconds
            static uint64_t last_debug_samples = 0;
            if (samples_written - last_debug_samples >= actual_sample_rate * 3) {
                int64_t elapsed_us = esp_timer_get_time() - audio_start_time_us;
                int64_t elapsed_ms = elapsed_us / 1000;
                int64_t audio_pos_ms = (samples_written * 1000) / actual_sample_rate;
                int64_t drift_ms = audio_pos_ms - elapsed_ms;
                ESP_LOGI(TAG, "Audio sync: elapsed=%lldms, audio_pos=%lldms, drift=%+lldms, timeouts=%lu",
                         elapsed_ms, audio_pos_ms, drift_ms, (unsigned long)i2s_timeout_count);
                last_debug_samples = samples_written;
            }
        } else if (ret != ESP_AUDIO_ERR_OK) {
            frames_errors++;
            ESP_LOGW(TAG, "AAC decode error: %d (frame_size=%zu)", ret, frame_size);
            // Continue to next frame on decode error
        }
    }

    heap_caps_free(frame_buffer);

cleanup:
    // Close AAC decoder
    if (aac_decoder) {
        esp_aac_dec_close(aac_decoder);
    }

    ESP_LOGI(TAG, "Audio task ending (samples written: %llu)", (unsigned long long)samples_written);

    audio_playing = false;
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}
