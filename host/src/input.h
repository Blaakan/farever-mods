// ---------------------------------------------------------------------------
// input.h - keyboard/mouse for the in-game UI.
//
// A WndProc subclass on the game's window. The game (SDL3) pumps ordinary
// Win32 messages, so subclassing sees everything first and can keep clicks
// meant for the UI from reaching the game. Messages the UI does not care
// about pass straight through.
// ---------------------------------------------------------------------------
#pragma once

namespace fmk {

// Snapshot for the draw thread. Coordinates are client pixels, matching the
// overlay's drawing space. `wheel` accumulates detents between snapshots and
// is consumed by the read. `clicks` increments on every press inside the UI
// rect; the UI acts when it sees the counter advance.
struct InputState {
    int  mouse_x = 0, mouse_y = 0;
    bool lbutton = false;      // held, and the press started inside the UI
    int  clicks = 0;
    int  click_x = 0, click_y = 0;
    int  wheel = 0;
    bool visible = false;      // the F8 toggle
};

bool input_install(void* hwnd);
void input_uninstall();

// Consumes the accumulated wheel delta; exactly one caller per frame may
// use it (the atlas grid does).
void input_get(InputState* out);

// Same snapshot without consuming the wheel, for a second widget in the
// same frame.
void input_peek(InputState* out);

void input_set_visible(bool v);

// The UI publishes its window rectangle every frame; mouse input inside it
// is swallowed while the UI is visible, everything outside stays the game's.
void input_set_ui_rect(int x, int y, int w, int h);

// A second rectangle with the same swallowing rule - the navigator's
// waypoint frame, which is only draggable (and only eats clicks) while the
// atlas window is open, so it never steals a click during normal play.
void input_set_aux_rect(int x, int y, int w, int h);

// True when the point falls inside the main window's rectangle. The atlas
// window draws over the navigator's frame, so the navigator uses this to
// yield any click the window is visually covering - input priority has to
// follow what the player can see.
bool input_in_main_rect(int x, int y);

}  // namespace fmk
