# The Cube - 3D Render Demo for Tanmatsu

## Project Overview

A software 3D renderer demo for the Tanmatsu ESP32-P4 badge. Displays a spinning textured cube on the 800x480 display using dual-core parallelization. The cube rotates on multiple axes while the camera and lighting remain fixed.

## Build Instructions

```bash
make build      # Build the project
make flash      # Flash to device
make monitor    # View debug output via USB
```

## Controls

- **ESC** - Return to launcher
- **Power button** - Return to launcher
- **Space** - Save screenshot to SD card (requires `CAVAC_DEBUG`, see below)

## Architecture

- **Pure C implementation** - No C++ dependencies to minimize binary size
- **PSRAM allocation** - Large buffers (z-buffer) allocated from external PSRAM to save internal SRAM
- **Software rasterization** - Triangle rasterization with z-buffer and texture mapping
- **Dual-core rendering** - Rasterization split by columns between Core 0 and Core 1
- **Direct framebuffer access** - Renderer writes directly to PAX framebuffer (no intermediate copy)
- **BGR byte order** - Display framebuffer uses BGR888 format (not RGB)

## Key Files

- `main/main.c` - Application entry point, display setup, input handling, main loop
- `main/renderer.c` - Pure C 3D renderer with cube geometry and dual-core parallelization
- `main/renderer.h` - Renderer API (renderer_init, renderer_render_frame)
- `main/texture_data.h` - Embedded texture (64x64 RGB, wooden crate)
- `main/sdcard.c` - SD card initialization (SPI mode) and mounting (debug only)
- `main/sdcard.h` - SD card API (debug only)
- `main/usb_device.c` - USB debug console initialization for ESP32-P4

## Debug Mode

Screenshot functionality is disabled by default. To enable it, uncomment `#define CAVAC_DEBUG` in `main/main.c`. This enables:
- SD card initialization and mounting
- Space key to save screenshots as PPM files to `/sd/cube_screenshot_XXX.ppm`

Note: SD card must be inserted for screenshots to work. Disabling debug mode reduces binary size by ~107KB.

## Renderer Details

The renderer implements:
- 4x4 matrix transformations (lookat, perspective, viewport, rotation)
- Cube rotation on X, Y, Z axes at different speeds (fixed camera/light)
- Incremental attribute interpolation for UV and depth (fixed-point, no per-pixel floats)
- Texture mapping with nearest-neighbor sampling
- Z-buffer depth testing (16-bit with z-clamping to prevent overflow)
- Diffuse lighting (modulates texture color)
- Backface culling

Cube is rendered at 480x480 pixels, centered on the 800x480 display with black bars on the sides.

### Z-Buffer Implementation

The z-buffer uses 16-bit integers for faster comparisons and lower memory usage. NDC z-values are clamped to [-1, 1] before scaling to prevent integer overflow, which was causing rendering artifacts near corners close to the camera. Buffer is 16-byte aligned for potential SIMD access.

### Dual-Core Parallelization

Rasterization is split by screen columns:
- Core 0 (main task): Columns 0-239
- Core 1 (worker task): Columns 240-479

Synchronization uses FreeRTOS binary semaphores with frame-level sync (one sync per frame, not per triangle) to minimize overhead.

## Performance

Typical frame timing (~25 fps):
- Render: ~39ms (dual-core rasterization)
- Blit: ~0.5ms (DMA to display)

### Optimization History
- Original single-core with float z-buffer: ~9 fps
- After eliminating intermediate copy: ~12 fps
- After dual-core parallelization: ~16 fps
- After 16-bit z-buffer optimization: ~19 fps
- After incremental integer edge evaluation: ~18 fps
- After incremental attribute interpolation: ~23 fps
- After span-based rasterization: ~25 fps

### Rasterization Optimizations

**Span-Based Rasterization**: Instead of testing every pixel against edge functions, analytically compute where each edge crosses the current scanline. The intersection of all three half-planes gives the exact span [left, right] where pixels are inside the triangle. The inner loop processes this span without any per-pixel edge tests.

**Incremental Attribute Interpolation (Float)**: Z-depth and UV coordinates use precomputed dx/dy gradients per triangle. At each scanline start, initial values are computed, then incremented per pixel. Float is used to avoid fixed-point overflow issues with large screen coordinates.

**Texture Sampling**: UV values are pre-multiplied by texture dimensions. Texture coordinate wrapping uses bit masking (& 63) instead of modulo.

### Build Optimizations

The following ESP-IDF settings are enabled in `sdkconfigs/tanmatsu`:
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y` - Compiler optimization for performance (-O2)
- `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` - Quad I/O flash mode (nearly 2x flash read speed)
- `CONFIG_SPIRAM_SPEED_200M=y` - Fast PSRAM access for z-buffer

### PIE SIMD Investigation

ESP32-P4's PIE (Processor Instruction Extensions) was investigated for SIMD acceleration. Key findings:
- PIE provides 128-bit vector registers (q0-q7) for 8x int16 operations
- Instructions: `esp.vld.128.ip` (load), `esp.vst.128.ip` (store), `esp.vadd.s16` (add)
- **Result**: PIE for z-buffer load/store was slower (~24 fps) than scalar code (~25 fps)
- **Reason**: The overhead of copying to/from aligned local buffers outweighs the SIMD benefit. The true bottleneck is texture sampling and pixel writes, which can't easily be vectorized due to per-pixel array indexing.

### Future Optimization Opportunities

- **PPA (Pixel Processing Accelerator)**: ESP32-P4's hardware pixel processor could potentially accelerate texture sampling or color blending.
- **Texture cache optimization**: Smaller textures or tiled textures might improve cache locality.
- **Reduced precision**: Using int8 colors throughout might enable better SIMD usage.

## Memory Considerations

This project is tight on internal SRAM. Large buffers must be allocated from PSRAM:
- `zbuffer` (460KB) - int16_t array for depth testing, allocated via heap_caps_aligned_alloc (16-byte aligned for SIMD)
- `texture_data` (12KB) - 64x64 RGB texture embedded in flash

The renderer writes directly to the PAX graphics library framebuffer with stride support, eliminating the need for an intermediate render buffer.

## Credits

- Loosely based on the [tinyrenderer](https://github.com/ssloy/tinyrenderer) project
- Author: Rene 'cavac' Schickbauer
