// Media Loader - JSON playlist parsing

#include "media_loader.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "media_loader";

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
