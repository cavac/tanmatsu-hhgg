#ifndef RENDERER_H
#define RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

void renderer_init(void);
void renderer_render_frame(unsigned char* output_rgb, int frame_number);

#ifdef __cplusplus
}
#endif

#endif
