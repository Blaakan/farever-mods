// ---------------------------------------------------------------------------
// routes.cpp
//
// Threading mirrors the rest of the host: the worker thread loads and saves,
// the render thread draws and edits. One critical section covers the route
// list, because both sides touch it and every hold is a copy of a few
// hundred structs at worst.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "paths.h"
#include "overlay.h"
#include "routes.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

CRITICAL_SECTION g_cs;
bool g_cs_init = false;

std::vector<Route> g_routes;
volatile LONG g_custom_dirty = 0;

// A generated chest route is hundreds of waypoints, and there can be dozens
// of routes, so copying the list under the lock every frame would be the
// most expensive thing the draw callback does. The render thread keeps its
// own copy and refreshes it only when this counter moves, which is when
// something was imported, saved or deleted.
volatile LONG g_gen = 0;
std::vector<Route> g_view;      // render thread only
LONG g_view_gen = -1;

std::wstring g_dir;

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

// --- files ------------------------------------------------------------------

bool read_all(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > 16u * 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    out->resize(size);
    DWORD got = 0;
    const BOOL ok =
        ReadFile(f, out->empty() ? nullptr : &(*out)[0], size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

void write_all(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(f);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

// The one text format, shared by both files and by the share code, so a
// route pasted out of a file imports and a route imported can be read.
//
//   [Primevalley - world chests]
//   mode = nearest
//   zone = Primevalley
//   -0.7, 1093.2, 112.5, World Chest
//
// A coordinate line is three numbers then a label that runs to end of line,
// so a label may contain commas.
void parse_routes(const std::string& text, bool custom,
                  std::vector<Route>* out) {
    Route cur;
    bool open = false;
    auto flush = [&]() {
        if (open && !cur.points.empty()) out->push_back(cur);
        cur = Route{};
        open = false;
    };
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']') {
            flush();
            cur.name = trim(line.substr(1, line.size() - 2));
            cur.custom = custom;
            open = true;
            continue;
        }
        if (!open) continue;

        const size_t eq = line.find('=');
        if (eq != std::string::npos && line.find(',') > eq) {
            const std::string k = trim(line.substr(0, eq));
            const std::string v = trim(line.substr(eq + 1));
            if (k == "mode")
                cur.mode = (v == "order") ? kNavOrder : kNavRoute;
            else if (k == "zone")
                cur.zone = v;
            continue;
        }

        NavTarget t{};
        if (sscanf_s(line.c_str(), "%lf,%lf,%lf", &t.x, &t.y, &t.z) != 3)
            continue;
        const char* s = line.c_str();
        const char* lab = s;
        for (int i = 0; i < 3 && lab; i++) {
            lab = strchr(lab, ',');
            if (lab) lab++;
        }
        _snprintf_s(t.label, sizeof(t.label), _TRUNCATE, "%s",
                    (lab && *lab) ? trim(lab).c_str() : cur.name.c_str());
        // A route long enough to outrun this is a bug in whatever wrote it,
        // not something to load into a 20Hz distance loop.
        if (cur.points.size() < 2000) cur.points.push_back(t);
    }
    flush();
}

std::string serialize_route(const Route& r) {
    std::string out = "[" + r.name + "]\n";
    out += std::string("mode = ") + (r.mode == kNavOrder ? "order" : "nearest") +
           "\n";
    if (!r.zone.empty()) out += "zone = " + r.zone + "\n";
    char one[224];
    for (const auto& t : r.points) {
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%.1f, %.1f, %.1f, %s\n", t.x,
                    t.y, t.z, t.label);
        out += one;
    }
    return out;
}

void save_custom_locked() {
    std::string out =
        "# farever-modkit custom routes.\n"
        "# Routes you record in game, and routes you import, land here.\n"
        "# Format: [name], then `mode = nearest|order`, then one\n"
        "# `x, y, z, label` line per waypoint. Safe to edit by hand.\n\n";
    for (const auto& r : g_routes)
        if (r.custom) out += serialize_route(r) + "\n";
    write_all(g_dir + L"farever-routes-custom.txt", out);
}

// --- share codes ------------------------------------------------------------

const char* kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) |
                           ((unsigned char)in[i + 1] << 8) |
                           (unsigned char)in[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        const bool two = (i + 1) < in.size();
        if (two) v |= (unsigned char)in[i + 1] << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += two ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string b64_decode(const std::string& in) {
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(unsigned char)kB64[i]] = i;
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = rev[(unsigned char)c];
        if (v < 0) continue;                 // whitespace, newlines, padding
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

const char kSharePrefix[] = "FMKR1:";

bool clipboard_get(std::string* out) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                                              nullptr, nullptr);
            if (n > 1 && n < 8 * 1024 * 1024) {
                out->resize(n - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, &(*out)[0], n, nullptr,
                                    nullptr);
                ok = true;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

bool clipboard_put(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!h) return false;
    if (wchar_t* w = (wchar_t*)GlobalLock(h)) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w, n);
        GlobalUnlock(h);
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(h);
        return false;
    }
    EmptyClipboard();
    // SetClipboardData takes ownership on success; on failure it is still
    // ours to release.
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(h);
    return ok;
}

// --- page state (render thread only) ----------------------------------------

float g_scroll = 0;
std::string g_toast;              // one line of feedback under the buttons
DWORD g_toast_tick = 0;
bool g_naming = false;            // the save-as-route field has focus
std::string g_name_buf;

void toast(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_toast = buf;
    g_toast_tick = GetTickCount();
}

// A flat button. Returns true on the frame it is clicked.
bool button(const InputState& in, bool clicked, float x, float y, float w,
            float h, const char* label, bool enabled, Color on) {
    const bool hot = enabled && in.mouse_x >= x && in.mouse_x < x + w &&
                     in.mouse_y >= y && in.mouse_y < y + h;
    Color bg = enabled ? (hot ? on : Color{0.14f, 0.17f, 0.24f, 1.0f})
                       : Color{0.09f, 0.10f, 0.14f, 1.0f};
    draw_rect(x, y, w, h, bg);
    draw_rect_outline(x, y, w, h, 1.0f,
                      enabled ? Color{0.30f, 0.38f, 0.52f, 1.0f}
                              : Color{0.16f, 0.18f, 0.24f, 1.0f});
    const float tw = measure_text(12, label);
    draw_text(x + (w - tw) * 0.5f, y + (h - 12) * 0.5f - 1, 12,
              enabled ? Color{0.90f, 0.93f, 0.98f, 1.0f}
                      : Color{0.35f, 0.38f, 0.45f, 1.0f},
              label);
    return enabled && clicked && in.click_x >= x && in.click_x < x + w &&
           in.click_y >= y && in.click_y < y + h;
}

// The route the pill is following, expressed as a Route so it can be saved
// or shared. Only the outstanding waypoints are kept: what you want to hand
// someone is the route, and what you want to save mid-run is usually the
// part you have not done.
Route active_as_route(const NavStatus& st) {
    Route r;
    r.name = st.name;
    // A saved list is a route by definition, so a plain item track - which is
    // "any one of these will do" - is normalised to the collection reading
    // rather than written out as a mode the file format cannot express.
    r.mode = (st.mode == kNavOrder) ? kNavOrder : kNavRoute;
    r.custom = true;
    return r;
}

}  // namespace

void routes_init() {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    g_dir = data_dir();

    std::vector<Route> loaded;
    std::string text;
    if (read_all(g_dir + L"farever-routes.txt", &text))
        parse_routes(text, false, &loaded);
    if (read_all(g_dir + L"farever-routes-custom.txt", &text))
        parse_routes(text, true, &loaded);

    // Generated routes first and alphabetical inside each group: the built-in
    // set is the one you scan for "is there already a chest route here", and
    // your own are the short list you know by name.
    std::stable_sort(loaded.begin(), loaded.end(),
                     [](const Route& a, const Route& b) {
                         if (a.custom != b.custom) return !a.custom;
                         return a.name < b.name;
                     });

    Lock lk;
    g_routes = std::move(loaded);
    host_log("routes: %d loaded", (int)g_routes.size());
}

void routes_tick() {
    if (!g_cs_init) return;
    if (!InterlockedExchange(&g_custom_dirty, 0)) return;
    Lock lk;
    save_custom_locked();
}

int routes_count() {
    if (!g_cs_init) return 0;
    Lock lk;
    return (int)g_routes.size();
}

void routes_draw(const InputState& in, bool clicked, float x, float y, float w,
                 float h) {
    if (!g_cs_init) return;

    NavStatus st;
    nav_status(&st);

    const float pad = 8;
    float cy = y;

    // --- what the navigator is doing right now ------------------------------
    //
    // The controls for a running route belong next to the statement that one
    // is running, not in a menu somewhere else.
    {
        const float box_h = 58;
        draw_rect(x, cy, w, box_h, {0.07f, 0.09f, 0.13f, 1.0f});
        draw_rect_outline(x, cy, w, box_h, 1.0f,
                          st.active ? Color{0.30f, 0.55f, 0.80f, 1.0f}
                                    : Color{0.16f, 0.18f, 0.24f, 1.0f});
        if (st.active) {
            char head[192];
            _snprintf_s(head, sizeof(head), _TRUNCATE, "Following: %s", st.name);
            draw_text(x + pad, cy + 7, 14, {0.92f, 0.95f, 1.0f, 1.0f}, head);
            char sub[256];
            if (st.is_route)
                _snprintf_s(sub, sizeof(sub), _TRUNCATE,
                            "%d of %d done - next: %s%s%s", st.done, st.total,
                            st.label[0] ? st.label : "-",
                            st.where[0] ? "  -  " : "", st.where);
            else
                _snprintf_s(sub, sizeof(sub), _TRUNCATE,
                            "tracking one place - %s%s%s",
                            st.label[0] ? st.label : "-",
                            st.where[0] ? "  -  " : "", st.where);
            draw_text(x + pad, cy + 27, 12, {0.60f, 0.66f, 0.76f, 1.0f}, sub);

            const float bw = 68, bh = 22, by = cy + 16;
            float bx = x + w - pad - bw;
            if (button(in, clicked, bx, by, bw, bh, "Stop", true,
                       {0.45f, 0.20f, 0.22f, 1.0f}))
                nav_untrack();
            bx -= bw + 6;
            if (button(in, clicked, bx, by, bw, bh, "Restart", st.is_route,
                       {0.20f, 0.30f, 0.45f, 1.0f}))
                nav_restart();
            bx -= bw + 6;
            if (button(in, clicked, bx, by, bw, bh, "Skip", st.is_route,
                       {0.20f, 0.30f, 0.45f, 1.0f}))
                nav_skip();
            // What the list means, and the one place it can be changed. It
            // matters most for a queue built by hand: "add this first" only
            // decides where the arrow goes next if order is what decides.
            bx -= 106 + 6;
            const bool ordered = st.mode == kNavOrder;
            if (button(in, clicked, bx, by, 106, bh,
                       ordered ? "In order" : "Nearest first", st.is_route,
                       {0.20f, 0.30f, 0.45f, 1.0f}))
                nav_set_mode(ordered ? kNavRoute : kNavOrder);

            // The same two actions on keys, because they are wanted while
            // running and this page is behind F8.
            draw_text(x + pad, cy + 41, 11, {0.40f, 0.45f, 0.55f, 1.0f},
                      "F10 skips the current waypoint - Shift+F10 clears the "
                      "route");
        } else {
            draw_text(x + pad, cy + 7, 14, {0.55f, 0.60f, 0.70f, 1.0f},
                      "Nothing tracked");
            draw_text(x + pad, cy + 27, 12, {0.40f, 0.45f, 0.55f, 1.0f},
                      "Start a route below, click a point of interest on the "
                      "game's own map, or press F9 to drop a waypoint where "
                      "you stand.");
        }
        cy += box_h + 8;
    }

    // --- make and share ------------------------------------------------------
    {
        const float bh = 24;
        float bx = x;
        double hx = 0, hy = 0, hz = 0;
        const bool have_hero = nav_hero_pos(&hx, &hy, &hz);

        if (button(in, clicked, bx, cy, 128, bh, "Drop waypoint here",
                   have_hero, {0.20f, 0.34f, 0.50f, 1.0f})) {
            nav_queue("Waypoint", hx, hy, hz);
            toast("Waypoint dropped at %.0f, %.0f", hx, hy);
        }
        bx += 134;

        if (button(in, clicked, bx, cy, 120, bh, "Import from clipboard", true,
                   {0.20f, 0.34f, 0.50f, 1.0f})) {
            std::string text;
            if (!clipboard_get(&text)) {
                toast("Could not read the clipboard.");
            } else {
                std::string body = text;
                const size_t at = body.find(kSharePrefix);
                if (at != std::string::npos)
                    body = b64_decode(body.substr(at + sizeof(kSharePrefix) - 1));
                std::vector<Route> got;
                parse_routes(body, true, &got);
                if (got.empty()) {
                    toast("No route found on the clipboard.");
                } else {
                    Lock lk;
                    int pts = 0;
                    for (auto& r : got) {
                        r.custom = true;
                        pts += (int)r.points.size();
                        // Importing the same route twice should update it,
                        // not leave two rows with the same name.
                        auto same = std::find_if(
                            g_routes.begin(), g_routes.end(),
                            [&](const Route& e) { return e.name == r.name; });
                        if (same != g_routes.end() && same->custom)
                            *same = r;
                        else
                            g_routes.push_back(r);
                    }
                    InterlockedExchange(&g_custom_dirty, 1);
                    InterlockedIncrement(&g_gen);
                    toast("Imported %d route%s, %d waypoints.", (int)got.size(),
                          got.size() == 1 ? "" : "s", pts);
                }
            }
        }
        bx += 126;

        if (button(in, clicked, bx, cy, 118, bh, "Copy active as code",
                   st.active, {0.20f, 0.34f, 0.50f, 1.0f})) {
            Route r = active_as_route(st);
            // Rebuilt from the routes list when the active thing is one of
            // ours, so the code carries the whole route rather than only the
            // part still outstanding.
            {
                Lock lk;
                auto same = std::find_if(
                    g_routes.begin(), g_routes.end(),
                    [&](const Route& e) { return e.name == r.name; });
                if (same != g_routes.end()) r = *same;
            }
            if (r.points.empty()) {
                toast("That is a tracked item, not a saved route - save it "
                      "first.");
            } else {
                const std::string code =
                    std::string(kSharePrefix) + b64_encode(serialize_route(r));
                toast(clipboard_put(code)
                          ? "Share code copied - paste it anywhere."
                          : "Could not write to the clipboard.");
            }
        }
        cy += bh + 8;
    }

    // --- save what is being followed as a named route ------------------------
    if (st.active) {
        const float bh = 24, fw = 240;
        const bool hot = in.mouse_x >= x && in.mouse_x < x + fw &&
                         in.mouse_y >= cy && in.mouse_y < cy + bh;
        if (clicked) {
            const bool hit = in.click_x >= x && in.click_x < x + fw &&
                             in.click_y >= cy && in.click_y < cy + bh;
            if (hit != g_naming) {
                g_naming = hit;
                input_set_text_capture(hit);
                if (hit && g_name_buf.empty()) g_name_buf = st.name;
            }
        }
        if (g_naming && !input_text_capture()) g_naming = false;

        bool submit = false;
        if (g_naming) {
            char typed[64];
            const int n = input_take_text(typed, sizeof(typed));
            for (int i = 0; i < n; i++) {
                if (typed[i] == '\b') {
                    if (!g_name_buf.empty()) g_name_buf.pop_back();
                } else if (typed[i] == '\n') {
                    submit = true;
                } else if (g_name_buf.size() < 60) {
                    g_name_buf.push_back(typed[i]);
                }
            }
        }

        draw_rect(x, cy, fw, bh, {0.03f, 0.04f, 0.06f, 1.0f});
        draw_rect_outline(x, cy, fw, bh, 1.0f,
                          g_naming ? Color{0.35f, 0.75f, 1.0f, 1.0f}
                                   : (hot ? Color{0.45f, 0.50f, 0.60f, 1.0f}
                                          : Color{0.20f, 0.23f, 0.30f, 1.0f}));
        if (g_name_buf.empty() && !g_naming) {
            draw_text(x + 7, cy + 5, 12, {0.38f, 0.42f, 0.50f, 1.0f},
                      "Name it, then Save as route");
        } else {
            std::string shown = g_name_buf;
            if (g_naming) shown += "_";
            draw_text(x + 7, cy + 5, 12, {0.90f, 0.93f, 0.98f, 1.0f},
                      shown.c_str());
        }

        const bool can_save = !trim(g_name_buf).empty();
        if (button(in, clicked, x + fw + 6, cy, 110, bh, "Save as route",
                   can_save, {0.22f, 0.42f, 0.30f, 1.0f}) ||
            (submit && can_save)) {
            // The waypoints come from the navigator, which is the only place
            // that knows what has been dropped since the last save.
            Route r = active_as_route(st);
            r.name = trim(g_name_buf);
            // nav_status only names the current waypoint, so the list itself
            // comes straight out of the navigator - until this moment it
            // exists nowhere else.
            nav_active_points(&r.points);
            if (r.points.empty()) {
                toast("Nothing to save.");
            } else {
                Lock lk;
                auto same = std::find_if(
                    g_routes.begin(), g_routes.end(),
                    [&](const Route& e) { return e.name == r.name; });
                if (same != g_routes.end() && same->custom)
                    *same = r;
                else
                    g_routes.push_back(r);
                InterlockedExchange(&g_custom_dirty, 1);
                InterlockedIncrement(&g_gen);
                toast("Saved '%s' with %d waypoints.", r.name.c_str(),
                      (int)r.points.size());
            }
            g_naming = false;
            input_set_text_capture(false);
        }
        cy += bh + 8;
    }

    // --- feedback line -------------------------------------------------------
    if (!g_toast.empty()) {
        if (GetTickCount() - g_toast_tick > 8000) g_toast.clear();
        else {
            draw_text(x, cy, 12, {0.70f, 0.82f, 0.60f, 1.0f}, g_toast.c_str());
            cy += 18;
        }
    }

    // --- the list ------------------------------------------------------------
    const LONG gen = InterlockedCompareExchange(&g_gen, 0, 0);
    if (gen != g_view_gen) {
        Lock lk;
        g_view = g_routes;
        g_view_gen = gen;
    }
    const std::vector<Route>& list = g_view;

    const float list_y = cy;
    const float list_h = (y + h) - list_y;
    if (list_h < 40) return;

    // Scrolling moves by whole rows, and only whole rows are drawn. The
    // overlay has no way to clip text, so a half-row at either end would
    // paint over the buttons above or past the window's bottom edge - and a
    // list of named things reads better in whole lines anyway.
    const float row_h = 34;
    const int per_page = (int)(list_h / row_h);
    const int max_top = (int)list.size() - per_page;
    float max_scroll = max_top > 0 ? max_top * row_h : 0;
    g_scroll -= in.wheel * row_h * 2;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    const float total_h = list.size() * row_h;

    if (list.empty()) {
        draw_text(x, list_y + 6, 13, {0.45f, 0.50f, 0.60f, 1.0f},
                  "No routes yet. Run tools/gen-routes.mjs for the generated "
                  "set, or drop waypoints and save them.");
        return;
    }

    const int first = (int)(g_scroll / row_h);
    for (int i = first; i < (int)list.size(); i++) {
        const float ry = list_y + (i - first) * row_h;
        if (ry + row_h > list_y + list_h) break;
        const Route& r = list[i];
        const bool running = st.active && r.name == st.name;
        const bool hot = in.mouse_x >= x && in.mouse_x < x + w &&
                         in.mouse_y >= ry && in.mouse_y < ry + row_h - 2;

        draw_rect(x, ry, w, row_h - 2,
                  running ? Color{0.13f, 0.22f, 0.32f, 1.0f}
                          : (hot ? Color{0.11f, 0.13f, 0.19f, 1.0f}
                                 : Color{0.07f, 0.08f, 0.12f, 1.0f}));
        if (running) draw_rect(x, ry, 3, row_h - 2, {0.35f, 0.75f, 1.0f, 1.0f});

        draw_text(x + 10, ry + 4, 13, {0.90f, 0.93f, 0.98f, 1.0f},
                  r.name.c_str());
        char sub[160];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%d waypoints - %s%s%s",
                    (int)r.points.size(),
                    r.mode == kNavOrder ? "in order" : "nearest first",
                    r.zone.empty() ? "" : " - ", r.zone.c_str());
        draw_text(x + 10, ry + 19, 11, {0.48f, 0.53f, 0.63f, 1.0f}, sub);

        // Distance to the nearest waypoint: the one number that says whether
        // this route is worth starting from where you are standing.
        char dist[64];
        if (nav_format_distance(r.points.data(), (int)r.points.size(), dist,
                                sizeof(dist))) {
            const float dw = measure_text(12, dist);
            draw_text(x + w - 190 - dw, ry + 9, 12, {0.58f, 0.64f, 0.74f, 1.0f},
                      dist);
        }

        const float bw = 62, bh = 22, by = ry + 5;
        if (button(in, clicked, x + w - 10 - bw, by, bw, bh,
                   running ? "Restart" : "Start", true,
                   {0.20f, 0.34f, 0.50f, 1.0f})) {
            char key[224];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "route/%s", r.name.c_str());
            nav_start_route(key, r.name.c_str(), r.points.data(),
                            (int)r.points.size(), r.mode);
        }
        if (button(in, clicked, x + w - 16 - bw * 2, by, bw, bh, "Copy", true,
                   {0.20f, 0.34f, 0.50f, 1.0f})) {
            const std::string code =
                std::string(kSharePrefix) + b64_encode(serialize_route(r));
            if (clipboard_put(code))
                toast("Share code for '%s' copied.", r.name.c_str());
            else
                toast("Could not write to the clipboard.");
        }
        // Only your own routes can be deleted from in here: the generated
        // file is rewritten wholesale by the tool, so removing a row from it
        // would come back on the next patch and read as a bug.
        if (r.custom &&
            button(in, clicked, x + w - 22 - bw * 2 - 26, by, 26, bh, "x", true,
                   {0.45f, 0.20f, 0.22f, 1.0f})) {
            const std::string name = r.name;   // `r` dies with the refresh
            Lock lk;
            auto same = std::find_if(
                g_routes.begin(), g_routes.end(),
                [&](const Route& e) { return e.name == name && e.custom; });
            if (same != g_routes.end()) {
                g_routes.erase(same);
                InterlockedExchange(&g_custom_dirty, 1);
                InterlockedIncrement(&g_gen);
                toast("Deleted '%s'.", name.c_str());
            }
        }
    }

    if (max_scroll > 0) {
        const float tx = x + w - 4;
        draw_rect(tx, list_y, 4, list_h, {0.10f, 0.11f, 0.16f, 1.0f});
        const float th = list_h * (list_h / total_h);
        draw_rect(tx, list_y + (list_h - th) * (g_scroll / max_scroll), 4, th,
                  {0.30f, 0.38f, 0.52f, 1.0f});
    }
}

}  // namespace fmk
