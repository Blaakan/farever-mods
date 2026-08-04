// ---------------------------------------------------------------------------
// loot.cpp
//
// Threading: the loot thread polls and writes events, the render thread reads
// them, the worker persists layout. One critical section over the event ring
// and the baseline; holds are a few dozen string comparisons at worst.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "paths.h"
#include "atlas_ui.h"
#include "hl_reader.h"
#include "input.h"
#include "loot.h"
#include "overlay.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

CRITICAL_SECTION g_cs;
bool g_cs_init = false;

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

// How long a line stays up, and how many can be up at once. Long enough to
// look away and come back, short enough that the feed is about *now*.
constexpr DWORD kLineMs = 25000;
constexpr DWORD kFadeMs = 1200;
constexpr int   kMaxShown = 8;
constexpr int   kRing = 64;

enum EventKind : uint8_t { kItem = 0, kExp, kCurrency, kLevel };

struct Event {
    EventKind kind = kItem;
    std::string text;       // "Copper Ore", "+248 XP", "Level 13"
    std::string sub;        // "Lv 25 - Rare", or empty
    int count = 1;
    int rarity = 0;
    int icon = -1;
    DWORD tick = 0;
};

std::vector<Event> g_events;    // newest last

// The previous reading. Items are counted by identity-with-quality, so a
// second Copper Ore adds to the same bucket while a level 30 sword and a
// level 5 one stay apart.
struct Baseline {
    bool have = false;
    int32_t level = 0;
    int32_t exp = 0;
    std::unordered_map<std::string, int64_t> items;      // key -> total count
    std::unordered_map<std::string, int64_t> currencies;
};
Baseline g_prev;

std::wstring g_ini_path;
constexpr LONG kUnplaced = (LONG)0x80000000;
volatile LONG g_x = kUnplaced, g_y = kUnplaced;
volatile LONG g_layout_dirty = 0;
volatile LONG g_enabled = 1;
volatile LONG g_in_world = 0;

// Render-thread drag state, same contract as the navigator's frame: movable
// only while the atlas window is open, so the feed never eats a click during
// play.
bool  g_dragging = false;
float g_drag_dx = 0, g_drag_dy = 0;
int   g_seen_clicks = 0;

std::string item_key(const Item& it) {
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s|%d|%d|%d", it.kind.c_str(),
                it.level, it.upgrade, it.rarity);
    return buf;
}

void push_locked(Event e) {
    e.tick = GetTickCount();
    if (!e.tick) e.tick = 1;
    // Two of the same item inside a moment are one pickup counted twice by
    // the poll, or a stack arriving in pieces. Either way the reader wants
    // "Copper Ore x4", not four lines. Only items merge: "+248 XP x2" would
    // be worse than two lines, because the number is the point.
    if (e.kind == kItem && !g_events.empty()) {
        Event& last = g_events.back();
        if (last.kind == e.kind && last.text == e.text && last.sub == e.sub &&
            e.tick - last.tick < 3000) {
            last.count += e.count;
            last.tick = e.tick;
            return;
        }
    }
    g_events.push_back(std::move(e));
    if ((int)g_events.size() > kRing)
        g_events.erase(g_events.begin(),
                       g_events.begin() + (g_events.size() - kRing));
}

// An item gain, named and coloured the way the atlas would name and colour
// it. An id the atlas does not know still gets a line - the raw id reads
// worse than "Copper Ore" but far better than silence.
void emit_item_locked(const Item& it, int64_t gained) {
    Event e;
    e.kind = kItem;
    e.count = (int)gained;
    AtlasItemInfo info;
    if (atlas_ui_lookup(it.kind, &info)) {
        e.text = info.name;
        e.rarity = it.rarity >= 0 ? it.rarity : info.rarity;
        e.icon = info.icon;
    } else {
        e.text = it.kind;
        e.rarity = it.rarity >= 0 ? it.rarity : 0;
    }
    if (it.level > 0) {
        char buf[48];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Lv %d%s", it.level,
                    it.upgrade > 0 ? " +" : "");
        e.sub = buf;
        if (it.upgrade > 0) e.sub += std::to_string(it.upgrade);
    }
    push_locked(std::move(e));
}

void save_layout() { InterlockedExchange(&g_layout_dirty, 1); }

}  // namespace

void loot_init() {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    g_ini_path = data_dir() + L"farever-modkit.ini";
    g_x = GetPrivateProfileIntW(L"loot", L"x", kUnplaced, g_ini_path.c_str());
    g_y = GetPrivateProfileIntW(L"loot", L"y", kUnplaced, g_ini_path.c_str());
    g_enabled = GetPrivateProfileIntW(L"loot", L"enabled", 1, g_ini_path.c_str())
                    ? 1 : 0;
}

void loot_tick() {
    if (!g_cs_init) return;
    if (!InterlockedExchange(&g_layout_dirty, 0)) return;
    wchar_t buf[32];
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_x, 0, 0));
    WritePrivateProfileStringW(L"loot", L"x", buf, g_ini_path.c_str());
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_y, 0, 0));
    WritePrivateProfileStringW(L"loot", L"y", buf, g_ini_path.c_str());
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_enabled, 0, 0));
    WritePrivateProfileStringW(L"loot", L"enabled", buf, g_ini_path.c_str());
}

void loot_poll(bool in_world) {
    if (!g_cs_init) return;
    InterlockedExchange(&g_in_world, in_world ? 1 : 0);
    if (!in_world) {
        // Leaving the world invalidates everything: bags are re-read from the
        // server on the way back in, and comparing against a stale baseline
        // would announce your whole inventory as loot. The lines go with it -
        // nothing of ours belongs over the main menu.
        Lock lk;
        g_prev = Baseline{};
        g_events.clear();
        return;
    }
    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) return;

    LootState st;
    if (!reader_read_loot_state(&st) || !st.valid) return;

    std::unordered_map<std::string, int64_t> items;
    for (const auto& it : st.bags) items[item_key(it)] += it.count;
    std::unordered_map<std::string, int64_t> currencies;
    for (const auto& c : st.currencies) currencies[c.kind] += c.count;

    Lock lk;
    if (!g_prev.have) {
        // First reading of the session: this is what you already had, not
        // what you just found.
        g_prev.have = true;
        g_prev.level = st.level;
        g_prev.exp = st.exp;
        g_prev.items = std::move(items);
        g_prev.currencies = std::move(currencies);
        return;
    }

    // A read that comes back empty against a baseline that was not is a
    // transient - a zone handover, a pointer repointed mid-walk - not an
    // emptied bag. Believing it costs nothing now, because losses are silent,
    // and everything on the next poll, when the whole inventory reappears as
    // a gain. Skipping the pass leaves the baseline as it was.
    if (st.bags.empty() && !g_prev.items.empty()) return;

    for (const auto& it : st.bags) {
        const std::string key = item_key(it);
        const int64_t now = items[key];
        const auto f = g_prev.items.find(key);
        const int64_t before = f == g_prev.items.end() ? 0 : f->second;
        if (now <= before) continue;
        // Report the whole gain once, on the first stack of that key seen
        // this pass, then zero the bucket so the second stack of the same
        // item does not report it again.
        emit_item_locked(it, now - before);
        items[key] = before;
    }
    // items[] was used as scratch above; recompute for the new baseline.
    g_prev.items.clear();
    for (const auto& it : st.bags) g_prev.items[item_key(it)] += it.count;

    // Same reasoning as the bags: an empty purse against a non-empty baseline
    // is a failed read, and adopting it would report every coin again later.
    if (currencies.empty() && !g_prev.currencies.empty()) currencies = g_prev.currencies;
    for (const auto& kv : currencies) {
        const auto f = g_prev.currencies.find(kv.first);
        const int64_t before = f == g_prev.currencies.end() ? 0 : f->second;
        if (kv.second <= before) continue;
        Event e;
        e.kind = kCurrency;
        // Currencies are ordinary items in the game's database, so the atlas
        // has their display name and icon like anything else - "Demonic Soul"
        // rather than DemonicSoul.
        AtlasItemInfo info;
        const bool known = atlas_ui_lookup(kv.first, &info);
        if (known) e.icon = info.icon;
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "+%lld %s",
                    (long long)(kv.second - before),
                    known ? info.name.c_str() : kv.first.c_str());
        e.text = buf;
        push_locked(std::move(e));
    }
    g_prev.currencies = std::move(currencies);

    // Levelling resets experience to a small number, so a level-up is the one
    // case where experience going *down* is a gain. Announce the level and
    // say nothing about the number.
    if (st.level > g_prev.level) {
        Event e;
        e.kind = kLevel;
        e.text = "Level " + std::to_string(st.level);
        push_locked(std::move(e));
    } else if (st.level == g_prev.level && st.exp > g_prev.exp) {
        Event e;
        e.kind = kExp;
        e.text = "+" + std::to_string(st.exp - g_prev.exp) + " XP";
        push_locked(std::move(e));
    }
    g_prev.level = st.level;
    g_prev.exp = st.exp;
}

void loot_draw(float screen_w, float screen_h) {
    if (!g_cs_init) return;
    // Same rule the atlas window follows: at the main menu, a loading screen
    // or character select, none of this is on screen.
    if (!InterlockedCompareExchange(&g_in_world, 0, 0)) {
        input_set_aux_rect(1, 0, 0, 0, 0);
        g_dragging = false;
        return;
    }

    InputState in;
    input_peek(&in);
    const bool clicked = in.clicks != g_seen_clicks;
    g_seen_clicks = in.clicks;

    // Snapshot what is still live. The atlas window being open holds every
    // line open with it, so a feed can be read at leisure rather than raced.
    const bool holding = in.visible;
    struct Line { Event e; float alpha; };
    std::vector<Line> lines;
    const DWORD now = GetTickCount();
    {
        Lock lk;
        for (size_t i = g_events.size(); i-- > 0;) {
            const Event& e = g_events[i];
            const DWORD age = now - e.tick;
            float alpha = 1.0f;
            if (!holding) {
                if (age > kLineMs) break;    // older lines are older still
                if (age > kLineMs - kFadeMs)
                    alpha = (float)(kLineMs - age) / (float)kFadeMs;
            }
            lines.push_back({e, alpha});
            if ((int)lines.size() >= kMaxShown) break;
        }
    }
    if (lines.empty() && !holding) {
        input_set_aux_rect(1, 0, 0, 0, 0);
        return;
    }

    // --- layout -------------------------------------------------------------
    const float row_h = 26, pad = 8, icon = 20;
    const float name_sz = 13, sub_sz = 11;
    float w = 200;
    for (const auto& l : lines) {
        // icon, name, the count column on the right, and the level tag when
        // there is one.
        float need = pad + icon + 6 + measure_text(name_sz, l.e.text.c_str()) +
                     46 + pad;
        if (!l.e.sub.empty()) need += measure_text(sub_sz, l.e.sub.c_str()) + 8;
        if (need > w) w = need;
    }
    if (w > 420) w = 420;
    const float rows = (float)(lines.empty() ? 1 : lines.size());
    const float h = pad * 2 + rows * row_h + (holding ? 16 : 0);

    // Default home: right side, above the middle - out of the way of the
    // navigator's pill at top centre and of the game's own hotbar.
    const LONG sx = InterlockedCompareExchange(&g_x, 0, 0);
    const LONG sy = InterlockedCompareExchange(&g_y, 0, 0);
    float x = (sx == kUnplaced) ? screen_w - w - 40 : (float)sx;
    float y = (sy == kUnplaced) ? screen_h * 0.32f : (float)sy;

    const bool movable = holding;
    if (movable) {
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
                save_layout();
            }
        }
    } else {
        g_dragging = false;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > screen_w - w) x = screen_w - w;
    if (y > screen_h - h) y = screen_h - h;
    InterlockedExchange(&g_x, (LONG)x);
    InterlockedExchange(&g_y, (LONG)y);
    input_set_aux_rect(1, movable ? (int)x : 0, movable ? (int)y : 0,
                        movable ? (int)w : 0, movable ? (int)h : 0);

    // Frameless over the world; the panel and its controls appear with the
    // atlas window, which is also when it can be moved.
    if (holding) {
        draw_rect(x, y, w, h, {0.05f, 0.06f, 0.09f, 0.85f});
        draw_rect_outline(x, y, w, h, 1.0f, {0.35f, 0.75f, 1.0f, 0.8f});
    }

    float yy = y + pad;
    for (const auto& l : lines) {
        const Event& e = l.e;
        const float a = l.alpha;

        // A dark plate behind each line, so white text stays readable over
        // snow, sand and a lit spell alike.
        draw_rect(x + 2, yy - 2, w - 4, row_h - 2, {0.03f, 0.04f, 0.06f,
                                                    0.55f * a});
        if (e.kind == kItem) {
            const Color rc = atlas_ui_rarity_color(e.rarity);
            draw_rect(x + 2, yy - 2, 2.5f, row_h - 2, {rc.r, rc.g, rc.b, a});
        }

        float tx = x + pad;
        if (e.icon >= 0) {
            atlas_ui_draw_icon(e.icon, tx, yy, icon, a);
            tx += icon + 6;
        } else {
            // Non-items get a tinted pip so the column still lines up.
            const Color c = e.kind == kLevel ? Color{1.0f, 0.82f, 0.35f, a}
                          : e.kind == kExp   ? Color{0.55f, 0.80f, 1.0f, a}
                                             : Color{0.95f, 0.78f, 0.35f, a};
            draw_rect(tx + 5, yy + 5, 10, 10, c);
            tx += icon + 6;
        }

        Color name_col = e.kind == kItem ? atlas_ui_rarity_color(e.rarity)
                                         : Color{0.94f, 0.96f, 1.0f, 1.0f};
        name_col.a = a;
        draw_text(tx, yy + 2, name_sz, name_col, e.text.c_str());

        if (!e.sub.empty()) {
            const float nw = measure_text(name_sz, e.text.c_str());
            draw_text(tx + nw + 8, yy + 4, sub_sz, {0.55f, 0.60f, 0.70f, a},
                      e.sub.c_str());
        }
        if (e.count > 1) {
            char cnt[24];
            _snprintf_s(cnt, sizeof(cnt), _TRUNCATE, "x%d", e.count);
            const float cw = measure_text(name_sz, cnt);
            draw_text(x + w - pad - cw, yy + 2, name_sz,
                      {0.98f, 0.92f, 0.62f, a}, cnt);
        }
        yy += row_h;
    }

    // With the atlas open the feed gains its one control: turning it off.
    // It lives here rather than on a settings page because here is where you
    // are when you decide you have seen enough of it.
    if (holding) {
        const bool on = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
        const char* label = on ? "Turn feed off" : "Turn feed on";
        const float bw = 92, bh = 18;
        const float bx = x + w - pad - bw, by = y + h - bh - 5;
        const bool hot = in.mouse_x >= bx && in.mouse_x < bx + bw &&
                         in.mouse_y >= by && in.mouse_y < by + bh;
        draw_rect(bx, by, bw, bh, hot ? Color{0.20f, 0.24f, 0.32f, 1.0f}
                                      : Color{0.12f, 0.14f, 0.20f, 1.0f});
        draw_text(bx + 8, by + 2, 11, {0.75f, 0.80f, 0.88f, 1.0f}, label);
        if (clicked && in.click_x >= bx && in.click_x < bx + bw &&
            in.click_y >= by && in.click_y < by + bh) {
            InterlockedExchange(&g_enabled, on ? 0 : 1);
            save_layout();
            Lock lk;
            if (on) g_events.clear();
            // Turning it back on must not diff against a baseline taken
            // before everything that happened while it was off.
            g_prev = Baseline{};
            host_log("loot: feed %s", on ? "off" : "on");
        }
        if (lines.empty()) {
            draw_text(x + pad, y + pad + 2, 12, {0.45f, 0.50f, 0.60f, 1.0f},
                      on ? "Recent loot appears here."
                         : "Feed is off - nothing is being watched.");
        }
    }
}

}  // namespace fmk
