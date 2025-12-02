// Pure C software 3D renderer for rotating cube
// Based on tinyrenderer concepts, rewritten in C for embedded systems

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "renderer.h"

static const char* TAG = "renderer";

// Constants
#define WIDTH 480
#define HEIGHT 480
#define NUM_CUBE_VERTS 8
#define NUM_CUBE_FACES 12

// 3D Vector types
typedef struct { float x, y, z; } vec3f;
typedef struct { float x, y, z, w; } vec4f;
typedef struct { float x, y; } vec2f;

// 4x4 Matrix (row-major)
typedef struct { float m[4][4]; } mat4f;

// Cube vertex data (unit cube centered at origin)
static const vec3f cube_verts[NUM_CUBE_VERTS] = {
    {-0.5f, -0.5f, -0.5f},  // 0
    {-0.5f,  0.5f, -0.5f},  // 1
    { 0.5f,  0.5f, -0.5f},  // 2
    { 0.5f, -0.5f, -0.5f},  // 3
    {-0.5f, -0.5f,  0.5f},  // 4
    {-0.5f,  0.5f,  0.5f},  // 5
    { 0.5f,  0.5f,  0.5f},  // 6
    { 0.5f, -0.5f,  0.5f},  // 7
};

// Cube faces (triangles, indices into cube_verts)
static const int cube_faces[NUM_CUBE_FACES][3] = {
    {2, 6, 7}, {2, 7, 3},  // Right face (+X)
    {0, 4, 5}, {0, 5, 1},  // Left face (-X)
    {6, 2, 1}, {6, 1, 5},  // Top face (+Y)
    {3, 7, 4}, {3, 4, 0},  // Bottom face (-Y)
    {7, 6, 5}, {7, 5, 4},  // Front face (+Z)
    {2, 3, 0}, {2, 0, 1},  // Back face (-Z)
};

// Global matrices
static mat4f ModelView;
static mat4f Perspective;
static mat4f Viewport;

// Z-buffer
static float* zbuffer = NULL;

// Current framebuffer pointer and stride (set per frame)
static uint8_t* current_framebuffer = NULL;
static int current_stride = 0;

// Parallel rendering infrastructure
#define RENDER_SPLIT_X (WIDTH / 2)  // Split at column 240

// Job data for parallel rasterization (single triangle)
typedef struct {
    vec2f screen[3];      // Screen coordinates
    vec4f ndc[3];         // NDC coordinates for z interpolation
    float inv_det;        // Precomputed 1/determinant
    int intensity;        // Flat shading intensity
    int xmin, xmax;       // Bounding box
    int ymin, ymax;
    bool valid;           // Whether this triangle passed culling
} RasterJob;

// Frame job - contains all triangles for the frame
typedef struct {
    RasterJob triangles[NUM_CUBE_FACES];
    int num_triangles;
} FrameJob;

// Worker task state
static TaskHandle_t worker_task_handle = NULL;
static SemaphoreHandle_t job_ready_sem = NULL;
static SemaphoreHandle_t job_done_sem = NULL;
static volatile FrameJob current_frame_job;
static volatile bool worker_should_exit = false;

// Vector operations
static inline vec3f vec3f_sub(vec3f a, vec3f b) {
    return (vec3f){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline vec3f vec3f_cross(vec3f a, vec3f b) {
    return (vec3f){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static inline float vec3f_dot(vec3f a, vec3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline float vec3f_length(vec3f v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static inline vec3f vec3f_normalize(vec3f v) {
    float len = vec3f_length(v);
    if (len > 0.0001f) {
        return (vec3f){v.x / len, v.y / len, v.z / len};
    }
    return v;
}

// Matrix operations
static mat4f mat4f_identity(void) {
    mat4f m = {{{0}}};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

static mat4f mat4f_mul(mat4f a, mat4f b) {
    mat4f result = {{{0}}};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 4; k++) {
                result.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return result;
}

static vec4f mat4f_mul_vec4(mat4f m, vec4f v) {
    return (vec4f){
        m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3] * v.w,
        m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3] * v.w,
        m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3] * v.w,
        m.m[3][0] * v.x + m.m[3][1] * v.y + m.m[3][2] * v.z + m.m[3][3] * v.w
    };
}

// Setup lookat matrix
static void setup_lookat(vec3f eye, vec3f center, vec3f up) {
    vec3f n = vec3f_normalize(vec3f_sub(eye, center));
    vec3f l = vec3f_normalize(vec3f_cross(up, n));
    vec3f m = vec3f_normalize(vec3f_cross(n, l));

    mat4f rotation = mat4f_identity();
    rotation.m[0][0] = l.x; rotation.m[0][1] = l.y; rotation.m[0][2] = l.z;
    rotation.m[1][0] = m.x; rotation.m[1][1] = m.y; rotation.m[1][2] = m.z;
    rotation.m[2][0] = n.x; rotation.m[2][1] = n.y; rotation.m[2][2] = n.z;

    mat4f translation = mat4f_identity();
    translation.m[0][3] = -center.x;
    translation.m[1][3] = -center.y;
    translation.m[2][3] = -center.z;

    ModelView = mat4f_mul(rotation, translation);
}

// Setup perspective matrix
static void setup_perspective(float f) {
    Perspective = mat4f_identity();
    Perspective.m[3][2] = -1.0f / f;
}

// Setup viewport matrix
static void setup_viewport(int x, int y, int w, int h) {
    Viewport = mat4f_identity();
    Viewport.m[0][0] = w / 2.0f;
    Viewport.m[0][3] = x + w / 2.0f;
    Viewport.m[1][1] = h / 2.0f;
    Viewport.m[1][3] = y + h / 2.0f;
}

// Set pixel in framebuffer (RGB888) using current_stride
static inline void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
        int idx = y * current_stride + x * 3;
        current_framebuffer[idx + 0] = r;
        current_framebuffer[idx + 1] = g;
        current_framebuffer[idx + 2] = b;
    }
}

// Rasterize a portion of a triangle (columns from x_start to x_end)
static void rasterize_columns(const RasterJob* job, int x_start, int x_end) {
    const vec2f* screen = job->screen;
    const vec4f* ndc = job->ndc;
    float inv_det = job->inv_det;
    uint8_t intensity = (uint8_t)job->intensity;

    // Clamp x range to triangle bounding box
    int xmin = (x_start > job->xmin) ? x_start : job->xmin;
    int xmax = (x_end < job->xmax) ? x_end : job->xmax;

    for (int y = job->ymin; y <= job->ymax; y++) {
        for (int x = xmin; x <= xmax; x++) {
            // Compute barycentric coordinates
            float px = (float)x;
            float py = (float)y;

            float w0 = ((screen[1].x - screen[0].x) * (py - screen[0].y) -
                        (screen[1].y - screen[0].y) * (px - screen[0].x));
            float w1 = ((screen[2].x - screen[1].x) * (py - screen[1].y) -
                        (screen[2].y - screen[1].y) * (px - screen[1].x));
            float w2 = ((screen[0].x - screen[2].x) * (py - screen[2].y) -
                        (screen[0].y - screen[2].y) * (px - screen[2].x));

            // Check if inside triangle
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                // Normalize barycentric coordinates
                float bc0 = w1 * inv_det;
                float bc1 = w2 * inv_det;
                float bc2 = w0 * inv_det;

                // Interpolate z
                float z = bc0 * ndc[0].z + bc1 * ndc[1].z + bc2 * ndc[2].z;

                // Z-buffer test
                int zidx = x + y * WIDTH;
                if (z > zbuffer[zidx]) {
                    zbuffer[zidx] = z;
                    set_pixel(x, y, intensity, intensity, intensity);
                }
            }
        }
    }
}

// Worker task for Core 1 - processes right half of ALL triangles per frame
static void render_worker_task(void* arg) {
    ESP_LOGI(TAG, "Render worker started on core %d", xPortGetCoreID());

    while (!worker_should_exit) {
        // Wait for frame job
        if (xSemaphoreTake(job_ready_sem, portMAX_DELAY) == pdTRUE) {
            if (worker_should_exit) break;

            // Process ALL triangles for right half (columns 240-479)
            const FrameJob* frame = (const FrameJob*)&current_frame_job;
            for (int i = 0; i < frame->num_triangles; i++) {
                if (frame->triangles[i].valid) {
                    rasterize_columns(&frame->triangles[i], RENDER_SPLIT_X, WIDTH);
                }
            }

            // Signal completion (once per frame)
            xSemaphoreGive(job_done_sem);
        }
    }

    ESP_LOGI(TAG, "Render worker exiting");
    vTaskDelete(NULL);
}

// Prepare triangle data for rasterization (returns false if culled)
static bool prepare_triangle(vec4f clip[3], vec3f tri_eye[3], vec3f light_dir, RasterJob* job) {
    // Perspective divide to get NDC
    vec4f ndc[3];
    for (int i = 0; i < 3; i++) {
        float w = clip[i].w;
        if (fabsf(w) < 0.0001f) w = 0.0001f;
        ndc[i] = (vec4f){clip[i].x / w, clip[i].y / w, clip[i].z / w, 1.0f};
    }

    // Transform to screen coordinates
    vec2f screen[3];
    for (int i = 0; i < 3; i++) {
        vec4f s = mat4f_mul_vec4(Viewport, ndc[i]);
        screen[i] = (vec2f){s.x, s.y};
    }

    // Backface culling using 2D cross product (determinant)
    float det = (screen[1].x - screen[0].x) * (screen[2].y - screen[0].y) -
                (screen[2].x - screen[0].x) * (screen[1].y - screen[0].y);
    if (det < 1.0f) {
        job->valid = false;
        return false;  // Backface or degenerate triangle
    }

    // Compute face normal in eye space for flat shading
    vec3f edge1 = vec3f_sub(tri_eye[1], tri_eye[0]);
    vec3f edge2 = vec3f_sub(tri_eye[2], tri_eye[0]);
    vec3f normal = vec3f_normalize(vec3f_cross(edge1, edge2));

    // Compute diffuse lighting
    float diff = vec3f_dot(normal, light_dir);
    if (diff < 0.0f) diff = 0.0f;

    // Compute color intensity (ambient + diffuse)
    int intensity = (int)(30 + 225 * diff);
    if (intensity > 255) intensity = 255;

    // Bounding box
    int xmin = (int)fminf(fminf(screen[0].x, screen[1].x), screen[2].x);
    int xmax = (int)fmaxf(fmaxf(screen[0].x, screen[1].x), screen[2].x);
    int ymin = (int)fminf(fminf(screen[0].y, screen[1].y), screen[2].y);
    int ymax = (int)fmaxf(fmaxf(screen[0].y, screen[1].y), screen[2].y);

    // Clamp to screen
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= WIDTH) xmax = WIDTH - 1;
    if (ymax >= HEIGHT) ymax = HEIGHT - 1;

    // Fill job data
    for (int i = 0; i < 3; i++) {
        job->screen[i] = screen[i];
        job->ndc[i] = ndc[i];
    }
    job->inv_det = 1.0f / det;
    job->intensity = intensity;
    job->xmin = xmin;
    job->xmax = xmax;
    job->ymin = ymin;
    job->ymax = ymax;
    job->valid = true;

    return true;
}

void renderer_init(void) {
    // Allocate z-buffer from PSRAM to save internal RAM
    if (zbuffer == NULL) {
        zbuffer = (float*)heap_caps_malloc(WIDTH * HEIGHT * sizeof(float), MALLOC_CAP_SPIRAM);
        if (zbuffer == NULL) {
            zbuffer = (float*)malloc(WIDTH * HEIGHT * sizeof(float));
        }
    }

    // Create semaphores for parallel rendering
    if (job_ready_sem == NULL) {
        job_ready_sem = xSemaphoreCreateBinary();
    }
    if (job_done_sem == NULL) {
        job_done_sem = xSemaphoreCreateBinary();
    }

    // Create worker task on Core 1
    if (worker_task_handle == NULL && job_ready_sem != NULL && job_done_sem != NULL) {
        worker_should_exit = false;
        BaseType_t result = xTaskCreatePinnedToCore(
            render_worker_task,     // Task function
            "render_worker",        // Task name
            4096,                   // Stack size
            NULL,                   // Parameters
            5,                      // Priority (same as main task)
            &worker_task_handle,    // Task handle
            1                       // Core 1
        );
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create render worker task");
            worker_task_handle = NULL;
        } else {
            ESP_LOGI(TAG, "Parallel rendering enabled (dual-core)");
        }
    }
}

void renderer_render_frame(unsigned char* framebuffer, int stride, int frame_number) {
    // Store framebuffer and stride for use by set_pixel
    current_framebuffer = framebuffer;
    current_stride = stride;

    // Clear framebuffer with sky blue background (only the 480-pixel wide render area)
    for (int y = 0; y < HEIGHT; y++) {
        uint8_t* row = framebuffer + y * stride;
        for (int x = 0; x < WIDTH; x++) {
            row[x * 3 + 0] = 209;  // R (sky blue)
            row[x * 3 + 1] = 195;  // G
            row[x * 3 + 2] = 177;  // B
        }
    }

    // Clear z-buffer
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        zbuffer[i] = -1000.0f;
    }

    // Animate camera position
    float t = (float)frame_number;
    float cx = sinf(t / 20.0f) * 2.0f;
    float cy = sinf(t / 15.0f) * 1.5f;
    float cz = 3.0f + sinf(t / 40.0f) * 0.5f;

    // Animate light position
    /*
    float lx = cosf(t / 20.0f) * 2.0f;
    float ly = 2.0f + cosf(t / 15.0f) * 1.0f;
    float lz = cosf(t / 40.0f) * 2.0f;
    */
    float tt = 20.0;
    float lx = cosf(tt / 20.0f) * 2.0f;
    float ly = 2.0f + cosf(tt / 15.0f) * 1.0f;
    float lz = cosf(tt / 40.0f) * 2.0f;

    vec3f eye = {cx, cy, cz};
    vec3f center = {0.0f, 0.0f, 0.0f};
    vec3f up = {0.0f, 1.0f, 0.0f};
    vec3f light = {lx, ly, lz};

    // Setup transformation matrices
    setup_lookat(eye, center, up);
    float dist = vec3f_length(vec3f_sub(eye, center));
    setup_perspective(dist);
    setup_viewport(WIDTH / 16, HEIGHT / 16, WIDTH * 7 / 8, HEIGHT * 7 / 8);

    // Transform light direction to eye space
    vec4f light4 = {light.x, light.y, light.z, 0.0f};
    vec4f light_eye4 = mat4f_mul_vec4(ModelView, light4);
    vec3f light_dir = vec3f_normalize((vec3f){light_eye4.x, light_eye4.y, light_eye4.z});

    // Pre-compute all triangle data
    FrameJob frame_job;
    frame_job.num_triangles = NUM_CUBE_FACES;

    for (int f = 0; f < NUM_CUBE_FACES; f++) {
        vec4f clip[3];
        vec3f tri_eye[3];

        for (int v = 0; v < 3; v++) {
            int vi = cube_faces[f][v];
            vec3f vert = cube_verts[vi];

            // Transform to eye space
            vec4f v4 = {vert.x, vert.y, vert.z, 1.0f};
            vec4f eye_pos = mat4f_mul_vec4(ModelView, v4);
            tri_eye[v] = (vec3f){eye_pos.x, eye_pos.y, eye_pos.z};

            // Transform to clip space
            clip[v] = mat4f_mul_vec4(Perspective, eye_pos);
        }

        // Prepare triangle data (handles backface culling)
        prepare_triangle(clip, tri_eye, light_dir, &frame_job.triangles[f]);
    }

    // Check if parallel rendering is available
    if (worker_task_handle != NULL && job_ready_sem != NULL && job_done_sem != NULL) {
        // Copy frame job to shared location
        current_frame_job = frame_job;

        // Wake up worker task to process right half of ALL triangles
        xSemaphoreGive(job_ready_sem);

        // Process left half of ALL triangles on this core (columns 0-239)
        for (int i = 0; i < frame_job.num_triangles; i++) {
            if (frame_job.triangles[i].valid) {
                rasterize_columns(&frame_job.triangles[i], 0, RENDER_SPLIT_X);
            }
        }

        // Wait for worker to complete (ONCE per frame)
        xSemaphoreTake(job_done_sem, portMAX_DELAY);
    } else {
        // Fallback: single-threaded rasterization
        for (int i = 0; i < frame_job.num_triangles; i++) {
            if (frame_job.triangles[i].valid) {
                rasterize_columns(&frame_job.triangles[i], 0, WIDTH);
            }
        }
    }
}
