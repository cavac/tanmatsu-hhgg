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
#include "texture_data.h"

static const char* TAG = "renderer";

// Constants
#define WIDTH 480
#define HEIGHT 480
#define NUM_CUBE_VERTS 8
#define NUM_CUBE_FACES 12

// Fixed-point scale (16.16 format)
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define FP_HALF (1 << (FP_SHIFT - 1))

// Convert float to fixed-point
#define FLOAT_TO_FP(f) ((int32_t)((f) * FP_ONE))

// Convert fixed-point to int (truncate)
#define FP_TO_INT(fp) ((fp) >> FP_SHIFT)

// Fixed-point multiply (64-bit intermediate to avoid overflow)
#define FP_MUL(a, b) ((int32_t)(((int64_t)(a) * (int64_t)(b)) >> FP_SHIFT))

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

// UV coordinates for each triangle (maps full texture to each face)
// Two triangles per face form a quad: tri1=(0,0)-(1,0)-(1,1), tri2=(0,0)-(1,1)-(0,1)
static const vec2f cube_uvs[NUM_CUBE_FACES][3] = {
    // Right face (+X)
    {{0, 1}, {1, 1}, {1, 0}}, {{0, 1}, {1, 0}, {0, 0}},
    // Left face (-X)
    {{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {1, 1}, {0, 1}},
    // Top face (+Y)
    {{1, 1}, {1, 0}, {0, 0}}, {{1, 1}, {0, 0}, {0, 1}},
    // Bottom face (-Y)
    {{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {1, 1}, {0, 1}},
    // Front face (+Z)
    {{1, 0}, {1, 1}, {0, 1}}, {{1, 0}, {0, 1}, {0, 0}},
    // Back face (-Z)
    {{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {1, 1}, {0, 1}},
};

// Global matrices
static mat4f ModelView;
static mat4f Perspective;
static mat4f Viewport;

// Z-buffer (16-bit fixed point for faster comparisons)
// Z values scaled from [-1, 1] to [INT16_MIN, INT16_MAX]
static int16_t* zbuffer = NULL;
#define Z_SCALE 32767.0f

// Texture copy in internal SRAM for faster access (flash is slower)
static uint8_t* texture_sram = NULL;
#define TEXTURE_SIZE (TEX_WIDTH * TEX_HEIGHT * 3)

// Current framebuffer pointer and stride (set per frame)
static uint8_t* current_framebuffer = NULL;
static int current_stride = 0;

// Parallel rendering infrastructure
#define RENDER_SPLIT_X (WIDTH / 2)  // Split at column 240

// Job data for parallel rasterization (single triangle)
typedef struct {
    vec2f screen[3];       // Screen coordinates (float, for setup)
    vec2f uv[3];           // Texture coordinates
    float ndc_z[3];        // NDC z values for depth interpolation
    float inv_det;         // 1/det for barycentric normalization
    uint8_t intensity;     // Lighting intensity (0-255)
    int16_t xmin, xmax;    // Bounding box
    int16_t ymin, ymax;
    bool valid;            // Whether this triangle passed culling

    // Integer edge function coefficients for incremental evaluation
    // E(x,y) = A*x + B*y + C, E(x+1,y) = E(x,y) + A
    int32_t edge_a[3];     // A coefficient: -(y[j] - y[i]) as integer
    int32_t edge_b[3];     // B coefficient: (x[j] - x[i]) as integer
    int32_t edge_c[3];     // C coefficient: x[i]*y[j] - x[j]*y[i] as integer

    // Incremental attribute interpolation (float)
    // Attr = (w1*A0 + w2*A1 + w0*A2) / det
    // dAttr/dx = (a1*A0 + a2*A1 + a0*A2) / det (constant per triangle)
    float z_dx, z_dy, z_c;   // Z (scaled by Z_SCALE)
    float u_dx, u_dy, u_c;   // U (pre-multiplied by TEX_WIDTH)
    float v_dx, v_dy, v_c;   // V (pre-multiplied by TEX_HEIGHT)
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

// Rotation matrix around X axis
static mat4f mat4f_rotate_x(float angle) {
    mat4f m = mat4f_identity();
    float c = cosf(angle);
    float s = sinf(angle);
    m.m[1][1] = c;
    m.m[1][2] = -s;
    m.m[2][1] = s;
    m.m[2][2] = c;
    return m;
}

// Rotation matrix around Y axis
static mat4f mat4f_rotate_y(float angle) {
    mat4f m = mat4f_identity();
    float c = cosf(angle);
    float s = sinf(angle);
    m.m[0][0] = c;
    m.m[0][2] = s;
    m.m[2][0] = -s;
    m.m[2][2] = c;
    return m;
}

// Rotation matrix around Z axis
static mat4f mat4f_rotate_z(float angle) {
    mat4f m = mat4f_identity();
    float c = cosf(angle);
    float s = sinf(angle);
    m.m[0][0] = c;
    m.m[0][1] = -s;
    m.m[1][0] = s;
    m.m[1][1] = c;
    return m;
}

// Set pixel in framebuffer (BGR888 byte order) using current_stride
static inline void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
        int idx = y * current_stride + x * 3;
        current_framebuffer[idx + 0] = b;
        current_framebuffer[idx + 1] = g;
        current_framebuffer[idx + 2] = r;
    }
}

// Small negative epsilon for edge test (integer, ~1 pixel tolerance)
#define EDGE_EPSILON_INT (-1)

// Helper: compute x where edge function crosses zero for given y
// Edge: a*x + b*y + c = 0  =>  x = -(b*y + c) / a
static inline int edge_x_crossing(int32_t a, int32_t b, int32_t c, int y) {
    if (a == 0) return (b * y + c >= 0) ? -1000000 : 1000000;  // Horizontal edge
    int64_t num = -((int64_t)b * y + c);
    return (int)(num / a);
}

// Rasterize a portion of a triangle (columns from x_start to x_end)
// Uses span-based optimization: finds exact bounds per scanline, no per-pixel edge tests
static void rasterize_columns(const RasterJob* job, int x_start, int x_end) {
    const int intensity = job->intensity;

    // Get integer edge coefficients
    const int32_t a0 = job->edge_a[0], b0 = job->edge_b[0], c0 = job->edge_c[0];
    const int32_t a1 = job->edge_a[1], b1 = job->edge_b[1], c1 = job->edge_c[1];
    const int32_t a2 = job->edge_a[2], b2 = job->edge_b[2], c2 = job->edge_c[2];

    // Get float attribute interpolation coefficients
    const float z_dx = job->z_dx, z_dy = job->z_dy, z_c = job->z_c;
    const float u_dx = job->u_dx, u_dy = job->u_dy, u_c = job->u_c;
    const float v_dx = job->v_dx, v_dy = job->v_dy, v_c = job->v_c;

    // Clamp x range to triangle bounding box
    int xmin_bounds = (x_start > job->xmin) ? x_start : job->xmin;
    int xmax_bounds = (x_end < job->xmax) ? x_end : job->xmax;

    if (xmin_bounds > xmax_bounds) return;

    for (int y = job->ymin; y <= job->ymax; y++) {
        // Compute span bounds analytically from edge crossings
        // For each edge: if a > 0, edge limits from left; if a < 0, limits from right
        int span_left = xmin_bounds;
        int span_right = xmax_bounds;

        // Edge 0
        if (a0 > 0) {
            int x_cross = edge_x_crossing(a0, b0, c0, y);
            if (x_cross > span_left) span_left = x_cross;
        } else if (a0 < 0) {
            int x_cross = edge_x_crossing(a0, b0, c0, y);
            if (x_cross < span_right) span_right = x_cross;
        } else {
            // a0 == 0: horizontal edge, check if y is valid
            if (b0 * y + c0 < EDGE_EPSILON_INT) continue;  // Outside
        }

        // Edge 1
        if (a1 > 0) {
            int x_cross = edge_x_crossing(a1, b1, c1, y);
            if (x_cross > span_left) span_left = x_cross;
        } else if (a1 < 0) {
            int x_cross = edge_x_crossing(a1, b1, c1, y);
            if (x_cross < span_right) span_right = x_cross;
        } else {
            if (b1 * y + c1 < EDGE_EPSILON_INT) continue;
        }

        // Edge 2
        if (a2 > 0) {
            int x_cross = edge_x_crossing(a2, b2, c2, y);
            if (x_cross > span_left) span_left = x_cross;
        } else if (a2 < 0) {
            int x_cross = edge_x_crossing(a2, b2, c2, y);
            if (x_cross < span_right) span_right = x_cross;
        } else {
            if (b2 * y + c2 < EDGE_EPSILON_INT) continue;
        }

        // Clamp to bounds
        if (span_left < xmin_bounds) span_left = xmin_bounds;
        if (span_right > xmax_bounds) span_right = xmax_bounds;

        if (span_left > span_right) continue;  // No pixels on this scanline

        // Get row pointers
        int16_t* zbuf_row = zbuffer + y * WIDTH;
        uint8_t* fb_row = current_framebuffer + y * current_stride;

        // Compute attributes at span start
        float z_row = z_dx * span_left + z_dy * y + z_c;
        float u_row = u_dx * span_left + u_dy * y + u_c;
        float v_row = v_dx * span_left + v_dy * y + v_c;

        // Process span - no edge tests needed!
        for (int x = span_left; x <= span_right; x++) {
            // Clamp z to int16 range
            int16_t z16;
            if (z_row < -32767.0f) z16 = -32767;
            else if (z_row > 32767.0f) z16 = 32767;
            else z16 = (int16_t)z_row;

            // Z-buffer test
            if (z16 >= zbuf_row[x]) {
                zbuf_row[x] = z16;

                // Sample texture from SRAM (u/v are pre-multiplied by texture size)
                int tx = ((int)u_row) & (TEX_WIDTH - 1);
                int ty = ((int)v_row) & (TEX_HEIGHT - 1);

                int tidx = ty * (TEX_WIDTH * 3) + tx * 3;
                int r = texture_sram[tidx + 0];
                int g = texture_sram[tidx + 1];
                int b = texture_sram[tidx + 2];

                // Apply lighting
                r = (r * intensity) >> 8;
                g = (g * intensity) >> 8;
                b = (b * intensity) >> 8;

                // Write pixel directly (BGR order)
                int pidx = x * 3;
                fb_row[pidx + 0] = (uint8_t)b;
                fb_row[pidx + 1] = (uint8_t)g;
                fb_row[pidx + 2] = (uint8_t)r;
            }

            // Increment attributes (no edge increments needed!)
            z_row += z_dx;
            u_row += u_dx;
            v_row += v_dx;
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
static bool prepare_triangle(vec4f clip[3], vec3f tri_eye[3], vec3f light_dir, int face_idx, RasterJob* job) {
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
    if (det < 0.01f) {
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

    // Compute color intensity (ambient + diffuse, scaled for >> 8 in rasterizer)
    int intensity = (int)(77 + 178 * diff);  // Range: 77-255 (30% ambient, 70% diffuse)
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

    // Store screen coordinates and UVs (float versions for compatibility)
    for (int i = 0; i < 3; i++) {
        job->screen[i] = screen[i];
        job->ndc_z[i] = ndc[i].z;
        job->uv[i] = cube_uvs[face_idx][i];
    }

    job->inv_det = 1.0f / det;
    job->intensity = (uint8_t)intensity;
    job->xmin = (int16_t)xmin;
    job->xmax = (int16_t)xmax;
    job->ymin = (int16_t)ymin;
    job->ymax = (int16_t)ymax;
    job->valid = true;

    // Compute integer edge coefficients for incremental evaluation
    // Edge function: E(x,y) = (x[j]-x[i])*(y-y[i]) - (y[j]-y[i])*(x-x[i])
    // Expanded: E = A*x + B*y + C where A = -(y[j]-y[i]), B = (x[j]-x[i]), C = x[i]*y[j] - x[j]*y[i]
    // Using rounded integer screen coordinates for edge tests
    int32_t sx[3], sy[3];
    for (int i = 0; i < 3; i++) {
        sx[i] = (int32_t)(screen[i].x + 0.5f);  // Round to nearest
        sy[i] = (int32_t)(screen[i].y + 0.5f);
    }

    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        job->edge_a[i] = sy[i] - sy[j];                    // A = -(y[j] - y[i])
        job->edge_b[i] = sx[j] - sx[i];                    // B = x[j] - x[i]
        job->edge_c[i] = sx[i] * sy[j] - sx[j] * sy[i];    // C = x[i]*y[j] - x[j]*y[i]
    }

    // Compute determinant from INTEGER coordinates (must match edge coefficients)
    // det = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0)
    int32_t det_int = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (det_int == 0) det_int = 1;  // Avoid division by zero
    float inv_det_attr = 1.0f / (float)det_int;

    // Compute incremental attribute interpolation coefficients (fixed-point 16.16)
    // Attr(x,y) = (w1*A[0] + w2*A[1] + w0*A[2]) / det
    // where w_i = a_i*x + b_i*y + c_i
    // Expanding: Attr = Attr_dx * x + Attr_dy * y + Attr_c
    float a0f = (float)job->edge_a[0], a1f = (float)job->edge_a[1], a2f = (float)job->edge_a[2];
    float b0f = (float)job->edge_b[0], b1f = (float)job->edge_b[1], b2f = (float)job->edge_b[2];
    float c0f = (float)job->edge_c[0], c1f = (float)job->edge_c[1], c2f = (float)job->edge_c[2];

    // Z interpolation (scaled by Z_SCALE)
    float z0 = ndc[0].z * Z_SCALE, z1 = ndc[1].z * Z_SCALE, z2 = ndc[2].z * Z_SCALE;
    job->z_dx = (a1f * z0 + a2f * z1 + a0f * z2) * inv_det_attr;
    job->z_dy = (b1f * z0 + b2f * z1 + b0f * z2) * inv_det_attr;
    job->z_c  = (c1f * z0 + c2f * z1 + c0f * z2) * inv_det_attr;

    // U interpolation (pre-multiplied by TEX_WIDTH)
    float u0 = cube_uvs[face_idx][0].x * TEX_WIDTH;
    float u1 = cube_uvs[face_idx][1].x * TEX_WIDTH;
    float u2 = cube_uvs[face_idx][2].x * TEX_WIDTH;
    job->u_dx = (a1f * u0 + a2f * u1 + a0f * u2) * inv_det_attr;
    job->u_dy = (b1f * u0 + b2f * u1 + b0f * u2) * inv_det_attr;
    job->u_c  = (c1f * u0 + c2f * u1 + c0f * u2) * inv_det_attr;

    // V interpolation (pre-multiplied by TEX_HEIGHT)
    float v0 = cube_uvs[face_idx][0].y * TEX_HEIGHT;
    float v1 = cube_uvs[face_idx][1].y * TEX_HEIGHT;
    float v2 = cube_uvs[face_idx][2].y * TEX_HEIGHT;
    job->v_dx = (a1f * v0 + a2f * v1 + a0f * v2) * inv_det_attr;
    job->v_dy = (b1f * v0 + b2f * v1 + b0f * v2) * inv_det_attr;
    job->v_c  = (c1f * v0 + c2f * v1 + c0f * v2) * inv_det_attr;

    return true;
}

void renderer_init(void) {
    // Log available memory before allocations
    ESP_LOGI(TAG, "Memory before renderer init:");
    ESP_LOGI(TAG, "  Internal SRAM free: %d KB (largest block: %d KB)",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (int)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "  PSRAM free: %d KB (largest block: %d KB)",
             (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (int)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

    // Allocate z-buffer from PSRAM to save internal RAM
    // Using int16_t instead of float for faster comparisons (half the memory too)
    // Align to 16 bytes for PIE SIMD access
    if (zbuffer == NULL) {
        zbuffer = (int16_t*)heap_caps_aligned_alloc(16, WIDTH * HEIGHT * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (zbuffer == NULL) {
            zbuffer = (int16_t*)aligned_alloc(16, WIDTH * HEIGHT * sizeof(int16_t));
        }
        ESP_LOGI(TAG, "Z-buffer: %d KB (int16, 16-byte aligned, in %s)",
                 (int)(WIDTH * HEIGHT * sizeof(int16_t) / 1024),
                 zbuffer ? "PSRAM" : "SRAM");
    }

    // Copy texture to internal SRAM for faster access
    if (texture_sram == NULL) {
        texture_sram = (uint8_t*)heap_caps_malloc(TEXTURE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (texture_sram != NULL) {
            memcpy(texture_sram, texture_data, TEXTURE_SIZE);
            ESP_LOGI(TAG, "Texture: %d KB copied to internal SRAM", (int)(TEXTURE_SIZE / 1024));
        } else {
            ESP_LOGW(TAG, "Texture: staying in flash (SRAM alloc failed)");
            texture_sram = (uint8_t*)texture_data;  // Fall back to flash
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

    // Clear framebuffer with sky blue background (BGR byte order)
    for (int y = 0; y < HEIGHT; y++) {
        uint8_t* row = framebuffer + y * stride;
        for (int x = 0; x < WIDTH; x++) {
            row[x * 3 + 0] = 177;  // B (sky blue)
            row[x * 3 + 1] = 195;  // G
            row[x * 3 + 2] = 209;  // R
        }
    }

    // Clear z-buffer using memset (set all bytes to 0x80 = INT16_MIN-ish)
    // This sets each int16_t to 0x8080 = -32640, close enough to minimum
    memset(zbuffer, 0x80, WIDTH * HEIGHT * sizeof(int16_t));

    // Fixed camera position
    vec3f eye = {0.0f, 0.0f, 3.0f};
    vec3f center = {0.0f, 0.0f, 0.0f};
    vec3f up = {0.0f, 1.0f, 0.0f};

    // Fixed light position (upper right, slightly in front)
    vec3f light = {2.0f, 2.0f, 2.0f};

    // Cube rotation angles (different speeds for each axis)
    float t = (float)frame_number;
    float angle_x = t / 47.0f;   // Slow X rotation
    float angle_y = t / 23.0f;   // Medium Y rotation
    float angle_z = t / 67.0f;   // Very slow Z rotation

    // Build cube rotation matrix (apply in order: Z, Y, X)
    mat4f rot_x = mat4f_rotate_x(angle_x);
    mat4f rot_y = mat4f_rotate_y(angle_y);
    mat4f rot_z = mat4f_rotate_z(angle_z);
    mat4f cube_rotation = mat4f_mul(rot_x, mat4f_mul(rot_y, rot_z));

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

            // Apply cube rotation first (model space)
            vec4f v4 = {vert.x, vert.y, vert.z, 1.0f};
            vec4f rotated = mat4f_mul_vec4(cube_rotation, v4);

            // Transform to eye space
            vec4f eye_pos = mat4f_mul_vec4(ModelView, rotated);
            tri_eye[v] = (vec3f){eye_pos.x, eye_pos.y, eye_pos.z};

            // Transform to clip space
            clip[v] = mat4f_mul_vec4(Perspective, eye_pos);
        }

        // Prepare triangle data (handles backface culling)
        prepare_triangle(clip, tri_eye, light_dir, f, &frame_job.triangles[f]);
    }

    // Check if parallel rendering is available
    if (worker_task_handle != NULL && job_ready_sem != NULL && job_done_sem != NULL) {
        // Copy frame job to shared location
        current_frame_job = frame_job;

        // Wake up worker task to process right half of ALL triangles
        xSemaphoreGive(job_ready_sem);

        // Process left half of ALL triangles on this core (columns 0 to 239)
        for (int i = 0; i < frame_job.num_triangles; i++) {
            if (frame_job.triangles[i].valid) {
                rasterize_columns(&frame_job.triangles[i], 0, RENDER_SPLIT_X - 1);
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
