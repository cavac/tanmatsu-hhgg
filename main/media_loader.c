// Media Loader - JSON playlist parsing, PSRAM preload, stream parsing

#include "media_loader.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "media_loader";

// === Playlist functions ===

esp_err_t playlist_load(const char* json_path, playlist_t* playlist) {
    memset(playlist, 0, sizeof(playlist_t));

    // Open and read the JSON file
    FILE* f = fopen(json_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open playlist: %s", json_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 64 * 1024) {
        ESP_LOGE(TAG, "Invalid playlist file size: %ld", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    // Read file content
    char* json_str = malloc(file_size + 1);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_size = fread(json_str, 1, file_size, f);
    fclose(f);
    json_str[read_size] = '\0';

    // Parse JSON
    cJSON* root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse playlist JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Get title
    cJSON* title = cJSON_GetObjectItem(root, "title");
    if (title && cJSON_IsString(title)) {
        strncpy(playlist->title, title->valuestring, MAX_TITLE - 1);
    } else {
        strcpy(playlist->title, "Video Player");
    }

    // Get videos array
    cJSON* videos = cJSON_GetObjectItem(root, "videos");
    if (!videos || !cJSON_IsArray(videos)) {
        ESP_LOGE(TAG, "No videos array in playlist");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int count = cJSON_GetArraySize(videos);
    if (count > MAX_VIDEOS) {
        ESP_LOGW(TAG, "Playlist has %d videos, limiting to %d", count, MAX_VIDEOS);
        count = MAX_VIDEOS;
    }

    playlist->video_count = count;

    for (int i = 0; i < count; i++) {
        cJSON* video = cJSON_GetArrayItem(videos, i);
        video_entry_t* entry = &playlist->videos[i];

        cJSON* id = cJSON_GetObjectItem(video, "id");
        cJSON* display_name = cJSON_GetObjectItem(video, "display_name");
        cJSON* video_file = cJSON_GetObjectItem(video, "video_file");
        cJSON* audio_file = cJSON_GetObjectItem(video, "audio_file");
        cJSON* duration = cJSON_GetObjectItem(video, "duration_sec");

        if (id && cJSON_IsString(id)) {
            strncpy(entry->id, id->valuestring, MAX_FILENAME - 1);
        }
        if (display_name && cJSON_IsString(display_name)) {
            strncpy(entry->display_name, display_name->valuestring, MAX_DISPLAY_NAME - 1);
        } else if (id && cJSON_IsString(id)) {
            strncpy(entry->display_name, id->valuestring, MAX_DISPLAY_NAME - 1);
        }
        if (video_file && cJSON_IsString(video_file)) {
            strncpy(entry->video_file, video_file->valuestring, MAX_FILENAME - 1);
        }
        if (audio_file && cJSON_IsString(audio_file)) {
            strncpy(entry->audio_file, audio_file->valuestring, MAX_FILENAME - 1);
        }
        if (duration && cJSON_IsNumber(duration)) {
            entry->duration_sec = duration->valueint;
        }

        ESP_LOGI(TAG, "Loaded video %d: %s (%ds)", i, entry->display_name, entry->duration_sec);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded playlist '%s' with %d videos", playlist->title, playlist->video_count);
    return ESP_OK;
}

void playlist_free(playlist_t* playlist) {
    // Static allocation, nothing to free
    memset(playlist, 0, sizeof(playlist_t));
}

// === Media preload functions ===

static esp_err_t load_file_to_psram(const char* path, uint8_t** data, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %s", path);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Loading %s (%ld bytes) to PSRAM...", path, file_size);

    // Allocate in PSRAM
    *data = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!*data) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes in PSRAM", file_size);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    // Read file in chunks for progress
    size_t total_read = 0;
    size_t chunk_size = 64 * 1024;  // 64KB chunks
    while (total_read < file_size) {
        size_t to_read = file_size - total_read;
        if (to_read > chunk_size) to_read = chunk_size;

        size_t read = fread(*data + total_read, 1, to_read, f);
        if (read == 0) {
            ESP_LOGE(TAG, "Read error at offset %zu", total_read);
            break;
        }
        total_read += read;
    }

    fclose(f);

    if (total_read != file_size) {
        ESP_LOGE(TAG, "Incomplete read: %zu of %ld bytes", total_read, file_size);
        heap_caps_free(*data);
        *data = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    *size = total_read;
    ESP_LOGI(TAG, "Loaded %zu bytes to PSRAM", total_read);
    return ESP_OK;
}

esp_err_t media_preload(const char* base_path, const video_entry_t* entry,
                        preloaded_media_t* media) {
    memset(media, 0, sizeof(preloaded_media_t));

    // Copy metadata
    snprintf(media->display_name, sizeof(media->display_name), "%s", entry->display_name);
    media->duration_sec = entry->duration_sec;

    // Build file paths
    char video_path[128];
    char audio_path[128];
    snprintf(video_path, sizeof(video_path), "%s/%s", base_path, entry->video_file);
    snprintf(audio_path, sizeof(audio_path), "%s/%s", base_path, entry->audio_file);

    // Load video file
    esp_err_t ret = load_file_to_psram(video_path, &media->video_data, &media->video_size);
    if (ret != ESP_OK) {
        return ret;
    }

    // Load audio file
    ret = load_file_to_psram(audio_path, &media->audio_data, &media->audio_size);
    if (ret != ESP_OK) {
        heap_caps_free(media->video_data);
        media->video_data = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Preloaded media: video=%zu bytes, audio=%zu bytes",
             media->video_size, media->audio_size);
    return ESP_OK;
}

void media_unload(preloaded_media_t* media) {
    if (media->video_data) {
        heap_caps_free(media->video_data);
        media->video_data = NULL;
    }
    if (media->audio_data) {
        heap_caps_free(media->audio_data);
        media->audio_data = NULL;
    }
    media->video_size = 0;
    media->audio_size = 0;
    media->video_pos = 0;
    media->audio_pos = 0;
}

// === Stream parsing functions ===

// Find next H.264 start code (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01)
static size_t find_next_start_code(uint8_t* data, size_t size, size_t start) {
    for (size_t i = start; i < size - 3; i++) {
        // Check for 4-byte start code
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            return i;
        }
        // Check for 3-byte start code
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            return i;
        }
    }
    return size;  // End of data
}

uint8_t* stream_next_video_nal(preloaded_media_t* media, size_t* nal_size) {
    if (!media->video_data || media->video_pos >= media->video_size) {
        return NULL;
    }

    // Skip to start code
    size_t start = find_next_start_code(media->video_data, media->video_size, media->video_pos);
    if (start >= media->video_size) {
        return NULL;
    }

    // Determine start code length (3 or 4 bytes)
    int start_code_len = 4;
    if (media->video_data[start+2] == 1) {
        start_code_len = 3;
    }

    // Find the next start code (or end of data)
    size_t end = find_next_start_code(media->video_data, media->video_size, start + start_code_len);

    // Return pointer to NAL unit (including start code)
    uint8_t* nal = media->video_data + start;
    *nal_size = end - start;

    // Update position for next call
    media->video_pos = end;

    return nal;
}

uint8_t* stream_next_audio_frame(preloaded_media_t* media, size_t* frame_size) {
    if (!media->audio_data || media->audio_pos >= media->audio_size) {
        return NULL;
    }

    uint8_t* frame = media->audio_data + media->audio_pos;

    // Verify ADTS sync word (0xFFF)
    if (frame[0] != 0xFF || (frame[1] & 0xF0) != 0xF0) {
        ESP_LOGE(TAG, "Invalid ADTS sync at offset %zu", media->audio_pos);
        // Try to find next sync word
        for (size_t i = media->audio_pos + 1; i < media->audio_size - 1; i++) {
            if (media->audio_data[i] == 0xFF && (media->audio_data[i+1] & 0xF0) == 0xF0) {
                media->audio_pos = i;
                frame = media->audio_data + i;
                break;
            }
        }
        if (frame[0] != 0xFF || (frame[1] & 0xF0) != 0xF0) {
            return NULL;
        }
    }

    // ADTS header: frame length in bits 30-43 (13 bits)
    // Bytes: [0]sync [1]sync [2]profile [3]rate+size [4]size [5]size+buffer [6]buffer
    uint16_t frame_len = ((frame[3] & 0x03) << 11) |
                         (frame[4] << 3) |
                         ((frame[5] & 0xE0) >> 5);

    if (frame_len < 7 || media->audio_pos + frame_len > media->audio_size) {
        ESP_LOGE(TAG, "Invalid ADTS frame length: %u at offset %zu", frame_len, media->audio_pos);
        return NULL;
    }

    *frame_size = frame_len;
    media->audio_pos += frame_len;

    return frame;
}

void stream_rewind(preloaded_media_t* media) {
    media->video_pos = 0;
    media->audio_pos = 0;
}

int stream_get_video_frame_num(preloaded_media_t* media) {
    // Rough estimate based on position (assumes ~10KB per frame at 1Mbps, 10fps)
    return media->video_pos / 10000;
}

int stream_get_audio_position_ms(preloaded_media_t* media) {
    // AAC at 44.1kHz, 1024 samples per frame
    // Each ADTS frame is about 23ms
    // Rough estimate based on position
    int estimated_frames = media->audio_pos / 400;  // ~400 bytes per AAC frame
    return estimated_frames * 23;  // 23ms per frame
}
