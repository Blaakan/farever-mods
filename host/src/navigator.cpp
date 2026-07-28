// ---------------------------------------------------------------------------
// navigator.cpp
//
// State is tiny and shared across three threads (worker: position + persist,
// window thread: nothing, render thread: track/draw), so one critical
// section guards all of it; every hold is microseconds.
//
// A tracked thing is a list of waypoints plus a NavMode saying what the list
// means. Crossing a waypoint off happens on the pose thread, because that is
// the only place that learns the hero moved - the render thread would notice
// too, but only while the frame is on screen, and a route has to advance
// whether or not you are looking at it.
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

// The ad-hoc list every loose waypoint lands in. One key, so queueing a
// second one extends the first rather than replacing it.
const char* const kQueueKey = "nav/waypoints";

// Tracked thing.
std::string g_key;                    // "" = nothing tracked
std::string g_name;
std::vector<NavTarget> g_targets;
std::vector<char> g_done;             // parallel to g_targets
std::vector<char> g_armed;            // parallel; 0 = cannot be reached yet
NavMode g_mode = kNavNearest;

// When the last outstanding waypoint was crossed off, so the pill can say so
// before it disappears. 0 = not finished.
DWORD g_finished_tick = 0;
constexpr DWORD kFinishedMs = 6000;

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
std::wstring g_state_path;
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

int remaining_locked() {
    int n = 0;
    for (char d : g_done)
        if (!d) n++;
    return n;
}

// Which waypoint the arrow points at, or -1 when the list is exhausted.
//
// kNavOrder follows the list, because someone chose that order. The other two
// take the closest one still outstanding - which is the same computation, so
// alternatives ("three vendors sell this") and a collection route ("every
// chest in the zone") share it. Without a live hero position there is nothing
// to be nearest to, so the first outstanding waypoint stands in; the frame
// hides itself in that state anyway.
int current_locked() {
    if (g_targets.empty()) return -1;
    if (g_mode == kNavOrder) {
        for (size_t i = 0; i < g_targets.size(); i++)
            if (!g_done[i]) return (int)i;
        return -1;
    }
    const bool fresh = pos_fresh_locked();
    int best = -1;
    double best_d = 0;
    for (size_t i = 0; i < g_targets.size(); i++) {
        if (g_done[i]) continue;
        if (!fresh) return (int)i;
        const double dx = g_targets[i].x - g_hx, dy = g_targets[i].y - g_hy;
        const double d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) {
            best = (int)i;
            best_d = d;
        }
    }
    return best;
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

// Crosses off every waypoint the hero is currently standing on. Plural
// because a dense route can have two chests inside one arrival radius, and
// stopping after the first would leave the arrow pointing at a spot the
// player is already on.
//
// The vertical gate is what makes this safe in a world with caves stacked
// under hills: horizontal proximity alone would cross off the cave chest
// while you ride over the top of it.
// Arms every waypoint the hero has got well clear of. Until a waypoint is
// armed it cannot be reached, which is what stops one dropped underfoot from
// being crossed off in the same breath.
void arm_departures_locked() {
    if (!pos_fresh_locked()) return;
    for (size_t i = 0; i < g_targets.size(); i++) {
        if (g_armed[i] || g_done[i]) continue;
        const double dx = g_targets[i].x - g_hx, dy = g_targets[i].y - g_hy;
        if (sqrt(dx * dx + dy * dy) >= kArmDistance) {
            g_armed[i] = 1;
            InterlockedExchange(&g_dirty, 1);
        }
    }
}

void consume_arrivals_locked() {
    if (g_mode == kNavNearest || g_targets.empty()) return;
    for (int guard = 0; guard < 64; guard++) {
        const int i = current_locked();
        if (i < 0) break;
        if (!g_armed[i]) break;
        const NavTarget& t = g_targets[i];
        const double dx = t.x - g_hx, dy = t.y - g_hy, dz = t.z - g_hz;
        if (sqrt(dx * dx + dy * dy) > kArriveRadius) break;
        if (fabs(dz) > kArriveHeight) break;
        g_done[i] = 1;
        InterlockedExchange(&g_dirty, 1);
        const int left = remaining_locked();
        host_log("nav: reached '%s' (%d of %d done)", t.label,
                 (int)g_targets.size() - left, (int)g_targets.size());
        if (left == 0) {
            g_finished_tick = GetTickCount();
            if (!g_finished_tick) g_finished_tick = 1;
            break;
        }
    }
}

// --- persistence ------------------------------------------------------------
//
// The route lives in its own file rather than the INI: a chest route is
// hundreds of waypoints, and GetPrivateProfileString reads into a fixed
// buffer. The INI keeps the frame's position, which is two integers.

bool read_all(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > 8u * 1024 * 1024) {
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

// One waypoint line: `w=<state>,<x>,<y>,<z>,<label>`, where state is
//   0  outstanding
//   1  crossed off
//   2  outstanding, and not yet armed - dropped where the player stood and
//      not left since
// The label goes last and runs to the end of the line, so a name with a comma
// in it - "Zoey, Demon Huntress" - survives the round trip. A file written by
// a build that only knew 0 and 1 still loads: everything reads as armed,
// which is the safe reading for a route someone was already following.
bool parse_waypoint(const char* s, NavTarget* t, bool* done, bool* armed) {
    int d = 0;
    if (sscanf_s(s, "%d,%lf,%lf,%lf", &d, &t->x, &t->y, &t->z) != 4)
        return false;
    const char* p = s;
    for (int field = 0; field < 4; field++) {
        p = strchr(p, ',');
        if (!p) return false;
        p++;
    }
    strncpy_s(t->label, p, _TRUNCATE);
    *done = d == 1;
    *armed = d != 2;
    return true;
}

// The pre-route format, kept so an existing install does not lose what it was
// tracking the first time it runs a build with routes in it.
void load_legacy_ini_locked() {
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
    if (list.empty()) return;
    g_key = key;
    g_name = name[0] ? name : key;
    g_targets = std::move(list);
    g_done.assign(g_targets.size(), 0);
    g_armed.assign(g_targets.size(), 1);
    g_mode = kNavNearest;
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
    g_state_path = std::wstring(path) + L"farever-nav-state.txt";

    g_nav_x = GetPrivateProfileIntW(L"navigator", L"x", kUnplaced,
                                    g_ini_path.c_str());
    g_nav_y = GetPrivateProfileIntW(L"navigator", L"y", kUnplaced,
                                    g_ini_path.c_str());

    std::string text;
    if (!read_all(g_state_path, &text)) {
        Lock lk;
        load_legacy_ini_locked();
        return;
    }

    std::string key, name;
    NavMode mode = kNavNearest;
    std::vector<NavTarget> list;
    std::vector<char> done, armed;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 4, "key=") == 0) key = line.substr(4);
        else if (line.compare(0, 5, "name=") == 0) name = line.substr(5);
        else if (line.compare(0, 5, "mode=") == 0) {
            const int m = atoi(line.c_str() + 5);
            mode = (m == kNavRoute || m == kNavOrder) ? (NavMode)m : kNavNearest;
        } else if (line.compare(0, 2, "w=") == 0) {
            NavTarget t{};
            bool d = false, a = true;
            if (parse_waypoint(line.c_str() + 2, &t, &d, &a)) {
                list.push_back(t);
                done.push_back(d ? 1 : 0);
                armed.push_back(a ? 1 : 0);
            }
        }
    }
    if (key.empty() || list.empty()) return;
    Lock lk;
    g_key = key;
    g_name = name.empty() ? key : name;
    g_targets = std::move(list);
    g_done = std::move(done);
    g_armed = std::move(armed);
    g_mode = mode;
    // A route restored with nothing left would draw "complete" forever;
    // finishing it in the previous session already said so. The ad-hoc queue
    // is exempt: those waypoints exist because someone walked out and dropped
    // them, and are usually on their way to being saved.
    if (remaining_locked() == 0 && g_mode != kNavNearest && g_key != kQueueKey) {
        g_key.clear();
        g_name.clear();
        g_targets.clear();
        g_done.clear();
        g_armed.clear();
    }
}

void nav_tick() {
    if (!InterlockedExchange(&g_dirty, 0)) return;
    std::string out =
        "# farever-modkit navigator state - rewritten as you travel.\n"
        "# Safe to delete; the navigator simply forgets what it was following.\n";
    {
        Lock lk;
        if (!g_key.empty() && !g_targets.empty()) {
            out += "key=" + g_key + "\n";
            out += "name=" + g_name + "\n";
            char head[32];
            _snprintf_s(head, sizeof(head), _TRUNCATE, "mode=%d\n", (int)g_mode);
            out += head;
            char one[224];
            for (size_t i = 0; i < g_targets.size(); i++) {
                const NavTarget& t = g_targets[i];
                const int state = g_done[i] ? 1 : (g_armed[i] ? 0 : 2);
                _snprintf_s(one, sizeof(one), _TRUNCATE,
                            "w=%d,%.1f,%.1f,%.1f,%s\n", state, t.x, t.y, t.z,
                            t.label);
                out += one;
            }
        }
    }
    write_all(g_state_path, out);

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
        // Leaving arms; arriving consumes. In that order, so a waypoint can
        // never be armed and consumed by the same reading.
        arm_departures_locked();
        consume_arrivals_locked();
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
        g_done.clear();
        g_armed.clear();
        g_finished_tick = 0;
        InterlockedExchange(&g_dirty, 1);
        return false;
    }
    g_key = key;
    g_name = name ? name : key;
    g_targets.assign(targets, targets + count);
    g_done.assign(g_targets.size(), 0);
    g_armed.assign(g_targets.size(), 1);
    g_mode = kNavNearest;
    g_finished_tick = 0;
    InterlockedExchange(&g_dirty, 1);
    return true;
}

bool nav_start_route(const char* key, const char* name,
                     const NavTarget* targets, int count, NavMode mode) {
    if (!g_cs_init || !key || count <= 0) return false;
    Lock lk;
    g_key = key;
    g_name = name ? name : key;
    g_targets.assign(targets, targets + count);
    g_done.assign(g_targets.size(), 0);
    // A saved route's waypoints are places, not marks you just made, so
    // standing on the first one means you have done that one.
    g_armed.assign(g_targets.size(), 1);
    g_mode = (mode == kNavOrder) ? kNavOrder : kNavRoute;
    g_finished_tick = 0;
    // Starting a route while already standing on its first waypoint should
    // not leave the arrow pointing at your own feet.
    consume_arrivals_locked();
    InterlockedExchange(&g_dirty, 1);
    host_log("nav: route '%s' started, %d waypoints, mode %d", g_name.c_str(),
             count, (int)g_mode);
    return true;
}

void nav_queue(const char* label, double x, double y, double z) {
    if (!g_cs_init) return;
    Lock lk;
    if (g_key != kQueueKey) {
        g_key = kQueueKey;
        g_name = "Waypoints";
        g_targets.clear();
        g_done.clear();
        g_armed.clear();
        g_mode = kNavRoute;
    }
    // Finishing the queue and then adding to it starts it going again.
    g_finished_tick = 0;
    NavTarget t{};
    _snprintf_s(t.label, sizeof(t.label), _TRUNCATE, "%s",
                (label && label[0]) ? label : "Waypoint");
    t.x = x;
    t.y = y;
    t.z = z;
    g_targets.push_back(t);
    g_done.push_back(0);
    // Unarmed: a waypoint you are standing on, or looking at from the map,
    // must not be crossed off before you have gone anywhere.
    g_armed.push_back(0);
    InterlockedExchange(&g_dirty, 1);
    host_log("nav: queued '%s' at %.1f,%.1f,%.1f (%d waypoints)", t.label, x, y,
             z, (int)g_targets.size());
}

void nav_add(const char* label, const NavTarget* targets, int count,
             bool front) {
    if (!g_cs_init || !targets || count <= 0) return;
    Lock lk;
    // Alternatives, so take one: the nearest when there is a live hero to be
    // near to, the first otherwise.
    const NavTarget* pick = pos_fresh_locked() ? nearest_locked(targets, count)
                                               : &targets[0];
    if (!pick) return;

    // A single tracked atlas item is not a list to add to - it means "show me
    // this one place" - so adding turns it into the ad-hoc list. A route is a
    // list already, and growing it is the point. `g_key` is checked too:
    // clearing a route leaves the mode behind it, and without this the first
    // add after a clear would build a list with no name.
    if (g_key.empty() || g_mode == kNavNearest) {
        g_key = kQueueKey;
        g_name = "Waypoints";
        g_targets.clear();
        g_done.clear();
        g_armed.clear();
        g_mode = kNavRoute;
    }
    g_finished_tick = 0;

    NavTarget t = *pick;
    if (label && label[0]) {
        // The entry's own name leads, because on the pill that is what the
        // reader is looking for; where it came from follows it.
        char merged[96];
        _snprintf_s(merged, sizeof(merged), _TRUNCATE, "%s - %s", label,
                    pick->label);
        _snprintf_s(t.label, sizeof(t.label), _TRUNCATE, "%s",
                    pick->label[0] ? merged : label);
    }

    const size_t at = front ? 0 : g_targets.size();
    g_targets.insert(g_targets.begin() + at, t);
    g_done.insert(g_done.begin() + at, 0);
    // Armed: this is a place out in the world, not a mark made underfoot, so
    // there is no reason it should survive being stood on.
    g_armed.insert(g_armed.begin() + at, 1);
    InterlockedExchange(&g_dirty, 1);
    host_log("nav: added '%s' at %s (%d waypoints, mode %d)", t.label,
             front ? "the front" : "the end", (int)g_targets.size(),
             (int)g_mode);
}

void nav_set_mode(NavMode mode) {
    if (!g_cs_init) return;
    Lock lk;
    // Only ever switches between the two route readings. Turning a tracked
    // atlas item into a route is nav_add's job, and doing it here by accident
    // would start crossing off the vendors you walk past.
    if (g_targets.empty() || g_mode == kNavNearest) return;
    g_mode = (mode == kNavOrder) ? kNavOrder : kNavRoute;
    g_finished_tick = 0;
    InterlockedExchange(&g_dirty, 1);
}

void nav_untrack() {
    if (!g_cs_init) return;
    Lock lk;
    g_key.clear();
    g_name.clear();
    g_targets.clear();
    g_done.clear();
    g_armed.clear();
    g_mode = kNavNearest;
    g_finished_tick = 0;
    InterlockedExchange(&g_dirty, 1);
}

bool nav_is_tracked(const char* key) {
    if (!g_cs_init || !key) return false;
    Lock lk;
    return g_key == key;
}

bool nav_hero_pos(double* x, double* y, double* z) {
    if (!g_cs_init) return false;
    Lock lk;
    if (!pos_fresh_locked()) return false;
    if (x) *x = g_hx;
    if (y) *y = g_hy;
    if (z) *z = g_hz;
    return true;
}

void nav_skip() {
    if (!g_cs_init) return;
    Lock lk;
    if (g_mode == kNavNearest) return;
    const int i = current_locked();
    if (i < 0) return;
    g_done[i] = 1;
    if (remaining_locked() == 0) {
        g_finished_tick = GetTickCount();
        if (!g_finished_tick) g_finished_tick = 1;
    }
    InterlockedExchange(&g_dirty, 1);
}

void nav_restart() {
    if (!g_cs_init) return;
    Lock lk;
    if (g_targets.empty()) return;
    g_done.assign(g_targets.size(), 0);
    // Restarting a list means walking it again from here, so its waypoints
    // are places rather than fresh marks - armed, like a route being started.
    g_armed.assign(g_targets.size(), 1);
    g_finished_tick = 0;
    consume_arrivals_locked();
    InterlockedExchange(&g_dirty, 1);
}

void nav_status(NavStatus* out) {
    if (!out) return;
    *out = NavStatus{};
    if (!g_cs_init) return;
    Lock lk;
    if (g_key.empty() || g_targets.empty()) return;
    out->active = true;
    out->is_route = g_mode != kNavNearest;
    out->mode = g_mode;
    out->total = (int)g_targets.size();
    out->done = out->total - remaining_locked();
    _snprintf_s(out->key, sizeof(out->key), _TRUNCATE, "%s", g_key.c_str());
    _snprintf_s(out->name, sizeof(out->name), _TRUNCATE, "%s", g_name.c_str());
    const int i = current_locked();
    if (i >= 0) {
        _snprintf_s(out->label, sizeof(out->label), _TRUNCATE, "%s",
                    g_targets[i].label);
        if (pos_fresh_locked())
            format_to_locked(g_targets[i], out->where, sizeof(out->where));
    }
}

void nav_active_points(std::vector<NavTarget>* out) {
    if (!out) return;
    out->clear();
    if (!g_cs_init) return;
    Lock lk;
    *out = g_targets;
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

// A tick, for the moment a route runs out of waypoints. Same two-facet
// shading as the arrow so the pill does not change character when it
// changes message.
static void draw_check(float cx, float cy, float r) {
    const Color lit{0.55f, 0.92f, 0.55f, 1.0f};
    const Color dim{0.28f, 0.66f, 0.34f, 1.0f};
    const float t = r * 0.30f;    // stroke half-width
    // Short arm, down-right; long arm, up-right. Two quads as four triangles.
    auto bar = [&](float x0, float y0, float x1, float y1, Color col) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.0001f) return;
        const float nx = -dy / len * t, ny = dx / len * t;
        draw_triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, col);
        draw_triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, col);
    };
    bar(cx - r * 0.62f, cy + r * 0.02f, cx - r * 0.14f, cy + r * 0.52f, dim);
    bar(cx - r * 0.14f, cy + r * 0.52f, cx + r * 0.66f, cy - r * 0.50f, lit);
}

// Nothing tracked, or nothing drawn: the frame must stop claiming screen
// space, or it goes on swallowing clicks in an area showing nothing.
void nav_clear_frame() {
    g_last_rect[2] = 0;
    g_last_rect[3] = 0;
    g_dragging = false;
    input_set_aux_rect(0, 0, 0, 0, 0);
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

    char name[96], where[64], label[96], progress[48] = {0}, key[192] = {0};
    bool have = false, fresh = false, used_camera = false, finished = false;
    int done = 0, total = 0;
    double rel = 0;                   // radians clockwise from "dead ahead"
    double diag_target_b = 0, diag_cam_d = 0, diag_rz = 0, diag_cam_dist = 0;
    {
        Lock lk;
        if (!g_key.empty() && !g_targets.empty()) {
            _snprintf_s(key, sizeof(key), _TRUNCATE, "%s", g_key.c_str());
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%s", g_name.c_str());
            fresh = pos_fresh_locked();
            total = (int)g_targets.size();
            done = total - remaining_locked();
            const int cur = current_locked();
            finished = cur < 0;

            if (finished) {
                // Hold the frame briefly so the last waypoint reads as
                // finished rather than as the mod having crashed.
                _snprintf_s(label, sizeof(label), _TRUNCATE, "%d waypoints",
                            total);
                _snprintf_s(where, sizeof(where), _TRUNCATE, "Route complete");
                have = true;
            } else {
                const NavTarget& t = g_targets[cur];
                _snprintf_s(label, sizeof(label), _TRUNCATE, "%s", t.label);
                if (fresh) {
                    format_to_locked(t, where, sizeof(where));
                    // One convention for both paths: how far clockwise the
                    // target sits from whatever is currently "forward". The
                    // arrow rotates clockwise for positive, so right reads
                    // right.
                    const double target_b = bearing(t.x - g_hx, t.y - g_hy);
                    if (g_cam_valid) {
                        // The screen faces along the camera's own view vector.
                        rel = target_b - bearing(g_view_dx, g_view_dy);
                        used_camera = true;
                    } else {
                        // Fallback: the hero's own facing, whose vector is
                        // (cos rotationZ, sin rotationZ) in world axes. That
                        // is the same relation farever-minimap's own example
                        // nav_arrow.lua uses, and it reduces to the arrow
                        // behaviour already confirmed in game.
                        rel = target_b - bearing(cos(g_rz), sin(g_rz));
                    }
                    diag_target_b = target_b;
                } else {
                    _snprintf_s(where, sizeof(where), _TRUNCATE, "...");
                }
                have = true;
            }
            if (g_mode != kNavNearest)
                _snprintf_s(progress, sizeof(progress), _TRUNCATE,
                            "%d / %d - %d left", done, total, total - done);
            diag_cam_d = g_cam_valid ? bearing(g_view_dx, g_view_dy) : 0;
            diag_rz = g_rz;
            diag_cam_dist = g_cam_dist;
        }
    }

    // A finished route holds the pill for a few seconds and then lets go of
    // the screen, so it returns to how it was without a click. It does not
    // let go of the *route*: those waypoints are still the thing you might
    // want to save or walk again, and quietly deleting them a few seconds
    // after the last one would throw away a recording as it was finished.
    // The Routes page keeps showing it, with Restart and Stop next to it.
    if (finished) {
        const DWORD at = g_finished_tick;
        if (!at || GetTickCount() - at > kFinishedMs) {
            nav_clear_frame();
            return;
        }
    } else if (!have || !fresh) {
        // No tracked target, or no live hero to measure from (main menu,
        // logout, loading): draw nothing at all rather than a frame frozen on
        // the last position it knew.
        nav_clear_frame();
        return;
    }

    // One line whenever the source changes, outside the lock. If the arrow
    // ever points wrongly, this says which path drew it and with what
    // numbers - no guessing at a second attempt.
    static int last_source = -1;
    const int source = used_camera ? 1 : 0;
    if (fresh && !finished && source != last_source) {
        last_source = source;
        host_log("nav: arrow from %s (targetBearing=%.1fdeg viewBearing=%.1fdeg "
                 "heroRotZ=%.1fdeg camToHero=%.1f rel=%.1fdeg)",
                 used_camera ? "camera view vector" : "hero facing",
                 diag_target_b * 57.2957795, diag_cam_d * 57.2957795,
                 diag_rz * 57.2957795, diag_cam_dist, rel * 57.2957795);
    }

    // --- layout: arrow above, distance under it, then what and where ------
    const float kArrowR = 34;
    const float kDistSz = 22, kNameSz = 14, kLabelSz = 12, kProgSz = 12;
    const float kKeysSz = 11;
    const float pad = 10;
    const bool show_progress = progress[0] != 0;
    const float bar_h = show_progress ? 4.0f : 0.0f;

    // The keys that drive the thing being drawn, shown where you are already
    // looking. For the first stretch of a new route, then out of the way -
    // teach it once, and let the frameless pill be frameless after that. The
    // atlas being open brings it back, alongside the border and the drag
    // handle.
    static char seen_key[192] = {0};
    static DWORD key_tick = 0;
    if (strcmp(seen_key, key) != 0) {
        strncpy_s(seen_key, key, _TRUNCATE);
        key_tick = GetTickCount();
        if (!key_tick) key_tick = 1;
    }
    const bool just_started = key_tick && (GetTickCount() - key_tick) < 20000;
    const char* keys = "F10 skip     Shift+F10 clear";
    const bool show_keys = show_progress && (in.visible || just_started);

    const float dist_w = measure_text(kDistSz, where);
    const float name_w = measure_text(kNameSz, name);
    const float label_w = measure_text(kLabelSz, label);
    const float prog_w = show_progress ? measure_text(kProgSz, progress) : 0;
    const float keys_w = show_keys ? measure_text(kKeysSz, keys) : 0;
    float w = dist_w;
    if (name_w > w) w = name_w;
    if (label_w > w) w = label_w;
    if (prog_w > w) w = prog_w;
    if (keys_w > w) w = keys_w;
    if (kArrowR * 2.4f > w) w = kArrowR * 2.4f;
    w += 2 * pad;
    float h = pad + kArrowR * 2.1f + 6 + kDistSz + 4 + kNameSz + 3 + kLabelSz +
              pad;
    if (show_progress) h += 3 + kProgSz + 4 + bar_h;
    if (show_keys) h += 6 + kKeysSz;

    // Placement: persisted, defaulting to just under the top edge, centred -
    // where a waypoint arrow is expected before anyone moves it.
    // The saved position is the ARROW's centre, not the frame's corner.
    // Distance text changes width constantly - "980m NE" to "1.02km NE" -
    // and anchoring the corner made the arrow slide left and right as it
    // did. Anchoring the arrow instead lets the frame grow around it.
    const LONG saved_x = InterlockedCompareExchange(&g_nav_x, 0, 0);
    const LONG saved_y = InterlockedCompareExchange(&g_nav_y, 0, 0);
    float anchor_x = (saved_x == kUnplaced) ? screen_w * 0.5f : (float)saved_x;
    float y = (saved_y == kUnplaced) ? 64.0f : (float)saved_y;
    float x = anchor_x - w * 0.5f;

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
    anchor_x = x + w * 0.5f;
    InterlockedExchange(&g_nav_x, (LONG)anchor_x);
    InterlockedExchange(&g_nav_y, (LONG)y);

    g_last_rect[0] = x;
    g_last_rect[1] = y;
    g_last_rect[2] = w;
    g_last_rect[3] = h;
    input_set_aux_rect(0, movable ? (int)x : 0, movable ? (int)y : 0,
                       movable ? (int)w : 0, movable ? (int)h : 0);

    // Frameless while playing, like TomTom - only the arrow and its text
    // sit over the world. The panel appears when it can be dragged.
    if (movable) {
        draw_rect(x, y, w, h, {0.05f, 0.06f, 0.09f, 0.80f});
        draw_rect_outline(x, y, w, h, 1.0f, {0.35f, 0.75f, 1.0f, 0.8f});
    }

    const float cx = x + w * 0.5f;
    float yy = y + pad;

    if (finished)
        draw_check(cx, yy + kArrowR, kArrowR);
    else
        draw_arrow_3d(cx, yy + kArrowR, kArrowR, rel);
    yy += kArrowR * 2.1f + 6;

    draw_text(cx - dist_w * 0.5f, yy, kDistSz,
              finished ? Color{0.62f, 0.94f, 0.62f, 1.0f}
                       : Color{1.0f, 1.0f, 1.0f, 1.0f},
              where);
    yy += kDistSz + 4;
    draw_text(cx - name_w * 0.5f, yy, kNameSz, {0.86f, 0.89f, 0.95f, 1.0f}, name);
    yy += kNameSz + 3;
    draw_text(cx - label_w * 0.5f, yy, kLabelSz, {0.55f, 0.60f, 0.70f, 1.0f},
              label);

    if (show_progress) {
        yy += kLabelSz + 3;
        draw_text(cx - prog_w * 0.5f, yy, kProgSz, {0.50f, 0.56f, 0.66f, 1.0f},
                  progress);
        yy += kProgSz + 4;
        // The bar carries the whole story at a glance, which the pill needs
        // more than the numbers do while you are running.
        const float bw = w - 2 * pad;
        draw_rect(x + pad, yy, bw, bar_h, {0.14f, 0.16f, 0.22f, 0.9f});
        const float frac = total > 0 ? (float)done / (float)total : 0.0f;
        if (frac > 0)
            draw_rect(x + pad, yy, bw * frac, bar_h, {0.40f, 0.78f, 1.0f, 0.95f});
        yy += bar_h;
    }

    if (show_keys) {
        yy += 6;
        draw_text(cx - keys_w * 0.5f, yy, kKeysSz, {0.45f, 0.52f, 0.62f, 1.0f},
                  keys);
    }
}

}  // namespace fmk
