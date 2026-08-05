// ---------------------------------------------------------------------------
// chat.cpp
//
// Threading: the chat thread polls and decodes, the render thread draws, the
// worker persists. One critical section covers the decoded ring, the ignore
// list and the aligned bounds; every hold is an append or a bounded copy.
//
// The render thread keeps its own copy of the ring and tops it up
// incrementally, the way routes.cpp keeps g_view. A session's chat is
// thousands of strings, and copying all of them under the lock every frame
// would be the most expensive thing the draw callback does.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <share.h>   // _SH_DENYNO
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "paths.h"
#include "atlas_ui.h"
#include "chat.h"
#include "hl_reader.h"
#include "input.h"
#include "navigator.h"
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

// The game keeps the whole session and never trims, so the only reason to
// bound this is memory. Three thousand lines is a long evening in a busy
// zone and costs a few hundred kilobytes.
constexpr int kRing = 3000;

// Most decoded in one poll. The first poll after a long load has the whole
// backlog waiting; taking it in slices keeps any single hold short, and the
// next poll is a tenth of a second away.
constexpr int kBatch = 400;

constexpr int kChannels = 6;      // st.Channel's constructor count

// The default is fully opaque, and that is a correctness matter rather than a
// preference. The game draws its own copy of every one of these lines
// underneath this window, and the host cannot hide the game's chat box -
// doing that would mean writing to it. So anything below 100 leaves the
// game's text legible through ours, which reads as a rendering fault rather
// than as a setting. It is offered anyway, for anyone who would rather see
// the world behind the window and can live with the double image.
constexpr LONG kOpaque = 100;

// Free placement, when there is nothing to align to.
constexpr float kFreeW = 560, kFreeH = 250;
constexpr LONG kUnplaced = (LONG)0x80000000;

// Which of input.h's four aux rectangles this frame publishes. They are one
// per module and not interchangeable: 0 is the navigator's pill, 1 the loot
// feed, 2 the atlas HUD panel. Sharing a slot does not fail loudly - the
// module that draws last simply overwrites the other's rectangle, and the
// one that drew first silently stops taking clicks and wheel.
constexpr int kAux = 3;

struct Line {
    uint64_t seq = 0;         // monotonic, and what the scroll anchors to
    ChatChannel channel = kChatUnknown;
    std::string sender;
    std::string text;
    std::string other;        // far end of a whisper; empty means unread
    bool mine = false;
    bool host = false;        // printed by this module, not by the game
    uint8_t hh = 0, mm = 0;   // wall clock when the host first saw it
};

// --- shared state (g_cs) ----------------------------------------------------

std::vector<Line> g_lines;        // oldest first
uint64_t g_seq = 0;
// `!!clear` empties the *view*, not the history: the game's own history is
// the only copy of the session and throwing it away to tidy a window would
// lose the thing worth keeping. Everything at or below this seq is hidden.
uint64_t g_clear_seq = 0;
std::vector<std::string> g_ignore;    // as typed, matched case-insensitively
LONG g_ignore_gen = 0;                // bumped when the list changes

// The `messages` flow of the game's own chat box, in the UI scene's units.
// Kept from the last poll that read it: a transient miss must not teleport
// the window to the free placement and back on the next frame.
struct AlignRect {
    bool ok = false;
    double x = 0, y = 0, w = 0, h = 0;
    int32_t sw = 0, sh = 0;
};
AlignRect g_align;

// --- settings ---------------------------------------------------------------

std::wstring g_ini_path;
std::wstring g_dir;

volatile LONG g_in_world = 0;
volatile LONG g_console = 0;      // the developer console is up
volatile LONG g_enabled = 1;
volatile LONG g_aligned = 1;
volatile LONG g_timestamps = 1;
volatile LONG g_text_size = 13;   // body text size in pixels
volatile LONG g_opacity = kOpaque;
volatile LONG g_log_on = 0;
volatile LONG g_x = kUnplaced, g_y = kUnplaced;
volatile LONG g_w = 0, g_h = 0;   // 0 = never sized
volatile LONG g_show[kChannels] = {1, 1, 1, 1, 1, 1};
volatile LONG g_settings_dirty = 0;
volatile LONG g_ignore_dirty = 0;
// Bumped whenever the ring is dropped wholesale. The render thread's copy is
// incremental, so it needs to be told the difference between "nothing new"
// and "everything you have is from a session that ended".
volatile LONG g_epoch = 0;
// Set while there is no world, and cleared by the first poll that has read
// the history again. Between the hero pointer coming back and that poll, the
// ring might still belong to the character who just logged out - so nothing
// is drawn until the poll thread has checked. Starts set: nothing has been
// read yet.
volatile LONG g_resync = 1;

// --- poll thread only -------------------------------------------------------

int32_t g_tail = 0;               // how far into the history we have decoded
bool g_shrink_seen = false;       // the history read short on the last poll
std::string g_cmd_buf;            // last non-empty chat input seen
bool g_cmd_armed = false;         // that input begins with the `!!` prefix
// Consecutive polls that saw exactly that text. Two is as high as it needs to
// count: the question is only whether the text sat still for a whole poll
// interval before the box emptied. See poll_chatbox.
int32_t g_cmd_samples = 0;
FILE* g_log = nullptr;            // opened by chat_init, written only here

// --- render thread only -----------------------------------------------------

std::vector<Line> g_view;
uint64_t g_view_last = 0;
LONG g_view_epoch = -1;
std::vector<std::string> g_view_ignore;
LONG g_view_ignore_gen = -1;
bool g_follow = true;             // pinned to the newest line
// The ignore panel is open. Not persisted on purpose: it is a thing you open
// to change the list and shut again, not a mode the window sits in.
bool g_show_ignores = false;
uint64_t g_anchor = 0;            // newest line drawn while not following
bool g_dragging = false, g_resizing = false;
float g_drag_dx = 0, g_drag_dy = 0;
int g_seen_clicks = 0;

// --- text helpers -----------------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

bool name_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return tail.size() <= s.size() &&
           s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::string first_token(const std::string& s) {
    const size_t sp = s.find(' ');
    return sp == std::string::npos ? s : s.substr(0, sp);
}

// Chat arrives from other players. A tab or a newline in it would break the
// row layout, and a very long line would push everything else off screen, so
// both are dealt with once on the way in rather than on every frame.
std::string sanitise(const std::string& s) {
    std::string o = s.substr(0, 600);
    for (char& c : o)
        if ((unsigned char)c < 0x20) c = ' ';
    return o;
}

// Levenshtein, for naming the closest command when one is mistyped. Bounded
// because it runs on typed input and a pathological length is not worth a
// heap allocation.
int edit_distance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    if (n > 30 || m > 30) return 99;
    int prev[32], cur[32];
    for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; j++) {
            const int cost =
                tolower((unsigned char)a[i - 1]) ==
                        tolower((unsigned char)b[j - 1]) ? 0 : 1;
            int v = prev[j - 1] + cost;
            if (prev[j] + 1 < v) v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            cur[j] = v;
        }
        memcpy(prev, cur, sizeof(int) * (m + 1));
    }
    return prev[m];
}

bool ignored(const std::vector<std::string>& list, const std::string& who) {
    if (who.empty() || list.empty()) return false;
    for (const auto& n : list)
        if (name_eq(n, who)) return true;
    return false;
}

// --- the ring ---------------------------------------------------------------

void push_locked(Line l) {
    l.seq = ++g_seq;
    g_lines.push_back(std::move(l));
    if ((int)g_lines.size() > kRing)
        g_lines.erase(g_lines.begin(),
                      g_lines.begin() + (g_lines.size() - kRing));
}

// A reply from this module. Styled as a host line so it is never mistaken
// for something a player said.
void host_line(const std::string& text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    Line l;
    l.host = true;
    l.text = sanitise(text);
    l.hh = (uint8_t)st.wHour;
    l.mm = (uint8_t)st.wMinute;
    Lock lk;
    push_locked(std::move(l));
}

void reply(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    host_line(buf);
}

// --- files ------------------------------------------------------------------

bool read_all(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > 1u * 1024 * 1024) {
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
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(f);
}

void load_ignore() {
    std::string text;
    if (!read_all(g_dir + L"farever-chat-ignore.txt", &text)) return;
    std::vector<std::string> names;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line.size() > 64) continue;
        bool dup = false;
        for (const auto& n : names) dup = dup || name_eq(n, line);
        if (!dup) names.push_back(line);
    }
    Lock lk;
    g_ignore = std::move(names);
    g_ignore_gen++;
}

void save_ignore() {
    std::vector<std::string> names;
    {
        Lock lk;
        names = g_ignore;
    }
    std::string out =
        "# farever-modkit chat ignore list.\n"
        "# One character name per line; '#' starts a comment. Matching is\n"
        "# case-insensitive. Edit by hand, or use !!ignore / !!unignore in\n"
        "# the game's own chat box.\n\n";
    for (const auto& n : names) out += n + "\n";
    write_all(g_dir + L"farever-chat-ignore.txt", out);
}

// Nine to twenty-eight pixels. Below nine the font atlas has nothing legible
// to scale from and above twenty-eight two lines fill the window.
LONG clamp_size(LONG v) { return v < 9 ? 9 : (v > 28 ? 28 : v); }

// Twenty per cent is the floor: at zero the window is invisible and the
// setting reads as the mod having stopped working.
LONG clamp_opacity(LONG v) { return v < 20 ? 20 : (v > 100 ? 100 : v); }

void write_settings() {
    auto put = [](const wchar_t* key, LONG v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", (int)v);
        WritePrivateProfileStringW(L"chat", key, buf, g_ini_path.c_str());
    };
    put(L"enabled", InterlockedCompareExchange(&g_enabled, 0, 0));
    put(L"x", InterlockedCompareExchange(&g_x, 0, 0));
    put(L"y", InterlockedCompareExchange(&g_y, 0, 0));
    put(L"w", InterlockedCompareExchange(&g_w, 0, 0));
    put(L"h", InterlockedCompareExchange(&g_h, 0, 0));
    put(L"align", InterlockedCompareExchange(&g_aligned, 0, 0));
    put(L"timestamps", InterlockedCompareExchange(&g_timestamps, 0, 0));
    put(L"textsize", InterlockedCompareExchange(&g_text_size, 0, 0));
    put(L"opacity", InterlockedCompareExchange(&g_opacity, 0, 0));
    put(L"log", InterlockedCompareExchange(&g_log_on, 0, 0));
    static const wchar_t* kShowKey[kChannels] = {
        L"show_local", L"show_all", L"show_allsystem",
        L"show_whisper", L"show_group", L"show_system"};
    for (int i = 0; i < kChannels; i++)
        put(kShowKey[i], InterlockedCompareExchange(&g_show[i], 0, 0));
}

void mark_settings() { InterlockedExchange(&g_settings_dirty, 1); }

// --- the log ----------------------------------------------------------------

const char* channel_tag(const Line& l) {
    if (l.host) return "!!";
    switch (l.channel) {
        case kChatLocal:     return "L";
        case kChatAll:       return "A";
        case kChatAllSystem: return "A*";
        case kChatPlayer:    return "W";
        case kChatGroup:     return "G";
        case kChatSystem:    return "S";
        default:             return "?";
    }
}

// Poll thread only, and only for lines that are not from an ignored sender:
// the whole point of the ignore list is that those people leave no trace.
//
// The channel filters deliberately do NOT apply here. They are a chip you
// click to quieten the window for a minute, and a log that silently stopped
// recording a channel because of that would be worth less than no log - the
// record is the thing you cannot get back.
void log_line(const Line& l) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s%s\n", st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            channel_tag(l), l.sender.empty() ? "" : (l.sender + ": ").c_str(),
            l.text.c_str());
    // Flushed per line so the file can be tailed while the game runs, which
    // is the only reason it is opened shared in the first place.
    fflush(g_log);
}

// --- item links -------------------------------------------------------------

// The atlas indexes by item id, not by display name, and exposes no name
// search. What makes a name reachable anyway is that the ids *are* the
// display names with the spaces taken out - "Copper Ore" is CopperOre - so
// the id is derivable from what a person types. When none of the derivations
// resolve, that is reported plainly rather than guessed at.
bool link_lookup(const std::string& q, AtlasItemInfo* out) {
    const std::string t = trim(q);
    if (t.empty() || t.size() > 64) return false;
    // By display name first, because that is what a person types and what the
    // atlas's own search box matches. This used to go straight to the id and
    // then try squashing the spaces out of the name to make one - which works
    // for a minority of the database and silently fails for most weapons.
    // `!!link Credence` finding nothing while the atlas search bar found it
    // immediately is exactly that gap.
    if (atlas_ui_find_by_name(t, out)) return true;
    if (atlas_ui_lookup(t, out)) return true;
    std::string squash, camel;
    bool up = true;
    for (char c : t) {
        if (!isalnum((unsigned char)c)) {
            up = true;
            continue;
        }
        squash += c;
        camel += up ? (char)toupper((unsigned char)c)
                    : (char)tolower((unsigned char)c);
        up = false;
    }
    if (squash.empty()) return false;
    if (squash != t && atlas_ui_lookup(squash, out)) return true;
    if (camel != squash && atlas_ui_lookup(camel, out)) return true;
    return false;
}

// The clipboard belongs to whoever is using the machine, and `!!link` is only
// worth doing if it either replaces what is on it or leaves it alone. Two
// things are needed for that:
//
//   * a real owner window. OpenClipboard(nullptr) makes EmptyClipboard set
//     the clipboard owner to NULL, which is documented to make the
//     SetClipboardData that follows fail - so the old code emptied somebody's
//     clipboard and then reliably failed to put anything back.
//   * everything built and checked before the clipboard is touched at all.
//     EmptyClipboard has to come first (it is what makes `owner` the owner),
//     so it is the last thing done before the handover and nothing that can
//     fail is left after it.
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
    // something. A few tries at 10ms costs nothing on a thread that sleeps
    // for a hundred, and it is the difference between `!!link` working and
    // `!!link` losing a race with whatever else is running.
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

// --- commands ---------------------------------------------------------------
//
// None of these touch the game. They are typed into the game's own chat box
// and never reach the network, because ui.hud.ChatBox.processMessage returns
// after chatError() for a command it does not know (ChatBox.hx:165). See
// chat.h for the whole mechanism; the reading-back half lives in
// poll_chatbox() below.

const char* kCommands[] = {"!!help",  "!!ignore", "!!unignore", "!!ignores",
                           "!!find",  "!!clear",  "!!chat",     "!!time",
                           "!!align", "!!where",  "!!link",     "!!size",
                           "!!opacity"};

void cmd_help() {
    reply("Chat commands - typed into the game's own box, never sent.");
    reply("  !!ignore <name>    hide someone, here and in the log");
    reply("  !!unignore <name>  undo that");
    reply("  !!ignores          list who is ignored");
    reply("  !!find <text>      search this session's chat");
    reply("  !!clear            empty this window (the history stays)");
    reply("  !!chat             turn this window off and on");
    reply("  !!time             timestamps on and off");
    reply("  !!align            follow the game's chat box, or float free");
    reply("  !!where            what the navigator is following");
    reply("  !!link <text>      copy [Item Name] for pasting into chat");
    reply("  !!size <9-28>      text size");
    reply("  !!opacity <20-100> how solid this window is");
}

// The ignore list is stored as one name per line and parsed back by
// load_ignore, so a name is only storable if it survives that round trip. A
// newline in it becomes two entries; a leading '#' is read back as a comment
// and the name silently stops being ignored on the next start; a control
// character comes back as something nobody is called. Saying no here, and
// saying why, beats writing a file that reads differently from what was
// typed. The length bound is the same one load_ignore applies, tightened to
// what a character name can actually be.
bool ignore_name_ok(const std::string& who, const char** why) {
    if (who.empty()) {
        *why = "there is no name in that";
        return false;
    }
    if (who.size() > 32) {
        *why = "that is longer than a character name can be";
        return false;
    }
    if (who[0] == '#') {
        *why = "a name cannot start with '#' - the list file reads that as a "
               "comment and would drop it on the next start";
        return false;
    }
    for (const unsigned char c : who) {
        if (c < 0x20 || c == 0x7f) {
            *why = "that has a control character in it, and the list file is "
                   "one name to a line";
            return false;
        }
    }
    return true;
}

void cmd_ignore(const std::string& who) {
    if (who.empty()) {
        reply("Usage: !!ignore <name>");
        return;
    }
    const char* why = "";
    if (!ignore_name_ok(who, &why)) {
        reply("Not ignoring that: %s.", why);
        return;
    }
    bool already = false;
    {
        Lock lk;
        for (const auto& n : g_ignore) already = already || name_eq(n, who);
        if (!already) {
            g_ignore.push_back(who);
            g_ignore_gen++;
        }
    }
    if (already) {
        reply("%s is already ignored.", who.c_str());
        return;
    }
    InterlockedExchange(&g_ignore_dirty, 1);
    // Both halves name the string that actually went into the list, and the
    // undo is spelled out. What was typed and what was sampled are not always
    // the same (see run_command), so the name being ignored has to be on
    // screen next to the one command that can put it right.
    reply("Ignoring %s - their lines are hidden and not logged. "
          "!!unignore %s undoes it.", who.c_str(), who.c_str());
}

void cmd_unignore(const std::string& who) {
    if (who.empty()) {
        reply("Usage: !!unignore <name>");
        return;
    }
    bool found = false;
    {
        Lock lk;
        for (size_t i = 0; i < g_ignore.size(); i++) {
            if (!name_eq(g_ignore[i], who)) continue;
            g_ignore.erase(g_ignore.begin() + i);
            g_ignore_gen++;
            found = true;
            break;
        }
    }
    if (!found) {
        reply("%s was not on the ignore list.", who.c_str());
        return;
    }
    InterlockedExchange(&g_ignore_dirty, 1);
    // Hiding is applied when the window draws rather than when a line
    // arrives, so this brings their backlog back too.
    reply("%s is no longer ignored.", who.c_str());
}

void cmd_ignores() {
    std::vector<std::string> names;
    {
        Lock lk;
        names = g_ignore;
    }
    if (names.empty()) {
        reply("Nobody is ignored. !!ignore <name> to start.");
        return;
    }
    reply("Ignored (%d):", (int)names.size());
    std::string row;
    for (size_t i = 0; i < names.size(); i++) {
        if (!row.empty()) row += ", ";
        row += names[i];
        if (row.size() > 90 || i + 1 == names.size()) {
            reply("  %s", row.c_str());
            row.clear();
        }
    }
}

void cmd_find(const std::string& needle) {
    if (needle.empty()) {
        reply("Usage: !!find <text>");
        return;
    }
    constexpr int kMaxHits = 12;
    std::vector<std::string> hits;
    int total = 0;
    {
        Lock lk;
        for (size_t i = g_lines.size(); i-- > 0;) {
            const Line& l = g_lines[i];
            if (l.host) continue;                   // do not find old replies
            if (ignored(g_ignore, l.sender)) continue;
            if (!contains_ci(l.text, needle) && !contains_ci(l.sender, needle))
                continue;
            total++;
            if ((int)hits.size() >= kMaxHits) continue;
            char buf[400];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  %02d:%02d %s%.200s",
                        l.hh, l.mm,
                        l.sender.empty() ? "" : (l.sender + ": ").c_str(),
                        l.text.c_str());
            hits.push_back(buf);
        }
    }
    if (!total) {
        reply("Nothing in this session's chat matches '%s'.", needle.c_str());
        return;
    }
    reply("%d line%s matching '%s'%s:", total, total == 1 ? "" : "s",
          needle.c_str(),
          total > kMaxHits ? " (newest 12 shown)" : "");
    // Collected newest first, printed oldest first, so the newest match ends
    // up nearest the bottom where the eye already is.
    for (size_t i = hits.size(); i-- > 0;) host_line(hits[i]);
}

void cmd_where() {
    NavStatus ns;
    nav_status(&ns);
    if (!ns.active) {
        reply("The navigator is not following anything.");
        return;
    }
    const char* where = ns.where[0] ? ns.where : "";
    if (ns.is_route) {
        reply("Route '%s': heading for %s, %d of %d done%s%s", ns.name,
              ns.label[0] ? ns.label : "(unnamed)", ns.done, ns.total,
              where[0] ? " - " : "", where);
    } else {
        reply("Tracking %s%s%s", ns.name, where[0] ? " - " : "", where);
    }
}

// Both of these take a number, and both say what they ended up at rather than
// what they were told - the clamp is the interesting part when somebody types
// a 4 or a 200, and silently doing something other than what was asked reads
// as the command not working.
void cmd_size(const std::string& arg) {
    const std::string a = trim(arg);
    if (a.empty()) {
        reply("Text size is %d. !!size <9-28> changes it.",
              (int)InterlockedCompareExchange(&g_text_size, 0, 0));
        return;
    }
    const LONG want = (LONG)atoi(a.c_str());
    if (want <= 0) {
        reply("!!size takes a number from 9 to 28.");
        return;
    }
    const LONG got = clamp_size(want);
    InterlockedExchange(&g_text_size, got);
    InterlockedExchange(&g_settings_dirty, 1);
    if (got != want)
        reply("Text size %d - %d is outside 9 to 28.", (int)got, (int)want);
    else
        reply("Text size %d.", (int)got);
}

void cmd_opacity(const std::string& arg) {
    const std::string a = trim(arg);
    if (a.empty()) {
        reply("Opacity is %d%%. !!opacity <20-100> changes it. Below 100 the "
              "game's own copy of each line shows through from underneath - "
              "the host cannot hide the game's chat box, because that would "
              "mean writing to it.",
              (int)InterlockedCompareExchange(&g_opacity, 0, 0));
        return;
    }
    const LONG want = (LONG)atoi(a.c_str());
    if (want <= 0) {
        reply("!!opacity takes a number from 20 to 100.");
        return;
    }
    const LONG got = clamp_opacity(want);
    InterlockedExchange(&g_opacity, got);
    InterlockedExchange(&g_settings_dirty, 1);
    if (got != want)
        reply("Opacity %d%% - %d is outside 20 to 100.", (int)got, (int)want);
    else if (got < 100)
        reply("Opacity %d%% - the game's own chat will show through.",
              (int)got);
    else
        reply("Opacity 100%%.");
}

void cmd_link(const std::string& what) {
    if (what.empty()) {
        reply("Usage: !!link <item name>");
        return;
    }
    AtlasItemInfo info;
    if (!link_lookup(what, &info)) {
        // Three different things read as "no match" from here - no such
        // name, a name that matched several entries, and no atlas installed
        // at all - so say all three rather than send somebody hunting for a
        // typo that is not there.
        reply("No single atlas item matched '%s'. Try the exact name, or "
              "more of it if several items share it. With no "
              "farever-atlas.tsv installed nothing will match at all.",
              what.c_str());
        return;
    }
    const std::string link = "[" + info.name + "]";
    if (!clipboard_put(link)) {
        reply("Could not write to the clipboard.");
        return;
    }
    reply("Copied %s - paste it into chat. Anyone running this mod sees the "
          "icon; everyone else sees the text.", link.c_str());
}

// `settled` says the text was seen on at least two consecutive polls, so it
// stood unchanged for a full poll interval before the box emptied. When it
// was seen only once there is no evidence the player had stopped typing, and
// the sample can be a prefix of what was submitted - `!!ignore Emsey` read as
// `!!ignore Em` ignores the wrong person. Nothing available to a reader can
// close that window, so the string being acted on is said out loud first.
// Never silently, which is the part that matters.
void run_command(const std::string& line, bool settled) {
    if (!settled)
        reply("Running '%s' - that text appeared and was submitted inside one "
              "poll, so check it is not cut short.", line.c_str());

    const std::string cmd = lower(first_token(line));
    std::string args = trim(line.substr(first_token(line).size()));

    if (cmd == "!!size") return cmd_size(args);
    if (cmd == "!!opacity") return cmd_opacity(args);
    if (cmd == "!!help") return cmd_help();
    if (cmd == "!!ignore") return cmd_ignore(args);
    if (cmd == "!!unignore") return cmd_unignore(args);
    if (cmd == "!!ignores") return cmd_ignores();
    if (cmd == "!!find") return cmd_find(args);
    if (cmd == "!!where") return cmd_where();
    if (cmd == "!!link") return cmd_link(args);
    if (cmd == "!!clear") {
        {
            Lock lk;
            g_clear_seq = g_seq;
        }
        reply("View cleared. The session's history is untouched - !!find "
              "still searches all of it.");
        return;
    }
    if (cmd == "!!chat") {
        const bool on = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
        InterlockedExchange(&g_enabled, on ? 0 : 1);
        mark_settings();
        // The reply to turning it off lands in a window that is no longer
        // drawn. It is written anyway, because it is in the log, and because
        // the window says how to come back when the atlas is open.
        reply(on ? "Chat window off. Type !!chat again to bring it back."
                 : "Chat window on.");
        return;
    }
    if (cmd == "!!time") {
        const bool on = InterlockedCompareExchange(&g_timestamps, 0, 0) != 0;
        InterlockedExchange(&g_timestamps, on ? 0 : 1);
        mark_settings();
        reply(on ? "Timestamps hidden." : "Timestamps shown.");
        return;
    }
    if (cmd == "!!align") {
        const bool on = InterlockedCompareExchange(&g_aligned, 0, 0) != 0;
        InterlockedExchange(&g_aligned, on ? 0 : 1);
        mark_settings();
        reply(on ? "Free placement - open the atlas (F8) to move and size it."
                 : "Aligned to the game's own chat box.");
        return;
    }

    // An unmatched !! command. Naming the near miss is worth more than
    // repeating the list, because a mistyped command is nearly always one
    // letter away from the one that was meant.
    const char* best = nullptr;
    int best_d = 99;
    for (const char* c : kCommands) {
        const int d = edit_distance(cmd, c);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    if (best && best_d <= 3)
        reply("%s is not a command. Did you mean %s? !!help lists them.",
              cmd.c_str(), best);
    else
        reply("%s is not a command. !!help lists them.", cmd.c_str());
}

// --- polling ----------------------------------------------------------------

// Returns whether the ring can be trusted to belong to the session that is
// running - not whether anything new arrived. A read that failed and a
// shrink that has not been confirmed yet both answer no, and the window stays
// dark until one of them resolves.
//
// This is also the one place that decides a session has ended, and it decides
// it from the history itself. See chat_poll for why that matters.
bool poll_history() {
    std::vector<ChatMessage> msgs;
    int32_t total = 0;
    if (!reader_read_chat(g_tail, kBatch, &msgs, &total)) return false;

    if (total < g_tail) {
        // Not corruption. localReceiveMessage never trims, so a history that
        // got shorter is a different history: the client built a new
        // ChatClient, which is what a relog or a character swap does. An
        // index into the old one means nothing now.
        //
        // Confirmed over two polls before acting, because acting means
        // decoding the whole history again from nought - which is precisely
        // the thing that must not happen twice, since it is what appends a
        // second copy of the session to the log. A single length that read as
        // nought while the object was being torn down would be enough to
        // trigger it otherwise. The confirmation is "shorter again", not
        // "shorter by the same amount": a genuinely new history is already
        // growing by the next poll, and waiting for a stable number would
        // wait forever.
        if (!g_shrink_seen) {
            g_shrink_seen = true;
            return false;
        }
        g_shrink_seen = false;
        host_log("chat: history is %d, tail was %d - new session, restarting",
                 total, g_tail);
        g_tail = 0;
        {
            // The epoch moves under the same hold that empties the ring, so
            // the render thread cannot see one without the other and draw a
            // frame of the last session's chat.
            Lock lk;
            g_lines.clear();
            g_clear_seq = 0;
            InterlockedIncrement(&g_epoch);
        }
        return true;
    }
    g_shrink_seen = false;
    if (msgs.empty()) return true;
    g_tail += (int32_t)msgs.size();

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::vector<Line> to_log;
    {
        Lock lk;
        for (auto& m : msgs) {
            Line l;
            l.channel = m.channel;
            l.sender = std::move(m.sender);
            l.text = sanitise(m.text);
            l.other = std::move(m.other);
            l.mine = m.mine;
            l.hh = (uint8_t)st.wHour;
            l.mm = (uint8_t)st.wMinute;
            if (InterlockedCompareExchange(&g_log_on, 0, 0) &&
                !ignored(g_ignore, l.sender))
                to_log.push_back(l);
            push_locked(std::move(l));
        }
    }
    // Writing to a file is not something to do with the ring locked.
    for (const auto& l : to_log) log_line(l);
    return true;
}

void poll_chatbox() {
    ChatBoxState box;
    if (!reader_read_chatbox(&box) || !box.found) {
        // No box means nothing to align to and nothing to read a command
        // out of. The last good bounds stay; the half-typed command does
        // not, since there is no longer any way to learn whether it was sent.
        g_cmd_buf.clear();
        g_cmd_armed = false;
        g_cmd_samples = 0;
        return;
    }

    // The height is what is insisted on, because a rectangle with no height
    // cannot be drawn into and the free placement is the better answer. The
    // width is now the game's own too - h2d.Flow records the box its layout
    // settled on - but it is still allowed to be missing, since a width can
    // be supplied from the saved one and a height cannot be invented.
    if (box.visible && box.msg_h > 0 && box.scene_w > 0 && box.scene_h > 0) {
        Lock lk;
        g_align.ok = true;
        g_align.x = box.msg_x;
        g_align.y = box.msg_y;
        g_align.w = box.msg_w;
        g_align.h = box.msg_h;
        g_align.sw = box.scene_w;
        g_align.sh = box.scene_h;
    }

    // The command surface: buffer the input while it has text, and act when
    // it goes empty.
    //
    // This used to also require the game's own "Unknown chat command " echo
    // to appear as a new bare ui.hud.ChatBoxLine in the messages flow, as a
    // second signal that Enter rather than Escape had been pressed. That
    // signal is not available: the flow's children read back as
    // `ui.UIElement`, not as ChatBoxLine or ChatBoxMessage, so the test never
    // once passed and no command ever ran.
    //
    // It turns out not to be needed either, and that is the more useful half.
    // The only thing the second signal bought was not acting on a command the
    // player cancelled - and since every `!!` token is unmatched by
    // ChatBox.processMessage, it is swallowed locally at ChatBox.hx:165 and
    // never reaches the network whatever we do. So the worst an over-eager
    // trigger can do is run a local read-only command somebody meant to
    // abandon. That is a far better failure than the surface not working.
    //
    // The known gap stays, and it is worse than it used to be. What is
    // buffered is the last sample before Enter, so a command typed and
    // submitted inside one poll interval is seen half-typed - and under the
    // old echo-matching design a half-typed command matched nothing and did
    // nothing, which is what the comment here used to claim. It is not true
    // now: `!!ignore Emsey` sampled as `!!ignore Em` is a valid command with
    // a shorter argument, and it RUNS, on the wrong name.
    //
    // Nothing available to a reader closes that window. Waiting for the text
    // to hold still for two polls before arming it would not either - a human
    // types the rest and presses Enter well inside two hundred milliseconds,
    // so it would refuse ordinary commands and still not prove the sample was
    // complete; and re-reading the input on the transition reads the empty
    // box, which is how the transition was noticed. What is available is
    // whether the text stood still for a whole interval before the box
    // emptied, which is the difference between "they had stopped typing" and
    // "no idea". That is counted here and carried into run_command, which
    // names the string it is about to act on whenever there is no evidence.
    const std::string input = trim(box.input);
    if (!input.empty()) {
        // Not gated on `focused`. Text only appears in that box because the
        // player put it there, and the focus test is the most fragile read
        // on this path - it depends on resolving a virtual currentFocus and
        // reports false when it cannot, which would take the whole command
        // surface with it. Buffering unconditionally costs nothing: what is
        // buffered is only ever acted on if it starts with `!!`.
        if (g_cmd_buf != input) {
            g_cmd_buf = input;
            g_cmd_armed = input.size() > 2 && input.compare(0, 2, "!!") == 0;
            g_cmd_samples = 1;
        } else if (g_cmd_samples < 2) {
            g_cmd_samples++;
        }
    } else if (!g_cmd_buf.empty()) {
        const std::string buffered = g_cmd_buf;
        const bool armed = g_cmd_armed;
        const bool settled = g_cmd_samples >= 2;
        g_cmd_buf.clear();
        g_cmd_armed = false;
        g_cmd_samples = 0;
        if (armed) run_command(buffered, settled);
    }
}

// --- drawing ----------------------------------------------------------------

// Body text size and the two metrics derived from it. Deliberately not
// constants: `!!size` moves them, and every row, icon and wrap point is laid
// out from these rather than from numbers of its own, so one setting moves
// the whole window together instead of leaving the text and the rows to
// disagree. Render thread only - set at the top of chat_draw, from the
// persisted value, before anything reads them.
float kSize = 13.0f;
float kRowH = 17.0f;
float kIconSz = 15.0f;
constexpr float kPad = 6.0f;
constexpr float kChipH = 18.0f;

const char* kChanName[kChannels] = {"Local", "All",   "All sys",
                                    "Whisper", "Group", "System"};

float frame_alpha() {
    return (float)InterlockedCompareExchange(&g_opacity, 0, 0) / 100.0f;
}

Color channel_color(const Line& l) {
    if (l.host) return {0.35f, 0.78f, 1.00f, 1.0f};
    switch (l.channel) {
        case kChatLocal:     return {0.93f, 0.93f, 0.86f, 1.0f};
        case kChatAll:       return {0.52f, 0.78f, 1.00f, 1.0f};
        case kChatAllSystem: return {0.40f, 0.86f, 0.78f, 1.0f};
        case kChatPlayer:    return {1.00f, 0.60f, 0.90f, 1.0f};
        case kChatGroup:     return {0.58f, 0.92f, 0.52f, 1.0f};
        case kChatSystem:    return {0.98f, 0.78f, 0.38f, 1.0f};
        default:             return {0.70f, 0.72f, 0.78f, 1.0f};
    }
}

bool line_visible(const Line& l, const bool* show,
                  const std::vector<std::string>& ign, uint64_t floor_seq) {
    if (l.seq <= floor_seq) return false;
    if (l.host) return true;               // your own replies are never filtered
    if (l.channel >= 0 && l.channel < kChannels && !show[l.channel])
        return false;
    return !ignored(ign, l.sender);
}

// One drawable word. An atom is what wrapping moves around, so an item link
// that spans a space is two atoms with the same colour and the icon on the
// first - which is also why a link can break across rows without any special
// case.
struct Atom {
    std::string text;
    Color color{1, 1, 1, 1};
    int icon = -1;        // drawn immediately before the text
    float w = 0;          // text advance, icon excluded
    bool glue = false;    // no space before this one (a mid-word break)
};

void push_atom(std::vector<Atom>* out, const std::string& text, Color c) {
    if (text.empty()) return;
    Atom a;
    a.text = text;
    a.color = c;
    a.w = measure_text(kSize, text.c_str());
    out->push_back(std::move(a));
}

// Splits a message body into atoms, colouring any [bracketed run] the atlas
// resolves with that item's rarity colour and hanging its icon off the front.
// A run it cannot resolve is drawn plainly, which is what everyone without
// the mod sees anyway.
void build_body(const std::string& text, Color base, std::vector<Atom>* out) {
    struct Span {
        size_t b = 0, e = 0;
        Color c{1, 1, 1, 1};
        int icon = -1;
    };
    std::vector<Span> spans;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[') {
            const size_t close = text.find(']', i + 1);
            const size_t next = text.find('[', i + 1);
            if (close != std::string::npos && close - i <= 64 &&
                (next == std::string::npos || next > close)) {
                Span s;
                s.b = i;
                s.e = close;
                s.c = base;
                AtlasItemInfo info;
                if (link_lookup(text.substr(i + 1, close - i - 1), &info)) {
                    s.c = atlas_ui_rarity_color(info.rarity);
                    s.c.a = base.a;
                    s.icon = info.icon;
                }
                spans.push_back(s);
                i = close + 1;
                continue;
            }
        }
        i++;
    }

    size_t p = 0;
    while (p < text.size()) {
        while (p < text.size() && text[p] == ' ') p++;
        if (p >= text.size()) break;
        size_t q = text.find(' ', p);
        if (q == std::string::npos) q = text.size();
        Atom a;
        a.text = text.substr(p, q - p);
        a.color = base;
        for (const Span& s : spans) {
            if (p < s.b || p > s.e) continue;
            a.color = s.c;
            if (p == s.b) a.icon = s.icon;
            break;
        }
        a.w = measure_text(kSize, a.text.c_str());
        out->push_back(std::move(a));
        p = q;
    }
}

// A single token wider than the window has to break mid-word: the
// alternative is a row that runs off the edge, and one pasted URL should not
// be able to do that to the whole window.
void split_wide(std::vector<Atom>* atoms, float avail) {
    for (size_t i = 0; i < atoms->size(); i++) {
        const float lead = (*atoms)[i].icon >= 0 ? kIconSz + 3 : 0;
        if (lead + (*atoms)[i].w <= avail || (*atoms)[i].text.size() < 2)
            continue;
        float acc = lead;
        size_t cut = 0;
        const std::string& t = (*atoms)[i].text;
        while (cut < t.size()) {
            const char ch[2] = {t[cut], 0};
            const float cw = measure_text(kSize, ch);
            if (cut > 0 && acc + cw > avail) break;
            acc += cw;
            cut++;
        }
        if (cut >= t.size()) continue;
        Atom rest;
        rest.text = t.substr(cut);
        rest.color = (*atoms)[i].color;
        rest.glue = true;
        rest.w = measure_text(kSize, rest.text.c_str());
        (*atoms)[i].text.erase(cut);
        (*atoms)[i].w = measure_text(kSize, (*atoms)[i].text.c_str());
        atoms->insert(atoms->begin() + i + 1, rest);
    }
}

struct Row {
    size_t first = 0, count = 0;
};

void wrap_atoms(const std::vector<Atom>& atoms, float avail, float sp,
                std::vector<Row>* rows) {
    Row r;
    float used = 0;
    for (size_t i = 0; i < atoms.size(); i++) {
        const float lead = atoms[i].icon >= 0 ? kIconSz + 3 : 0;
        const float aw = lead + atoms[i].w;
        const float gap = (r.count && !atoms[i].glue) ? sp : 0;
        if (r.count && used + gap + aw > avail) {
            rows->push_back(r);
            r.first = i;
            r.count = 1;
            used = aw;
            continue;
        }
        if (!r.count) r.first = i;
        r.count++;
        used += gap + aw;
    }
    if (r.count) rows->push_back(r);
}

void draw_row(const std::vector<Atom>& atoms, const Row& r, float x, float y,
              float sp, float alpha) {
    float pen = x;
    for (size_t i = r.first; i < r.first + r.count; i++) {
        const Atom& a = atoms[i];
        if (i > r.first && !a.glue) pen += sp;
        if (a.icon >= 0) {
            atlas_ui_draw_icon(a.icon, pen, y + 1, kIconSz, alpha);
            pen += kIconSz + 3;
        }
        Color c = a.color;
        c.a *= alpha;
        draw_text(pen, y, kSize, c, a.text.c_str());
        pen += a.w;
    }
}

// Everything one line contributes, ready to place: the sender prefix and the
// body, already coloured.
void build_line(const Line& l, std::vector<Atom>* out) {
    const Color chan = channel_color(l);
    const Color body = l.host ? chan
                     : l.sender.empty() ? chan
                                        : Color{0.88f, 0.90f, 0.94f, 1.0f};
    if (!l.host) {
        if (l.channel == kChatPlayer && l.mine && !l.other.empty()) {
            push_atom(out, "To", chan);
            push_atom(out, l.other + ":", chan);
        } else if (l.channel == kChatPlayer && !l.mine && !l.sender.empty()) {
            push_atom(out, l.sender, chan);
            push_atom(out, "whispers:", chan);
        } else if (!l.sender.empty()) {
            push_atom(out, l.sender + ":", chan);
        }
    }
    build_body(l.text, body, out);
}

bool chip(const InputState& in, bool clicked, float x, float y, float w,
          const char* label, bool on) {
    const bool hot = in.mouse_x >= x && in.mouse_x < x + w &&
                     in.mouse_y >= y && in.mouse_y < y + kChipH;
    draw_rect(x, y, w, kChipH,
              on ? Color{0.16f, 0.24f, 0.34f, 0.95f}
                 : Color{0.10f, 0.11f, 0.15f, 0.95f});
    draw_rect_outline(x, y, w, kChipH, 1.0f,
                      hot ? Color{0.45f, 0.62f, 0.82f, 1.0f}
                          : Color{0.22f, 0.26f, 0.34f, 1.0f});
    draw_text(x + 6, y + 3, 11,
              on ? Color{0.88f, 0.93f, 1.00f, 1.0f}
                 : Color{0.42f, 0.46f, 0.54f, 1.0f},
              label);
    return clicked && in.click_x >= x && in.click_x < x + w &&
           in.click_y >= y && in.click_y < y + kChipH;
}

}  // namespace

void chat_init() {
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    g_dir = data_dir();
    g_ini_path = g_dir + L"farever-modkit.ini";

    const wchar_t* ini = g_ini_path.c_str();
    g_enabled = GetPrivateProfileIntW(L"chat", L"enabled", 1, ini) ? 1 : 0;
    g_x = GetPrivateProfileIntW(L"chat", L"x", kUnplaced, ini);
    g_y = GetPrivateProfileIntW(L"chat", L"y", kUnplaced, ini);
    g_w = GetPrivateProfileIntW(L"chat", L"w", 0, ini);
    g_h = GetPrivateProfileIntW(L"chat", L"h", 0, ini);
    g_aligned = GetPrivateProfileIntW(L"chat", L"align", 1, ini) ? 1 : 0;
    // On by default: the game records a timestamp on every message and then
    // never shows it, which is the one thing here that is free to fix.
    g_timestamps = GetPrivateProfileIntW(L"chat", L"timestamps", 1, ini) ? 1 : 0;
    // Clamped on the way in, not only where they are set: these are two of
    // the few settings somebody will reasonably hand-edit, and a 2px font or
    // a window at zero opacity reads as the mod being broken.
    g_text_size = clamp_size(GetPrivateProfileIntW(L"chat", L"textsize", 13, ini));
    g_opacity = clamp_opacity(
        GetPrivateProfileIntW(L"chat", L"opacity", kOpaque, ini));
    g_log_on = GetPrivateProfileIntW(L"chat", L"log", 0, ini) ? 1 : 0;
    static const wchar_t* kShowKey[kChannels] = {
        L"show_local", L"show_all", L"show_allsystem",
        L"show_whisper", L"show_group", L"show_system"};
    for (int i = 0; i < kChannels; i++)
        g_show[i] = GetPrivateProfileIntW(L"chat", kShowKey[i], 1, ini) ? 1 : 0;

    load_ignore();

    if (g_log_on && !g_log) {
        // Shared read, like the host log: a chat log nobody can open while
        // the game is running is a chat log for nobody. Appended rather than
        // truncated - yesterday's whisper is exactly what a log is for.
        g_log = _wfsopen((g_dir + L"farever-chat-log.txt").c_str(), L"a",
                         _SH_DENYNO);
        if (g_log) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(g_log, "\n--- session %04d-%02d-%02d %02d:%02d:%02d ---\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                    st.wSecond);
            fflush(g_log);
        }
    }

    size_t ignores = 0;
    {
        Lock lk;
        ignores = g_ignore.size();
    }
    host_log("chat: %s, %d ignored, log %s, %s placement",
             g_enabled ? "on" : "off", (int)ignores, g_log ? "on" : "off",
             g_aligned ? "aligned" : "free");
}

void chat_poll(bool in_world) {
    if (!g_cs_init) return;
    InterlockedExchange(&g_in_world, in_world ? 1 : 0);

    if (!in_world) {
        // Neither the decoded lines nor the index into the history is dropped
        // here, and that is the point. Losing the world does not mean the
        // game built a new ChatClient: a loading screen, a zone handover, any
        // transient where the hero pointer goes null and comes straight back
        // leaves the same client with the same history behind it. Resetting
        // the index on that re-decoded the whole session on the way back in -
        // appending a second copy of it to farever-chat-log.txt and pushing
        // every line of it into the window as though it had just been said.
        //
        // The reset itself was right for the case it was written for: a real
        // relog builds a new ChatClient with an empty history, and keeping
        // the old index would skip the whole next session. The two cases are
        // told apart by the history rather than by the hero pointer - a new
        // history is shorter than the index into the old one, which is the
        // `total < g_tail` test poll_history already makes. One signal, read
        // from the thing it is actually about.
        //
        // What is dropped here is only what cannot outlive the world: the
        // bounds of a chat box that is gone, and a half-typed command whose
        // fate can no longer be learned.
        {
            Lock lk;
            g_align.ok = false;
        }
        if (!InterlockedExchange(&g_resync, 1))
            host_log("chat: out of world, history kept - only a new "
                     "ChatClient restarts it");
        g_cmd_buf.clear();
        g_cmd_armed = false;
        g_cmd_samples = 0;
        return;
    }

    // Read even while the window is off. Turning it back on should show what
    // was said in between, the log should keep running, and - the reason it
    // matters - `!!chat` is typed into the game's box, so the way back has
    // to work when nothing of ours is on screen.
    InterlockedExchange(&g_console, reader_console_open() ? 1 : 0);
    const bool read_ok = poll_history();
    poll_chatbox();
    // Only now is the ring known to belong to the session that is running:
    // if this was the first poll after a relog, poll_history has already
    // emptied it. Until the history reads, the honest thing is to draw
    // nothing rather than the last character's chat.
    if (read_ok) InterlockedExchange(&g_resync, 0);
}

void chat_tick() {
    // No directory means chat_init could not find the game beside it. Writing
    // anyway would hand WritePrivateProfileStringW a bare filename, which it
    // resolves against the Windows directory - a stray file, and settings
    // that never come back.
    if (!g_cs_init || g_ini_path.empty()) return;
    if (InterlockedExchange(&g_settings_dirty, 0)) write_settings();
    if (InterlockedExchange(&g_ignore_dirty, 0)) save_ignore();
}

void chat_draw(float screen_w, float screen_h) {
    if (!g_cs_init) return;

    // Before anything measures or wraps. The ratios are the ones the window
    // was designed at (17/13 and 15/13), so changing the size keeps the rows
    // and the item icons in proportion with the text rather than leaving
    // gaps that grow with it.
    kSize = (float)InterlockedCompareExchange(&g_text_size, 0, 0);
    kRowH = kSize * (17.0f / 13.0f);
    kIconSz = kSize * (15.0f / 13.0f);

    // Same rule as the atlas window and the loot feed: at the main menu, a
    // loading screen or character select, none of this is on screen. It stays
    // off a little longer than that - the ring is now kept across a loading
    // screen, so until the poll thread has read the history once since the
    // world came back, what is held here could still be the chat of a
    // character who has logged out.
    if (!InterlockedCompareExchange(&g_in_world, 0, 0) ||
        InterlockedCompareExchange(&g_resync, 0, 0)) {
        input_set_aux_rect(kAux, 0, 0, 0, 0);
        input_set_wheel_rect(0, 0, 0, 0);
        g_dragging = g_resizing = false;
        g_view.clear();
        g_view_last = 0;
        g_follow = true;
        return;
    }

    InputState in;
    input_peek(&in);
    const bool clicked = in.clicks != g_seen_clicks;
    g_seen_clicks = in.clicks;
    // The console is a password-gated admin surface that owns its own keys.
    // While it is up the host takes no click and no wheel anywhere near it.
    const bool console = InterlockedCompareExchange(&g_console, 0, 0) != 0;
    const bool holding = in.visible && !console;

    const bool enabled = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
    if (!enabled) {
        input_set_aux_rect(kAux, 0, 0, 0, 0);
        input_set_wheel_rect(0, 0, 0, 0);
        g_dragging = g_resizing = false;
        // Off means off during play. But `!!chat` prints its reply into a
        // window that is not being drawn, so with the atlas open there has
        // to be something saying how to come back.
        if (holding) {
            const float w = 330, h = 24;
            const float x = 24, y = screen_h - h - 260;
            draw_rect(x, y, w, h, {0.05f, 0.06f, 0.09f, frame_alpha()});
            draw_text(x + 8, y + 5, 12, {0.55f, 0.60f, 0.70f, 1.0f},
                      "Chat window off - type !!chat in the game's chat box.");
        }
        return;
    }

    // --- take the shared state, once ----------------------------------------
    uint64_t clear_seq = 0;
    AlignRect align;
    {
        Lock lk;
        const LONG epoch = InterlockedCompareExchange(&g_epoch, 0, 0);
        if (epoch != g_view_epoch) {
            g_view.clear();
            g_view_last = 0;
            g_view_epoch = epoch;
            g_follow = true;
        }
        size_t first_new = g_lines.size();
        while (first_new > 0 && g_lines[first_new - 1].seq > g_view_last)
            first_new--;
        for (size_t i = first_new; i < g_lines.size(); i++)
            g_view.push_back(g_lines[i]);
        if (!g_lines.empty()) g_view_last = g_lines.back().seq;
        if (g_ignore_gen != g_view_ignore_gen) {
            g_view_ignore = g_ignore;
            g_view_ignore_gen = g_ignore_gen;
        }
        clear_seq = g_clear_seq;
        align = g_align;
    }
    if ((int)g_view.size() > kRing)
        g_view.erase(g_view.begin(), g_view.begin() + (g_view.size() - kRing));

    bool show[kChannels];
    for (int i = 0; i < kChannels; i++)
        show[i] = InterlockedCompareExchange(&g_show[i], 0, 0) != 0;
    const bool timestamps =
        InterlockedCompareExchange(&g_timestamps, 0, 0) != 0;

    // --- where the window goes ----------------------------------------------
    // Two aligneds, and they are not the same thing: what was asked for, and
    // what is actually being done. They part company when the game's own
    // bounds do not read, and the chips have to show the setting rather than
    // the fallback or the toggle appears to do nothing.
    const bool align_pref = InterlockedCompareExchange(&g_aligned, 0, 0) != 0;
    bool aligned = align_pref;
    float x = 0, y = 0, w = 0, h = 0;
    if (aligned && align.ok && align.sw > 0 && align.sh > 0) {
        // Markers and bounds are in the UI scene's own units and the overlay
        // draws in swap-chain pixels; the scene's own dimensions are the
        // ratio between them. reader_map_pick undoes this conversion for the
        // map, and this is the same one the other way round.
        const double kx = screen_w / (double)align.sw;
        const double ky = screen_h / (double)align.sh;
        x = (float)(align.x * kx);
        y = (float)(align.y * ky);
        w = (float)(align.w * kx);
        h = (float)(align.h * ky);
        // Only if the flow's own width did not read. It normally does, and
        // then all four edges are the game's own - which is the whole point:
        // the window lands on the message area and stops above the footer and
        // the text field, leaving both clickable.
        if (w < 120) {
            const LONG sw = InterlockedCompareExchange(&g_w, 0, 0);
            w = sw > 120 ? (float)sw : kFreeW;
            if (w > screen_w - x) w = screen_w - x;
        }
    }
    if (w < 120 || h < 50) {
        // The bounds did not read, or read as nothing. Falling back to the
        // free placement is the only option that still puts the window
        // somewhere a person can see it.
        aligned = false;
        const LONG sx = InterlockedCompareExchange(&g_x, 0, 0);
        const LONG sy = InterlockedCompareExchange(&g_y, 0, 0);
        const LONG sw = InterlockedCompareExchange(&g_w, 0, 0);
        const LONG sh = InterlockedCompareExchange(&g_h, 0, 0);
        w = sw > 120 ? (float)sw : kFreeW;
        h = sh > 60 ? (float)sh : kFreeH;
        x = (sx == kUnplaced) ? 24.0f : (float)sx;
        y = (sy == kUnplaced) ? screen_h - h - 210.0f : (float)sy;
    }

    // --- move and size it, but only with the atlas open ---------------------
    const bool movable = holding && !aligned;
    if (movable) {
        const float grip = 14;
        const bool hit = clicked && in.click_x >= x && in.click_x < x + w &&
                         in.click_y >= y && in.click_y < y + h &&
                         !input_in_main_rect(in.click_x, in.click_y);
        const bool on_grip = hit && in.click_x >= x + w - grip &&
                             in.click_y >= y + h - grip;
        // The chips own the bottom strip, so a click there is a filter
        // toggle and not the start of a drag.
        // The chip row is not draggable, or every click on a filter would
        // also shove the window. The ignore panel sits above the chips and
        // needs the same exemption, and it is the taller of the two - so
        // measure from the row it grows out of rather than from a constant.
        float dead_top = y + h - kChipH - 4;
        if (g_show_ignores) {
            const float rowh = 17.0f;
            const int n = (int)g_view_ignore.size();
            dead_top -= 4 + 20.0f + (n ? n * rowh : rowh);
            if (dead_top < y + kPad) dead_top = y + kPad;
        }
        const bool on_chips = hit && in.click_y >= dead_top;
        if (on_grip) {
            g_resizing = true;
            g_drag_dx = (x + w) - in.click_x;
            g_drag_dy = (y + h) - in.click_y;
        } else if (hit && !on_chips) {
            g_dragging = true;
            g_drag_dx = in.click_x - x;
            g_drag_dy = in.click_y - y;
        }
        if (g_resizing) {
            if (in.lbutton) {
                w = in.mouse_x + g_drag_dx - x;
                h = in.mouse_y + g_drag_dy - y;
                if (w < 220) w = 220;
                if (h < 90) h = 90;
            } else {
                g_resizing = false;
                mark_settings();
            }
        } else if (g_dragging) {
            if (in.lbutton) {
                x = in.mouse_x - g_drag_dx;
                y = in.mouse_y - g_drag_dy;
            } else {
                g_dragging = false;
                mark_settings();
            }
        }
    } else {
        g_dragging = g_resizing = false;
    }
    // Free placement is clamped on every frame, not only while dragging: a
    // rect saved on a bigger monitor must not put the window off the edge of
    // this one. The aligned rect is left alone - it is where the game's own
    // box is, and second-guessing that would only misalign it.
    if (!aligned) {
        if (w > screen_w) w = screen_w;
        if (h > screen_h) h = screen_h;
        if (x > screen_w - w) x = screen_w - w;
        if (y > screen_h - h) y = screen_h - h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        InterlockedExchange(&g_x, (LONG)x);
        InterlockedExchange(&g_y, (LONG)y);
        InterlockedExchange(&g_w, (LONG)w);
        InterlockedExchange(&g_h, (LONG)h);
    }
    // Clicks inside are only taken while the atlas window is open, the same
    // contract the navigator's pill and the loot feed follow - during play
    // the game's own chat box keeps every click that lands on it.
    input_set_aux_rect(kAux, holding ? (int)x : 0, holding ? (int)y : 0,
                       holding ? (int)w : 0, holding ? (int)h : 0);

    // The wheel is the exception to that contract, and it is published
    // whether or not the atlas is open. Scrollback that only works while a
    // second window is up is not scrollback - reading what somebody said a
    // minute ago is a thing you do mid-play, which is the whole reason this
    // window exists. Only the wheel is taken; clicks over the frame still
    // reach the game's own chat box underneath.
    //
    // Not while the console is up: it owns its own input and the host stays
    // clear of it.
    if (console)
        input_set_wheel_rect(0, 0, 0, 0);
    else
        input_set_wheel_rect((int)x, (int)y, (int)w, (int)h);

    // --- which lines are showing --------------------------------------------
    static std::vector<int> vis;
    vis.clear();
    for (int i = 0; i < (int)g_view.size(); i++)
        if (line_visible(g_view[i], show, g_view_ignore, clear_seq))
            vis.push_back(i);

    // --- the wheel ----------------------------------------------------------
    // Claimed only when the cursor is inside the frame, and never when the
    // atlas window is drawn over it: input priority has to follow what the
    // player can see.
    int detents = 0;
    if (!console && !input_in_main_rect(in.mouse_x, in.mouse_y))
        detents = input_take_wheel_in((int)x, (int)y, (int)w, (int)h);

    int anchor = (int)vis.size() - 1;
    if (!g_follow) {
        // Anchoring by sequence rather than by index is what keeps a line
        // arriving while you are reading back from yanking the view down.
        anchor = -1;
        for (int k = (int)vis.size(); k-- > 0;) {
            if (g_view[vis[k]].seq > g_anchor) continue;
            anchor = k;
            break;
        }
        if (anchor < 0) {
            g_follow = true;
            anchor = (int)vis.size() - 1;
        }
    }
    if (detents && !vis.empty()) {
        anchor -= detents * 3;      // positive is a wheel away from the hand
        if (anchor < 0) anchor = 0;
        if (anchor >= (int)vis.size() - 1) {
            anchor = (int)vis.size() - 1;
            g_follow = true;
        } else {
            g_follow = false;
            g_anchor = g_view[vis[anchor]].seq;
        }
    }

    // --- the frame ----------------------------------------------------------
    // Opaque on purpose. The window sits over the game's own message area,
    // and its whole job is to replace what is drawn there - including the
    // "Unknown chat command" echoes, which would otherwise show through.
    // Opaque by default, and it has to be: the game draws its own copy of
    // every one of these lines underneath, and the host cannot hide it.
    draw_rect(x, y, w, h, {0.04f, 0.05f, 0.08f, frame_alpha()});
    if (holding)
        draw_rect_outline(x, y, w, h, 1.0f, {0.35f, 0.75f, 1.0f, 0.8f});

    const float ts_w = timestamps ? measure_text(kSize, "00:00") + 7 : 0;
    const float tag_w = measure_text(kSize, "A*") + 8;
    const float body_x = x + kPad + ts_w + tag_w;
    const float body_w = x + w - kPad - body_x;
    const float sp = measure_text(kSize, " ");
    const float top = y + kPad;
    const float bottom = y + h - kPad - (holding ? kChipH + 4 : 0);

    if (body_w > 40) {
        float ycur = bottom;
        for (int k = anchor; k >= 0 && ycur > top; k--) {
            const Line& l = g_view[vis[k]];
            std::vector<Atom> atoms;
            build_line(l, &atoms);
            split_wide(&atoms, body_w);
            std::vector<Row> rows;
            wrap_atoms(atoms, body_w, sp, &rows);
            if (rows.empty()) continue;

            ycur -= rows.size() * kRowH;
            for (size_t r = 0; r < rows.size(); r++) {
                const float ry = ycur + r * kRowH;
                // The oldest message on screen is usually cut off by the top
                // edge; its rows above the edge are dropped rather than
                // drawn over whatever is up there.
                if (ry < top - 1 || ry + kRowH > bottom + 1) continue;
                draw_row(atoms, rows[r], body_x, ry, sp, 1.0f);
            }
            // The time and the channel sit against the first row only, so a
            // wrapped message reads as one thing rather than three.
            if (ycur >= top - 1 && ycur + kRowH <= bottom + 1) {
                if (timestamps) {
                    char clock[8];
                    _snprintf_s(clock, sizeof(clock), _TRUNCATE, "%02d:%02d",
                                l.hh, l.mm);
                    draw_text(x + kPad, ycur, kSize,
                              {0.40f, 0.44f, 0.52f, 1.0f}, clock);
                }
                Color tc = channel_color(l);
                tc.r *= 0.85f;
                tc.g *= 0.85f;
                tc.b *= 0.85f;
                draw_text(x + kPad + ts_w, ycur, kSize, tc, channel_tag(l));
            }
        }
    }

    if (vis.empty() && holding) {
        draw_text(x + kPad, top, 12, {0.45f, 0.50f, 0.60f, 1.0f},
                  "Nothing to show yet - !!help in the game's chat box lists "
                  "the commands.");
    }

    // How much is below the view, so scrolling back never feels like the
    // chat stopped.
    const int newer = (int)vis.size() - 1 - anchor;
    if (newer > 0) {
        char note[48];
        _snprintf_s(note, sizeof(note), _TRUNCATE, "%d newer", newer);
        const float nw = measure_text(11, note);
        const float ny = bottom - 13;
        draw_rect(x + w - kPad - nw - 8, ny - 2, nw + 8, 14,
                  {0.10f, 0.14f, 0.20f, 0.90f});
        draw_text(x + w - kPad - nw - 4, ny, 11, {0.70f, 0.78f, 0.90f, 1.0f},
                  note);
    }

    // --- the chips ----------------------------------------------------------
    if (!holding) return;
    float cx = x + kPad;
    const float cy = y + h - kChipH - 3;
    for (int i = 0; i < kChannels; i++) {
        const float cw = measure_text(11, kChanName[i]) + 12;
        if (cx + cw > x + w - kPad) break;
        if (chip(in, clicked, cx, cy, cw, kChanName[i], show[i])) {
            InterlockedExchange(&g_show[i], show[i] ? 0 : 1);
            mark_settings();
        }
        cx += cw + 4;
    }
    const float tw = measure_text(11, "Time") + 12;
    if (cx + tw <= x + w - kPad) {
        if (chip(in, clicked, cx, cy, tw, "Time", timestamps)) {
            InterlockedExchange(&g_timestamps, timestamps ? 0 : 1);
            mark_settings();
        }
        cx += tw + 4;
    }
    const float aw = measure_text(11, "Aligned") + 12;
    if (cx + aw <= x + w - kPad) {
        if (chip(in, clicked, cx, cy, aw, "Aligned", align_pref)) {
            InterlockedExchange(&g_aligned, align_pref ? 0 : 1);
            mark_settings();
        }
        cx += aw + 4;
    }
    // The ignore list, where it can be seen rather than only recited by
    // `!!ignores`. The count is on the chip so an ignore that did not take -
    // a rejected name, a file that would not load - is visible without
    // opening anything.
    char ig_label[32];
    _snprintf_s(ig_label, sizeof(ig_label), _TRUNCATE, "Ignored %d",
                (int)g_view_ignore.size());
    const float iw = measure_text(11, ig_label) + 12;
    if (cx + iw <= x + w - kPad) {
        if (chip(in, clicked, cx, cy, iw, ig_label, g_show_ignores))
            g_show_ignores = !g_show_ignores;
    }

    if (g_show_ignores) {
        // Above the chips and inside the frame, so it never draws over the
        // game's own input box below.
        const float rowh = 17.0f;
        const int n = (int)g_view_ignore.size();
        const float panel_h = 20.0f + (n ? n * rowh : rowh);
        float py = cy - 4 - panel_h;
        if (py < y + kPad) py = y + kPad;
        const float pw = w - kPad * 2;
        draw_rect(x + kPad, py, pw, panel_h, {0.02f, 0.03f, 0.05f, 0.97f});
        draw_rect_outline(x + kPad, py, pw, panel_h, 1.0f,
                          {0.35f, 0.45f, 0.60f, 0.9f});
        draw_text(x + kPad + 6, py + 3, 11, {0.60f, 0.68f, 0.80f, 1.0f},
                  n ? "Ignored - click a name to unignore"
                    : "Nobody is ignored. !!ignore <name> adds one.");
        float ry = py + 20;
        // A copy, because unignoring mutates the shared list and the render
        // thread's own copy is only refreshed at the top of the next frame.
        std::string remove;
        for (int i = 0; i < n; i++) {
            const std::string& who = g_view_ignore[i];
            if (ry + rowh > py + panel_h) break;
            const bool hot = in.mouse_x >= x + kPad && in.mouse_x < x + kPad + pw &&
                             in.mouse_y >= ry && in.mouse_y < ry + rowh;
            if (hot)
                draw_rect(x + kPad + 1, ry, pw - 2, rowh,
                          {0.20f, 0.12f, 0.14f, 0.9f});
            draw_text(x + kPad + 8, ry + 2, 11,
                      hot ? Color{1.00f, 0.72f, 0.72f, 1.0f}
                          : Color{0.80f, 0.84f, 0.92f, 1.0f},
                      who.c_str());
            if (hot) {
                const char* hint = "click to remove";
                const float hw = measure_text(10, hint);
                draw_text(x + kPad + pw - hw - 8, ry + 3, 10,
                          {0.70f, 0.50f, 0.50f, 1.0f}, hint);
            }
            if (clicked && in.click_x >= x + kPad &&
                in.click_x < x + kPad + pw && in.click_y >= ry &&
                in.click_y < ry + rowh)
                remove = who;
            ry += rowh;
        }
        if (!remove.empty()) cmd_unignore(remove);
    }
    // Asked to align and could not. Better said than silently ignored: the
    // window is somewhere the player put it, not where the game's box is.
    if (align_pref && !aligned) {
        draw_text(x + kPad, y + kPad - 1, 11, {0.85f, 0.60f, 0.40f, 1.0f},
                  "The game's chat box did not read - placed freely.");
    }
    if (movable) {
        // The resize grip, and the only hint that the frame can be moved.
        const float g = 14;
        draw_rect(x + w - g, y + h - g, g, g, {0.35f, 0.75f, 1.0f, 0.35f});
    }
}

}  // namespace fmk
