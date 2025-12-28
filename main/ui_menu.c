// LCARS-style video menu interface

#include "ui_menu.h"
#include "ui_draw.h"
#include "hershey_font.h"
#include <stdio.h>
#include <string.h>

// Layout constants (screen coordinates: 800x480)
#define HEADER_HEIGHT       60
#define FOOTER_HEIGHT       50
#define MENU_PADDING        40
#define ITEM_HEIGHT         70
#define ITEM_SPACING        10
#define ITEM_CORNER_RADIUS  15
#define LCARS_BAR_WIDTH     120

// Maximum visible items
#define MAX_VISIBLE_ITEMS   4

// Initialize menu with a playlist
void ui_menu_init(ui_menu_state_t* state, playlist_t* playlist) {
    state->playlist = playlist;
    state->selected_index = 0;
    state->scroll_offset = 0;
    state->needs_redraw = true;
}

// Format duration as MM:SS string
void ui_format_duration(int seconds, char* buf, int buf_size) {
    int mins = seconds / 60;
    int secs = seconds % 60;
    snprintf(buf, buf_size, "%02d:%02d", mins, secs);
}

// Draw the menu to framebuffer
void ui_menu_draw(ui_menu_state_t* state, uint8_t* fb, int fb_stride, int fb_height) {
    // Clear screen to black
    ui_clear(fb, fb_stride, fb_height, COLOR_BG);

    // Screen dimensions (as user sees it)
    int screen_w = 800;
    int screen_h = 480;

    // === Draw LCARS header ===
    // Left vertical bar
    ui_fill_rect(fb, fb_stride, fb_height, 0, 0, 30, screen_h, COLOR_ACCENT1);

    // Top horizontal bar with rounded left end
    ui_draw_lcars_bar(fb, fb_stride, fb_height, 30, 0, screen_w - 30, HEADER_HEIGHT - 10, COLOR_ACCENT1);

    // Small accent bars
    //ui_fill_rect(fb, fb_stride, fb_height, 35, HEADER_HEIGHT - 8, 80, 6, COLOR_ACCENT2);
    //ui_fill_rect(fb, fb_stride, fb_height, 120, HEADER_HEIGHT - 8, 40, 6, COLOR_ACCENT3);

    // Title text
    const char* title = state->playlist ? state->playlist->title : "VIDEO PLAYER";
    hershey_draw_string(fb, fb_stride, fb_height, 180, 15, title, 32,
                        COLOR_R(COLOR_TEXT), COLOR_G(COLOR_TEXT), COLOR_B(COLOR_TEXT));

    // === Draw menu items ===
    int menu_start_y = HEADER_HEIGHT + 20;
    int menu_item_x = MENU_PADDING + 20;
    int menu_item_w = screen_w - MENU_PADDING * 2 - 40;

    if (state->playlist && state->playlist->video_count > 0) {
        int visible_start = state->scroll_offset;
        int visible_end = visible_start + MAX_VISIBLE_ITEMS;
        if (visible_end > state->playlist->video_count) {
            visible_end = state->playlist->video_count;
        }

        for (int i = visible_start; i < visible_end; i++) {
            int item_y = menu_start_y + (i - visible_start) * (ITEM_HEIGHT + ITEM_SPACING);
            video_entry_t* entry = &state->playlist->videos[i];
            bool is_selected = (i == state->selected_index);

            // Draw item background
            uint32_t bg_color = is_selected ? COLOR_SELECTED : COLOR_ACCENT4;
            ui_fill_rounded_rect(fb, fb_stride, fb_height,
                                 menu_item_x, item_y, menu_item_w, ITEM_HEIGHT,
                                 ITEM_CORNER_RADIUS, bg_color);

            // Draw selection indicator
            if (is_selected) {
                // Arrow/play indicator
                int arrow_x = menu_item_x + 15;
                int arrow_y = item_y + ITEM_HEIGHT / 2;
                // Draw a simple triangle (play button shape)
                for (int dy = -12; dy <= 12; dy++) {
                    int line_len = 12 - abs(dy);
                    ui_draw_hline(fb, fb_stride, fb_height,
                                  arrow_x, arrow_y + dy, line_len, COLOR_BG);
                }

                // Accent bar on right edge
                ui_fill_rect(fb, fb_stride, fb_height,
                             menu_item_x + menu_item_w - 8, item_y + 5,
                             6, ITEM_HEIGHT - 10, COLOR_ACCENT1);
            }

            // Draw video name
            uint32_t text_color = is_selected ? COLOR_BG : COLOR_TEXT;
            hershey_draw_string(fb, fb_stride, fb_height,
                               menu_item_x + 45, item_y + 20,
                               entry->display_name, 28,
                               COLOR_R(text_color), COLOR_G(text_color), COLOR_B(text_color));

            // Draw duration
            char duration_str[16];
            ui_format_duration(entry->duration_sec, duration_str, sizeof(duration_str));
            int duration_width = hershey_string_width(duration_str, 22);
            hershey_draw_string(fb, fb_stride, fb_height,
                               menu_item_x + menu_item_w - duration_width - 25,
                               item_y + 25,
                               duration_str, 22,
                               COLOR_R(COLOR_DIM), COLOR_G(COLOR_DIM), COLOR_B(COLOR_DIM));
        }

        // Draw scroll indicators if needed
        if (state->scroll_offset > 0) {
            // Up arrow indicator
            hershey_draw_string(fb, fb_stride, fb_height,
                               screen_w / 2 - 10, menu_start_y - 15, "^", 20,
                               COLOR_R(COLOR_ACCENT2), COLOR_G(COLOR_ACCENT2), COLOR_B(COLOR_ACCENT2));
        }
        if (visible_end < state->playlist->video_count) {
            // Down arrow indicator
            int bottom_y = menu_start_y + MAX_VISIBLE_ITEMS * (ITEM_HEIGHT + ITEM_SPACING);
            hershey_draw_string(fb, fb_stride, fb_height,
                               screen_w / 2 - 10, bottom_y, "v", 20,
                               COLOR_R(COLOR_ACCENT2), COLOR_G(COLOR_ACCENT2), COLOR_B(COLOR_ACCENT2));
        }
    } else {
        // No videos message
        hershey_draw_string(fb, fb_stride, fb_height,
                           screen_w / 2 - 100, screen_h / 2,
                           "No videos found", 28,
                           COLOR_R(COLOR_DIM), COLOR_G(COLOR_DIM), COLOR_B(COLOR_DIM));
    }

    // === Draw LCARS footer ===
    // Bottom horizontal bar
    ui_draw_lcars_bar(fb, fb_stride, fb_height, 30, screen_h - FOOTER_HEIGHT + 10,
                      screen_w - 30, FOOTER_HEIGHT - 10, COLOR_ACCENT2);

    // Control hints
    hershey_draw_string(fb, fb_stride, fb_height, 180, screen_h - 35,
                       "SELECT: ENTER", 18,
                       COLOR_R(COLOR_TEXT), COLOR_G(COLOR_TEXT), COLOR_B(COLOR_TEXT));
    hershey_draw_string(fb, fb_stride, fb_height, 380, screen_h - 35,
                       "EXIT: ESC", 18,
                       COLOR_R(COLOR_TEXT), COLOR_G(COLOR_TEXT), COLOR_B(COLOR_TEXT));

    // Decorative accent bars in footer
    //ui_fill_rect(fb, fb_stride, fb_height, 550, screen_h - FOOTER_HEIGHT + 12, 60, 8, COLOR_ACCENT3);
    //ui_fill_rect(fb, fb_stride, fb_height, 620, screen_h - FOOTER_HEIGHT + 12, 30, 8, COLOR_ACCENT1);
    //ui_fill_rect(fb, fb_stride, fb_height, 660, screen_h - FOOTER_HEIGHT + 12, 100, 8, COLOR_ACCENT4);

    state->needs_redraw = false;
}

// Handle input, returns true if a video was selected
bool ui_menu_handle_input(ui_menu_state_t* state, bool up, bool down, bool enter,
                          video_entry_t** selected_entry) {
    if (!state->playlist || state->playlist->video_count == 0) {
        return false;
    }

    if (up && state->selected_index > 0) {
        state->selected_index--;
        // Adjust scroll if needed
        if (state->selected_index < state->scroll_offset) {
            state->scroll_offset = state->selected_index;
        }
        state->needs_redraw = true;
    }

    if (down && state->selected_index < state->playlist->video_count - 1) {
        state->selected_index++;
        // Adjust scroll if needed
        if (state->selected_index >= state->scroll_offset + MAX_VISIBLE_ITEMS) {
            state->scroll_offset = state->selected_index - MAX_VISIBLE_ITEMS + 1;
        }
        state->needs_redraw = true;
    }

    if (enter) {
        *selected_entry = &state->playlist->videos[state->selected_index];
        return true;
    }

    return false;
}
