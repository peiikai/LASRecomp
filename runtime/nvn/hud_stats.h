// What Video > Show FPS > Show advanced information shows (runtime/ui/menu.mm perf_overlay): frame pacing's own
// measurements (pace.c) and the backend's counters (renderer.m / vulkan/exec.c). Read by the overlay once a frame on
// the presenting thread; the counters are written on others, and a stale or torn value only shows for a frame.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HUD_FRAMES 120  // the frame-time graph: the last 120 game frames (2 s at 60)

typedef struct {
    uint64_t frames;          // game frames so far
    double fps;               // game frames a second (moving average)
    float frame_ms[HUD_FRAMES];  // each game frame's time, oldest first (n of them)
    int n;
    double cpu_game, cpu_render;  // the game's thread and the render thread busy, % of a frame (-1: no render thread)
    double gpu;               // the GPU busy with the game, % of a frame (-1: no GPU timer)
    double work_ms;           // a frame's work: the longest of the two threads' CPU time and the GPU's (moving average)
    bool hold30;              // frame pacing is holding the game at 30
} PaceHud;
void pace_hud(PaceHud *h);

typedef struct {
    uint64_t draws, passes, dispatches;  // so far (the overlay takes them per frame)
    uint64_t compiles;                   // shaders / pipelines built so far
    double compile_ms;                   // their build time so far (-1: not timed by this backend)
    int textures;                        // the game's textures alive
    uint64_t texture_bytes;
    uint64_t gpu_bytes;                  // GPU memory this process uses (0: unknown)
    const char *gpu_name, *backend;
} RendererHud;
void renderer_hud(RendererHud *r);
