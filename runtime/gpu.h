/* GPU frame output (gpu.c), used by display.c in the SDL build. */
#ifndef RUSTY_GPU_H
#define RUSTY_GPU_H

#include <SDL3/SDL.h>

/* window 0: no swapchain (only the decode pass, for RUSTY_GPU_CHECK). Returns 0 when no GPU device can be used. */
int gpu_init(SDL_Window *window);
void gpu_frame(void);

#endif
