// ---------------------------------------------------------------------------
// overlay.h - drawing surface for the mods.
//
// The host renders by hooking IDXGISwapChain3::Present, which it reaches
// without any pattern scanning: the dxgi proxy already sits in front of
// CreateDXGIFactory*, so it sees the factory the game creates and can wrap it
// to observe the swap chain that comes out. That is a far more stable hook
// point than scanning for a vtable in memory.
//
// The drawing API is deliberately small: filled/outlined rectangles, text, and
// textured quads (for the game's own skill icons). That is everything the two
// mods actually need - an icon strip with countdowns, bars and stack counts.
// There is no widget toolkit, and no plan for one: configuration lives in a
// hot-reloaded file rather than in-game panels.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace fmk {

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};

// Installed once, from the worker thread, after the first frame is seen.
bool overlay_install();
void overlay_shutdown();

// True once the swap chain has been observed and the device objects are up.
bool overlay_ready();

// Called from the presenting thread with the frame's dimensions. Mods draw
// here. Everything below is only legal inside this callback.
using DrawFn = void (*)(float width, float height);
void overlay_set_draw(DrawFn fn);

// --- draw API (valid only inside the draw callback) ------------------------
void draw_rect(float x, float y, float w, float h, Color c);
void draw_rect_outline(float x, float y, float w, float h, float thickness, Color c);
void draw_text(float x, float y, float size, Color c, const char* text);
float measure_text(float size, const char* text);

// Draws a sub-rectangle of a loaded texture atlas. `atlas` is an index handed
// back by overlay_load_atlas.
void draw_image(int atlas, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, Color tint);

// Loads a PNG atlas from disk once and returns its handle, or -1.
int overlay_load_atlas(const char* path);

}  // namespace fmk
