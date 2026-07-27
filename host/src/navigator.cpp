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

std::wstring g_ini_path;
volatile LONG g_dirty = 0;

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

// +y = north, +x = east (the POI table's axes as the minimap renders them).
// If the compass reads mirrored in game, this one table is the fix.
const char* kCompass[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

void format_to_locked(const NavTarget& t, char* out, int out_len) {
    const double dx = t.x - g_hx, dy = t.y - g_hy;
    const double dist = sqrt(dx * dx + dy * dy);
    double ang = atan2(dx, dy) * 180.0 / 3.14159265358979;   // 0 = N, 90 = E
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

void nav_draw(float screen_w, float screen_h) {
    (void)screen_h;
    if (!g_cs_init) return;

    char name[96], where[64], label[96];
    bool have = false, fresh = false;
    double rel = 0;                   // radians clockwise from "dead ahead"
    {
        Lock lk;
        if (g_key.empty() || g_targets.empty()) return;
        _snprintf_s(name, sizeof(name), _TRUNCATE, "%s", g_name.c_str());
        fresh = pos_fresh_locked();
        const NavTarget* t = fresh
            ? nearest_locked(g_targets.data(), (int)g_targets.size())
            : &g_targets[0];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%s", t->label);
        if (fresh) {
            format_to_locked(*t, where, sizeof(where));
            // Bearings measured like the compass, atan2(east, north); the
            // facing bearing comes from rotationZ. These two lines are the
            // calibration surface, settled empirically: the compass labels
            // verified the world axes, and the first arrow build read
            // mirrored (a target ahead-left rendered ahead-right), so the
            // relative angle is facing-minus-target, not the reverse.
            const double target_b = atan2(t->x - g_hx, t->y - g_hy);
            const double facing_b = atan2(cos(g_rz), sin(g_rz));
            rel = facing_b - target_b;
        } else {
            _snprintf_s(where, sizeof(where), _TRUNCATE, "...");
        }
        have = true;
    }
    if (!have) return;

    char line[220];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s  %s  (%s)", name, where,
                label);
    const float size = 14;
    const float tw = measure_text(size, line);
    const float pad = 10;
    const float arrow_w = fresh ? 24.0f : 0.0f;
    const float w = tw + 2 * pad + arrow_w;
    const float h = 26;
    const float x = (screen_w - w) * 0.5f;
    const float y = 14;

    draw_rect(x, y, w, h, {0.05f, 0.06f, 0.09f, 0.85f});
    draw_rect_outline(x, y, w, h, 1.0f, {0.35f, 0.75f, 1.0f, 0.7f});

    if (fresh) {
        // Arrow rotated by `rel`: 0 = straight up = dead ahead. Screen y
        // grows downward, so "up" is (sin, -cos).
        const float cx = x + pad + 7, cy = y + h * 0.5f;
        const float s = (float)sin(rel), c = (float)cos(rel);
        const float dx = s, dy = -c;          // forward
        const float px = c, py = s;           // right-hand perpendicular
        draw_triangle(cx + dx * 10, cy + dy * 10,
                      cx - dx * 5 + px * 6, cy - dy * 5 + py * 6,
                      cx - dx * 5 - px * 6, cy - dy * 5 - py * 6,
                      {1.0f, 0.78f, 0.30f, 1.0f});
    }
    draw_text(x + pad + arrow_w, y + 4, size, {0.92f, 0.93f, 0.96f, 1.0f},
              line);
}

}  // namespace fmk
