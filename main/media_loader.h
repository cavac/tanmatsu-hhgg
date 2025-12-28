// Media Loader - JSON playlist parsing
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
    char video_file[MAX_FILENAME];  // AVI file with interleaved MJPEG video + MP3 audio
    int duration_sec;
} video_entry_t;

// Playlist structure
typedef struct {
    char title[MAX_TITLE];
    video_entry_t videos[MAX_VIDEOS];
    int video_count;
} playlist_t;

// Load playlist from JSON file
esp_err_t playlist_load(const char* json_path, playlist_t* playlist);

// Free playlist resources (not needed if using static allocation)
void playlist_free(playlist_t* playlist);
