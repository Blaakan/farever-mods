// ---------------------------------------------------------------------------
// atlas_ui.cpp
//
// Immediate-mode UI over the overlay's four primitives. Everything the draw
// callback touches is either render-thread-local, immutable after init, or
// swapped in whole under a critical section (the ownership snapshot) - the
// draw callback never walks game memory and never blocks on the reader.
//
// Threading:
//   worker thread: atlas_ui_init / atlas_ui_update / atlas_ui_tick
//   game window thread: input.cpp's WndProc (publishes InputState)
//   render thread: atlas_ui_draw inside Present
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas_ui.h"
#include "input.h"
#include "navigator.h"
#include "overlay.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

// --- static item database (immutable once g_loaded is set) ------------------

constexpr int kCats = 12;
const char* kCatNames[kCats] = {"Appearances", "Mounts", "Pets", "Gliders",
                                "Trinkets", "Weapons", "Consumables",
                                "Materials", "Recipes", "Augments", "Misc",
                                "Creatures"};
const char* kCatTsv[kCats] = {"appearances", "mounts", "pets", "gliders",
                              "trinkets", "weapons", "consumables",
                              "materials", "recipes", "augments", "misc",
                              "creatures"};

// Three kinds of page, in this order: account-wide collection unlocks
// (0..3), real items owned only while they sit in a bank, bag or equipment
// slot (4..10), and the bestiary, whose progress comes from the codex
// rather than from anything you carry (11).
constexpr int kFirstItemCat = 4;
constexpr int kRecipesCat = 8;
constexpr int kCreaturesCat = 11;

struct Entry {
    std::string id, name, desc;
    std::string search;               // name + id, lowercased, for matching
    std::vector<std::string> acquire;
    std::vector<std::string> tags;    // "slot:Chest", "class:Mage", "area:Z1"
    std::vector<NavTarget> targets;   // tracker destinations, often empty
    int rarity = 0;      // 0..4 = common..legendary, from the CastleDB
    int icon = -1;       // cell in the icon atlas, -1 = none
    int cat = 0;
};

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

std::vector<Entry> g_entries;            // grouped by category
int g_cat_begin[kCats + 1]{};            // entry index ranges per category
std::unordered_map<std::string, int> g_entry_by_id[kCats];   // id -> entry idx

int   g_atlas = -1;                      // overlay texture handle
float g_atlas_w = 0, g_atlas_h = 0;      // for uv math
volatile LONG g_loaded = 0;

// --- ownership snapshot (swapped whole under g_own_cs) ----------------------

// One physical stack of an item somewhere: the bank, or a specific
// character's equipped gear or bags. Collection unlocks (appearances,
// mounts, pets, gliders) have no copies - just `unlocked`.
enum Where : uint8_t { kBank = 0, kBankSlots, kEquipped, kBags, kWhereCount };
const char* kWhereName[kWhereCount] = {"Bank", "Bank slots", "Equipped", "Bags"};

struct OwnedCopy {
    uint8_t where = kBank;
    std::string character;   // empty for the account-wide bank
    int level = 0;
    int rarity = -1;         // -1 = use the CastleDB rarity
    int count = 1;
};

struct Owned {
    bool unlocked = false;             // collection categories
    std::vector<OwnedCopy> copies;     // item categories
    int dropped = 0;                   // stacks past the per-copy cap
    int best_rarity = -1;
    int max_level = -1;
    int total = 0;
};

struct OwnSnap {
    std::unordered_map<std::string, Owned> byId[kCats];
    int owned_count[kCats]{};
    std::string character;
    // craft id -> the characters who know it. Recipes are learned per
    // character, so "known" is a question of by whom.
    std::unordered_map<std::string, std::set<std::string>> learned_by;
    std::unordered_map<std::string, int> job_level;
};

// Aggregates always update, even when the per-stack list is full: the total,
// the border rarity and the max level must reflect everything the account
// holds, not just the stacks the tooltip has room to list.
void owned_add_copy(Owned* o, uint8_t where, const std::string& character,
                    int level, int rarity, int count) {
    o->total += count;
    if (rarity > o->best_rarity) o->best_rarity = rarity;
    if (level > o->max_level) o->max_level = level;
    for (auto& c : o->copies) {
        if (c.where == where && c.character == character &&
            c.level == level && c.rarity == rarity) {
            c.count += count;
            return;
        }
    }
    if (o->copies.size() < 24)
        o->copies.push_back({where, character, level, rarity, count});
    else
        o->dropped++;
}

void owned_finalize(Owned* o) {
    if (o->unlocked && o->total == 0) o->total = 1;
    // Creature progress is set outright rather than accumulated per stack,
    // so leave a non-zero total alone.
}

CRITICAL_SECTION g_own_cs;
std::shared_ptr<const OwnSnap> g_own;
bool g_own_cs_init = false;

std::shared_ptr<const OwnSnap> own_get() {
    if (!g_own_cs_init) return nullptr;
    EnterCriticalSection(&g_own_cs);
    auto p = g_own;
    LeaveCriticalSection(&g_own_cs);
    return p;
}

// --- persisted layout -------------------------------------------------------

std::wstring g_ini_path;
volatile LONG g_layout_dirty = 0;

// Written by the render thread, persisted by the worker in atlas_ui_tick.
volatile LONG g_win_x = 140, g_win_y = 110;
volatile LONG g_tab = 0;

// --- render-thread state ----------------------------------------------------

volatile LONG g_in_world = 0;

// Search and filters. Search spans every page; filters belong to the page
// they were set on, so switching tabs does not carry a slot filter onto the
// creatures list.
std::string g_search;
bool g_search_focus = false;
std::vector<std::string> g_filters[kCats];   // selected tags, per page

bool has_filter(int cat, const std::string& tag) {
    for (const auto& t : g_filters[cat]) if (t == tag) return true;
    return false;
}

void toggle_filter(int cat, const std::string& tag) {
    for (size_t i = 0; i < g_filters[cat].size(); i++) {
        if (g_filters[cat][i] == tag) {
            g_filters[cat].erase(g_filters[cat].begin() + i);
            return;
        }
    }
    g_filters[cat].push_back(tag);
}

// A facet is the part before the colon. Selections inside one facet widen
// the result (Chest or Legs); selections across facets narrow it (a Chest
// piece that a Mage can wear).
// The value of a tag with the given prefix, e.g. tag_value(e, "craft:").
std::string tag_value(const Entry& e, const char* prefix) {
    const size_t n = strlen(prefix);
    for (const auto& t : e.tags)
        if (t.compare(0, n, prefix) == 0) return t.substr(n);
    return {};
}

std::string facet_of(const std::string& tag) {
    const size_t c = tag.find(':');
    return c == std::string::npos ? tag : tag.substr(0, c);
}

bool passes_filters(const Entry& e, int cat) {
    if (g_filters[cat].empty()) return true;
    std::vector<std::string> facets;
    for (const auto& f : g_filters[cat]) {
        const std::string fa = facet_of(f);
        if (std::find(facets.begin(), facets.end(), fa) == facets.end())
            facets.push_back(fa);
    }
    for (const auto& fa : facets) {
        bool any = false;
        for (const auto& sel : g_filters[cat]) {
            if (facet_of(sel) != fa) continue;
            for (const auto& t : e.tags) {
                if (t == sel) { any = true; break; }
            }
            if (any) break;
        }
        if (!any) return false;
    }
    return true;
}

float g_scroll[kCats]{};
bool  g_dragging = false;
float g_drag_dx = 0, g_drag_dy = 0;
int   g_seen_clicks = 0;
DWORD g_ready_tick = 0;                  // for the F8 discoverability hint

// --- style ------------------------------------------------------------------

const Color kBg        {0.055f, 0.065f, 0.095f, 0.94f};
const Color kBgTitle   {0.09f, 0.11f, 0.16f, 1.0f};
const Color kEdge      {0.35f, 0.75f, 1.0f, 0.9f};
const Color kText      {0.92f, 0.93f, 0.96f, 1.0f};
const Color kTextDim   {0.62f, 0.65f, 0.72f, 1.0f};
const Color kTextFaint {0.42f, 0.44f, 0.50f, 1.0f};
const Color kAccent    {0.35f, 0.75f, 1.0f, 1.0f};
const Color kTabOn     {0.16f, 0.22f, 0.32f, 1.0f};
const Color kTabOff    {0.08f, 0.10f, 0.15f, 1.0f};
const Color kCellBg    {0.10f, 0.11f, 0.15f, 1.0f};
const Color kMissTint  {0.34f, 0.34f, 0.38f, 0.9f};
const Color kMissEdge  {0.20f, 0.21f, 0.26f, 1.0f};
const Color kOwnTint   {1.0f, 1.0f, 1.0f, 1.0f};
const Color kAcquire   {0.55f, 0.85f, 0.75f, 1.0f};

const Color kRarity[5] = {
    {0.92f, 0.92f, 0.92f, 1.0f},     // common - white
    {0.35f, 0.82f, 0.35f, 1.0f},     // uncommon - green
    {0.36f, 0.58f, 1.0f, 1.0f},      // rare - blue
    {0.72f, 0.42f, 0.92f, 1.0f},     // epic - purple
    {1.0f, 0.73f, 0.25f, 1.0f},      // legendary - gold
};
const char* kRarityName[5] = {"Common", "Uncommon", "Rare", "Epic", "Legendary"};

// Window metrics.
constexpr float kTitleH = 34;
constexpr float kTabsH = 30;
constexpr float kPad = 12;
constexpr float kIcon = 48;
constexpr float kGap = 8;
constexpr float kStride = kIcon + kGap;
constexpr float kWinW = 985;
constexpr float kWinH = 660;

// --- helpers ----------------------------------------------------------------

std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return L"";
    slash[1] = 0;
    return path;
}

bool read_file(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr);
    // Nothing this UI reads is legitimately past a few MB; INVALID_FILE_SIZE
    // is 0xFFFFFFFF and would be a 4GB allocation.
    if (size == INVALID_FILE_SIZE || size > 64u * 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    out->resize(size);
    DWORD got = 0;
    BOOL ok = ReadFile(f, out->empty() ? nullptr : &(*out)[0], size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

// Mirror of the reader's JSON filename sanitizer: character names appear
// sanitized inside the files, so comparisons must sanitize the live name the
// same way or a name with any special character never matches its own file.
std::string sanitize_name(const std::string& s) {
    std::string out;
    for (char c : s)
        if (isalnum((unsigned char)c) || c == '_' || c == '-') out.push_back(c);
    return out;
}

int clamp_rarity(int r) { return (r >= 0 && r <= 4) ? r : -1; }

int cat_index(const std::string& s) {
    for (int i = 0; i < kCats; i++)
        if (s == kCatTsv[i]) return i;
    return -1;
}

// --- TSV / atlas loading (worker thread) ------------------------------------

bool load_tsv(const std::wstring& path) {
    std::string text;
    if (!read_file(path, &text)) {
        host_log("atlas_ui: %ls missing - run tools/gen-atlas.mjs", path.c_str());
        return false;
    }

    std::vector<Entry> entries;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) { f.push_back(line.substr(start)); break; }
            f.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (f.size() < 7) continue;

        Entry e;
        e.cat = cat_index(f[0]);
        if (e.cat < 0) continue;
        e.id = f[1];
        e.name = f[2];
        e.rarity = atoi(f[3].c_str());
        if (e.rarity < 0 || e.rarity > 4) e.rarity = 0;
        e.icon = atoi(f[4].c_str());
        e.desc = f[5];
        // acquire is " | "-joined
        std::string& a = f[6];
        size_t p = 0;
        while (p < a.size()) {
            size_t sep = a.find(" | ", p);
            if (sep == std::string::npos) {
                if (p < a.size()) e.acquire.push_back(a.substr(p));
                break;
            }
            e.acquire.push_back(a.substr(p, sep - p));
            p = sep + 3;
        }
        e.search = lower(e.name) + " " + lower(e.id);
        // tags are ","-joined facets
        if (f.size() >= 9 && !f[8].empty()) {
            size_t tp = 0;
            const std::string& tg = f[8];
            while (tp < tg.size()) {
                size_t sep = tg.find(',', tp);
                if (sep == std::string::npos) sep = tg.size();
                if (sep > tp) e.tags.push_back(tg.substr(tp, sep - tp));
                tp = sep + 1;
            }
        }
        // track is ";"-joined label@x,y,z
        if (f.size() >= 8 && !f[7].empty()) {
            size_t tp = 0;
            const std::string& tr = f[7];
            while (tp < tr.size() && e.targets.size() < 8) {
                size_t sep = tr.find(';', tp);
                if (sep == std::string::npos) sep = tr.size();
                std::string one = tr.substr(tp, sep - tp);
                tp = sep + 1;
                size_t at = one.find('@');
                if (at == std::string::npos || at == 0) continue;
                NavTarget t{};
                strncpy_s(t.label, one.substr(0, at).c_str(), _TRUNCATE);
                if (sscanf_s(one.c_str() + at + 1, "%lf,%lf,%lf",
                             &t.x, &t.y, &t.z) == 3)
                    e.targets.push_back(t);
            }
        }
        entries.push_back(std::move(e));
    }
    if (entries.empty()) {
        host_log("atlas_ui: %ls parsed to zero entries", path.c_str());
        return false;
    }

    // The TSV is already grouped by category in page order; keep that order
    // but rebuild the ranges defensively in case it ever is not.
    std::vector<Entry> grouped;
    grouped.reserve(entries.size());
    for (int c = 0; c < kCats; c++) {
        g_cat_begin[c] = (int)grouped.size();
        for (auto& e : entries)
            if (e.cat == c) grouped.push_back(e);
    }
    g_cat_begin[kCats] = (int)grouped.size();
    g_entries = std::move(grouped);
    for (int c = 0; c < kCats; c++)
        for (int i = g_cat_begin[c]; i < g_cat_begin[c + 1]; i++)
            g_entry_by_id[c][g_entries[i].id] = i;

    std::string counts;
    for (int c = 0; c < kCats; c++) {
        char one[32];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s=%d", counts.empty() ? "" : " ",
                    kCatTsv[c], g_cat_begin[c + 1] - g_cat_begin[c]);
        counts += one;
    }
    host_log("atlas_ui: %zu entries (%s)", g_entries.size(), counts.c_str());
    return true;
}

bool load_icons(const std::wstring& dir) {
    std::wstring path = dir + L"farever-atlas-icons.dds";
    // Only the header is needed here for uv math; the overlay does the load.
    std::string head;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        host_log("atlas_ui: icon atlas missing (icons will be flat tiles)");
        return true;                       // icons are optional
    }
    uint8_t hdr[20];
    DWORD got = 0;
    ReadFile(f, hdr, 20, &got, nullptr);
    CloseHandle(f);
    if (got == 20 && memcmp(hdr, "DDS ", 4) == 0) {
        g_atlas_h = (float)*(uint32_t*)(hdr + 12);
        g_atlas_w = (float)*(uint32_t*)(hdr + 16);
    }

    char utf8[MAX_PATH * 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8),
                             nullptr, nullptr)) {
        host_log("atlas_ui: icon atlas path conversion failed");
        return true;
    }
    g_atlas = overlay_load_atlas(utf8);
    if (g_atlas < 0)
        host_log("atlas_ui: icon atlas failed to load (flat tiles instead)");
    return true;
}

// --- other characters' inventories (worker thread) --------------------------
//
// One JSON per character, written by this host. Only bags and equipped are
// character-scoped - the bank repeats in every file, so only the sections
// from "equipped" onward are merged (counting the shared bank once per file
// would multiply every stack by the number of characters). The scanner only
// understands the exact flat shape hl_reader writes, which is all it has to.

void merge_item(OwnSnap* snap, const std::string& kind, uint8_t where,
                const std::string& character, int level, int rarity,
                int count) {
    for (int c = kFirstItemCat; c < kCreaturesCat; c++) {
        auto it = g_entry_by_id[c].find(kind);
        if (it == g_entry_by_id[c].end()) continue;
        owned_add_copy(&snap->byId[c][kind], where, character, level,
                       clamp_rarity(rarity), count);
    }
}

// Scans one section's item objects within [from, to).
void scan_section(const std::string& text, size_t from, size_t to,
                  uint8_t where, const std::string& character, OwnSnap* snap) {
    size_t pos = from;
    while (pos < to && (pos = text.find("{\"kind\":\"", pos)) != std::string::npos) {
        if (pos >= to) break;
        size_t kstart = pos + 9;
        size_t kend = text.find('"', kstart);
        if (kend == std::string::npos) break;
        std::string kind = text.substr(kstart, kend - kstart);
        size_t obj_end = text.find('}', kend);
        if (obj_end == std::string::npos) break;
        std::string obj = text.substr(kend, obj_end - kend);
        pos = obj_end;

        auto num = [&](const char* key, int fallback) {
            size_t p = obj.find(key);
            if (p == std::string::npos) return fallback;
            return atoi(obj.c_str() + p + strlen(key));
        };
        int level = num("\"level\":", 0);
        // Files written by older reader versions carry garbage rarity
        // indices; clamp_rarity keeps them off the kRarity[] tables.
        int rarity = num("\"rarity\":", -1);
        int count = num("\"count\":", 1);
        if (level < 0 || level > 999) level = 0;
        if (count < 1 || count > 100000) count = 1;
        merge_item(snap, kind, where, character, level, rarity, count);
    }
}

void scan_inventory_json(const std::string& text,
                         const std::string& skip_char_sanitized,
                         OwnSnap* snap) {
    // {"character": "Name", ...} - the file stores the sanitized name.
    std::string who = "?";
    size_t cpos = text.find("\"character\": \"");
    if (cpos != std::string::npos) {
        cpos += 14;
        size_t end = text.find('"', cpos);
        if (end != std::string::npos) who = text.substr(cpos, end - cpos);
    }
    if (who == skip_char_sanitized) return;   // live data covers this one
    if (who == "unknown" || who == "?") return;   // stale, unattributable file

    const size_t eq = text.find("\"equipped\"");
    if (eq == std::string::npos) return;
    const size_t bags = text.find("\"bags\"", eq);
    scan_section(text, eq, bags == std::string::npos ? text.size() : bags,
                 kEquipped, who, snap);
    if (bags != std::string::npos)
        scan_section(text, bags, text.size(), kBags, who, snap);
}

void merge_collection_list(const std::vector<std::string>& ids, int cat,
                           OwnSnap* snap) {
    for (const auto& id : ids) snap->byId[cat][id].unlocked = true;
}

// --- drawing helpers (render thread) ----------------------------------------

void icon_uv(int cell, float* u0, float* v0, float* u1, float* v1) {
    const int cols = (int)(g_atlas_w / 64.0f);
    const int cx = cell % (cols > 0 ? cols : 1);
    const int cy = cell / (cols > 0 ? cols : 1);
    // Half-texel inset so linear filtering never bleeds the neighbour cell.
    *u0 = (cx * 64 + 0.5f) / g_atlas_w;
    *v0 = (cy * 64 + 0.5f) / g_atlas_h;
    *u1 = ((cx + 1) * 64 - 0.5f) / g_atlas_w;
    *v1 = ((cy + 1) * 64 - 0.5f) / g_atlas_h;
}

// A rectangle trimmed to a vertical band, so it can be drawn inside a
// scrolling list without spilling over the edges.
void draw_rect_clipped(float x, float y, float w, float h, float clip_y0,
                       float clip_y1, Color c) {
    const float y0 = y < clip_y0 ? clip_y0 : y;
    const float y1 = (y + h) > clip_y1 ? clip_y1 : y + h;
    if (y1 > y0) draw_rect(x, y0, w, y1 - y0, c);
}

// The cell border, clipped the same way its icon is. Drawing it only when
// the whole cell fitted meant the top row lost its border the moment the
// list was scrolled by even a pixel.
void draw_cell_border(float x, float y, float size, float t, float clip_y0,
                      float clip_y1, Color c) {
    draw_rect_clipped(x, y, size, t, clip_y0, clip_y1, c);                 // top
    draw_rect_clipped(x, y + size - t, size, t, clip_y0, clip_y1, c);      // bottom
    draw_rect_clipped(x, y + t, t, size - 2 * t, clip_y0, clip_y1, c);     // left
    draw_rect_clipped(x + size - t, y + t, t, size - 2 * t, clip_y0, clip_y1,
                      c);                                                   // right
}

// Draws an icon clipped to [clip_y0, clip_y1), trimming uv proportionally.
void draw_icon_clipped(const Entry& e, float x, float y, float size,
                       float clip_y0, float clip_y1, Color tint) {
    float y0 = y, y1 = y + size;
    if (y1 <= clip_y0 || y0 >= clip_y1) return;
    float cut_top = y0 < clip_y0 ? (clip_y0 - y0) / size : 0;
    float cut_bot = y1 > clip_y1 ? (y1 - clip_y1) / size : 0;

    if (g_atlas >= 0 && e.icon >= 0 && g_atlas_w >= 64 && g_atlas_h >= 64) {
        float u0, v0, u1, v1;
        icon_uv(e.icon, &u0, &v0, &u1, &v1);
        float vr = v1 - v0;
        draw_image(g_atlas, x, y0 + cut_top * size, size,
                   size * (1 - cut_top - cut_bot),
                   u0, v0 + vr * cut_top, u1, v1 - vr * cut_bot, tint);
    } else {
        // No atlas: a flat tile in the rarity colour carries the slot.
        Color c = kRarity[e.rarity];
        c.a = tint.a * 0.35f;
        draw_rect(x, y0 + cut_top * size, size, size * (1 - cut_top - cut_bot), c);
    }
}

int wrap_text(const std::string& s, float size, float max_w,
              std::vector<std::string>* out) {
    size_t start = 0;
    while (start < s.size()) {
        size_t line_end = start;
        size_t word_end = start;
        while (word_end < s.size()) {
            size_t next = s.find(' ', word_end);
            if (next == std::string::npos) next = s.size();
            std::string cand = s.substr(start, next - start);
            if (measure_text(size, cand.c_str()) > max_w && line_end > start)
                break;
            line_end = next;
            word_end = next + 1;
            if (next >= s.size()) break;
        }
        if (line_end == start) line_end = s.size();   // one unbreakable word
        out->push_back(s.substr(start, line_end - start));
        start = line_end + (line_end < s.size() ? 1 : 0);
    }
    return (int)out->size();
}

struct Hover {
    bool valid = false;
    int entry = -1;
    float cell_x = 0, cell_y = 0;
};

// One "Bank x3 - Lv 25 - Rare" line per stored stack, colored by that
// stack's own rarity - the aggregate header stays rarity-free because the
// stacks can differ.
std::string copy_line(const OwnedCopy& c) {
    char buf[160];
    std::string s = kWhereName[c.where];
    if (!c.character.empty()) s += " (" + c.character + ")";
    if (c.count > 1) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, " x%d", c.count);
        s += buf;
    }
    if (c.level > 0) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, " - Lv %d", c.level);
        s += buf;
    }
    if (c.rarity >= 0 && c.rarity <= 4) {
        s += " - ";
        s += kRarityName[c.rarity];
    }
    return s;
}

Color copy_color(const OwnedCopy& c, const Entry& e) {
    const int r = (c.rarity >= 0 && c.rarity <= 4) ? c.rarity : e.rarity;
    return kRarity[r];
}

void draw_tooltip(const Entry& e, const Owned* owned, const char* track_key,
                  const OwnSnap* snap, float mx, float my, float screen_w,
                  float screen_h) {
    const float tw = 340;
    const float pad = 10;
    const float name_sz = 15, body_sz = 13, small_sz = 12;
    constexpr size_t kMaxCopyLines = 5;

    // Measure first, draw second - the box height needs every line known.
    std::vector<std::string> desc_lines, acq_lines;
    std::vector<std::pair<std::string, Color>> copy_lines;
    if (!e.desc.empty()) wrap_text(e.desc, body_sz, tw - 2 * pad, &desc_lines);
    for (const auto& a : e.acquire) {
        std::vector<std::string> lines;
        wrap_text(a, body_sz, tw - 2 * pad - 10, &lines);
        for (auto& l : lines) acq_lines.push_back(l);
    }
    if (owned) {
        for (const auto& c : owned->copies) {
            if (copy_lines.size() == kMaxCopyLines) {
                char more[48];
                _snprintf_s(more, sizeof(more), _TRUNCATE, "+%zu more stacks",
                            owned->copies.size() - kMaxCopyLines +
                                (size_t)owned->dropped);
                copy_lines.push_back({more, kTextDim});
                break;
            }
            copy_lines.push_back({copy_line(c), copy_color(c, e)});
        }
    }

    // Tracker lines: live distance when the hero position is fresh, the
    // target list otherwise, and the click hint.
    char track_dist[96] = {0};
    const bool tracked = nav_is_tracked(track_key);
    if (!e.targets.empty())
        nav_format_distance(e.targets.data(), (int)e.targets.size(),
                            track_dist, sizeof(track_dist));

    float th = pad + 20 /*name*/ + 17 /*status line*/;
    th += copy_lines.size() * 16.0f;
    if (!desc_lines.empty()) th += 6 + desc_lines.size() * 16.0f;
    if (!acq_lines.empty()) th += 6 + 16 /*header*/ + acq_lines.size() * 16.0f;
    if (!e.targets.empty()) th += 6 + 16 + (track_dist[0] ? 16.0f : 0);
    th += 4 + 14 /*id line*/ + pad;

    float tx = mx + 18, ty = my + 18;
    if (tx + tw > screen_w - 8) tx = mx - tw - 12;
    if (ty + th > screen_h - 8) ty = screen_h - th - 8;
    if (tx < 8) tx = 8;
    if (ty < 8) ty = 8;

    const int rar = owned && owned->best_rarity >= 0 ? owned->best_rarity
                                                     : e.rarity;
    draw_rect(tx, ty, tw, th, {0.03f, 0.04f, 0.06f, 0.97f});
    draw_rect_outline(tx, ty, tw, th, 1.5f, owned ? kRarity[rar] : kMissEdge);

    float yy = ty + pad;
    draw_text(tx + pad, yy, name_sz, kRarity[rar], e.name.c_str());
    yy += 20;

    // When stacks exist they can differ in rarity, so the aggregate line
    // stays rarity-free and each stack line carries its own. Collection
    // unlocks have no stack lines - for them the header keeps the rarity,
    // or hovering an owned mount would never name it.
    char status[160];
    if (e.cat == kRecipesCat) {
        // Known by whom is the whole question for a recipe.
        std::string who;
        if (snap) {
            const std::string craft = tag_value(e, "craft:");
            auto f = craft.empty() ? snap->learned_by.end()
                                   : snap->learned_by.find(craft);
            if (f != snap->learned_by.end()) {
                for (const auto& name : f->second) {
                    if (!who.empty()) who += ", ";
                    who += name;
                }
            }
        }
        if (!who.empty())
            _snprintf_s(status, sizeof(status), _TRUNCATE, "Known by %s",
                        who.c_str());
        else
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "Not learned by any character seen");
        draw_text(tx + pad, yy, small_sz,
                  who.empty() ? kTextDim : Color{0.45f, 0.85f, 0.45f, 1.0f},
                  status);
    } else if (e.cat == kCreaturesCat) {
        // The bestiary counts encounters, not possessions.
        if (owned)
            sprintf_s(status, "Encountered - codex progress %d", owned->total);
        else
            sprintf_s(status, "Not yet encountered");
        draw_text(tx + pad, yy, small_sz,
                  owned ? Color{0.45f, 0.85f, 0.45f, 1.0f} : kTextDim, status);
    } else if (owned) {
        if (owned->copies.empty())
            sprintf_s(status, "Owned - %s", kRarityName[rar]);
        else if (owned->total > 1)
            sprintf_s(status, "Owned x%d", owned->total);
        else
            sprintf_s(status, "Owned");
        draw_text(tx + pad, yy, small_sz, {0.45f, 0.85f, 0.45f, 1.0f}, status);
    } else if (e.cat < kFirstItemCat) {
        sprintf_s(status, "Not collected - %s", kRarityName[rar]);
        draw_text(tx + pad, yy, small_sz, kTextDim, status);
    } else {
        // Consumables and materials are not "collected" - you either have
        // some right now or you do not, and saying where we looked is more
        // use than calling it missing.
        sprintf_s(status, "None in bank or bags - %s", kRarityName[rar]);
        draw_text(tx + pad, yy, small_sz, kTextDim, status);
    }
    yy += 17;

    for (const auto& l : copy_lines) {
        draw_text(tx + pad + 10, yy, body_sz, l.second, l.first.c_str());
        yy += 16;
    }

    if (!desc_lines.empty()) {
        yy += 6;
        for (const auto& l : desc_lines) {
            draw_text(tx + pad, yy, body_sz, kTextDim, l.c_str());
            yy += 16;
        }
    }
    if (!acq_lines.empty()) {
        yy += 6;
        draw_text(tx + pad, yy, body_sz, kAccent, "How to get:");
        yy += 16;
        for (const auto& l : acq_lines) {
            draw_text(tx + pad + 10, yy, body_sz, kAcquire, l.c_str());
            yy += 16;
        }
    }
    if (!e.targets.empty()) {
        yy += 6;
        if (track_dist[0]) {
            char line[128];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "Nearest: %s",
                        track_dist);
            draw_text(tx + pad, yy, body_sz, kAccent, line);
            yy += 16;
        }
        draw_text(tx + pad, yy, small_sz,
                  tracked ? Color{1.0f, 0.75f, 0.35f, 1.0f}
                          : Color{0.55f, 0.60f, 0.70f, 1.0f},
                  tracked ? "Tracking - click to stop"
                          : "Click to track this location");
        yy += 16;
    }
    yy += 4;
    draw_text(tx + pad, yy, 11, kTextFaint, e.id.c_str());
}

void save_layout_dirty() { InterlockedExchange(&g_layout_dirty, 1); }

}  // namespace

// --- worker-thread API ------------------------------------------------------

bool atlas_ui_init() {
    if (!g_own_cs_init) {
        InitializeCriticalSection(&g_own_cs);
        g_own_cs_init = true;
    }
    std::wstring dir = exe_dir();
    if (dir.empty()) return false;
    g_ini_path = dir + L"farever-modkit.ini";

    if (!load_tsv(dir + L"farever-atlas.tsv")) return false;
    load_icons(dir);

    g_win_x = GetPrivateProfileIntW(L"atlas", L"x", 140, g_ini_path.c_str());
    g_win_y = GetPrivateProfileIntW(L"atlas", L"y", 110, g_ini_path.c_str());
    LONG tab = GetPrivateProfileIntW(L"atlas", L"tab", 0, g_ini_path.c_str());
    g_tab = (tab >= 0 && tab < kCats) ? tab : 0;

    g_ready_tick = GetTickCount();
    InterlockedExchange(&g_loaded, 1);
    host_log("atlas_ui: ready (F8 toggles)");
    return true;
}

// Recipes are per-character, so the ones a character knows are written out
// the way inventories are - that is how the atlas can say "known by Emsei"
// while you are playing someone else.
void write_jobs_json(const std::vector<JobState>& jobs,
                     const std::string& character) {
    if (jobs.empty() || character.empty()) return;
    std::wstring path = exe_dir();
    if (path.empty()) return;
    std::string safe = sanitize_name(character);
    if (safe.empty()) return;
    path += L"farever-jobs-" + std::wstring(safe.begin(), safe.end()) + L".json";

    std::string out = "{\n  \"character\": \"" + safe + "\",\n  \"jobs\": [";
    for (size_t i = 0; i < jobs.size(); i++) {
        char head[128];
        _snprintf_s(head, sizeof(head), _TRUNCATE,
                    "%s\n    {\"job\":\"%s\",\"level\":%d,\"learned\":[",
                    i ? "," : "", jobs[i].job.c_str(), jobs[i].level);
        out += head;
        for (size_t k = 0; k < jobs[i].learned.size(); k++) {
            if (k) out += ",";
            out += "\"" + jobs[i].learned[k] + "\"";
        }
        out += "]}";
    }
    out += "\n  ]\n}\n";

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(f);
}

// Pulls "learned" lists out of a farever-jobs-*.json this host wrote.
void scan_jobs_json(const std::string& text, const std::string& who,
                    OwnSnap* snap) {
    size_t pos = 0;
    while ((pos = text.find("\"learned\":[", pos)) != std::string::npos) {
        pos += 11;
        const size_t end = text.find(']', pos);
        if (end == std::string::npos) break;
        size_t p = pos;
        while (p < end) {
            const size_t a = text.find('"', p);
            if (a == std::string::npos || a > end) break;
            const size_t b = text.find('"', a + 1);
            if (b == std::string::npos || b > end) break;
            snap->learned_by[text.substr(a + 1, b - a - 1)].insert(who);
            p = b + 1;
        }
        pos = end;
    }
}

void atlas_ui_update(const Collection& c, const Inventories& inv,
                     const std::vector<std::pair<std::string, int32_t>>&
                         unit_progress,
                     const std::vector<JobState>& jobs) {
    if (!InterlockedCompareExchange(&g_loaded, 0, 0)) return;

    auto snap = std::make_shared<OwnSnap>();
    snap->character = inv.valid ? inv.character : "";

    if (c.valid) {
        merge_collection_list(c.gears, 0, snap.get());
        merge_collection_list(c.mounts, 1, snap.get());
        merge_collection_list(c.pets, 2, snap.get());
        merge_collection_list(c.gliders, 3, snap.get());
    }

    if (inv.valid) {
        // The bank is account-wide: no character attribution. Equipped and
        // bags belong to whoever is logged in.
        const std::string& who = snap->character;
        auto add_items = [&](const std::vector<Item>& items, uint8_t where,
                             const std::string& character) {
            for (const Item& it : items)
                merge_item(snap.get(), it.kind, where, character, it.level,
                           it.rarity, it.count);
        };
        add_items(inv.bank, kBank, "");
        add_items(inv.bank_equipment, kBankSlots, "");
        add_items(inv.equipped, kEquipped, who);
        add_items(inv.bags, kBags, who);
    }

    // Crafting: the live character's jobs, then every other character's as
    // this host last saw them.
    for (const auto& j : jobs) {
        snap->job_level[j.job] = j.level;
        for (const auto& craft : j.learned)
            snap->learned_by[craft].insert(snap->character);
    }
    write_jobs_json(jobs, snap->character);
    {
        const std::string skip = sanitize_name(snap->character);
        std::wstring dir = exe_dir();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"farever-jobs-*.json").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string text;
                if (!read_file(dir + fd.cFileName, &text)) continue;
                // farever-jobs-<name>.json
                std::wstring wname(fd.cFileName);
                std::string who(wname.begin(), wname.end());
                const size_t dash = who.find("jobs-");
                const size_t dot = who.rfind(".json");
                if (dash == std::string::npos || dot == std::string::npos) continue;
                who = who.substr(dash + 5, dot - dash - 5);
                if (who == skip || who.empty()) continue;
                scan_jobs_json(text, who, snap.get());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    // The bestiary: an entry in the codex map means encountered, and the
    // value is how far along that creature's progress stands.
    for (const auto& kv : unit_progress) {
        if (!g_entry_by_id[kCreaturesCat].count(kv.first)) continue;
        Owned& o = snap->byId[kCreaturesCat][kv.first];
        o.unlocked = true;
        o.total = kv.second > 0 ? kv.second : 1;
    }

    // Offline characters: their bags/equipped only exist in the JSON files
    // this host wrote while they were logged in.
    const std::string skip = sanitize_name(snap->character);
    std::wstring dir = exe_dir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"farever-inventory-*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string text;
            if (read_file(dir + fd.cFileName, &text))
                scan_inventory_json(text, skip, snap.get());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // A recipe counts as owned when the craft it teaches is known by
    // someone - having the paper in a bag is not the point of the page.
    for (int i = g_cat_begin[kRecipesCat]; i < g_cat_begin[kRecipesCat + 1]; i++) {
        const Entry& e = g_entries[i];
        const std::string craft = tag_value(e, "craft:");
        if (craft.empty()) continue;
        auto f = snap->learned_by.find(craft);
        if (f == snap->learned_by.end() || f->second.empty()) continue;
        Owned& o = snap->byId[kRecipesCat][e.id];
        o.unlocked = true;
    }

    // Finalize aggregates, then count owned ids that exist in the database.
    for (int cat = 0; cat < kCats; cat++) {
        int n = 0;
        for (auto& kv : snap->byId[cat]) {
            owned_finalize(&kv.second);
            if (g_entry_by_id[cat].count(kv.first)) n++;
        }
        snap->owned_count[cat] = n;
    }

    EnterCriticalSection(&g_own_cs);
    g_own = snap;
    LeaveCriticalSection(&g_own_cs);
}

void atlas_ui_set_in_world(bool in_world) {
    const LONG was = InterlockedExchange(&g_in_world, in_world ? 1 : 0);
    if (was && !in_world) {
        // Leaving the world: the snapshot describes a character who is no
        // longer loaded, so drop it rather than show it to the next one.
        if (g_own_cs_init) {
            EnterCriticalSection(&g_own_cs);
            g_own.reset();
            LeaveCriticalSection(&g_own_cs);
        }
        input_set_visible(false);
    }
}

void atlas_ui_tick() {
    if (!InterlockedCompareExchange(&g_layout_dirty, 0, 0)) return;
    InterlockedExchange(&g_layout_dirty, 0);
    wchar_t buf[32];
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_win_x, 0, 0));
    WritePrivateProfileStringW(L"atlas", L"x", buf, g_ini_path.c_str());
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_win_y, 0, 0));
    WritePrivateProfileStringW(L"atlas", L"y", buf, g_ini_path.c_str());
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_tab, 0, 0));
    WritePrivateProfileStringW(L"atlas", L"tab", buf, g_ini_path.c_str());
}

// --- render-thread draw -----------------------------------------------------

void atlas_ui_draw(float screen_w, float screen_h) {
    if (!InterlockedCompareExchange(&g_loaded, 0, 0)) return;

    // Nothing of ours belongs on screen at the main menu or a loading
    // screen - not the window, not even the hint.
    if (!InterlockedCompareExchange(&g_in_world, 0, 0)) {
        input_set_ui_rect(0, 0, 0, 0);
        g_dragging = false;
        return;
    }

    InputState in;
    input_get(&in);

    if (!in.visible) {
        input_set_ui_rect(0, 0, 0, 0);
        g_dragging = false;
        g_seen_clicks = in.clicks;
        // Discoverability: a quiet hint for the first minute of a session.
        if (GetTickCount() - g_ready_tick < 60000) {
            draw_text(24, screen_h - 46, 13, {0.7f, 0.75f, 0.85f, 0.65f},
                      "F8 - Collection Atlas");
        }
        return;
    }

    auto own = own_get();

    // --- window placement ---------------------------------------------------
    float win_w = kWinW, win_h = kWinH;
    if (win_w > screen_w - 40) win_w = screen_w - 40;
    if (win_h > screen_h - 40) win_h = screen_h - 40;

    float wx = (float)(LONG)InterlockedCompareExchange(&g_win_x, 0, 0);
    float wy = (float)(LONG)InterlockedCompareExchange(&g_win_y, 0, 0);

    const bool clicked = in.clicks != g_seen_clicks;
    g_seen_clicks = in.clicks;

    // Dragging: a press on the title bar picks the window up; releasing the
    // button anywhere drops it.
    if (clicked && in.click_x >= wx && in.click_x < wx + win_w - 36 &&
        in.click_y >= wy && in.click_y < wy + kTitleH) {
        g_dragging = true;
        g_drag_dx = in.click_x - wx;
        g_drag_dy = in.click_y - wy;
    }
    if (g_dragging) {
        if (in.lbutton) {
            wx = in.mouse_x - g_drag_dx;
            wy = in.mouse_y - g_drag_dy;
        } else {
            g_dragging = false;
            save_layout_dirty();
        }
    }
    if (wx < 8 - win_w + 80) wx = 8 - win_w + 80;
    if (wy < 0) wy = 0;
    if (wx > screen_w - 80) wx = screen_w - 80;
    if (wy > screen_h - kTitleH) wy = screen_h - kTitleH;
    InterlockedExchange(&g_win_x, (LONG)wx);
    InterlockedExchange(&g_win_y, (LONG)wy);
    input_set_ui_rect((int)wx, (int)wy, (int)win_w, (int)win_h);

    int tab = (int)InterlockedCompareExchange(&g_tab, 0, 0);
    if (tab < 0 || tab >= kCats) tab = 0;

    // --- chrome -------------------------------------------------------------
    draw_rect(wx, wy, win_w, win_h, kBg);
    draw_rect(wx, wy, win_w, kTitleH, kBgTitle);
    draw_rect_outline(wx, wy, win_w, win_h, 1.5f, kEdge);
    draw_text(wx + kPad, wy + 8, 16, kText, "Collection Atlas");

    if (own && !own->character.empty()) {
        // The name comes out of game memory - truncate, never abort.
        char who[80];
        _snprintf_s(who, sizeof(who), _TRUNCATE,
                    "%s + bank + offline characters", own->character.c_str());
        float ww = measure_text(12, who);
        draw_text(wx + win_w - 40 - ww, wy + 11, 12, kTextFaint, who);
    }

    // Close button.
    const float cx0 = wx + win_w - 32, cy0 = wy + 7;
    const bool close_hot = in.mouse_x >= cx0 && in.mouse_x < cx0 + 20 &&
                           in.mouse_y >= cy0 && in.mouse_y < cy0 + 20;
    draw_rect(cx0, cy0, 20, 20,
              close_hot ? Color{0.75f, 0.25f, 0.25f, 1.0f}
                        : Color{0.18f, 0.20f, 0.28f, 1.0f});
    draw_text(cx0 + 5.5f, cy0 + 1.5f, 14, kText, "x");
    if (clicked && in.click_x >= cx0 && in.click_x < cx0 + 20 &&
        in.click_y >= cy0 && in.click_y < cy0 + 20) {
        input_set_visible(false);
        return;
    }

    // --- tabs ---------------------------------------------------------------
    //
    // Eleven pages do not fit on one row, so the strip wraps and the content
    // below starts wherever it ends.
    const float tab_row_h = kTabsH - 6;
    const float tab_area_w = win_w - 12;
    char labels[kCats][64];
    float tab_bx[kCats], tab_by[kCats], tab_bw[kCats];
    int tab_rows = 1;
    {
        float lx = 0, ly = 0;
        for (int c = 0; c < kCats; c++) {
            const int total = g_cat_begin[c + 1] - g_cat_begin[c];
            if (own)
                sprintf_s(labels[c], "%s %d/%d", kCatNames[c],
                          own->owned_count[c], total);
            else
                sprintf_s(labels[c], "%s %d", kCatNames[c], total);
            const float tw = measure_text(13, labels[c]) + 18;
            if (lx > 0 && lx + tw > tab_area_w) {
                lx = 0;
                ly += tab_row_h + 3;
                tab_rows++;
            }
            tab_bx[c] = lx;
            tab_by[c] = ly;
            tab_bw[c] = tw;
            lx += tw + 4;
        }
    }
    const float tabs_h = tab_rows * (tab_row_h + 3) + 4;
    const float tab_ox = wx + 6, tab_oy = wy + kTitleH + 2;

    for (int c = 0; c < kCats; c++) {
        const float bx = tab_ox + tab_bx[c], by = tab_oy + tab_by[c];
        const float tw = tab_bw[c];
        const bool hot = in.mouse_x >= bx && in.mouse_x < bx + tw &&
                         in.mouse_y >= by && in.mouse_y < by + tab_row_h;
        draw_rect(bx, by, tw, tab_row_h,
                  c == tab ? kTabOn : (hot ? Color{0.12f, 0.15f, 0.22f, 1.0f}
                                           : kTabOff));
        if (c == tab) draw_rect(bx, by + tab_row_h - 2, tw, 2, kAccent);
        draw_text(bx + 9, by + 4, 13, c == tab ? kText : kTextDim, labels[c]);
        if (clicked && in.click_x >= bx && in.click_x < bx + tw &&
            in.click_y >= by && in.click_y < by + tab_row_h) {
            tab = c;
            InterlockedExchange(&g_tab, c);
            save_layout_dirty();
        }
    }

    // --- search box ---------------------------------------------------------
    //
    // Typing only reaches the box while it has focus; otherwise the movement
    // keys keep working with the window open, which matters more than saving
    // a click.
    const float sb_x = wx + kPad, sb_y = wy + kTitleH + tabs_h;
    const float sb_w = 260, sb_h = 22;
    const bool sb_hot = in.mouse_x >= sb_x && in.mouse_x < sb_x + sb_w &&
                        in.mouse_y >= sb_y && in.mouse_y < sb_y + sb_h;
    if (clicked) {
        const bool hit = in.click_x >= sb_x && in.click_x < sb_x + sb_w &&
                         in.click_y >= sb_y && in.click_y < sb_y + sb_h;
        if (hit != g_search_focus) {
            g_search_focus = hit;
            input_set_text_capture(hit);
        }
    }
    if (g_search_focus && !input_text_capture()) g_search_focus = false;

    if (g_search_focus) {
        char typed[64];
        const int n = input_take_text(typed, sizeof(typed));
        for (int i = 0; i < n; i++) {
            if (typed[i] == '\b') {
                if (!g_search.empty()) g_search.pop_back();
            } else if (typed[i] == '\n') {
                g_search_focus = false;
                input_set_text_capture(false);
            } else if (g_search.size() < 40) {
                g_search.push_back((char)tolower((unsigned char)typed[i]));
            }
        }
    }

    draw_rect(sb_x, sb_y, sb_w, sb_h, {0.03f, 0.04f, 0.06f, 1.0f});
    draw_rect_outline(sb_x, sb_y, sb_w, sb_h, 1.0f,
                      g_search_focus ? kAccent
                                     : (sb_hot ? kTextDim : kMissEdge));
    if (g_search.empty() && !g_search_focus) {
        draw_text(sb_x + 7, sb_y + 4, 13, kTextFaint,
                  "Search all pages - click here");
    } else {
        std::string shown = g_search;
        if (g_search_focus) shown += "_";
        draw_text(sb_x + 7, sb_y + 4, 13, kText, shown.c_str());
    }
    // A clear button, once there is something to clear.
    if (!g_search.empty()) {
        const float cx1 = sb_x + sb_w + 6;
        draw_rect(cx1, sb_y, 22, sb_h, {0.18f, 0.20f, 0.28f, 1.0f});
        draw_text(cx1 + 7, sb_y + 4, 13, kText, "x");
        if (clicked && in.click_x >= cx1 && in.click_x < cx1 + 22 &&
            in.click_y >= sb_y && in.click_y < sb_y + sb_h) {
            g_search.clear();
        }
    }

    // --- filter chips -------------------------------------------------------
    //
    // Built from the tags actually present on this page, so a page with
    // nothing to filter shows no row at all.
    const bool searching = !g_search.empty();
    float chips_h = 0;
    if (!searching) {
        std::vector<std::string> tags;
        for (int i = g_cat_begin[tab]; i < g_cat_begin[tab + 1]; i++) {
            for (const auto& t : g_entries[i].tags) {
                if (std::find(tags.begin(), tags.end(), t) == tags.end())
                    tags.push_back(t);
            }
        }
        std::sort(tags.begin(), tags.end());
        if (!tags.empty()) {
            const float row_h = 20;
            float cx1 = 0, cy1 = 0;
            const float avail = win_w - 2 * kPad;
            for (const auto& t : tags) {
                const std::string label = t.substr(t.find(':') + 1);
                const float cw = measure_text(12, label.c_str()) + 14;
                if (cx1 > 0 && cx1 + cw > avail) { cx1 = 0; cy1 += row_h + 3; }
                const float px = wx + kPad + cx1;
                const float py = sb_y + sb_h + 6 + cy1;
                const bool on = has_filter(tab, t);
                const bool hot = in.mouse_x >= px && in.mouse_x < px + cw &&
                                 in.mouse_y >= py && in.mouse_y < py + row_h;
                draw_rect(px, py, cw, row_h,
                          on ? Color{0.20f, 0.34f, 0.50f, 1.0f}
                             : (hot ? Color{0.14f, 0.16f, 0.23f, 1.0f}
                                    : Color{0.09f, 0.10f, 0.15f, 1.0f}));
                if (on) draw_rect_outline(px, py, cw, row_h, 1.0f, kAccent);
                draw_text(px + 7, py + 3, 12, on ? kText : kTextDim,
                          label.c_str());
                if (clicked && in.click_x >= px && in.click_x < px + cw &&
                    in.click_y >= py && in.click_y < py + row_h) {
                    toggle_filter(tab, t);
                }
                cx1 += cw + 4;
            }
            chips_h = cy1 + row_h + 6;
        }
    }

    // --- the visible set ----------------------------------------------------
    //
    // Search looks across every page; filters apply to the page you are on.
    static std::vector<int> visible;
    visible.clear();
    if (searching) {
        for (size_t i = 0; i < g_entries.size(); i++) {
            if (g_entries[i].search.find(g_search) != std::string::npos)
                visible.push_back((int)i);
        }
    } else {
        for (int i = g_cat_begin[tab]; i < g_cat_begin[tab + 1]; i++) {
            if (passes_filters(g_entries[i], tab)) visible.push_back(i);
        }
    }

    // --- grid ---------------------------------------------------------------
    const float header_h = sb_h + 6 + chips_h;
    const float content_x = wx + kPad;
    const float content_y = wy + kTitleH + tabs_h + 4 + header_h;
    const float content_w = win_w - 2 * kPad - 10;   // room for scrollbar
    const float content_h = win_h - (kTitleH + tabs_h + 4 + header_h) - kPad;
    const int cols = content_w >= kStride ? (int)((content_w + kGap) / kStride) : 1;

    const int count = (int)visible.size();
    const int rows = (count + cols - 1) / cols;
    const float total_h = rows * kStride;
    float max_scroll = total_h - content_h;
    if (max_scroll < 0) max_scroll = 0;

    // Searching has its own scroll position, so returning to a page does not
    // land halfway down a list it no longer shows.
    static float search_scroll = 0;
    float& scroll = searching ? search_scroll : g_scroll[tab];
    scroll -= in.wheel * kStride * 2;
    if (scroll < 0) scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;

    Hover hover;
    const bool mouse_in_content =
        in.mouse_x >= content_x && in.mouse_x < content_x + content_w &&
        in.mouse_y >= content_y && in.mouse_y < content_y + content_h;

    const int row0 = (int)(scroll / kStride);
    const int row1 = (int)((scroll + content_h) / kStride) + 1;
    for (int r = row0; r <= row1 && r < rows; r++) {
        for (int col = 0; col < cols; col++) {
            const int slot = r * cols + col;
            if (slot >= count) break;
            const int idx = visible[slot];
            const Entry& e = g_entries[idx];
            // While searching the grid mixes pages, so ownership has to be
            // looked up against the entry's own page rather than the tab.
            const int ecat = e.cat;
            const float x = content_x + col * kStride;
            const float y = content_y + r * kStride - scroll;
            if (y + kIcon < content_y || y > content_y + content_h) continue;

            const Owned* owned = nullptr;
            if (own) {
                auto f = own->byId[ecat].find(e.id);
                if (f != own->byId[ecat].end()) owned = &f->second;
            }

            // Cell background, then the icon clipped to the content band.
            const float clip0 = content_y, clip1 = content_y + content_h;
            float by0 = y < clip0 ? clip0 : y;
            float by1 = (y + kIcon) > clip1 ? clip1 : y + kIcon;
            if (by1 > by0) draw_rect(x, by0, kIcon, by1 - by0, kCellBg);
            draw_icon_clipped(e, x, y, kIcon, clip0, clip1,
                              owned ? kOwnTint : kMissTint);

            // Rarity border for owned, faint frame for missing, clipped to
            // the band like the icon so a half-scrolled row still has one.
            if (owned) {
                const int rar = owned->best_rarity >= 0 ? owned->best_rarity
                                                        : e.rarity;
                draw_cell_border(x, y, kIcon, 2, clip0, clip1, kRarity[rar]);
            } else {
                draw_cell_border(x, y, kIcon, 1, clip0, clip1, kMissEdge);
            }

            if (mouse_in_content && in.mouse_x >= x && in.mouse_x < x + kIcon &&
                in.mouse_y >= by0 && in.mouse_y < by1) {
                hover.valid = true;
                hover.entry = idx;
                hover.cell_x = x;
                hover.cell_y = y;
            }

            // A click on a cell with known coordinates toggles the tracker.
            if (clicked && !e.targets.empty() &&
                in.click_x >= x && in.click_x < x + kIcon &&
                in.click_y >= by0 && in.click_y < by1) {
                char key[192];
                _snprintf_s(key, sizeof(key), _TRUNCATE, "%s/%s",
                            kCatTsv[ecat], e.id.c_str());
                nav_track(key, e.name.c_str(), e.targets.data(),
                          (int)e.targets.size());
            }
        }
    }

    // Scrollbar.
    if (max_scroll > 0) {
        const float track_x = wx + win_w - kPad + 2;
        draw_rect(track_x, content_y, 6, content_h, {0.10f, 0.11f, 0.16f, 1.0f});
        const float thumb_h = content_h * (content_h / total_h);
        const float thumb_y = content_y + (content_h - thumb_h) *
                                           (scroll / max_scroll);
        draw_rect(track_x, thumb_y, 6, thumb_h, {0.30f, 0.38f, 0.52f, 1.0f});
    }

    // Hover highlight + tooltip last, above everything. The outline only
    // draws when the cell sits fully inside the content band - a partial
    // outline would paint over the tab strip or the window border.
    if (hover.valid) {
        const Entry& e = g_entries[hover.entry];
        draw_cell_border(hover.cell_x, hover.cell_y, kIcon, 2, content_y,
                         content_y + content_h, {1.0f, 1.0f, 1.0f, 0.85f});
        const Owned* owned = nullptr;
        if (own) {
            auto f = own->byId[e.cat].find(e.id);
            if (f != own->byId[e.cat].end()) owned = &f->second;
        }
        char key[192];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%s/%s", kCatTsv[e.cat],
                    e.id.c_str());
        draw_tooltip(e, owned, key, own.get(), (float)in.mouse_x, (float)in.mouse_y,
                     screen_w, screen_h);
    }

}

}  // namespace fmk
