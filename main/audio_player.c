// Audio Player - AAC decode and I2S output
// Runs on Core 1 for parallel audio/video processing

#include "audio_player.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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
        // Not initialized yet, try to initialize
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

    ESP_LOGI(TAG, "Audio player initialized (44.1kHz stereo)");
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

    xSemaphoreGive(audio_mutex);

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
    if (!audio_playing && !audio_task_handle) {
        return;
    }

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

    // Disable amplifier to stop any garbage audio
    bsp_audio_set_amplifier(false);

    // Small delay then re-enable for next playback
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_audio_set_amplifier(true);

    ESP_LOGI(TAG, "Audio playback stopped");
}

bool audio_player_is_playing(void) {
    return audio_playing;
}

uint32_t audio_player_get_position_ms(void) {
    // Convert samples to milliseconds (44.1kHz sample rate)
    return (uint32_t)((samples_written * 1000ULL) / 44100ULL);
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

    // Main decode loop
    while (!audio_stop_requested) {
        // Get next AAC-ADTS frame from preloaded media
        size_t frame_size = 0;
        uint8_t* aac_frame = stream_next_audio_frame(media, &frame_size);

        if (!aac_frame || frame_size == 0) {
            // End of audio stream
            ESP_LOGI(TAG, "End of audio stream");
            break;
        }

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
            // Log format info once
            static bool format_logged = false;
            if (!format_logged) {
                ESP_LOGI(TAG, "Audio format: %d Hz, %d ch, %d bits",
                         dec_info.sample_rate, dec_info.channel, dec_info.bits_per_sample);
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

            // Write PCM data to I2S
            size_t bytes_written = 0;
            esp_err_t i2s_ret = i2s_channel_write(i2s_tx_handle, pcm_buffer, copy_size,
                                                   &bytes_written, pdMS_TO_TICKS(100));

            if (i2s_ret == ESP_OK) {
                // Track samples for A/V sync (16-bit stereo = 4 bytes per sample)
                samples_written += bytes_written / 4;
            } else {
                ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(i2s_ret));
            }
        } else if (ret != ESP_AUDIO_ERR_OK) {
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
