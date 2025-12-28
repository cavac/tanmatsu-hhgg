// LCARS-style video menu interface
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "media_loader.h"

// Menu state
typedef struct {
    playlist_t* playlist;       // Loaded playlist
    int selected_index;         // Currently selected video
    int scroll_offset;          // For scrolling if many videos
    bool needs_redraw;          // Flag to trigger redraw
} ui_menu_state_t;

// Initialize menu with a playlist
void ui_menu_init(ui_menu_state_t* state, playlist_t* playlist);

// Draw the menu to framebuffer
void ui_menu_draw(ui_menu_state_t* state, uint8_t* fb, int fb_stride, int fb_height);

// Handle input, returns true if a video was selected
// selected_entry will be set to the chosen video entry
bool ui_menu_handle_input(ui_menu_state_t* state, bool up, bool down, bool enter,
                          video_entry_t** selected_entry);

// Format duration as MM:SS string
void ui_format_duration(int seconds, char* buf, int buf_size);
