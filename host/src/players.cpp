// ---------------------------------------------------------------------------
// players.cpp
//
// Threading mirrors routes.cpp: one critical section over the snapshot, and a
// generation counter so the render thread only copies it when it moved. The
// difference is where the read comes from - routes are files, and this is the
// live game - so the poll is throttled rather than triggered by an edit.
//
// Nothing in here writes, calls into the game, or invites anybody. Both
// actions a row offers end at the clipboard: the name on its own, or the
// game's own `!to <name> ` whisper command for pasting into its chat box.
//
// Clicking a row aims the host's own navigator pill at that player and keeps
// it aimed at them as they move. That is still only reading: the pill is an
// overlay readout, not a marker placed in the game's map.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "paths.h"
#include "hl_reader.h"
#include "navigator.h"
#include "overlay.h"
#include "players.h"

namespace fmk {

namespace {

CRITICAL_SECTION g_cs;

// players_init runs on the worker before either of the other two threads can
// reach this module, so a plain flag would do. It is a state anyway, because
// the cost is one interlocked compare and the failure it rules out - the
// render thread entering a critical section that has not been created - is
// silent, intermittent and would only ever show up on somebody else's
// machine.
//   0 = untouched, 1 = being built, 2 = usable
volatile LONG g_state = 0;

bool ready() { return InterlockedCompareExchange(&g_state, 0, 0) == 2; }

Color class_dot_color(const std::string& cls) {
    if (cls == "Warrior") return {0.86f, 0.35f, 0.30f, 1.0f};
    if (cls == "Rogue") return {0.34f, 0.39f, 0.51f, 1.0f};
    if (cls == "Mage") return {0.26f, 0.68f, 0.76f, 1.0f};
    if (cls == "Priest") return {0.82f, 0.68f, 0.62f, 1.0f};
    return {0.55f, 0.58f, 0.65f, 1.0f};
}

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

// One row as the page wants it. `ranked` is not "has a distance to show" - it
// is "this row can be ordered against the others", which additionally needs
// our own position. Kept apart from `has_pos` so the two unknowns stay two
// unknowns: a player with no replicated character, and a moment when the
// local hero cannot be read.
struct Row {
    std::string name;
    int64_t uid = 0;
    bool me = false;
    bool party = false;
    bool has_pos = false;
    bool ranked = false;
    double x = 0, y = 0, z = 0;
    double dist = 0;
    // The class, verbatim from the hero's unit id. Empty is a third unknown,
    // separate from the two above: a player whose hero has not been
    // replicated has no class to read, and the column stays blank rather than
    // inventing a default.
    std::string cls;
    int level = 0;
};

struct View {
    bool valid = false;      // the last read succeeded
    bool self_pos = false;   // and our own position was known when it did
    int with_pos = 0;        // how many rows had a replicated character
    bool leader = false;
    std::vector<std::string> group;
    std::vector<Row> rows;
};

View g_shared;                  // under g_cs
volatile LONG g_gen = 0;

View g_view;                    // render thread only
LONG g_view_gen = -1;

// Twice a second. The roster changes when somebody zones in or out, which is
// not something worth chasing at frame rate, and the ordering settling slower
// than the distances is deliberate - see the draw.
const DWORD kPollMs = 500;
bool g_polled = false;
DWORD g_last_poll = 0;

// --- following one player ----------------------------------------------------
//
// Clicking a row points the navigator at that player and keeps it pointed at
// them: the poll re-publishes their position every tick, so the pill's arrow
// tracks them as they move. The click lands on the render thread and the
// re-publish happens on the worker, so who is being followed is shared state
// the same way the roster is.
//
// Identity is the uid, never the row index. The list re-sorts under the
// pointer twice a second, and following an index would quietly turn into
// following whoever slid into it.

// Prefixed so nav_status can be asked whether what the navigator has restored
// from disk is one of ours - see players_init.
const char kFollowPrefix[] = "players/";

struct Follow {
    bool on = false;
    int64_t uid = 0;
    std::string name;
    // Why following stopped, written by the poll and drawn by the page. The
    // poll cannot toast: g_toast belongs to the render thread.
    std::string note;
};

Follow g_follow;                // under g_cs

std::string follow_key(int64_t uid) {
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s%lld", kFollowPrefix,
                (long long)uid);
    return buf;
}

// Point the pill at one live position under `key`.
//
// None of the navigator's entry points is a plain "replace the position under
// this key", so this is the least bad of them and the reason is worth having
// written down. nav_track's contract is a toggle - a second call with the
// same key switches tracking off - so re-publishing with it alone would blink
// the pill off every other tick. nav_start_route does replace the list under
// the same key without toggling, but a route crosses waypoints off as they
// are reached: walking up to the person you are following would finish it,
// and standing beside them would restart and re-finish it twice a second.
// nav_add and nav_queue append, so a minute of following would build a
// 120-waypoint route.
//
// So: drop the key, then set it again. nav_untrack is unconditional, which is
// why nothing calls this without having established that what is tracked is
// ours - if the user has since pointed the navigator at something else, that
// is theirs.
//
// One consequence worth knowing: each re-publish marks the navigator dirty,
// so nav_tick rewrites farever-nav-state.txt about once a second for as long
// as a follow is running. It is a few hundred bytes, and it is why
// players_init drops a follow target restored from that file.
void follow_publish(const std::string& key, const std::string& name,
                    double x, double y, double z) {
    if (nav_is_tracked(key.c_str())) nav_untrack();
    const char* label = name.empty() ? "player" : name.c_str();
    NavTarget t{};
    _snprintf_s(t.label, sizeof(t.label), _TRUNCATE, "%s", label);
    t.x = x;
    t.y = y;
    t.z = z;
    nav_track(key.c_str(), label, &t, 1);
}

// --- the sort, and where it is kept -----------------------------------------
//
// Which column the list is ordered by. The header is clicked on the render
// thread and the ini is written on the worker, so these are interlocked the
// way chat.cpp's settings are rather than owned by either side.

enum SortKey : LONG { kSortName = 0, kSortDist = 1, kSortClass = 2 };

volatile LONG g_sort = kSortDist;   // nearest first, as the page always was
volatile LONG g_sort_rev = 0;
volatile LONG g_sort_dirty = 0;
std::wstring g_ini_path;            // worker thread only

// A row is ordered against the others only when it HAS a value in the active
// column. `-` is not the largest distance and a blank class is not the last
// class in the alphabet: both are absences, so they sit at the bottom in
// either direction. Reversing reverses the rows that have a value; promoting
// the ones that have none would present an absence as a ranking.
bool has_key(const Row& r, LONG key) {
    switch (key) {
        case kSortDist:  return r.ranked;
        case kSortClass: return !r.cls.empty();
        default:         return !r.name.empty();
    }
}

// stable_sort throughout, and the input is the order the poll published -
// nearest first - so rows that tie on the active column keep a settled order
// instead of shuffling every time the list is re-sorted.
void apply_sort(std::vector<Row>& rows, LONG key, bool rev) {
    std::stable_sort(rows.begin(), rows.end(),
                     [key, rev](const Row& a, const Row& b) {
                         const bool ka = has_key(a, key);
                         const bool kb = has_key(b, key);
                         if (ka != kb) return ka;
                         if (!ka) return false;
                         int c = 0;
                         if (key == kSortDist)
                             c = a.dist < b.dist ? -1
                                                 : (a.dist > b.dist ? 1 : 0);
                         else if (key == kSortClass)
                             c = _stricmp(a.cls.c_str(), b.cls.c_str());
                         else
                             c = _stricmp(a.name.c_str(), b.name.c_str());
                         if (c == 0) return false;
                         return rev ? c > 0 : c < 0;
                     });
}

// Read once on the worker at startup, from farever-modkit.ini beside the game
// - the same file and the same mechanism chat.cpp uses, because a second
// settings file for one integer would be a second thing to explain.
void load_settings() {
    g_ini_path = data_dir() + L"farever-modkit.ini";

    const wchar_t* ini = g_ini_path.c_str();
    // Range-checked because this file is meant to be hand-editable: a key
    // outside the three columns would leave the header marking nothing while
    // the list stayed in whatever order the poll published.
    const LONG key = GetPrivateProfileIntW(L"players", L"sort", kSortDist, ini);
    g_sort = (key >= kSortName && key <= kSortClass) ? key : kSortDist;
    g_sort_rev = GetPrivateProfileIntW(L"players", L"reverse", 0, ini) ? 1 : 0;
}

void write_settings() {
    auto put = [](const wchar_t* key, LONG v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", (int)v);
        WritePrivateProfileStringW(L"players", key, buf, g_ini_path.c_str());
    };
    put(L"sort", InterlockedCompareExchange(&g_sort, 0, 0));
    put(L"reverse", InterlockedCompareExchange(&g_sort_rev, 0, 0));
}

// --- page state (render thread only) ----------------------------------------

float g_scroll = 0;
std::string g_toast;
DWORD g_toast_tick = 0;

// Which sort the copy in g_view is currently in. The re-sort happens here
// rather than in the poll so a header click lands on the frame it is made,
// and it costs nothing at frame rate because it only runs when one of these
// three things has moved.
LONG g_view_sort = -1;
LONG g_view_rev = -1;

const Color kName{0.90f, 0.93f, 0.98f, 1.0f};
const Color kDim{0.48f, 0.53f, 0.63f, 1.0f};
const Color kFaint{0.40f, 0.45f, 0.55f, 1.0f};
const Color kAccent{0.35f, 0.75f, 1.0f, 1.0f};
const Color kParty{0.45f, 0.80f, 0.55f, 1.0f};
// Deliberately neither of the two above: being followed is not being you and
// not being in your party, and three markers that share a colour are one
// marker.
const Color kFollow{0.95f, 0.74f, 0.35f, 1.0f};

void toast(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_toast = buf;
    g_toast_tick = GetTickCount();
}

// The clipboard belongs to whoever is using the machine, so this mirrors the
// careful version in chat.cpp rather than the older one: a real owner window
// (OpenClipboard(nullptr) makes EmptyClipboard set the owner to NULL, which is
// documented to make the SetClipboardData that follows fail - so it would
// empty somebody's clipboard and then reliably put nothing back), everything
// built and checked before the clipboard is touched at all, and GlobalFree on
// every path that does not hand the block over.
bool clipboard_put(const std::string& text) {
    HWND owner = (HWND)overlay_game_hwnd();
    if (!owner) return false;      // before the first frame there is no window

    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!h) return false;
    wchar_t* w = (wchar_t*)GlobalLock(h);
    if (!w) {
        // Nothing was written into the block. Handing it over anyway would
        // paste whatever the allocator happened to leave there.
        GlobalFree(h);
        return false;
    }
    const int wrote = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w, n);
    GlobalUnlock(h);
    if (wrote != n) {
        GlobalFree(h);
        return false;
    }

    // Another process holds the clipboard for a moment every time it copies
    // something, and losing that race silently is worse than waiting 50ms.
    bool open = false;
    for (int i = 0; i < 5 && !open; i++) {
        open = OpenClipboard(owner) != 0;
        if (!open) Sleep(10);
    }
    if (!open) {
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

// A flat button, the same one the Routes page uses. Returns true on the frame
// it is clicked.
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

// One clickable column header. Returns true on the frame it is clicked.
//
// The hit box is handed in whole rather than measured off the label, so the
// target does not move when the arrow appears and the column becomes a pixel
// or two wider. The active column gets a rule under it as well as the arrow,
// because at 11px the arrow is a few pixels of text and marking the sort with
// only that is marking it with nothing.
bool header(const InputState& in, bool clicked, float bx, float by, float bw,
            float bh, const char* label, bool active, bool rev,
            bool right_align) {
    char text[48];
    // ASCII: the overlay's font atlas has no glyph for an arrow and would
    // draw a blank where the whole marking is meant to be.
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s%s", label,
                active ? (rev ? " v" : " ^") : "");
    const bool hot = in.mouse_x >= bx && in.mouse_x < bx + bw &&
                     in.mouse_y >= by && in.mouse_y < by + bh;
    const float tw = measure_text(11, text);
    const float tx = right_align ? bx + bw - tw : bx;
    draw_text(tx, by + 3, 11, active ? kName : (hot ? kDim : kFaint), text);
    if (active) draw_rect(tx, by + bh - 3, tw, 1, kAccent);
    return clicked && in.click_x >= bx && in.click_x < bx + bw &&
           in.click_y >= by && in.click_y < by + bh;
}

// The notes are the point of this page, and the atlas window is resizable, so
// they are wrapped to whatever width we are handed. The overlay has no way to
// clip text: a line too long for the band would simply be painted past the
// window's edge.
std::vector<std::string> wrap(const char* text, float size, float w) {
    std::vector<std::string> out;
    std::string line, word;
    for (const char* p = text;; p++) {
        if (*p && *p != ' ') {
            word += *p;
            continue;
        }
        if (!word.empty()) {
            const std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && measure_text(size, cand.c_str()) > w) {
                out.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            word.clear();
        }
        if (!*p) break;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

// --- the on-screen wording ---------------------------------------------------
//
// These strings are the feature. What the roster is, what it is not, and what
// a missing distance means - written out where somebody reading the list will
// see them, rather than left in a comment nobody opens.

const char kNoteWhat[] =
    "Everyone the server has replicated to this client, listed in full. It is "
    "not proof of who else is on the shard - the server decides what to send "
    "you, and this can only show what arrived. The game's own Manage Party "
    "window shows just the players within 100 units of you; that limit is "
    "presentation, and the rest of the list is already here in memory.";

const char kNoteDash[] =
    "A distance of \"-\" means that player's character has not been "
    "replicated to this client, so nothing here says where they are. It does "
    "not mean they are far away. Only your own party is readable, so this "
    "page cannot tell whether anyone else is already in one, and does not "
    "guess.";

const char kNoteClass[] =
    "The class is the hero's own unit id, read off that hero - so a player "
    "whose character has not been replicated here has no class either, and "
    "that column is left blank rather than filled with a guess. An id that is "
    "not one of the four classes is shown exactly as it reads.";

const char kNoteNoPos[] =
    "Your own position cannot be read at the moment, so no distances are "
    "shown, and sorting by distance leaves the list in name order.";

// The second half of this is the useful part, and it is nothing to do with
// the host: processMessage only rewrites a line as "!<dropdown> <text>" when
// it does not already begin with "!" (ChatBox.hx:133-134), so a line that
// starts with a prefix picks its own channel and the dropdown is never
// consulted.
const char kNoteDM[] =
    "DM puts \"!to <name> \" on the clipboard, ready to paste into the game's "
    "own chat box with your message typed after it. The host cannot type it "
    "for you - that would be writing to the game - so the paste is yours and "
    "the send is the game's. The same goes for \"!say\", \"!map\" and "
    "\"!group\": typing one of those at the start of a line picks that "
    "channel from the keyboard, so the channel dropdown never has to be "
    "clicked.";

// Worth saying on the page, because it turns the paste from a chore into a
// one-off. Whispering somebody once puts them in the game's OWN channel
// dropdown for the rest of the session: processMessage searches
// `channelOptions` for the whisper it just built and, not finding it, pushes
// {name, icon, value} and rebuilds the dropdown (ChatBox.hx:159-162). After
// that they are a channel you select, and every line you type goes to them.
//
// Receiving a whisper does NOT do this - ChatBox.receiveMessage
// (ChatBox.hx:126-129) only builds the line and scrolls. So somebody can
// whisper you all evening without ever appearing in your dropdown, and the
// DM button is the quickest way to put them there.
const char kNoteDMChannel[] =
    "Whisper somebody once and the game adds them to its own channel "
    "dropdown, where they stay for the session - pick them there and every "
    "line you type goes to them, no prefix needed. Being whispered does not "
    "add anyone, so this button is also the quickest way to get a reply "
    "channel for somebody who messaged you first.";

}  // namespace

void players_init() {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;
    InitializeCriticalSection(&g_cs);
    load_settings();
    // dllmain calls nav_init first, so by now the navigator has restored
    // whatever it was following when the last session ended - and if that was
    // a player, it is a position that was live at the time and is now an old
    // coordinate with somebody's name on it. A followed player does not
    // survive the process that read them, so drop it rather than come up
    // aiming at where they stood yesterday. Only our own keys are touched.
    NavStatus ns;
    nav_status(&ns);
    if (strncmp(ns.key, kFollowPrefix, sizeof(kFollowPrefix) - 1) == 0)
        nav_untrack();
    InterlockedExchange(&g_state, 2);
}

void players_poll() {
    if (!ready()) return;

    // The settings write lives here, before the throttle, for the reason
    // chat_tick gives: it is a file write and this is the worker thread. An
    // empty path means load_settings could not find the game beside us, and
    // WritePrivateProfileStringW would resolve a bare filename against the
    // Windows directory - a stray file, and a sort that never comes back.
    if (!g_ini_path.empty() && InterlockedExchange(&g_sort_dirty, 0))
        write_settings();

    // Under the lock, even though only the worker reaches this today: the
    // throttle's two variables are beside the snapshot they gate, and a second
    // caller finding them half-updated is exactly the failure this module was
    // rewritten to remove. The draw side never calls this - see players.h.
    {
        Lock lk;
        const DWORD now = GetTickCount();
        if (g_polled && now - g_last_poll < kPollMs) return;
        g_polled = true;
        g_last_poll = now;
    }

    View v;
    RosterState st;
    // reader_read_roster leaves it open to the caller to keep its last value
    // on a failure. This page does not, and the reason is what the failure
    // means here: it is overwhelmingly "no character in the world" - the main
    // menu, character select, a logout - and in that state there IS no roster.
    // Leaving the previous zone's list up, with distances recomputed against
    // wherever you now stand, would be a screen full of numbers that are all
    // wrong. So a failed read publishes an empty view and the page says it
    // could not read one.
    if (reader_read_roster(&st) && st.valid) {
        double hx = 0, hy = 0, hz = 0;
        v.self_pos = nav_hero_pos(&hx, &hy, &hz);
        v.valid = true;
        v.leader = st.i_am_leader;
        v.group = st.group;
        v.rows.reserve(st.players.size());
        for (const RosterPlayer& p : st.players) {
            Row r;
            r.name = p.name;
            r.uid = p.uid;
            r.me = p.me;
            r.party = p.in_my_group;
            r.has_pos = p.has_hero;
            // Carried across as it reads. The reader has already decided that
            // an empty string means "no hero to read it off", and this page
            // draws that as an empty cell.
            r.cls = p.hero_kind;
            r.level = p.level;
            r.x = p.x;
            r.y = p.y;
            r.z = p.z;
            if (r.has_pos) {
                v.with_pos++;
                if (v.self_pos) {
                    // Horizontal, because that is what nav_format_distance
                    // measures and the number on the row comes from there. A
                    // sort key that counted height would put a row above one
                    // showing a larger distance, and the list would look
                    // broken rather than three-dimensional.
                    const double dx = p.x - hx, dy = p.y - hy;
                    r.dist = sqrt(dx * dx + dy * dy);
                    r.ranked = true;
                }
            }
            v.rows.push_back(r);
        }
        // Nearest first, and everything whose distance is unknown after it -
        // never interleaved, because a row with no number in the middle of the
        // list looks like a row whose number failed to draw.
        std::stable_sort(v.rows.begin(), v.rows.end(),
                         [](const Row& a, const Row& b) {
                             if (a.ranked != b.ranked) return a.ranked;
                             if (a.ranked) return a.dist < b.dist;
                             return _stricmp(a.name.c_str(),
                                             b.name.c_str()) < 0;
                         });
    }

    // --- keep the navigator on whoever is being followed ---------------------
    //
    // This is the whole of the live-target feature: the same key, a fresh
    // position, every tick. The target is read out and the outcome written
    // back, with the deciding done in between - the navigator has a lock of
    // its own and holding two at once is how deadlocks are built.
    bool on = false;
    int64_t uid = 0;
    std::string fname;
    {
        Lock lk;
        on = g_follow.on;
        uid = g_follow.uid;
        fname = g_follow.name;
    }
    if (on) {
        const std::string key = follow_key(uid);
        const Row* fr = nullptr;
        for (const Row& row : v.rows) {
            if (row.uid == uid) {
                fr = &row;
                break;
            }
        }
        // Four different things, and not one of them is "they left" or "they
        // are far away". Neither of those is knowable from here: the roster is
        // what the server chose to send this client, so a row going away says
        // something about this client's copy and nothing about the player.
        const bool ours = nav_is_tracked(key.c_str());
        const char* why = nullptr;
        if (!ours)
            why = "the navigator was pointed at something else, or stopped";
        else if (!v.valid)
            why = "the roster cannot be read at all just now";
        else if (!fr)
            why = "they are no longer in the roster this client has been "
                  "sent, so this client cannot see them any more";
        else if (!fr->has_pos)
            why = "their character is no longer replicated to this client, "
                  "so there is no position left to point at";

        if (why) {
            // Not when the navigator is already somebody else's: that would
            // stop what the user just started.
            if (ours) nav_untrack();
            char note[320];
            _snprintf_s(note, sizeof(note), _TRUNCATE,
                        "Stopped following %s: %s.",
                        fname.empty() ? "that player" : fname.c_str(), why);
            Lock lk;
            // Unless the page has started following somebody else in the
            // meantime - that click runs on the other thread.
            if (g_follow.on && g_follow.uid == uid) {
                g_follow.on = false;
                g_follow.uid = 0;
                g_follow.name.clear();
                g_follow.note = note;
            }
        } else {
            // The name is taken from the row rather than from what was
            // clicked, so a name that arrives late ends up on the pill.
            follow_publish(key, fr->name.empty() ? fname : fr->name, fr->x,
                           fr->y, fr->z);
        }
    }

    Lock lk;
    g_shared = std::move(v);
    InterlockedIncrement(&g_gen);
}

int players_count() {
    if (!ready()) return 0;
    Lock lk;
    return (int)g_shared.rows.size();
}

void players_draw(const InputState& in, bool clicked, float x, float y,
                  float w, float h) {
    // Draws the last published view and reads nothing. The roster walk is a
    // few hundred validated reads and a string per player, and dllmain says
    // why that cannot happen here: the draw callback never walks game memory.
    // It runs on the worker's once-a-second tick instead, like every other
    // module's poll.
    if (!ready()) return;

    const LONG gen = InterlockedCompareExchange(&g_gen, 0, 0);
    // A fresh snapshot arrives in the poll's own order, so it always needs the
    // chosen sort putting back on it.
    bool resort = false;
    if (gen != g_view_gen) {
        Lock lk;
        g_view = g_shared;
        g_view_gen = gen;
        resort = true;
    }
    const View& v = g_view;

    // Who is being followed, copied once per frame the way the roster is: the
    // poll can end a follow between two frames, and half of one is no use.
    Follow follow;
    {
        Lock lk;
        follow = g_follow;
    }

    const float pad = 8;
    float cy = y;

    // --- what this list is ---------------------------------------------------
    {
        const float text_w = w - pad * 2;
        std::vector<std::vector<std::string>> blocks;
        blocks.push_back(wrap(kNoteWhat, 11, text_w));
        blocks.push_back(wrap(kNoteDash, 11, text_w));
        blocks.push_back(wrap(kNoteClass, 11, text_w));
        if (v.valid && !v.self_pos)
            blocks.push_back(wrap(kNoteNoPos, 11, text_w));
        blocks.push_back(wrap(kNoteDM, 11, text_w));
        blocks.push_back(wrap(kNoteDMChannel, 11, text_w));

        float body_h = 0;
        for (const auto& b : blocks) body_h += b.size() * 14.0f + 5.0f;
        const float box_h = 6 + 19 + body_h + 2;

        draw_rect(x, cy, w, box_h, {0.07f, 0.09f, 0.13f, 1.0f});
        draw_rect_outline(x, cy, w, box_h, 1.0f, {0.16f, 0.18f, 0.24f, 1.0f});
        draw_text(x + pad, cy + 6, 13, {0.92f, 0.95f, 1.0f, 1.0f},
                  "Players this client can see");
        float ty = cy + 25;
        for (const auto& b : blocks) {
            for (const auto& line : b) {
                draw_text(x + pad, ty, 11, kDim, line.c_str());
                ty += 14;
            }
            ty += 5;
        }
        cy += box_h + 8;
    }

    // --- your own party ------------------------------------------------------
    //
    // Readable only because st.Player.group is replicated to its owner. The
    // same field is null for every other row on the page, which is why this
    // block is about you and says so.
    if (v.valid) {
        std::string party;
        if (v.group.empty()) {
            party = "You are not in a party.";
        } else {
            party = "Your party: ";
            for (size_t i = 0; i < v.group.size(); i++) {
                if (i) party += ", ";
                party += v.group[i];
                // st.Group.get_leader is players[0], so the leader is a
                // position in the list rather than a flag on a member.
                if (i == 0)
                    party += v.leader ? " (leader - that is you)" : " (leader)";
            }
        }
        for (const auto& line : wrap(party.c_str(), 12, w - pad * 2)) {
            draw_text(x, cy, 12, kParty, line.c_str());
            cy += 15;
        }
        cy += 5;
    }

    // --- what the navigator is being pointed at ------------------------------
    //
    // Said here as well as marked on the row, because the roster is long and
    // the followed row is usually scrolled off it.
    {
        std::string line;
        if (follow.on) {
            line = "Following ";
            line += follow.name.empty() ? "that player" : follow.name;
            line += " - the waypoint moves with them. Click the row again to "
                    "stop.";
        } else {
            // Written by the poll when a follow ended on its own. It stays up
            // until the next click rather than timing out like a toast: the
            // reason it went away is the one thing worth not missing.
            line = follow.note;
        }
        for (const auto& s : wrap(line.c_str(), 12, w - pad * 2)) {
            draw_text(x, cy, 12, kFollow, s.c_str());
            cy += 15;
        }
        if (!line.empty()) cy += 3;
    }

    // --- feedback line -------------------------------------------------------
    if (!g_toast.empty()) {
        if (GetTickCount() - g_toast_tick > 8000) {
            g_toast.clear();
        } else {
            // Wrapped: some of these say why something cannot be done, which
            // takes a sentence, and the overlay cannot clip - an unwrapped
            // line would simply be painted past the window's edge.
            for (const auto& s : wrap(g_toast.c_str(), 12, w - pad * 2)) {
                draw_text(x, cy, 12, {0.70f, 0.82f, 0.60f, 1.0f}, s.c_str());
                cy += 15;
            }
            cy += 3;
        }
    }

    // --- the list ------------------------------------------------------------
    if (!v.valid) {
        draw_text(x, cy + 6, 13, kDim,
                  "The roster could not be read. It needs a character in the "
                  "world; farever-modkit.log says what the walk found.");
        return;
    }
    if (v.rows.empty()) {
        draw_text(x, cy + 6, 13, kDim,
                  "The roster came back empty - not even your own player was "
                  "in it.");
        return;
    }

    {
        char head[192];
        _snprintf_s(head, sizeof(head), _TRUNCATE,
                    "%d player%s, %d with a character replicated here  -  "
                    "click a column to sort, again to reverse",
                    (int)v.rows.size(), v.rows.size() == 1 ? "" : "s",
                    v.with_pos);
        draw_text(x, cy, 11, kFaint, head);
        cy += 16;
    }

    // --- columns -------------------------------------------------------------
    //
    // Shared by the headers and the rows, so a header can never end up over a
    // column it does not head. The window is resizable and the overlay cannot
    // clip, so the class column is dropped outright below the width at which
    // it would land on top of the names rather than drawn across them.
    const float cx_dist_end = x + w - 130;   // distances are right-aligned here
    const float cx_level = x + w - 390;
    const float cx_class = x + w - 300;
    const bool show_level = cx_level > x + 150;
    const bool show_class = cx_class > x + 150;

    {
        const float hdr_h = 18;
        LONG key = InterlockedCompareExchange(&g_sort, 0, 0);
        LONG rev = InterlockedCompareExchange(&g_sort_rev, 0, 0);
        // A different column starts in its own natural direction - names A-Z,
        // nearest first - rather than inheriting the last column's, which
        // would silently sort the new one backwards.
        auto pick = [&](LONG k) {
            if (key == k) {
                rev = rev ? 0 : 1;
            } else {
                key = k;
                rev = 0;
            }
            InterlockedExchange(&g_sort, key);
            InterlockedExchange(&g_sort_rev, rev);
            InterlockedExchange(&g_sort_dirty, 1);
            // Everything under the pointer has just moved, so a scroll
            // position measured against the old order means nothing.
            g_scroll = 0;
        };
        if (header(in, clicked, x + 10, cy, 90, hdr_h, "Name",
                   key == kSortName, rev != 0, false))
            pick(kSortName);
        if (show_level)
            draw_text(cx_level, cy + 3, 11, kFaint, "Level");
        if (show_class && header(in, clicked, cx_class, cy, 80, hdr_h, "Class",
                                 key == kSortClass, rev != 0, false))
            pick(kSortClass);
        if (header(in, clicked, cx_dist_end - 70, cy, 70, hdr_h, "Distance",
                   key == kSortDist, rev != 0, true))
            pick(kSortDist);
        cy += hdr_h;
    }

    // After the headers, so a click sorts the list on the frame it is made
    // rather than on the next one. Sorting a snapshot is not walking game
    // memory - every value it looks at was copied out by the poll.
    {
        const LONG key = InterlockedCompareExchange(&g_sort, 0, 0);
        const LONG rev = InterlockedCompareExchange(&g_sort_rev, 0, 0);
        if (resort || key != g_view_sort || rev != g_view_rev) {
            apply_sort(g_view.rows, key, rev != 0);
            g_view_sort = key;
            g_view_rev = rev;
        }
    }

    const float list_y = cy;
    const float list_h = (y + h) - list_y;
    if (list_h < 40) return;

    // Whole rows only, for the reason routes.cpp gives: the overlay cannot
    // clip, so a half-row at either end would paint over the notes above or
    // past the window's bottom edge.
    const float row_h = 42;
    const int per_page = (int)(list_h / row_h);
    const int max_top = (int)v.rows.size() - per_page;
    const float max_scroll = max_top > 0 ? max_top * row_h : 0;
    g_scroll -= in.wheel * row_h * 2;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    const float total_h = v.rows.size() * row_h;

    const int first = (int)(g_scroll / row_h);
    for (int i = first; i < (int)v.rows.size(); i++) {
        const float ry = list_y + (i - first) * row_h;
        if (ry + row_h > list_y + list_h) break;
        const Row& r = v.rows[i];
        const bool hot = in.mouse_x >= x && in.mouse_x < x + w &&
                         in.mouse_y >= ry && in.mouse_y < ry + row_h - 2;

        draw_rect(x, ry, w, row_h - 2,
                  r.me ? Color{0.13f, 0.22f, 0.32f, 1.0f}
                       : (hot ? Color{0.11f, 0.13f, 0.19f, 1.0f}
                              : Color{0.07f, 0.08f, 0.12f, 1.0f}));
        // Two markers, and they are different things: the accent bar is you,
        // the green bar is somebody in your party.
        if (r.me)
            draw_rect(x, ry, 3, row_h - 2, kAccent);
        else if (r.party)
            draw_rect(x, ry, 3, row_h - 2, kParty);

        // The followed row. An outline rather than a fill or a third bar: the
        // fill already carries "you" and "hovered", and the left edge already
        // carries two meanings.
        const bool followed = follow.on && follow.uid == r.uid;
        if (followed)
            draw_rect_outline(x, ry, w, row_h - 2, 2.0f, kFollow);

        const float name_x = x + 16;
        if (!r.cls.empty())
            draw_rect(x + 7, ry + 7, 6, 6, class_dot_color(r.cls));
        draw_text(name_x, ry + 2, 13, kName,
                  r.name.empty() ? "(name not readable)" : r.name.c_str());

        char sub[160];
        const char* tag = r.me ? "you" : (r.party ? "in your party" : "");
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s%suid %lld", tag,
                    tag[0] ? "  -  " : "", (long long)r.uid);
        draw_text(name_x, ry + 23, 11, r.me ? kDim : kFaint, sub);

        if (show_level) {
            char level[24];
            _snprintf_s(level, sizeof(level), _TRUNCATE, "%d", r.level);
            draw_text(cx_level, ry + 13, 12, kDim, r.level > 0 ? level : "-");
        }

        // The class, and nothing at all when there is none. A player whose
        // hero has not been replicated here has no class to read, so the cell
        // is empty rather than carrying a placeholder that would be read as
        // one - and a class the four known ones do not cover is drawn as it
        // reads, because this column reports what is there.
        if (show_class && !r.cls.empty())
            draw_text(cx_class, ry + 7, 12, kDim, r.cls.c_str());

        // Formatted here rather than in the poll so the number tracks the
        // hero at the pose thread's cadence while the ordering only settles
        // twice a second. A list that re-sorted every frame while you ran
        // would move the row out from under the pointer.
        char dist[64];
        bool shown = false;
        if (r.has_pos) {
            NavTarget t{};
            _snprintf_s(t.label, sizeof(t.label), _TRUNCATE, "%s",
                        r.name.c_str());
            t.x = r.x;
            t.y = r.y;
            t.z = r.z;
            shown = nav_format_distance(&t, 1, dist, sizeof(dist));
        }
        // "-" is the only thing said about a player with no replicated
        // character. Not "far", not a stale number, not an estimate.
        if (!shown) _snprintf_s(dist, sizeof(dist), _TRUNCATE, "-");
        const float dw = measure_text(12, dist);
        // Right-aligned clear of both buttons: they end 110 in from the edge,
        // and the gap is what stops a four-digit distance touching DM. The
        // edge itself is the column geometry above, so the header sits over
        // the numbers it heads however the window is sized.
        draw_text(cx_dist_end - dw, ry + 7, 12,
                  shown ? Color{0.58f, 0.64f, 0.74f, 1.0f} : kFaint, dist);

        // Both actions on a row end at the clipboard. Copying text is not a
        // call into the game; inviting somebody would be, so there is no
        // button for it.
        const float bw = 58, bh = 20;
        if (button(in, clicked, x + w - 10 - bw, ry + 4, bw, bh, "Copy",
                   !r.name.empty(), {0.20f, 0.34f, 0.50f, 1.0f})) {
            if (clipboard_put(r.name))
                toast("Copied '%s'.", r.name.c_str());
            else
                toast("Could not write to the clipboard.");
        }

        // `!to <name> <message>` is the game's own whisper command, parsed by
        // ui.hud.ChatBox.processMessage (ChatBox.hx:132-171), which resolves
        // the name through the layer's own player lookup. So this pastes into
        // the game's box and the GAME sends it; the host never does. It cannot
        // type it either - the box and the channel dropdown are both writes,
        // and input synthesis is out - which is why the clipboard is the whole
        // mechanism and the trailing space is part of it: paste, then type.
        //
        // Disabled on your own row. Your own name is in the layer's player
        // list like everybody else's, so nothing would stop the command
        // resolving - it is just not a thing anyone means to paste.
        const float dmw = 36;
        if (button(in, clicked, x + w - 16 - bw - dmw, ry + 4, dmw, bh, "DM",
                   !r.name.empty() && !r.me, {0.20f, 0.34f, 0.50f, 1.0f})) {
            if (clipboard_put("!to " + r.name + " "))
                toast("Copied '!to %s ' - paste it into the game's chat box "
                      "and type your message after it.", r.name.c_str());
            else
                toast("Could not write to the clipboard.");
        }

        // Clicking the row itself follows that player; clicking it again
        // stops. Only the part of the row left of the buttons counts - the
        // strip they sit in owns its own clicks - and the boundary is derived
        // from where the leftmost of them starts so it stays put if another
        // button is ever added beside them.
        const float row_click_w = w - 16 - bw - dmw - 8;
        const char* nm =
            r.name.empty() ? "(name not readable)" : r.name.c_str();
        if (clicked && in.click_x >= x && in.click_x < x + row_click_w &&
            in.click_y >= ry && in.click_y < ry + row_h - 2) {
            if (followed) {
                const std::string key = follow_key(r.uid);
                if (nav_is_tracked(key.c_str())) nav_untrack();
                {
                    Lock lk;
                    g_follow.on = false;
                    g_follow.uid = 0;
                    g_follow.name.clear();
                    g_follow.note.clear();
                }
                toast("Stopped following %s.", nm);
            } else if (r.me) {
                toast("That row is you. The navigator already knows where you "
                      "are.");
            } else if (!r.has_pos) {
                // The one thing this page will not guess at, said out loud
                // rather than left as a click that does nothing.
                toast("Cannot follow %s: their character has not been "
                      "replicated to this client, so there is no position to "
                      "point at. That is not the same as them being far away.",
                      nm);
            } else if (r.uid == 0) {
                // __uid did not read. Following needs a handle that survives
                // the list re-sorting, and 0 is the value a failed read
                // leaves behind rather than an id.
                toast("Cannot follow %s: this row's uid did not read, and "
                      "there is nothing stable to keep hold of them by.", nm);
            } else {
                const std::string key = follow_key(r.uid);
                {
                    Lock lk;
                    g_follow.on = true;
                    g_follow.uid = r.uid;
                    g_follow.name = r.name;
                    g_follow.note.clear();
                }
                // Once now, so the pill appears on this click rather than up
                // to a second later; the poll takes it from here.
                follow_publish(key, r.name, r.x, r.y, r.z);
                toast("Following %s. The waypoint moves with them for as long "
                      "as this client can see them.", nm);
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
