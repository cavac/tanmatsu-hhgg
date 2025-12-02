# The Cube - 3D Render Demo for Tanmatsu

## Project Overview

A software 3D renderer demo for the Tanmatsu ESP32-P4 badge. Displays a rotating flat-shaded cube on the 800x480 display at ~16 fps using dual-core parallelization.

## Build Instructions

```bash
make build      # Build the project
make flash      # Flash to device
make monitor    # View debug output via USB
```

## Architecture

- **Pure C implementation** - No C++ dependencies to minimize binary size
- **PSRAM allocation** - Large buffers (z-buffer) allocated from external PSRAM to save internal SRAM
- **Software rasterization** - Triangle rasterization with z-buffer and flat shading
- **Dual-core rendering** - Rasterization split by columns between Core 0 and Core 1
- **Direct framebuffer access** - Renderer writes directly to PAX framebuffer (no intermediate copy)

## Key Files

- `main/main.c` - Application entry point, display setup, main loop with performance timing
- `main/renderer.c` - Pure C 3D renderer with cube geometry and dual-core parallelization
- `main/renderer.h` - Renderer API (renderer_init, renderer_render_frame)
- `main/usb_device.c` - USB debug console initialization for ESP32-P4

## Renderer Details

The renderer implements:
- 4x4 matrix transformations (lookat, perspective, viewport)
- Barycentric coordinate interpolation
- Z-buffer depth testing
- Flat shading with diffuse lighting
- Backface culling

Cube is rendered at 480x480 pixels, centered on the 800x480 display with black bars on the sides.

### Dual-Core Parallelization

Rasterization is split by screen columns:
- Core 0 (main task): Columns 0-239
- Core 1 (worker task): Columns 240-479

Synchronization uses FreeRTOS binary semaphores with frame-level sync (one sync per frame, not per triangle) to minimize overhead.

## Performance

Typical frame timing (~16 fps):
- Render: ~63ms (dual-core rasterization)
- Blit: ~0.5ms (DMA to display)

## Memory Considerations

This project is tight on internal SRAM. Large buffers must be allocated from PSRAM:
- `zbuffer` (921KB) - float array for depth testing, allocated via heap_caps_malloc(..., MALLOC_CAP_SPIRAM)

The renderer writes directly to the PAX graphics library framebuffer with stride support, eliminating the need for an intermediate render buffer.

## Credits

- Loosely based on the [tinyrenderer](https://github.com/ssloy/tinyrenderer) project
- Author: Rene 'cavac' Schickbauer
