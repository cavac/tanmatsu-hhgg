// Audio Player - AAC decode and I2S output
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "media_loader.h"

// Initialize audio subsystem
esp_err_t audio_player_init(void);

// Start playing audio from a preloaded media stream
// Starts a dedicated audio task on Core 1
esp_err_t audio_player_start(preloaded_media_t* media);

// Stop audio playback
void audio_player_stop(void);

// Check if audio is currently playing
bool audio_player_is_playing(void);

// Get current playback position in milliseconds (for A/V sync)
uint32_t audio_player_get_position_ms(void);

// Set volume (0-100)
void audio_player_set_volume(int volume);

// Deinitialize audio subsystem
void audio_player_deinit(void);
