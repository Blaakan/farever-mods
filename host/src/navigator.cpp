// ---------------------------------------------------------------------------
// navigator.cpp
//
// State is tiny and shared across three threads (worker: position + persist,
// window thread: nothing, render thread: track/draw), so one critical
// section guards all of it; every hold is microseconds.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "input.h"
#include "navigator.h"
#include "overlay.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

CRITICAL_SECTION g_cs;
bool g_cs_init = false;

// Tracked item.
std::string g_key;                    // "" = nothing tracked
std::string g_name;
std::vector<NavTarget> g_targets;

// Hero pose, stamped by the pose thread at ~20Hz.
bool   g_pos_valid = false;
double g_hx = 0, g_hy = 0, g_hz = 0;
double g_rz = 0;                      // facing, radians
DWORD  g_pos_tick = 0;
constexpr DWORD kPosFreshMs = 5000;

// Camera view, same cadence. g_cam_valid says the sanity check passed;
// g_view_* is the horizontal direction the screen faces.
bool   g_cam_valid = false;
double g_view_dx = 0, g_view_dy = 0;
double g_cam_dist = 0;      // camera-to-hero distance, diagnostics only

std::wstring g_ini_path;
volatile LONG g_dirty = 0;

// Frame placement, persisted. INT_MIN means "never placed" - the first draw
// centres it near the top, where a waypoint arrow is expected.
constexpr LONG kUnplaced = (LONG)0x80000000;
volatile LONG g_nav_x = kUnplaced, g_nav_y = kUnplaced;
volatile LONG g_layout_dirty = 0;

// Render-thread drag state.
bool  g_dragging = false;
float g_drag_dx = 0, g_drag_dy = 0;
int   g_seen_clicks = 0;
float g_last_rect[4] = {0, 0, 0, 0};

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

bool pos_fresh_locked() {
    return g_pos_valid && (GetTickCount() - g_pos_tick) < kPosFreshMs;
}

// Nearest target by 2D distance; vertical difference rarely matters for
// "which way do I run" and the z values mix terrain heights anyway.
const NavTarget* nearest_locked(const NavTarget* targets, int count) {
    const NavTarget* best = nullptr;
    double best_d = 0;
    for (int i = 0; i < count; i++) {
        const double dx = targets[i].x - g_hx, dy = targets[i].y - g_hy;
        const double d = dx * dx + dy * dy;
        if (!best || d < best_d) { best = &targets[i]; best_d = d; }
    }
    return best;
}

const char* kCompass[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

// Compass bearing of a world-space offset: 0 = north, clockwise positive.
//
// **North is -y.** The game's own data says so: averaging the POI positions
// of the zones it names North and South puts CrimsonIsland_North at y=-743
// against South at y=-420, and Krisomal_North at y=1001 against South at
// y=1237. Both pairs agree. x is east either way, which is why an east/west
// readout looked right while north and south were quietly swapped.
//
// Everything angular in this file goes through here, so the arrow and the
// compass label can never disagree.
double bearing(double dx, double dy) { return atan2(dx, -dy); }

void format_to_locked(const NavTarget& t, char* out, int out_len) {
    const double dx = t.x - g_hx, dy = t.y - g_hy;
    const double dist = sqrt(dx * dx + dy * dy);
    double ang = bearing(dx, dy) * 180.0 / 3.14159265358979;
    if (ang < 0) ang += 360.0;
    const char* dir = kCompass[(int)((ang + 22.5) / 45.0) & 7];
    if (dist >= 1000.0)
        _snprintf_s(out, out_len, _TRUNCATE, "%.2fkm %s", dist / 1000.0, dir);
    else
        _snprintf_s(out, out_len, _TRUNCATE, "%.0fm %s", dist, dir);
}

}  // namespace

void nav_init() {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;
    g_ini_path = std::wstring(path) + L"farever-modkit.ini";

    // Restore the tracked target: key, name, and targets serialized as
    // label@x,y,z;... in the same shape the atlas TSV uses. Every conversion
    // is checked: a value too long for its buffer (hand-edited INI, or a
    // longer format from a future version) must read back as empty, not as
    // uninitialized stack memory handed to strtok.
    wchar_t buf[1024];
    char key[256] = {0}, name[96] = {0}, targets[1200] = {0};
    auto get = [&](const wchar_t* k, char* dst, int dst_len) {
        GetPrivateProfileStringW(L"navigator", k, L"", buf, 1024,
                                 g_ini_path.c_str());
        if (!buf[0]) { dst[0] = 0; return; }
        if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, dst, dst_len, nullptr,
                                 nullptr))
            dst[0] = 0;
    };
    g_nav_x = GetPrivateProfileIntW(L"navigator", L"x", kUnplaced,
                                    g_ini_path.c_str());
    g_nav_y = GetPrivateProfileIntW(L"navigator", L"y", kUnplaced,
                                    g_ini_path.c_str());

    get(L"key", key, sizeof(key));
    if (!key[0]) return;
    get(L"name", name, sizeof(name));
    get(L"targets", targets, sizeof(targets));

    std::vector<NavTarget> list;
    char* ctx = nullptr;
    for (char* tok = strtok_s(targets, ";", &ctx); tok;
         tok = strtok_s(nullptr, ";", &ctx)) {
        char* at = strchr(tok, '@');
        if (!at) continue;
        *at = 0;
        NavTarget t{};
        strncpy_s(t.label, tok, _TRUNCATE);
        if (sscanf_s(at + 1, "%lf,%lf,%lf", &t.x, &t.y, &t.z) == 3)
            list.push_back(t);
    }
    if (key[0] && !list.empty()) {
        Lock lk;
        g_key = key;
        g_name = name[0] ? name : key;
        g_targets = std::move(list);
    }
}

void nav_tick() {
    if (!InterlockedExchange(&g_dirty, 0)) return;
    std::string key, name, ser;
    {
        Lock lk;
        key = g_key;
        name = g_name;
        char one[160];
        for (const auto& t : g_targets) {
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s@%.1f,%.1f,%.1f",
                        ser.empty() ? "" : ";", t.label, t.x, t.y, t.z);
            ser += one;
        }
    }
    auto put = [&](const wchar_t* k, const std::string& v) {
        wchar_t wide[1024] = {0};
        if (!v.empty() &&
            !MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, wide, 1024))
            wide[0] = 0;   // too long or invalid: store empty, not garbage
        WritePrivateProfileStringW(L"navigator", k, wide, g_ini_path.c_str());
    };
    put(L"key", key);
    put(L"name", name);
    put(L"targets", ser);

    if (InterlockedExchange(&g_layout_dirty, 0)) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_nav_x, 0, 0));
        WritePrivateProfileStringW(L"navigator", L"x", buf, g_ini_path.c_str());
        swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_nav_y, 0, 0));
        WritePrivateProfileStringW(L"navigator", L"y", buf, g_ini_path.c_str());
    }
}

void nav_set_hero_pose(bool valid, double x, double y, double z, double rot_z) {
    if (!g_cs_init) return;
    Lock lk;
    g_pos_valid = valid;
    if (valid) {
        g_hx = x;
        g_hy = y;
        g_hz = z;
        g_rz = rot_z;
        g_pos_tick = GetTickCount();
    }
}

void nav_set_camera(bool valid, double px, double py, double pz,
                    double tx, double ty, double tz) {
    if (!g_cs_init) return;
    Lock lk;
    g_cam_valid = false;
    if (!valid) return;

    // The view vector, flattened. A near-vertical view has no meaningful
    // horizontal bearing, so require some length before trusting it.
    const double vx = tx - px, vy = ty - py;
    if (sqrt(vx * vx + vy * vy) < 0.05) return;

    g_view_dx = vx;
    g_view_dy = vy;
    (void)pz;
    (void)tz;
    // Diagnostics: how far the camera sits from the hero. Not a gate - the
    // view vector stands on its own - but a wrong object shows up here.
    g_cam_dist = g_pos_valid
        ? sqrt((px - g_hx) * (px - g_hx) + (py - g_hy) * (py - g_hy))
        : 0.0;
    g_cam_valid = true;
}

bool nav_track(const char* key, const char* name, const NavTarget* targets,
               int count) {
    if (!g_cs_init || !key || count <= 0) return false;
    Lock lk;
    if (g_key == key) {
        g_key.clear();
        g_name.clear();
        g_targets.clear();
        InterlockedExchange(&g_dirty, 1);
        return false;
    }
    g_key = key;
    g_name = name ? name : key;
    g_targets.assign(targets, targets + count);
    InterlockedExchange(&g_dirty, 1);
    return true;
}

void nav_untrack() {
    if (!g_cs_init) return;
    Lock lk;
    g_key.clear();
    g_name.clear();
    g_targets.clear();
    InterlockedExchange(&g_dirty, 1);
}

bool nav_is_tracked(const char* key) {
    if (!g_cs_init || !key) return false;
    Lock lk;
    return g_key == key;
}

bool nav_format_distance(const NavTarget* targets, int count, char* out,
                         int out_len) {
    if (!g_cs_init || count <= 0 || out_len <= 0) return false;
    Lock lk;
    if (!pos_fresh_locked()) return false;
    const NavTarget* t = nearest_locked(targets, count);
    if (!t) return false;
    format_to_locked(*t, out, out_len);
    return true;
}

// A chunky dart, drawn as two facets split down its centre line so the
// lighter left and darker right catch the eye as a crease - the same trick
// that makes TomTom's arrow read as three-dimensional without a mesh, a
// texture or a light. `a` rotates it clockwise; 0 points straight up.
static void draw_arrow_3d(float cx, float cy, float r, double a) {
    const float s = (float)sin(a), c = (float)cos(a);
    // Screen y grows downward, so this matrix turns clockwise on screen.
    auto rot = [&](float x, float y, float* ox, float* oy) {
        *ox = cx + (x * c - y * s) * r;
        *oy = cy + (x * s + y * c) * r;
    };

    // Local outline, tip at the top, with a notched tail.
    const float tip_x = 0.00f, tip_y = -1.00f;
    const float lw_x = -0.78f, lw_y = 0.62f;
    const float rw_x = 0.78f, rw_y = 0.62f;
    const float nt_x = 0.00f, nt_y = 0.22f;    // tail notch
    const float md_x = 0.00f, md_y = -0.10f;   // crease waist

    struct P { float x, y; };
    auto build = [&](float scale, float ox, float oy, P out[5]) {
        const float pts[5][2] = {{tip_x, tip_y}, {lw_x, lw_y}, {rw_x, rw_y},
                                 {nt_x, nt_y}, {md_x, md_y}};
        for (int i = 0; i < 5; i++) {
            float x, y;
            rot(pts[i][0] * scale, pts[i][1] * scale, &x, &y);
            out[i] = {x + ox, y + oy};
        }
    };

    P o[5], p[5];
    build(1.16f, 0, 1.5f, o);      // shadow: slightly larger, nudged down
    build(1.00f, 0, 0, p);

    const Color shadow{0.02f, 0.03f, 0.05f, 0.55f};
    draw_triangle(o[0].x, o[0].y, o[1].x, o[1].y, o[3].x, o[3].y, shadow);
    draw_triangle(o[0].x, o[0].y, o[3].x, o[3].y, o[2].x, o[2].y, shadow);

    // Left facet catches the light, right facet falls away.
    const Color lit{1.00f, 0.86f, 0.46f, 1.0f};
    const Color dim{0.78f, 0.55f, 0.14f, 1.0f};
    draw_triangle(p[0].x, p[0].y, p[1].x, p[1].y, p[4].x, p[4].y, lit);
    draw_triangle(p[1].x, p[1].y, p[3].x, p[3].y, p[4].x, p[4].y, lit);
    draw_triangle(p[0].x, p[0].y, p[4].x, p[4].y, p[2].x, p[2].y, dim);
    draw_triangle(p[2].x, p[2].y, p[4].x, p[4].y, p[3].x, p[3].y, dim);
}

// Nothing tracked, or nothing drawn: the frame must stop claiming screen
// space, or it goes on swallowing clicks in an area showing nothing.
void nav_clear_frame() {
    g_last_rect[2] = 0;
    g_last_rect[3] = 0;
    g_dragging = false;
    input_set_aux_rect(0, 0, 0, 0);
}

void nav_draw(float screen_w, float screen_h) {
    if (!g_cs_init) return;

    // Sample input before any early exit, so the click counter stays in step
    // even on frames that draw nothing - otherwise the click that starts a
    // track would be seen as new here on the next frame and grab a drag.
    InputState in;
    input_peek(&in);
    const bool clicked = in.clicks != g_seen_clicks;
    g_seen_clicks = in.clicks;

    char name[96], where[64], label[96];
    bool have = false, fresh = false, used_camera = false;
    double rel = 0;                   // radians clockwise from "dead ahead"
    double diag_target_b = 0, diag_cam_d = 0, diag_rz = 0, diag_cam_dist = 0;
    {
        Lock lk;
        if (!g_key.empty() && !g_targets.empty()) {
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%s", g_name.c_str());
            fresh = pos_fresh_locked();
            const NavTarget* t = fresh
                ? nearest_locked(g_targets.data(), (int)g_targets.size())
                : &g_targets[0];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "%s", t->label);
            if (fresh) {
                format_to_locked(*t, where, sizeof(where));
                // One convention for both paths: how far clockwise the target
                // sits from whatever is currently "forward". The arrow rotates
                // clockwise for positive, so right reads right.
                const double target_b = bearing(t->x - g_hx, t->y - g_hy);
                if (g_cam_valid) {
                    // The screen faces along the camera's own view vector.
                    rel = target_b - bearing(g_view_dx, g_view_dy);
                    used_camera = true;
                } else {
                    // Fallback: the hero's own facing, whose vector is
                    // (cos rotationZ, sin rotationZ) in world axes. That is the
                    // same relation farever-minimap's own example nav_arrow.lua
                    // uses, and it reduces to the arrow behaviour already
                    // confirmed in game.
                    rel = target_b - bearing(cos(g_rz), sin(g_rz));
        }
        } else {
            _snprintf_s(where, sizeof(where), _TRUNCATE, "...");
        }
        diag_target_b = fresh ? bearing(t->x - g_hx, t->y - g_hy) : 0;
        diag_cam_d = g_cam_valid ? bearing(g_view_dx, g_view_dy) : 0;
        diag_rz = g_rz;
        diag_cam_dist = g_cam_dist;
        have = true;
        }
    }
    // No tracked target, or no live hero to measure from (main menu, logout,
    // loading): draw nothing at all rather than a frame frozen on the last
    // position it knew.
    if (!have || !fresh) {
        nav_clear_frame();
        return;
    }

    // One line whenever the source changes, outside the lock. If the arrow
    // ever points wrongly, this says which path drew it and with what
    // numbers - no guessing at a second attempt.
    static int last_source = -1;
    const int source = used_camera ? 1 : 0;
    if (fresh && source != last_source) {
        last_source = source;
        host_log("nav: arrow from %s (targetBearing=%.1fdeg viewBearing=%.1fdeg "
                 "heroRotZ=%.1fdeg camToHero=%.1f rel=%.1fdeg)",
                 used_camera ? "camera view vector" : "hero facing",
                 diag_target_b * 57.2957795, diag_cam_d * 57.2957795,
                 diag_rz * 57.2957795, diag_cam_dist, rel * 57.2957795);
    }

    // --- layout: arrow above, distance under it, then what and where ------
    const float kArrowR = 34;
    const float kDistSz = 22, kNameSz = 14, kLabelSz = 12;
    const float pad = 10;

    const float dist_w = measure_text(kDistSz, where);
    const float name_w = measure_text(kNameSz, name);
    const float label_w = measure_text(kLabelSz, label);
    float w = dist_w;
    if (name_w > w) w = name_w;
    if (label_w > w) w = label_w;
    if (kArrowR * 2.4f > w) w = kArrowR * 2.4f;
    w += 2 * pad;
    const float h = pad + kArrowR * 2.1f + 6 + kDistSz + 4 + kNameSz + 3 +
                    kLabelSz + pad;

    // Placement: persisted, defaulting to just under the top edge, centred -
    // where a waypoint arrow is expected before anyone moves it.
    const LONG saved_x = InterlockedCompareExchange(&g_nav_x, 0, 0);
    const LONG saved_y = InterlockedCompareExchange(&g_nav_y, 0, 0);
    float x = (saved_x == kUnplaced) ? (screen_w - w) * 0.5f : (float)saved_x;
    float y = (saved_y == kUnplaced) ? 64.0f : (float)saved_y;

    // Dragging is only possible while the atlas window is open. That keeps
    // the frame from ever swallowing a click during normal play, and the
    // visible border doubles as the cue that it can be moved right now.
    const bool movable = in.visible;

    if (movable) {
        // Yield anything the atlas window is covering: it draws on top, so
        // it must receive the click too.
        const bool hit = clicked && in.click_x >= x && in.click_x < x + w &&
                         in.click_y >= y && in.click_y < y + h &&
                         !input_in_main_rect(in.click_x, in.click_y);
        if (hit) {
            g_dragging = true;
            g_drag_dx = in.click_x - x;
            g_drag_dy = in.click_y - y;
        }
        if (g_dragging) {
            if (in.lbutton) {
                x = in.mouse_x - g_drag_dx;
                y = in.mouse_y - g_drag_dy;
            } else {
                g_dragging = false;
                InterlockedExchange(&g_layout_dirty, 1);
                InterlockedExchange(&g_dirty, 1);
            }
        }
    } else {
        g_dragging = false;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > screen_w - w) x = screen_w - w;
    if (y > screen_h - h) y = screen_h - h;
    InterlockedExchange(&g_nav_x, (LONG)x);
    InterlockedExchange(&g_nav_y, (LONG)y);

    g_last_rect[0] = x;
    g_last_rect[1] = y;
    g_last_rect[2] = w;
    g_last_rect[3] = h;
    input_set_aux_rect(movable ? (int)x : 0, movable ? (int)y : 0,
                       movable ? (int)w : 0, movable ? (int)h : 0);

    // Frameless while playing, like TomTom - only the arrow and its text
    // sit over the world. The panel appears when it can be dragged.
    if (movable) {
        draw_rect(x, y, w, h, {0.05f, 0.06f, 0.09f, 0.80f});
        draw_rect_outline(x, y, w, h, 1.0f, {0.35f, 0.75f, 1.0f, 0.8f});
    }

    const float cx = x + w * 0.5f;
    float yy = y + pad;

    draw_arrow_3d(cx, yy + kArrowR, kArrowR, rel);
    yy += kArrowR * 2.1f + 6;

    draw_text(cx - dist_w * 0.5f, yy, kDistSz, {1.0f, 1.0f, 1.0f, 1.0f}, where);
    yy += kDistSz + 4;
    draw_text(cx - name_w * 0.5f, yy, kNameSz, {0.86f, 0.89f, 0.95f, 1.0f}, name);
    yy += kNameSz + 3;
    draw_text(cx - label_w * 0.5f, yy, kLabelSz, {0.55f, 0.60f, 0.70f, 1.0f},
              label);
}

}  // namespace fmk
