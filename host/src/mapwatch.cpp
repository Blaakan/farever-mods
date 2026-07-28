// ---------------------------------------------------------------------------
// mapwatch.cpp
//
// Single-threaded: everything here runs on the pose thread, which is also
// where the reader's map walk belongs (it is a short pointer chase, the same
// shape as the camera read next to it). No locks, no shared state beyond the
// two counters input.cpp publishes.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "hl_reader.h"
#include "input.h"
#include "mapwatch.h"
#include "navigator.h"
#include "overlay.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

enum ClickMode : int { kClickOff = 0, kClickAny = 1, kClickShift = 2 };
int g_click_mode = kClickAny;
bool g_mirror_pins = true;

int g_seen_clicks = -1;         // -1 = not yet synchronised

// The last place a waypoint was dropped, so a double-click on one POI - or a
// click the game also treats as a double - does not queue it twice.
bool   g_have_last = false;
double g_lx = 0, g_ly = 0, g_lz = 0;
DWORD  g_last_tick = 0;
constexpr DWORD kRepeatMs = 2500;
constexpr double kRepeatDist = 2.0;

// The pins seen on the last poll. A pin that was not there before is one the
// player has just placed, which is as clear a "take me there" as a click.
std::vector<MapPin> g_pins;
bool g_pins_known = false;

bool is_repeat(double x, double y, double z) {
    if (!g_have_last) return false;
    if (GetTickCount() - g_last_tick > kRepeatMs) return false;
    const double dx = x - g_lx, dy = y - g_ly, dz = z - g_lz;
    return sqrt(dx * dx + dy * dy + dz * dz) < kRepeatDist;
}

void queue(const char* label, double x, double y, double z, const char* how) {
    if (is_repeat(x, y, z)) return;
    g_have_last = true;
    g_lx = x;
    g_ly = y;
    g_lz = z;
    g_last_tick = GetTickCount();
    if (!g_last_tick) g_last_tick = 1;
    nav_queue(label, x, y, z);
    host_log("map: waypoint from %s - '%s' at %.1f,%.1f,%.1f", how, label, x, y,
             z);
}

bool same_place(const MapPin& a, const MapPin& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrt(dx * dx + dy * dy + dz * dz) < 1.0;
}

}  // namespace

void mapwatch_init() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;
    const std::wstring ini = std::wstring(path) + L"farever-modkit.ini";
    const int v = GetPrivateProfileIntW(L"map", L"click", kClickAny, ini.c_str());
    g_click_mode = (v == kClickOff || v == kClickShift) ? v : kClickAny;
    g_mirror_pins = GetPrivateProfileIntW(L"map", L"pins", 1, ini.c_str()) != 0;
}

void mapwatch_poll(bool in_world) {
    const int f9 = input_take_waypoint_presses();

    RawClick click;
    input_peek_raw_click(&click);
    // First pass only synchronises: a click made before the host was ready
    // is not a click at a map that was not being read.
    const bool new_click = g_seen_clicks >= 0 && click.count != g_seen_clicks;
    g_seen_clicks = click.count;

    if (!in_world) return;

    // One read per tick, shared by every path, and every tick: a click is
    // noticed on the tick it arrives, so skipping ticks would drop the odd
    // one. This walks the open-window list and the pins, not the markers -
    // the marker walk is hundreds of objects and only happens on a click.
    MapState map;
    const bool open = reader_read_map_state(&map) && map.open;

    // One line each time the map opens or closes, naming everything that
    // could have supplied a target. The first attempt at this feature read
    // `nearClickableMarker`, which belongs to the gamepad crosshair and is
    // null with a mouse - this line is what turned that from a theory into a
    // fact, so it stays.
    static int last_open = -1;
    if ((int)open != last_open) {
        last_open = (int)open;
        if (open) {
            float fw = 0, fh = 0;
            overlay_frame_size(&fw, &fh);
            host_log("map: open (window=%p visible=%d parented=%d markers=%d "
                     "pins=%d scene=%dx%d frame=%.0fx%.0f nearClickable=%p "
                     "mouseCursor=%p)",
                     map.window, (int)map.visible, (int)map.parented,
                     map.markers, (int)map.pins.size(), map.scene_w,
                     map.scene_h, fw, fh, map.near_clickable,
                     map.mouse_cursor);
        } else {
            host_log("map: closed");
        }
    }

    if (!open) {
        // Pins are only knowable while the map is up; forget them rather than
        // announce the whole set as new on the next open.
        g_pins.clear();
        g_pins_known = false;
    }

    // --- a click on a point of interest -------------------------------------
    //
    // The marker list is walked only here. Each marker knows where it is on
    // screen, so this is a proximity test in the UI's own units against the
    // mouse mapped into them - the map's zoom and panning never enter into it.
    if (new_click && open && g_click_mode != kClickOff &&
        (g_click_mode != kClickShift || click.shift)) {
        float fw = 0, fh = 0;
        MapPin hit;
        if (overlay_frame_size(&fw, &fh) &&
            reader_map_pick(click.x, click.y, fw, fh, &hit)) {
            queue(hit.label.c_str(), hit.x, hit.y, hit.z, "map click");
        } else {
            // A click on the map that found nothing is worth a line: clicks
            // are rare enough not to spam, and this is the difference between
            // "nothing was there" and "the hit test is looking in the wrong
            // place", which the coordinates answer.
            host_log("map: click at %d,%d found no marker (frame=%.0fx%.0f)",
                     click.x, click.y, fw, fh);
        }
    }

    // --- the player's own pins ----------------------------------------------
    if (open && g_mirror_pins) {
        if (!g_pins_known) {
            // First sight of the map this session: whatever is already
            // pinned was pinned before, not just now.
            g_pins = map.pins;
            g_pins_known = true;
        } else {
            for (const auto& p : map.pins) {
                bool seen = false;
                for (const auto& old : g_pins)
                    if (same_place(p, old)) { seen = true; break; }
                if (!seen)
                    queue(p.label.c_str(), p.x, p.y, p.z, "a map pin");
            }
            g_pins = map.pins;
        }
    }

    // --- F9 -----------------------------------------------------------------
    //
    // It has always meant "drop a waypoint"; what counts as "here" depends on
    // what you are looking at. Over the map that is whatever is under the
    // mouse, which is more useful than your own feet - you cannot see those
    // from the map anyway.
    for (int i = f9; i > 0; i--) {
        float fw = 0, fh = 0;
        MapPin hit;
        POINT pt{};
        if (open && overlay_frame_size(&fw, &fh) && GetCursorPos(&pt) &&
            ScreenToClient((HWND)overlay_game_hwnd(), &pt) &&
            reader_map_pick(pt.x, pt.y, fw, fh, &hit)) {
            queue(hit.label.c_str(), hit.x, hit.y, hit.z, "F9 over the map");
        } else {
            double x = 0, y = 0, z = 0;
            if (nav_hero_pos(&x, &y, &z))
                queue("Waypoint", x, y, z, "F9 in the world");
        }
    }
}

}  // namespace fmk
