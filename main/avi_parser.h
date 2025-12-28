// AVI Parser - RIFF/AVI container parsing for MJPEG video (file streaming)
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"

// AVI stream info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t video_frames;
    uint32_t audio_sample_rate;
    uint16_t audio_channels;
    uint16_t audio_bits_per_sample;
    bool has_video;
    bool has_audio;
} avi_info_t;

// AVI chunk types
typedef enum {
    AVI_CHUNK_VIDEO,    // 00dc - compressed video
    AVI_CHUNK_AUDIO,    // 01wb - audio data
    AVI_CHUNK_OTHER,    // Other chunks (ignored)
    AVI_CHUNK_END,      // End of movi list
} avi_chunk_type_t;

// AVI chunk descriptor
typedef struct {
    avi_chunk_type_t type;
    uint8_t* data;      // Pointer into frame buffer
    size_t size;        // Chunk data size
} avi_chunk_t;

// AVI parser state (for streaming from file)
typedef struct {
    FILE* file;             // Open file handle
    size_t file_size;       // Total file size
    size_t movi_start;      // Start of movi list data (file offset)
    size_t movi_end;        // End of movi list
    size_t current_pos;     // Current position in file
    uint8_t* frame_buffer;  // Buffer for reading chunks
    size_t frame_buffer_size;
    avi_info_t info;        // Parsed stream info
} avi_parser_t;

// Initialize parser with AVI file path
// Opens file with fastopen for optimal SD card performance
// Parses headers and locates movi list
esp_err_t avi_parser_open(avi_parser_t* parser, const char* path);

// Get stream info
const avi_info_t* avi_parser_get_info(const avi_parser_t* parser);

// Get next video chunk from movi list (skips audio chunks)
// Reads chunk into internal buffer
// Returns ESP_OK if chunk found, ESP_ERR_NOT_FOUND at end
esp_err_t avi_parser_next_chunk(avi_parser_t* parser, avi_chunk_t* chunk);

// Reset parser to beginning of movi list
void avi_parser_rewind(avi_parser_t* parser);

// Close file and free resources
void avi_parser_close(avi_parser_t* parser);

// Get current position estimate (for progress display)
// Returns percentage 0-100
int avi_parser_get_progress(const avi_parser_t* parser);
