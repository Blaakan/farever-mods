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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas_ui.h"
#include "input.h"
#include "overlay.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

// --- static item database (immutable once g_loaded is set) ------------------

constexpr int kCats = 6;
const char* kCatNames[kCats] = {"Appearances", "Mounts", "Pets",
                                "Gliders", "Trinkets", "Weapons"};
const char* kCatTsv[kCats] = {"appearances", "mounts", "pets",
                              "gliders", "trinkets", "weapons"};

struct Entry {
    std::string id, name, desc;
    std::vector<std::string> acquire;
    int rarity = 0;      // 0..4 = common..legendary, from the CastleDB
    int icon = -1;       // cell in the icon atlas, -1 = none
    int cat = 0;
};

std::vector<Entry> g_entries;            // grouped by category
int g_cat_begin[kCats + 1]{};            // entry index ranges per category
std::unordered_map<std::string, int> g_entry_by_id[kCats];   // id -> entry idx

int   g_atlas = -1;                      // overlay texture handle
float g_atlas_w = 0, g_atlas_h = 0;      // for uv math
volatile LONG g_loaded = 0;

// --- ownership snapshot (swapped whole under g_own_cs) ----------------------

struct Owned {
    int level = -1;      // -1 = not applicable (collection unlocks)
    int rarity = -1;     // -1 = use the CastleDB rarity
    int count = 0;
};

struct OwnSnap {
    std::unordered_map<std::string, Owned> byId[kCats];
    int owned_count[kCats]{};
    std::string character;
};

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

    host_log("atlas_ui: %zu entries (%d/%d/%d/%d/%d/%d)", g_entries.size(),
             g_cat_begin[1] - g_cat_begin[0], g_cat_begin[2] - g_cat_begin[1],
             g_cat_begin[3] - g_cat_begin[2], g_cat_begin[4] - g_cat_begin[3],
             g_cat_begin[5] - g_cat_begin[4], g_cat_begin[6] - g_cat_begin[5]);
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
// character-scoped - the bank repeats in every file, so only the section
// from "equipped" onward is merged (counting the shared bank once per file
// would multiply every stack by the number of characters). The scanner only
// understands the exact flat shape hl_reader writes, which is all it has to.

void scan_inventory_json(const std::string& text,
                         const std::string& skip_char_sanitized,
                         OwnSnap* snap) {
    // {"character": "Name", ...} - the file stores the sanitized name.
    size_t cpos = text.find("\"character\": \"");
    if (cpos != std::string::npos) {
        cpos += 14;
        size_t end = text.find('"', cpos);
        if (end != std::string::npos &&
            text.substr(cpos, end - cpos) == skip_char_sanitized)
            return;                        // live data already covers this one
    }

    size_t pos = text.find("\"equipped\"");
    if (pos == std::string::npos) return;
    while ((pos = text.find("{\"kind\":\"", pos)) != std::string::npos) {
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
        // indices; anything outside 0..4 would index kRarity[] out of
        // bounds on the render thread.
        int rarity = clamp_rarity(num("\"rarity\":", -1));
        int count = num("\"count\":", 1);
        if (level < 0 || level > 999) level = 0;
        if (count < 1 || count > 100000) count = 1;

        for (int c : {4, 5}) {             // trinkets, weapons
            auto it = g_entry_by_id[c].find(kind);
            if (it == g_entry_by_id[c].end()) continue;
            Owned& o = snap->byId[c][kind];
            if (level > o.level) o.level = level;
            if (rarity > o.rarity) o.rarity = rarity;
            o.count += count;
        }
    }
}

void merge_collection_list(const std::vector<std::string>& ids, int cat,
                           OwnSnap* snap) {
    for (const auto& id : ids) {
        Owned& o = snap->byId[cat][id];
        o.count = o.count ? o.count : 1;
    }
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

void draw_tooltip(const Entry& e, const Owned* owned, float mx, float my,
                  float screen_w, float screen_h) {
    const float tw = 340;
    const float pad = 10;
    const float name_sz = 15, body_sz = 13, small_sz = 12;

    // Measure first: lines of desc + acquire.
    std::vector<std::string> desc_lines, acq_lines;
    if (!e.desc.empty()) wrap_text(e.desc, body_sz, tw - 2 * pad, &desc_lines);
    for (const auto& a : e.acquire) {
        std::vector<std::string> lines;
        wrap_text(a, body_sz, tw - 2 * pad - 10, &lines);
        for (auto& l : lines) acq_lines.push_back(l);
    }

    float th = pad + 20 /*name*/ + 17 /*status line*/;
    if (!desc_lines.empty()) th += 6 + desc_lines.size() * 16.0f;
    if (!acq_lines.empty()) th += 6 + 16 /*header*/ + acq_lines.size() * 16.0f;
    th += 4 + 14 /*id line*/ + pad;

    float tx = mx + 18, ty = my + 18;
    if (tx + tw > screen_w - 8) tx = mx - tw - 12;
    if (ty + th > screen_h - 8) ty = screen_h - th - 8;
    if (tx < 8) tx = 8;
    if (ty < 8) ty = 8;

    draw_rect(tx, ty, tw, th, {0.03f, 0.04f, 0.06f, 0.97f});
    draw_rect_outline(tx, ty, tw, th, 1.5f,
                      owned ? kRarity[owned->rarity >= 0 ? owned->rarity
                                                         : e.rarity]
                            : kMissEdge);

    float yy = ty + pad;
    const int rar = owned && owned->rarity >= 0 ? owned->rarity : e.rarity;
    draw_text(tx + pad, yy, name_sz, kRarity[rar], e.name.c_str());
    yy += 20;

    char status[96];
    if (owned) {
        if (owned->level >= 0) {
            if (owned->count > 1)
                sprintf_s(status, "Owned x%d - Level %d - %s", owned->count,
                          owned->level, kRarityName[rar]);
            else
                sprintf_s(status, "Owned - Level %d - %s", owned->level,
                          kRarityName[rar]);
        } else {
            sprintf_s(status, "Owned - %s", kRarityName[rar]);
        }
        draw_text(tx + pad, yy, small_sz, {0.45f, 0.85f, 0.45f, 1.0f}, status);
    } else {
        sprintf_s(status, "Not collected - %s", kRarityName[rar]);
        draw_text(tx + pad, yy, small_sz, kTextDim, status);
    }
    yy += 17;

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

void atlas_ui_update(const Collection& c, const Inventories& inv) {
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
        auto add_items = [&](const std::vector<Item>& items) {
            for (const Item& it : items) {
                const int rarity = clamp_rarity(it.rarity);
                for (int cat : {4, 5}) {   // trinkets, weapons
                    auto f = g_entry_by_id[cat].find(it.kind);
                    if (f == g_entry_by_id[cat].end()) continue;
                    Owned& o = snap->byId[cat][it.kind];
                    if (it.level > o.level) o.level = it.level;
                    if (rarity > o.rarity) o.rarity = rarity;
                    o.count += it.count;
                }
            }
        };
        add_items(inv.bank);
        add_items(inv.bank_equipment);
        add_items(inv.equipped);
        add_items(inv.bags);
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

    // A collection unlock has no level; an inventory item does. Counts per
    // category only count ids that actually exist in the database.
    for (int cat = 0; cat < kCats; cat++) {
        int n = 0;
        for (const auto& kv : snap->byId[cat])
            if (g_entry_by_id[cat].count(kv.first)) n++;
        snap->owned_count[cat] = n;
    }

    EnterCriticalSection(&g_own_cs);
    g_own = snap;
    LeaveCriticalSection(&g_own_cs);
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
    float tab_x = wx + 6;
    const float tab_y = wy + kTitleH + 2;
    for (int c = 0; c < kCats; c++) {
        char label[64];
        const int total = g_cat_begin[c + 1] - g_cat_begin[c];
        if (own)
            sprintf_s(label, "%s %d/%d", kCatNames[c], own->owned_count[c], total);
        else
            sprintf_s(label, "%s %d", kCatNames[c], total);
        const float tw = measure_text(13, label) + 18;
        const bool hot = in.mouse_x >= tab_x && in.mouse_x < tab_x + tw &&
                         in.mouse_y >= tab_y && in.mouse_y < tab_y + kTabsH - 6;
        draw_rect(tab_x, tab_y, tw, kTabsH - 6,
                  c == tab ? kTabOn : (hot ? Color{0.12f, 0.15f, 0.22f, 1.0f}
                                           : kTabOff));
        if (c == tab) draw_rect(tab_x, tab_y + kTabsH - 8, tw, 2, kAccent);
        draw_text(tab_x + 9, tab_y + 4, 13, c == tab ? kText : kTextDim, label);
        if (clicked && in.click_x >= tab_x && in.click_x < tab_x + tw &&
            in.click_y >= tab_y && in.click_y < tab_y + kTabsH - 6) {
            tab = c;
            InterlockedExchange(&g_tab, c);
            save_layout_dirty();
        }
        tab_x += tw + 4;
    }

    // --- grid ---------------------------------------------------------------
    const float content_x = wx + kPad;
    const float content_y = wy + kTitleH + kTabsH + 6;
    const float content_w = win_w - 2 * kPad - 10;   // room for scrollbar
    const float content_h = win_h - (kTitleH + kTabsH + 6) - kPad;
    const int cols = content_w >= kStride ? (int)((content_w + kGap) / kStride) : 1;

    const int first = g_cat_begin[tab], last = g_cat_begin[tab + 1];
    const int count = last - first;
    const int rows = (count + cols - 1) / cols;
    const float total_h = rows * kStride;
    float max_scroll = total_h - content_h;
    if (max_scroll < 0) max_scroll = 0;

    float& scroll = g_scroll[tab];
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
            const int idx = first + r * cols + col;
            if (idx >= last) break;
            const Entry& e = g_entries[idx];
            const float x = content_x + col * kStride;
            const float y = content_y + r * kStride - scroll;
            if (y + kIcon < content_y || y > content_y + content_h) continue;

            const Owned* owned = nullptr;
            if (own) {
                auto f = own->byId[tab].find(e.id);
                if (f != own->byId[tab].end()) owned = &f->second;
            }

            // Cell background, then the icon clipped to the content band.
            const float clip0 = content_y, clip1 = content_y + content_h;
            float by0 = y < clip0 ? clip0 : y;
            float by1 = (y + kIcon) > clip1 ? clip1 : y + kIcon;
            if (by1 > by0) draw_rect(x, by0, kIcon, by1 - by0, kCellBg);
            draw_icon_clipped(e, x, y, kIcon, clip0, clip1,
                              owned ? kOwnTint : kMissTint);

            // Rarity border for owned, faint frame for missing - only when
            // the full cell is inside the band (partial borders look broken).
            if (y >= clip0 && y + kIcon <= clip1) {
                if (owned) {
                    const int rar = owned->rarity >= 0 ? owned->rarity : e.rarity;
                    draw_rect_outline(x, y, kIcon, kIcon, 2, kRarity[rar]);
                } else {
                    draw_rect_outline(x, y, kIcon, kIcon, 1, kMissEdge);
                }
            }

            if (mouse_in_content && in.mouse_x >= x && in.mouse_x < x + kIcon &&
                in.mouse_y >= by0 && in.mouse_y < by1) {
                hover.valid = true;
                hover.entry = idx;
                hover.cell_x = x;
                hover.cell_y = y;
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
        if (hover.cell_y >= content_y &&
            hover.cell_y + kIcon <= content_y + content_h)
            draw_rect_outline(hover.cell_x, hover.cell_y, kIcon, kIcon, 2,
                              {1.0f, 1.0f, 1.0f, 0.85f});
        const Owned* owned = nullptr;
        if (own) {
            auto f = own->byId[tab].find(e.id);
            if (f != own->byId[tab].end()) owned = &f->second;
        }
        draw_tooltip(e, owned, (float)in.mouse_x, (float)in.mouse_y,
                     screen_w, screen_h);
    }

}

}  // namespace fmk
