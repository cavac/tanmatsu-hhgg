// Media Loader - JSON playlist parsing, PSRAM preload, stream parsing
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Maximum values
#define MAX_VIDEOS          16
#define MAX_DISPLAY_NAME    64
#define MAX_FILENAME        32
#define MAX_TITLE           64

// Video entry from playlist
typedef struct {
    char id[MAX_FILENAME];
    char display_name[MAX_DISPLAY_NAME];
    char video_file[MAX_FILENAME];
    char audio_file[MAX_FILENAME];
    int duration_sec;
} video_entry_t;

// Playlist structure
typedef struct {
    char title[MAX_TITLE];
    video_entry_t videos[MAX_VIDEOS];
    int video_count;
} playlist_t;

// Preloaded media in PSRAM
typedef struct {
    // Video data in PSRAM
    uint8_t* video_data;
    size_t video_size;
    size_t video_pos;       // Current read position

    // Audio data in PSRAM
    uint8_t* audio_data;
    size_t audio_size;
    size_t audio_pos;       // Current read position

    // Metadata
    int duration_sec;
    char display_name[MAX_DISPLAY_NAME];
} preloaded_media_t;

// === Playlist functions ===

// Load playlist from JSON file
esp_err_t playlist_load(const char* json_path, playlist_t* playlist);

// Free playlist resources (not needed if using static allocation)
void playlist_free(playlist_t* playlist);

// === Media preload functions ===

// Preload video and audio files from SD card to PSRAM
// base_path: directory containing the files (e.g., "/sd/at.cavac.hhgg")
// entry: video entry from playlist
// media: output structure with preloaded data
esp_err_t media_preload(const char* base_path, const video_entry_t* entry,
                        preloaded_media_t* media);

// Free preloaded media (releases PSRAM)
void media_unload(preloaded_media_t* media);

// === Stream parsing functions (read from PSRAM) ===

// Get next H.264 NAL unit (returns pointer into PSRAM, no copy)
// Returns NULL at end of stream
uint8_t* stream_next_video_nal(preloaded_media_t* media, size_t* nal_size);

// Get next AAC-ADTS frame (returns pointer into PSRAM, no copy)
// Returns NULL at end of stream
uint8_t* stream_next_audio_frame(preloaded_media_t* media, size_t* frame_size);

// Reset stream positions to beginning
void stream_rewind(preloaded_media_t* media);

// Get current video position (frame number estimate)
int stream_get_video_frame_num(preloaded_media_t* media);

// Get current audio position in milliseconds (estimate)
int stream_get_audio_position_ms(preloaded_media_t* media);
