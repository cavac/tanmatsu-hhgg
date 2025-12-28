// AVI Parser - RIFF/AVI container parsing for MJPEG video (file streaming)

#include "avi_parser.h"
#include "fastopen.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char* TAG = "avi_parser";

// Maximum size for a single video frame (100KB should be plenty for MJPEG)
#define MAX_FRAME_SIZE (100 * 1024)

// Buffer size for reading AVI headers
#define HEADER_BUFFER_SIZE (16 * 1024)

// RIFF/AVI chunk IDs (little-endian FourCC)
#define FOURCC_RIFF 0x46464952  // "RIFF"
#define FOURCC_AVI  0x20495641  // "AVI "
#define FOURCC_LIST 0x5453494C  // "LIST"
#define FOURCC_HDRL 0x6C726468  // "hdrl"
#define FOURCC_MOVI 0x69766F6D  // "movi"
#define FOURCC_AVIH 0x68697661  // "avih"
#define FOURCC_STRH 0x68727473  // "strh"
#define FOURCC_STRF 0x66727473  // "strf"
#define FOURCC_VIDS 0x73646976  // "vids"
#define FOURCC_AUDS 0x73647561  // "auds"

// Read a 32-bit little-endian value
static inline uint32_t read_u32_le(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// Read a 16-bit little-endian value
static inline uint16_t read_u16_le(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

// Read bytes from file at current position
static bool read_bytes(FILE* f, uint8_t* buf, size_t count) {
    return fread(buf, 1, count, f) == count;
}

// Parse AVI main header (avih chunk)
static void parse_avih(const uint8_t* data, size_t size, avi_info_t* info) {
    if (size < 56) return;

    uint32_t us_per_frame = read_u32_le(data);
    info->fps = us_per_frame > 0 ? 1000000 / us_per_frame : 30;
    info->width = read_u32_le(data + 32);
    info->height = read_u32_le(data + 36);
    info->video_frames = read_u32_le(data + 16);

    ESP_LOGI(TAG, "AVI header: %lux%lu @ %lu fps, %lu frames",
             (unsigned long)info->width, (unsigned long)info->height,
             (unsigned long)info->fps, (unsigned long)info->video_frames);
}

// Parse stream header (strh chunk)
static void parse_strh(const uint8_t* data, size_t size, avi_info_t* info,
                       bool* is_video, bool* is_audio) {
    if (size < 48) return;

    uint32_t fcc_type = read_u32_le(data);

    if (fcc_type == FOURCC_VIDS) {
        *is_video = true;
        info->has_video = true;
        uint32_t scale = read_u32_le(data + 20);
        uint32_t rate = read_u32_le(data + 24);
        if (scale > 0) {
            info->fps = rate / scale;
        }
        ESP_LOGI(TAG, "Video stream: rate=%lu scale=%lu fps=%lu",
                 (unsigned long)rate, (unsigned long)scale, (unsigned long)info->fps);
    } else if (fcc_type == FOURCC_AUDS) {
        *is_audio = true;
        info->has_audio = true;
        ESP_LOGI(TAG, "Audio stream found");
    }
}

// Parse stream format (strf chunk) for video
static void parse_strf_video(const uint8_t* data, size_t size, avi_info_t* info) {
    if (size < 40) return;

    uint32_t width = read_u32_le(data + 4);
    uint32_t height = read_u32_le(data + 8);

    if ((int32_t)height < 0) {
        height = (uint32_t)(-(int32_t)height);
    }

    info->width = width;
    info->height = height;

    char fourcc[5] = {0};
    memcpy(fourcc, data + 16, 4);
    ESP_LOGI(TAG, "Video format: %lux%lu codec=%s",
             (unsigned long)width, (unsigned long)height, fourcc);
}

// Parse header list from buffer
static void parse_hdrl_buffer(const uint8_t* buffer, size_t size, avi_info_t* info) {
    bool current_is_video = false;
    bool current_is_audio = false;
    size_t offset = 0;

    while (offset + 8 <= size) {
        uint32_t chunk_id = read_u32_le(buffer + offset);
        uint32_t chunk_size = read_u32_le(buffer + offset + 4);

        if (chunk_id == FOURCC_AVIH) {
            parse_avih(buffer + offset + 8, chunk_size, info);
        } else if (chunk_id == FOURCC_LIST) {
            offset += 12;
            continue;
        } else if (chunk_id == FOURCC_STRH) {
            parse_strh(buffer + offset + 8, chunk_size, info,
                      &current_is_video, &current_is_audio);
        } else if (chunk_id == FOURCC_STRF) {
            if (current_is_video) {
                parse_strf_video(buffer + offset + 8, chunk_size, info);
            }
            current_is_video = false;
            current_is_audio = false;
        }

        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }
}

// Parse AVI headers and find movi list
static esp_err_t parse_avi_headers(avi_parser_t* parser) {
    uint8_t header[12];

    // Read and verify RIFF header
    fseek(parser->file, 0, SEEK_SET);
    if (!read_bytes(parser->file, header, 12)) {
        ESP_LOGE(TAG, "Failed to read RIFF header");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t riff_id = read_u32_le(header);
    uint32_t riff_size = read_u32_le(header + 4);
    uint32_t avi_id = read_u32_le(header + 8);

    if (riff_id != FOURCC_RIFF || avi_id != FOURCC_AVI) {
        ESP_LOGE(TAG, "Not a valid AVI file");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "AVI file: %zu bytes (RIFF size: %lu)",
             parser->file_size, (unsigned long)riff_size);

    // Scan for hdrl and movi lists
    size_t offset = 12;
    uint8_t chunk_header[12];

    while (offset + 12 <= parser->file_size) {
        fseek(parser->file, offset, SEEK_SET);
        if (!read_bytes(parser->file, chunk_header, 12)) {
            break;
        }

        uint32_t chunk_id = read_u32_le(chunk_header);
        uint32_t chunk_size = read_u32_le(chunk_header + 4);

        if (chunk_id == FOURCC_LIST) {
            uint32_t list_type = read_u32_le(chunk_header + 8);

            if (list_type == FOURCC_HDRL) {
                // Read header list into temporary buffer and parse
                size_t hdrl_size = chunk_size - 4;
                if (hdrl_size > HEADER_BUFFER_SIZE) {
                    hdrl_size = HEADER_BUFFER_SIZE;
                }

                uint8_t* hdrl_buffer = malloc(hdrl_size);
                if (hdrl_buffer) {
                    fseek(parser->file, offset + 12, SEEK_SET);
                    if (read_bytes(parser->file, hdrl_buffer, hdrl_size)) {
                        parse_hdrl_buffer(hdrl_buffer, hdrl_size, &parser->info);
                    }
                    free(hdrl_buffer);
                }
            } else if (list_type == FOURCC_MOVI) {
                // Found movi list
                parser->movi_start = offset + 12;
                parser->movi_end = offset + 8 + chunk_size;
                parser->current_pos = parser->movi_start;

                ESP_LOGI(TAG, "Found movi list: offset=%zu size=%zu",
                         parser->movi_start, parser->movi_end - parser->movi_start);

                ESP_LOGI(TAG, "AVI parsed: %lux%lu @ %lu fps, video=%d audio=%d",
                         (unsigned long)parser->info.width,
                         (unsigned long)parser->info.height,
                         (unsigned long)parser->info.fps,
                         parser->info.has_video, parser->info.has_audio);
                return ESP_OK;
            }
        }

        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }

    ESP_LOGE(TAG, "movi list not found");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t avi_parser_open(avi_parser_t* parser, const char* path) {
    if (!parser || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(parser, 0, sizeof(avi_parser_t));

    // Open file with fast I/O
    parser->file = fastopen(path, "rb");
    if (!parser->file) {
        ESP_LOGE(TAG, "Failed to open AVI file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(parser->file, 0, SEEK_END);
    parser->file_size = ftell(parser->file);
    fseek(parser->file, 0, SEEK_SET);

    // Allocate frame buffer in PSRAM
    parser->frame_buffer_size = MAX_FRAME_SIZE;
    parser->frame_buffer = heap_caps_malloc(parser->frame_buffer_size, MALLOC_CAP_SPIRAM);
    if (!parser->frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%zu bytes)", parser->frame_buffer_size);
        fastclose(parser->file);
        parser->file = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Parse headers
    esp_err_t ret = parse_avi_headers(parser);
    if (ret != ESP_OK) {
        heap_caps_free(parser->frame_buffer);
        parser->frame_buffer = NULL;
        fastclose(parser->file);
        parser->file = NULL;
        return ret;
    }

    return ESP_OK;
}

const avi_info_t* avi_parser_get_info(const avi_parser_t* parser) {
    return &parser->info;
}

esp_err_t avi_parser_next_chunk(avi_parser_t* parser, avi_chunk_t* chunk) {
    if (!parser || !chunk || !parser->file) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t chunk_header[8];

    while (parser->current_pos + 8 <= parser->movi_end) {
        // Seek to current position and read chunk header
        fseek(parser->file, parser->current_pos, SEEK_SET);
        if (!read_bytes(parser->file, chunk_header, 8)) {
            ESP_LOGE(TAG, "Failed to read chunk header at %zu", parser->current_pos);
            break;
        }

        uint32_t chunk_id = read_u32_le(chunk_header);
        uint32_t chunk_size = read_u32_le(chunk_header + 4);

        // Sanity check chunk size
        if (chunk_size > parser->frame_buffer_size) {
            ESP_LOGW(TAG, "Chunk too large: %lu bytes (max %zu), skipping",
                     (unsigned long)chunk_size, parser->frame_buffer_size);
            parser->current_pos += 8 + chunk_size;
            if (chunk_size & 1) parser->current_pos++;
            continue;
        }

        // Identify chunk type
        uint8_t type_hi = (chunk_id >> 16) & 0xFF;
        uint8_t type_lo = (chunk_id >> 24) & 0xFF;

        // Video: xxdc (compressed) or xxdb (uncompressed)
        if (type_hi == 'd' && (type_lo == 'c' || type_lo == 'b')) {
            // Read video chunk data
            if (!read_bytes(parser->file, parser->frame_buffer, chunk_size)) {
                ESP_LOGE(TAG, "Failed to read video chunk data");
                break;
            }

            chunk->type = AVI_CHUNK_VIDEO;
            chunk->data = parser->frame_buffer;
            chunk->size = chunk_size;

            // Move to next chunk
            parser->current_pos += 8 + chunk_size;
            if (chunk_size & 1) parser->current_pos++;

            return ESP_OK;
        }

        // Audio: xxwb (wave bytes)
        if (type_hi == 'w' && type_lo == 'b') {
            // Read audio chunk data
            if (!read_bytes(parser->file, parser->frame_buffer, chunk_size)) {
                ESP_LOGE(TAG, "Failed to read audio chunk data");
                break;
            }

            chunk->type = AVI_CHUNK_AUDIO;
            chunk->data = parser->frame_buffer;
            chunk->size = chunk_size;

            // Move to next chunk
            parser->current_pos += 8 + chunk_size;
            if (chunk_size & 1) parser->current_pos++;

            return ESP_OK;
        }

        // Skip other chunks (index, etc.)
        parser->current_pos += 8 + chunk_size;
        if (chunk_size & 1) parser->current_pos++;
    }

    chunk->type = AVI_CHUNK_END;
    chunk->data = NULL;
    chunk->size = 0;
    return ESP_ERR_NOT_FOUND;
}

void avi_parser_rewind(avi_parser_t* parser) {
    if (parser) {
        parser->current_pos = parser->movi_start;
    }
}

void avi_parser_close(avi_parser_t* parser) {
    if (!parser) return;

    if (parser->frame_buffer) {
        heap_caps_free(parser->frame_buffer);
        parser->frame_buffer = NULL;
    }

    if (parser->file) {
        fastclose(parser->file);
        parser->file = NULL;
    }

    parser->file_size = 0;
    parser->movi_start = 0;
    parser->movi_end = 0;
    parser->current_pos = 0;
}

int avi_parser_get_progress(const avi_parser_t* parser) {
    if (!parser || parser->movi_end <= parser->movi_start) {
        return 0;
    }

    size_t total = parser->movi_end - parser->movi_start;
    size_t current = parser->current_pos - parser->movi_start;

    return (int)((current * 100) / total);
}
