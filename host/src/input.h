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
    // Modifiers held when that click landed, recorded then rather than read
    // later: by the time the render thread looks, the key may be back up.
    bool click_shift = false, click_ctrl = false;
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

// The keys the host owns besides the F8 toggle. All are counters rather than
// flags, so a press between two frames is never lost, and the reader consumes
// what it takes.
//
//   F9         drop a waypoint - where you stand, or what the map is showing
//   F10        skip the waypoint being aimed at
//   Shift+F10  clear the route
//
// Skip and clear are separate counters rather than one counter and a shift
// flag: pressing both in quick succession must not lose which was which.
// They are keys and not only buttons on the Routes page because they are
// wanted while running, and that page is behind F8.
int input_take_waypoint_presses();
int input_take_skip_presses();
int input_take_clear_presses();

// A left-click the host did **not** take: pressed and released outside every
// UI rectangle, close enough together in time and place to be a click rather
// than the start of a drag. The message still reaches the game untouched -
// this only counts them, so a mod can react to the player clicking on the
// game's own UI without taking that click away from it.
//
// The counter advances on release, since only then is it known not to have
// been a drag. `x`/`y` receive where the press landed.
struct RawClick {
    int count = 0;
    int x = 0, y = 0;
    bool shift = false;   // held at press time, not at read time
    bool ctrl = false;
};
void input_peek_raw_click(RawClick* out);

// Text entry, off by default. While capture is on, printable characters and
// backspace are collected here and kept from the game; while it is off, only
// the toggle key and Escape are ever touched, so movement keys keep working
// with the window open. Turn it on only while a text field has focus.
void input_set_text_capture(bool on);
bool input_text_capture();

// Drains the characters typed since the last call. Backspace arrives as
// '\b'; Enter as '\n'.
int input_take_text(char* out, int max_len);

// The UI publishes its window rectangle every frame; mouse input inside it
// is swallowed while the UI is visible, everything outside stays the game's.
void input_set_ui_rect(int x, int y, int w, int h);

// Extra rectangles with the same swallowing rule, one per mod that draws a
// movable frame of its own - the navigator's waypoint pill, the loot feed.
// Each is only draggable (and only eats clicks) while the atlas window is
// open, so none of them steals a click during normal play. Publish an empty
// rect to give the space back.
constexpr int kAuxRects = 4;
void input_set_aux_rect(int slot, int x, int y, int w, int h);

// True when the point falls inside the main window's rectangle. The atlas
// window draws over the navigator's frame, so the navigator uses this to
// yield any click the window is visually covering - input priority has to
// follow what the player can see.
bool input_in_main_rect(int x, int y);

}  // namespace fmk
