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
// textured quads from loaded atlases. The Collection Atlas UI (atlas_ui.cpp)
// builds its widgets out of these primitives; there is still no general
// widget toolkit.
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

// Loads a BC7 DDS atlas (the shape tools/gen-atlas.mjs writes) and returns
// its handle, or -1. Call only after overlay_ready(); safe from the worker
// thread - the upload is fenced before this returns.
int overlay_load_atlas(const char* path);

// The window the game's swap chain presents to (an HWND), or null before the
// first frame. This is where the input hook attaches.
void* overlay_game_hwnd();

}  // namespace fmk
