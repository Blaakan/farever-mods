// ---------------------------------------------------------------------------
// input.cpp
//
// The subclass runs on the game's window thread; the UI reads on the render
// thread. Every shared field is a LONG accessed with interlocked ops - no
// locks in a WndProc.
//
// Swallowing policy: while the UI is visible, mouse presses/wheel inside the
// UI rectangle are consumed, and a press that started inside keeps the whole
// drag (moves + release) until the button goes up - the window takes mouse
// capture for that stretch so the release is seen even outside the window.
// Swallowed presses latch, so their release/double-click halves never leak
// to the game on their own. The toggle key and Escape (only while visible)
// are the only keys touched; WASD etc. keep working with the atlas open.
//
// Liveness: the UI republishes its rectangle every drawn frame. If the
// overlay goes dormant (device loss, swap-chain recreation), the rect goes
// stale; a stale rect stops all swallowing, so a dead overlay can never
// leave an invisible input dead zone in the middle of the screen.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include "input.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

constexpr UINT kToggleKey = VK_F8;
constexpr DWORD kRectFreshMs = 3000;

HWND    g_hwnd = nullptr;
WNDPROC g_orig = nullptr;

volatile LONG g_mouse_x = 0, g_mouse_y = 0;
volatile LONG g_lbutton = 0;        // press started inside the UI, still held
volatile LONG g_clicks = 0;
volatile LONG g_click_x = 0, g_click_y = 0;
volatile LONG g_wheel_raw = 0;      // accumulated wheel delta (not detents)
volatile LONG g_visible = 0;
volatile LONG g_rect[4] = {0, 0, 0, 0};   // x, y, w, h
volatile LONG g_aux[4] = {0, 0, 0, 0};    // the navigator's frame
volatile LONG g_rect_tick = 0;            // GetTickCount of the last publish

// Latches so the second half of a swallowed press/key never leaks.
volatile LONG g_rbtn_held = 0;
volatile LONG g_mbtn_held = 0;
volatile LONG g_xbtn_held = 0;
volatile LONG g_esc_held = 0;

bool in_rect(const volatile LONG* r, int x, int y) {
    const LONG rx = r[0], ry = r[1], rw = r[2], rh = r[3];
    return rw > 0 && rh > 0 && x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

bool in_ui_rect(int x, int y) {
    return in_rect(g_rect, x, y) || in_rect(g_aux, x, y);
}

// Visible AND the render thread is actually drawing the window. The rect is
// republished every drawn frame, so a stale tick means the overlay stopped.
bool ui_active() {
    if (!InterlockedCompareExchange(&g_visible, 0, 0)) return false;
    const DWORD tick = (DWORD)InterlockedCompareExchange(&g_rect_tick, 0, 0);
    return (GetTickCount() - tick) < kRectFreshMs;
}

void clear_held_buttons() {
    InterlockedExchange(&g_lbutton, 0);
    InterlockedExchange(&g_rbtn_held, 0);
    InterlockedExchange(&g_mbtn_held, 0);
    InterlockedExchange(&g_xbtn_held, 0);
}

LRESULT CALLBACK hook_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool active = ui_active();

    switch (msg) {
        case WM_KEYDOWN:
            if (wp == kToggleKey && !(lp & (1 << 30))) {   // ignore autorepeat
                InterlockedXor(&g_visible, 1);
                return 0;
            }
            if (wp == VK_ESCAPE && active) {
                InterlockedExchange(&g_visible, 0);
                InterlockedExchange(&g_esc_held, 1);
                return 0;
            }
            break;

        case WM_KEYUP:
            if (wp == kToggleKey) return 0;
            if (wp == VK_ESCAPE && InterlockedExchange(&g_esc_held, 0)) return 0;
            break;

        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            InterlockedExchange(&g_mouse_x, x);
            InterlockedExchange(&g_mouse_y, y);
            if (active && (g_lbutton || in_ui_rect(x, y)))
                return 0;
            break;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            InterlockedExchange(&g_mouse_x, x);
            InterlockedExchange(&g_mouse_y, y);
            if (active && in_ui_rect(x, y)) {
                InterlockedExchange(&g_click_x, x);
                InterlockedExchange(&g_click_y, y);
                InterlockedExchange(&g_lbutton, 1);
                InterlockedIncrement(&g_clicks);
                // Capture keeps the release visible even when it lands
                // outside the window (title-bar drags routinely do).
                SetCapture(hwnd);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP:
            if (InterlockedExchange(&g_lbutton, 0)) {
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            break;

        // Losing capture or focus mid-drag: the release will never arrive,
        // so drop every held-button latch instead of swallowing forever.
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            clear_held_buttons();
            break;

        case WM_MOUSEWHEEL: {
            // Wheel coordinates are screen, not client.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (active && in_ui_rect(pt.x, pt.y)) {
                // Raw delta, not detents: precision touchpads send fractions
                // of WHEEL_DELTA and truncating them here would eat them.
                InterlockedAdd(&g_wheel_raw, GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            }
            break;
        }

        // Other buttons get no UI meaning, but a press over the open window
        // must not walk or attack underneath it - and once the press is
        // swallowed, its release must be too, wherever it lands.
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK: {
            if (active && in_ui_rect(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
                InterlockedExchange(&g_rbtn_held, 1);
                return 0;
            }
            break;
        }
        case WM_RBUTTONUP:
            if (InterlockedExchange(&g_rbtn_held, 0)) return 0;
            break;

        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK: {
            if (active && in_ui_rect(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
                InterlockedExchange(&g_mbtn_held, 1);
                return 0;
            }
            break;
        }
        case WM_MBUTTONUP:
            if (InterlockedExchange(&g_mbtn_held, 0)) return 0;
            break;

        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK: {
            if (active && in_ui_rect(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
                InterlockedExchange(&g_xbtn_held, 1);
                return TRUE;
            }
            break;
        }
        case WM_XBUTTONUP:
            if (InterlockedExchange(&g_xbtn_held, 0)) return TRUE;
            break;
    }

    const WNDPROC orig = g_orig;
    if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

}  // namespace

bool input_install(void* hwnd) {
    if (g_hwnd) return true;
    HWND h = (HWND)hwnd;
    if (!h || !IsWindow(h)) return false;
    // g_orig must be valid BEFORE the hook goes live: the window thread can
    // enter hook_proc the instant SetWindowLongPtr swaps the pointer, and it
    // must never chain into null.
    WNDPROC prev = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!prev) {
        host_log("input: GetWindowLongPtr failed (%lu)", GetLastError());
        return false;
    }
    g_orig = prev;
    WNDPROC swapped = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC,
                                                 (LONG_PTR)&hook_proc);
    if (!swapped) {
        g_orig = nullptr;
        host_log("input: SetWindowLongPtr failed (%lu)", GetLastError());
        return false;
    }
    // If another hook slid in between the two calls, chain to what we
    // actually displaced.
    if (swapped != prev) g_orig = swapped;
    g_hwnd = h;
    host_log("input: WndProc hooked on %p", hwnd);
    return true;
}

void input_uninstall() {
    if (!g_hwnd) return;
    // Only restore if we are still the current proc; if something hooked
    // after us, restoring would unhook them too.
    if ((WNDPROC)GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) == &hook_proc)
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig);
    g_hwnd = nullptr;
    // g_orig stays set: the window thread may still be inside hook_proc.
}

void input_peek(InputState* out) {
    // Counter before payload, mirroring the writer's payload-then-counter
    // order: a click that lands mid-snapshot is seen next frame with its
    // payload complete, never this frame with the payload missing.
    out->clicks  = InterlockedCompareExchange(&g_clicks, 0, 0);
    out->click_x = InterlockedCompareExchange(&g_click_x, 0, 0);
    out->click_y = InterlockedCompareExchange(&g_click_y, 0, 0);
    out->lbutton = InterlockedCompareExchange(&g_lbutton, 0, 0) != 0;
    out->mouse_x = InterlockedCompareExchange(&g_mouse_x, 0, 0);
    out->mouse_y = InterlockedCompareExchange(&g_mouse_y, 0, 0);
    out->wheel = 0;
    out->visible = InterlockedCompareExchange(&g_visible, 0, 0) != 0;
}

void input_get(InputState* out) {
    input_peek(out);
    // Consume whole detents, keep the fractional remainder accumulating.
    const LONG raw = InterlockedExchange(&g_wheel_raw, 0);
    const LONG rem = raw % WHEEL_DELTA;
    if (rem) InterlockedAdd(&g_wheel_raw, rem);
    out->wheel = raw / WHEEL_DELTA;
}

void input_set_visible(bool v) {
    InterlockedExchange(&g_visible, v ? 1 : 0);
}

void input_set_ui_rect(int x, int y, int w, int h) {
    InterlockedExchange(&g_rect[0], x);
    InterlockedExchange(&g_rect[1], y);
    InterlockedExchange(&g_rect[2], w);
    InterlockedExchange(&g_rect[3], h);
    InterlockedExchange(&g_rect_tick, (LONG)GetTickCount());
}

bool input_in_main_rect(int x, int y) { return in_rect(g_rect, x, y); }

void input_set_aux_rect(int x, int y, int w, int h) {
    InterlockedExchange(&g_aux[0], x);
    InterlockedExchange(&g_aux[1], y);
    InterlockedExchange(&g_aux[2], w);
    InterlockedExchange(&g_aux[3], h);
}

}  // namespace fmk
