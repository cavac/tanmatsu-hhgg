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
- **Internal SRAM allocation** - Z-buffer and texture in fast internal SRAM (not PSRAM)
- **Software rasterization** - Triangle rasterization with z-buffer and texture mapping
- **Dual-core rendering** - Rasterization split by columns between Core 0 and Core 1
- **Direct framebuffer access** - Renderer writes directly to PAX framebuffer (no intermediate copy)
- **BGR byte order** - Display framebuffer uses BGR888 format (not RGB)
- **270° display rotation** - Display is internally rotated; coordinate transforms applied
- **Vsync synchronization** - Tearing effect (TE) pin used for smooth animation

## Key Files

- `main/main.c` - Application entry point, display setup, input handling, main loop
- `main/renderer.c` - Pure C 3D renderer with cube geometry and dual-core parallelization
- `main/renderer.h` - Renderer API (renderer_init, renderer_render_frame)
- `main/texture_data.h` - Embedded texture (64x64 RGB, wooden crate)
- `main/simple_font.h` - Fast 5x7 bitmap font for FPS display (rotation-aware)
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
- Span-based rasterization with incremental float attribute interpolation
- Texture mapping with nearest-neighbor sampling
- Z-buffer depth testing (16-bit with z-clamping to prevent overflow)
- Diffuse lighting (modulates texture color)
- Backface culling

Cube is rendered at 480x480 pixels, centered on the 800x480 display with black bars on the sides.

### Z-Buffer Implementation

The z-buffer uses 8-bit integers (configurable via `ZBUFFER_8BIT` define) to fit in internal SRAM for faster access. NDC z-values are mapped from [-1, 1] to [0, 254]. The 8-bit precision is sufficient for the cube demo with no visible z-fighting. Buffer is 16-byte aligned.

For more complex scenes requiring higher depth precision, set `ZBUFFER_8BIT 0` to use 16-bit z-buffer (450KB, allocated from PSRAM).

### Dual-Core Parallelization

Rasterization is split by screen columns:
- Core 0 (main task): Columns 0-239
- Core 1 (worker task): Columns 240-479

Synchronization uses FreeRTOS binary semaphores with frame-level sync (one sync per frame, not per triangle) to minimize overhead.

## Performance

Typical frame timing (~30 fps with vsync):
- Render: ~27ms (dual-core rasterization)
- Vsync wait: ~4ms (synchronized to 60Hz display)
- Blit: ~0.7ms (DMA to display)

With vsync disabled, raw render performance is ~35 fps. With vsync enabled (smooth animation), frames are locked to 30 fps (every other 60Hz refresh) since render time exceeds one vsync period.

### Optimization History
- Original single-core with float z-buffer: ~9 fps
- After eliminating intermediate copy: ~12 fps
- After dual-core parallelization: ~16 fps
- After 16-bit z-buffer optimization: ~19 fps
- After incremental integer edge evaluation: ~18 fps
- After incremental attribute interpolation: ~23 fps
- After span-based rasterization: ~25 fps
- After texture copy to internal SRAM: ~27 fps
- After 8-bit z-buffer in internal SRAM: ~35 fps (raw), ~30 fps (vsync)

### Rasterization Optimizations

**Span-Based Rasterization**: Instead of testing every pixel against edge functions, analytically compute where each edge crosses the current scanline. The intersection of all three half-planes gives the exact span [left, right] where pixels are inside the triangle. The inner loop processes this span without any per-pixel edge tests.

**Incremental Attribute Interpolation (Float)**: Z-depth and UV coordinates use precomputed dx/dy gradients per triangle. At each scanline start, initial values are computed, then incremented per pixel. Float is used to avoid fixed-point overflow issues with large screen coordinates.

**Texture Sampling**: UV values are pre-multiplied by texture dimensions. Texture coordinate wrapping uses bit masking (& 63) instead of modulo.

### Build Optimizations

The following ESP-IDF settings are enabled in `sdkconfigs/tanmatsu`:
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y` - Compiler optimization for performance (-O2)
- `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` - Quad I/O flash mode (nearly 2x flash read speed)
- `CONFIG_SPIRAM_SPEED_200M=y` - Fast PSRAM access for z-buffer

### Hardware Acceleration Investigation

**PIE SIMD** (Processor Instruction Extensions):
- PIE provides 128-bit vector registers (q0-q7) for 8x int16 operations
- Instructions: `esp.vld.128.ip` (load), `esp.vst.128.ip` (store), `esp.vadd.s16` (add)
- **Result**: PIE for z-buffer load/store was slower (~24 fps) than scalar code (~25 fps)
- **Reason**: Overhead of copying to/from aligned local buffers exceeds SIMD benefit. The true bottleneck is texture sampling and pixel writes, which can't be vectorized due to per-pixel array indexing.

**PPA** (Pixel Processing Accelerator):
- PPA supports: Scale, Rotate, Mirror (SRM), Alpha Blend, and Fill operations on rectangular image blocks
- **Result**: Not applicable to software triangle rasterization
- **Reason**: PPA operates on rectangular blocks with uniform transforms. Our texture mapping requires per-pixel UV lookups (arbitrary warp), triangles aren't rectangular, and PPA has no depth testing. PPA is designed for scaling camera frames or compositing UI layers, not 3D rendering.

### Vsync Synchronization

The display has a tearing effect (TE) pin that signals vertical blanking. Vsync is enabled via:
```c
bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
bsp_display_get_tearing_effect_semaphore(&vsync_sem);
```

Before each blit, the main loop waits on the semaphore to synchronize with the display refresh. This eliminates animation stuttering/jumping that occurred without vsync.

### Simple Bitmap Font

PAX font rendering is too slow for real-time FPS display. A custom 5x7 bitmap font (`simple_font.h`) draws directly to the framebuffer. The font uses screen coordinates (where text should appear as the user sees it) and the coordinate transform to buffer coordinates handles the 270° display rotation automatically - no glyph rotation needed. Supports digits 0-9, decimal point, space, and "fps" letters.

### Future Optimization Opportunities

- **Smaller render resolution**: Render at 240×240, use PPA to scale up to 480×480
- **Lower color depth**: RGB565 instead of RGB888 (fewer bytes per pixel write)

## Memory Considerations

Memory allocation strategy (all in internal SRAM for maximum performance):
- `zbuffer` (225KB) - uint8_t array for depth testing, allocated from internal SRAM
- `texture_sram` (12KB) - 64x64 RGB texture copied from flash to internal SRAM

At startup, ~533KB internal SRAM is free (largest block ~376KB). Using 8-bit z-buffer (225KB) instead of 16-bit (450KB) allows it to fit in internal SRAM, providing a ~30% performance boost over PSRAM.

The renderer writes directly to the PAX graphics library framebuffer with stride support, eliminating the need for an intermediate render buffer.

## Credits

- Loosely based on the [tinyrenderer](https://github.com/ssloy/tinyrenderer) project
- Author: Rene 'cavac' Schickbauer
