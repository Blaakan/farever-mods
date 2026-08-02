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
#include "players.h"
#include "routes.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

// --- static item database (immutable once g_loaded is set) ------------------

constexpr int kCats = 13;
const char* kCatNames[kCats] = {"Appearances", "Mounts", "Pets", "Gliders",
                                "Trinkets", "Weapons", "Consumables",
                                "Materials", "Recipes", "Augments", "Misc",
                                "Creatures", "Runes"};
const char* kCatTsv[kCats] = {"appearances", "mounts", "pets", "gliders",
                              "trinkets", "weapons", "consumables",
                              "materials", "recipes", "augments", "misc",
                              "creatures", "runes"};

// Three kinds of page, in this order: account-wide collection unlocks
// (0..3), real items owned only while they sit in a bank, bag or equipment
// slot (4..10), and the bestiary, whose progress comes from the codex
// rather than from anything you carry (11).
constexpr int kFirstItemCat = 4;
constexpr int kWeaponsCat = 5;
constexpr int kRecipesCat = 8;
constexpr int kCreaturesCat = 11;
// Runes are neither carried nor unlocked account-wide: like a craft, a
// character learns one permanently and another character has not. They sit
// past the bestiary so every range above keeps its meaning.
constexpr int kRunesCat = 12;

// Three more tabs than there are categories. None of them is a collection of
// anything the account owns, so they have no entries, no ownership and no
// grid - the tab strip is simply the window's navigation, and each page
// draws itself into the same content band.
//
// Mastery re-reads the weapons page rather than owning entries of its own: a
// weapon's mastery track belongs to the weapon, and what changes per
// character is only how far along it is.
constexpr int kMasteryTab = kCats;
constexpr int kRoutesTab = kCats + 1;
constexpr int kPlayersTab = kCats + 2;
constexpr int kTabs = kCats + 3;

struct Entry {
    std::string id, name, desc;
    std::string search;               // name + id, lowercased, for matching
    std::vector<std::string> acquire;
    std::vector<std::string> tags;    // "slot:Chest", "class:Mage", "area:Z1"
    std::vector<NavTarget> targets;   // tracker destinations, often empty
    // Parallel to `targets`: the id of the one-time thing each one is, or
    // empty when it is repeatable. A source already spent is dropped from
    // the list rather than sent you to twice.
    std::vector<std::string> target_once;
    int rarity = 0;      // 0..4 = common..legendary, from the CastleDB
    int icon = -1;       // cell in the icon atlas, -1 = none
    int cat = 0;
    // Weapons only, from the `mastery:<levels>/<kills>` tag: how many mastery
    // levels this weapon has in total, and how many kills each one costs.
    // Zero levels is a real answer - a capture net has no track - and a zero
    // cost means the generator could not work it out.
    int mastery_levels = 0;
    int mastery_per_level = 0;
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
    // Warrior / Rogue / Mage / Priest, or empty when the reader could not
    // make it out. Only the Mastery page uses it, and only to decide which
    // weapons to list.
    std::string hero_class;
    // Weapon mastery for the character logged in: weapon item id -> the
    // kills made with it. Absent means none, which is what the game means
    // by leaving a weapon out of its own map.
    std::unordered_map<std::string, int> weapon_kills;
    // What is in the main hand right now, which is what the on-screen bar
    // tracks. Empty for an empty hand.
    std::string active_weapon;
    // craft id -> the characters who know it. Recipes are learned per
    // character, so "known" is a question of by whom.
    std::unordered_map<std::string, std::set<std::string>> learned_by;
    std::unordered_map<std::string, int> job_level;
    // One-time sources this character has already spent, by the game's own
    // element and activity ids.
    std::set<std::string> done_sources;
};

// The targets of an entry worth still walking to: everything repeatable,
// plus the one-time sources not yet spent. Returned by value because the
// caller hands the result to the navigator, which copies it anyway.
//
// A source that ran out is not "somewhere you could go" - it is somewhere
// you already went, and leaving it on the list sends you across the map for
// a chest you opened last week.
// Three kinds of target, and the difference decides what is safe to walk to:
//
//   no id      repeatable - a vendor, a spawn cluster, a world crate. Always
//              good.
//   plain id   a chest or an activity, and the game records whether this
//              character finished it. Verifiable, so drop it once spent.
//   `q:` id    a quest. Nothing in the save records a quest being handed in -
//              checked every map on Progress and HeroData - so its state is
//              unknowable, and a waypoint to one is a coin flip.
//
// A coin flip is worse than nothing when there is a sure thing available, so
// unverifiable targets are used only when nothing else survives. That is the
// difference between the arrow sending you to a quest you finished last week
// and sending you to a chest that is definitely still there.
std::vector<NavTarget> live_targets(const Entry& e, const OwnSnap* snap) {
    if (e.target_once.empty()) return e.targets;

    std::vector<NavTarget> sure, unsure;
    for (size_t i = 0; i < e.targets.size(); i++) {
        const std::string& once =
            i < e.target_once.size() ? e.target_once[i] : std::string();
        if (once.compare(0, 2, "q:") == 0) {
            unsure.push_back(e.targets[i]);
            continue;
        }
        if (!once.empty() && snap && snap->done_sources.count(once)) continue;
        sure.push_back(e.targets[i]);
    }
    if (!sure.empty()) return sure;
    // Nothing checkable left. A quest is *not* an acceptable fallback: half
    // the time it sends you across the map to an NPC you already handed it
    // to, and being sent somewhere wrong is worse than being sent nowhere.
    // The quests are still named in the tooltip, where you can judge for
    // yourself which you have done.
    (void)unsure;
    return {};
}

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

// Declared with the flag it sets rather than beside its one-line body, so
// everything that moves a persisted thing can reach it.
void save_layout_dirty() { InterlockedExchange(&g_layout_dirty, 1); }

// Written by the render thread, persisted by the worker in atlas_ui_tick.
volatile LONG g_win_x = 140, g_win_y = 110;
volatile LONG g_tab = 0;

// Where the mastery bar sits. `kUnplaced` means "never moved", so the default
// position can change with the screen instead of being frozen at whatever the
// first frame was.
//
// Which weapon it shows is not stored at all: it is whatever is in your main
// hand, which the game already knows and you already decided by equipping it.
constexpr LONG kUnplaced = -100000;
volatile LONG g_hud_x = kUnplaced, g_hud_y = kUnplaced;

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
        // The one failure a new install actually hits, so it names the fix
        // rather than the tool: install.cmd runs the generator, and somebody
        // who unzipped a release has never heard of gen-atlas.mjs.
        host_log("atlas_ui: %ls missing - the item database was never built. "
                 "Run install.cmd (or node tools/gen-atlas.mjs).", path.c_str());
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
            const std::string track = tag_value(e, "mastery:");
            if (!track.empty()) {
                int levels = 0, per = 0;
                if (sscanf_s(track.c_str(), "%d/%d", &levels, &per) == 2 &&
                    levels >= 0 && levels <= 64 && per > 0) {
                    e.mastery_levels = levels;
                    e.mastery_per_level = per;
                }
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
                             &t.x, &t.y, &t.z) == 3) {
                    // A fourth field, when present, is the one-time source's
                    // own id: `label@x,y,z,NPC_Ratou`.
                    std::string once;
                    const std::string nums = one.substr(at + 1);
                    size_t c = 0, commas = 0;
                    for (; c < nums.size() && commas < 3; c++)
                        if (nums[c] == ',') commas++;
                    if (commas == 3 && c < nums.size()) once = nums.substr(c);
                    e.targets.push_back(t);
                    e.target_once.push_back(std::move(once));
                }
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
    const std::vector<NavTarget> live = live_targets(e, snap);
    const int spent = (int)e.targets.size() - (int)live.size();
    if (!live.empty())
        nav_format_distance(live.data(), (int)live.size(),
                            track_dist, sizeof(track_dist));

    float th = pad + 20 /*name*/ + 17 /*status line*/;
    th += copy_lines.size() * 16.0f;
    if (!desc_lines.empty()) th += 6 + desc_lines.size() * 16.0f;
    if (!acq_lines.empty()) th += 6 + 16 /*header*/ + acq_lines.size() * 16.0f;
    // Two hint lines - track, and add-to-route - plus the live distance and,
    // when any one-time source is spent, a line saying so.
    if (!e.targets.empty())
        th += 6 + 16 + 16 + (track_dist[0] ? 16.0f : 0) + (spent ? 16.0f : 0);
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
        // Known by whom is the whole question for a craft.
        std::string who;
        if (snap) {
            auto f = snap->learned_by.find(e.id);
            if (f != snap->learned_by.end()) {
                for (const auto& name : f->second) {
                    if (!who.empty()) who += ", ";
                    who += name;
                }
            }
        }
        // Saying which job is missing is more use than "not learned" when
        // the character has never taken the job at all.
        const std::string job = tag_value(e, "job:");
        const bool has_job = snap && !job.empty() &&
                             snap->job_level.count(job) != 0;
        if (!who.empty())
            _snprintf_s(status, sizeof(status), _TRUNCATE, "Known by %s",
                        who.c_str());
        else if (!has_job && !job.empty())
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "No character has taken up %s", job.c_str());
        else
            _snprintf_s(status, sizeof(status), _TRUNCATE, "Not learned yet");
        draw_text(tx + pad, yy, small_sz,
                  who.empty() ? kTextDim : Color{0.45f, 0.85f, 0.45f, 1.0f},
                  status);
    } else if (e.cat == kRunesCat) {
        // A rune is one-use and one-pickup, so "who has it" is the question -
        // and a rune learned by your Mage is gone for your Priest.
        std::string who;
        if (snap) {
            auto f = snap->learned_by.find(e.id);
            if (f != snap->learned_by.end()) {
                for (const auto& name : f->second) {
                    if (!who.empty()) who += ", ";
                    who += name;
                }
            }
        }
        if (!who.empty())
            _snprintf_s(status, sizeof(status), _TRUNCATE, "Learned by %s",
                        who.c_str());
        else
            _snprintf_s(status, sizeof(status), _TRUNCATE, "Not learned yet");
        draw_text(tx + pad, yy, small_sz,
                  who.empty() ? kTextDim : Color{0.45f, 0.85f, 0.45f, 1.0f},
                  status);
    } else if (e.cat == kCreaturesCat) {
        // The bestiary counts encounters, not possessions.
        if (owned)
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "Encountered - %d killed, codex rank %d",
                        owned->total, owned->max_level);
        else
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "Not yet encountered");
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
        // Only offer what a click would actually do. With every source spent
        // or unverifiable, "Click to track this location" describes a click
        // that goes nowhere - and a tooltip that promises three actions and
        // performs none reads as the mod being broken rather than as the
        // sources being gone. Stopping is still offered, because stopping
        // still works.
        if (tracked)
            draw_text(tx + pad, yy, small_sz, {1.0f, 0.75f, 0.35f, 1.0f},
                      "Tracking - click to stop");
        else if (!live.empty())
            draw_text(tx + pad, yy, small_sz, {0.55f, 0.60f, 0.70f, 1.0f},
                      "Click to track this location");
        yy += 16;
        if (!live.empty())
            draw_text(tx + pad, yy, small_sz, {0.45f, 0.50f, 0.60f, 1.0f},
                      "Ctrl+click: add to route   Shift: add first");
        yy += 16;
        if (spent) {
            // Naming what was dropped matters: silently showing fewer places
            // than yesterday would read as the atlas losing data.
            int quests = 0, done_here = 0;
            for (size_t i = 0; i < e.target_once.size(); i++) {
                const std::string& o = e.target_once[i];
                if (o.compare(0, 2, "q:") == 0) quests++;
                else if (!o.empty() && snap && snap->done_sources.count(o))
                    done_here++;
            }
            char line[160];
            if (done_here)
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "%d source%s already done on this character - "
                            "hidden", done_here, done_here == 1 ? "" : "s");
            else
                // Not hidden for being done - the game does not record that
                // for quests, so they are listed above but never walked to.
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "%d quest source%s listed above - not tracked, "
                            "the game does not record which you finished",
                            quests, quests == 1 ? "" : "s");
            draw_text(tx + pad, yy, small_sz, {0.55f, 0.60f, 0.45f, 1.0f},
                      line);
            yy += 16;
        }
    }
    yy += 4;
    draw_text(tx + pad, yy, 11, kTextFaint, e.id.c_str());
}

// --- the Mastery page -------------------------------------------------------
//
// Every weapon in the game levels separately, and levelling one is killing
// things with it. The game's own screen shows you the weapon in your hands,
// one level at a time; this shows the whole track for every weapon the
// character could be carrying, so "which of these have I actually invested
// in" is one glance rather than a tour of the inventory.
//
// One bar per weapon, one notch per level. The bar is the entire track, not
// the current level, which is the difference between "Level 3, 40%" and
// seeing that three of eight are done.

struct MasteryRow {
    const Entry* e = nullptr;
    int levels = 0;      // notches on the bar
    int per = 0;         // kills each notch costs
    int kills = 0;
    int points = 0;      // notches filled
    float frac = 0;      // how far into the next one, 0..1
    bool owned = false;
};

// Which weapons this character can pick up at all. The tag is the game's
// own rule: a weapon lists the aptitudes that may wield it, and each of the
// four classes carries exactly one. `class:Any` is a weapon that lists none.
bool wieldable_by(const Entry& e, const std::string& hero_class) {
    if (hero_class.empty()) return true;    // class unknown: show the lot
    bool saw_class = false;
    for (const auto& t : e.tags) {
        if (t.compare(0, 6, "class:") != 0) continue;
        saw_class = true;
        if (t.compare(6, std::string::npos, "Any") == 0) return true;
        if (t.compare(6, std::string::npos, hero_class) == 0) return true;
    }
    return !saw_class;
}

MasteryRow mastery_row_for(const Entry& e, const OwnSnap* snap) {
    MasteryRow r;
    r.e = &e;
    r.levels = e.mastery_levels;
    r.per = e.mastery_per_level;
    if (snap) {
        auto k = snap->weapon_kills.find(e.id);
        if (k != snap->weapon_kills.end()) r.kills = k->second;
        r.owned = snap->byId[kWeaponsCat].count(e.id) != 0;
    }
    // The game's own arithmetic (st/player/Progress.hx): kills divided by the
    // weapon's cost per level, floored, and capped at the length of the
    // track. Past the cap the counter keeps rising and the bar does not,
    // which is exactly what the game does with it.
    if (r.per > 0) {
        const double v = (double)r.kills / (double)r.per;
        r.points = (int)v;
        if (r.points >= r.levels) {
            r.points = r.levels;
            r.frac = 0;
        } else {
            r.frac = (float)(v - (double)r.points);
        }
    }
    return r;
}

std::vector<MasteryRow> mastery_rows(const OwnSnap* snap) {
    std::vector<MasteryRow> rows;
    const std::string cls = snap ? snap->hero_class : std::string();
    for (int i = g_cat_begin[kWeaponsCat]; i < g_cat_begin[kWeaponsCat + 1]; i++) {
        const Entry& e = g_entries[i];
        if (!wieldable_by(e, cls)) continue;
        rows.push_back(mastery_row_for(e, snap));
    }
    return rows;
}

// The one weapon the on-screen bar is about. Separate from the list above
// because the bar draws every frame during ordinary play, with the atlas
// closed - building all 37 rows and a vector to find one of them is work the
// render thread should not be doing inside Present.
//
// The id comes from the main hand, so an id that is not on the weapons page
// is a tool or something else that cannot be mastered, and gets no bar.
bool mastery_row_by_id(const std::string& id, const OwnSnap* snap,
                       MasteryRow* out) {
    auto f = g_entry_by_id[kWeaponsCat].find(id);
    if (f == g_entry_by_id[kWeaponsCat].end()) return false;
    const Entry& e = g_entries[f->second];
    if (!wieldable_by(e, snap ? snap->hero_class : std::string())) return false;
    *out = mastery_row_for(e, snap);
    return true;
}

// The bar: one segment per level, filled left to right, with the level in
// progress part-filled. Gold once the whole track is done.
//
// This is the point of the page. A per-level bar answers "how close am I to
// the next one" and hides the only question worth asking of a weapon you
// have not touched in a week, which is how much of it is left.
void draw_mastery_bar(const MasteryRow& r, float x, float y, float w, float h) {
    const Color empty{0.09f, 0.10f, 0.14f, 1.0f};
    if (r.levels <= 0) {
        draw_rect(x, y, w, h, empty);
        draw_text(x + 8, y + (h - 11) * 0.5f, 11, kTextFaint,
                  r.per > 0 ? "no mastery track" : "track unknown");
        return;
    }

    const bool done = r.points >= r.levels;
    const Color fill = done ? kRarity[4] : kAccent;
    const Color part{fill.r, fill.g, fill.b, 0.45f};

    // Notches are gaps in the fill rather than lines drawn over it, so a
    // level that is done reads as a solid block. Below about three pixels a
    // segment stops reading as one, so a very long track drops the gaps
    // rather than the segments.
    const float gap = r.levels > 1 ? 2.0f : 0.0f;
    const float probe = (w - gap * (r.levels - 1)) / (float)r.levels;
    const float use_gap = probe >= 3.0f ? gap : 0.0f;
    const float seg = (w - use_gap * (r.levels - 1)) / (float)r.levels;

    for (int i = 0; i < r.levels; i++) {
        const float sx = x + i * (seg + use_gap);
        draw_rect(sx, y, seg, h, empty);
        if (i < r.points) draw_rect(sx, y, seg, h, fill);
        else if (i == r.points && r.frac > 0)
            draw_rect(sx, y, seg * r.frac, h, part);
    }
    const Color edge = done ? Color{fill.r, fill.g, fill.b, 0.5f} : kMissEdge;
    draw_rect(x, y, w, 1, edge);
    draw_rect(x, y + h - 1, w, 1, edge);
}

// Trims to fit, on a character boundary, and says it trimmed.
std::string fit_text(std::string s, float size, float max_w) {
    if (measure_text(size, s.c_str()) <= max_w) return s;
    while (!s.empty() && measure_text(size, (s + "...").c_str()) > max_w) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    }
    return s + "...";
}

// --- the bar, on screen over the world --------------------------------------
//
// The track of the weapon in your main hand, and nothing else. There is no
// setting: equipping a weapon is already the act of choosing it, so asking a
// second time would only be a way to get it wrong. It goes away on its own
// when that weapon has nothing left to earn - a full bar tells you nothing
// you did not know a moment after it filled.
//
// Sized to sit alongside the game's own XP bar rather than next to it looking
// like a different game. Those numbers are the game's, not a guess:
// `UI/Style/style.css` in res.light.pak says
// `exp-bar { bar-width: 619; bar-height: 17 }`, and the UI scene those units
// belong to is 2048x1152 - which the map diagnostics confirm live, reporting
// `scene=2048x1152 frame=2560x1440`. So the scene is a fixed design size the
// game letterboxes onto the frame, and matching it means scaling by the same
// factor rather than pinning pixel sizes that would only be right on one
// monitor.
constexpr float kUiSceneW = 2048.0f;
constexpr float kUiSceneH = 1152.0f;
constexpr float kXpBarW = 619.0f;
constexpr float kXpBarH = 17.0f;

bool g_hud_dragging = false;
float g_hud_drag_dx = 0, g_hud_drag_dy = 0;

void mastery_hud_draw(const InputState& in, bool clicked, float screen_w,
                      float screen_h, const OwnSnap* snap) {
    MasteryRow found;
    const bool have = snap && !snap->active_weapon.empty() &&
                      mastery_row_by_id(snap->active_weapon, snap, &found);
    // Nothing in hand, something in hand with no track at all, or a track
    // already full: in every one of those there is no progress left to watch,
    // so the bar is not there rather than sitting at 100% forever.
    if (!have || found.levels <= 0 || found.points >= found.levels) {
        input_set_aux_rect(2, 0, 0, 0, 0);
        g_hud_dragging = false;
        return;
    }
    const MasteryRow* row = &found;

    const float scale = (std::min)(screen_w / kUiSceneW, screen_h / kUiSceneH);
    const float bar_w = kXpBarW * scale;
    const float bar_h = kXpBarH * scale;
    const float icon = 26 * scale;
    const float gap = 8 * scale;
    const float text_sz = 13 * scale;
    const float pad = 6 * scale;

    const float w = icon + gap + bar_w;
    const float h = text_sz + 3 * scale + bar_h;

    // Default: centred, sitting above where the game keeps its own hero bar,
    // so the first sight of it is next to the thing it is modelled on rather
    // than on top of it.
    const LONG sx = InterlockedCompareExchange(&g_hud_x, 0, 0);
    const LONG sy = InterlockedCompareExchange(&g_hud_y, 0, 0);
    float x = (sx == kUnplaced) ? (screen_w - w) * 0.5f : (float)sx;
    float y = (sy == kUnplaced) ? screen_h - 190 * scale : (float)sy;

    // Movable only while the atlas is open - the same rule the waypoint pill
    // follows, and for the same reason: over the world this must never eat a
    // click. The panel behind it is the cue that it can be moved right now.
    const bool movable = in.visible;
    if (movable) {
        const bool hit = clicked &&
                         in.click_x >= x - pad && in.click_x < x + w + pad &&
                         in.click_y >= y - pad && in.click_y < y + h + pad &&
                         !input_in_main_rect(in.click_x, in.click_y);
        if (hit) {
            g_hud_dragging = true;
            g_hud_drag_dx = in.click_x - x;
            g_hud_drag_dy = in.click_y - y;
        }
        if (g_hud_dragging) {
            if (in.lbutton) {
                x = in.mouse_x - g_hud_drag_dx;
                y = in.mouse_y - g_hud_drag_dy;
            } else {
                g_hud_dragging = false;
                save_layout_dirty();
            }
        }
    } else {
        g_hud_dragging = false;
    }

    if (x < pad) x = pad;
    if (y < pad) y = pad;
    if (x > screen_w - w - pad) x = screen_w - w - pad;
    if (y > screen_h - h - pad) y = screen_h - h - pad;
    InterlockedExchange(&g_hud_x, (LONG)x);
    InterlockedExchange(&g_hud_y, (LONG)y);
    input_set_aux_rect(2, movable ? (int)(x - pad) : 0,
                       movable ? (int)(y - pad) : 0,
                       movable ? (int)(w + 2 * pad) : 0,
                       movable ? (int)(h + 2 * pad) : 0);

    if (movable) {
        draw_rect(x - pad, y - pad, w + 2 * pad, h + 2 * pad,
                  {0.05f, 0.06f, 0.09f, 0.80f});
        draw_rect_outline(x - pad, y - pad, w + 2 * pad, h + 2 * pad, 1.0f,
                          {0.35f, 0.75f, 1.0f, 0.8f});
    }

    // Name on the left, the numbers on the right, the way the game labels its
    // own bars - then the track underneath, full width.
    const float bar_x = x + icon + gap;
    draw_text(bar_x, y, text_sz, {0.88f, 0.91f, 0.96f, 1.0f},
              fit_text(row->e->name, text_sz, bar_w * 0.6f).c_str());

    // Only ever a partly-filled track gets here, so there is always a next
    // level and always a number of kills to it.
    char right[96];
    _snprintf_s(right, sizeof(right), _TRUNCATE, "%d/%d  -  %d to next",
                row->points, row->levels, row->per - (row->kills % row->per));
    const float rw = measure_text(text_sz, right);
    draw_text(bar_x + bar_w - rw, y, text_sz, {0.62f, 0.68f, 0.78f, 1.0f},
              right);

    const float by = y + text_sz + 3 * scale;
    draw_icon_clipped(*row->e, x, by + (bar_h - icon) * 0.5f, icon,
                      by - icon, by + bar_h + icon, kOwnTint);
    draw_mastery_bar(*row, bar_x, by, bar_w, bar_h);
}

float g_mastery_scroll = 0;

void mastery_draw(const InputState& in, float x, float y, float w, float h,
                  const OwnSnap* snap) {
    constexpr float kRowH = 34;
    constexpr float kRowIcon = 26;
    constexpr float kNameW = 250;
    constexpr float kReadW = 150;

    char line[224];
    if (!snap) {
        draw_text(x, y + 4, 13, kTextDim,
                  "Waiting for the reader - mastery appears once a character "
                  "is in the world.");
        return;
    }

    const std::vector<MasteryRow> rows = mastery_rows(snap);

    int total_points = 0, total_levels = 0, started = 0;
    for (const auto& r : rows) {
        total_points += r.points;
        total_levels += r.levels;
        if (r.kills > 0) started++;
    }

    // --- header -------------------------------------------------------------
    if (snap->hero_class.empty()) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "%s - every weapon in the game (the class did not read "
                    "back, so nothing is filtered out)",
                    snap->character.c_str());
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "%s the %s - %d weapon%s this class can equip, %d started",
                    snap->character.c_str(), snap->hero_class.c_str(),
                    (int)rows.size(), rows.size() == 1 ? "" : "s", started);
    }
    draw_text(x, y + 3, 13, kText, line);

    _snprintf_s(line, sizeof(line), _TRUNCATE, "%d / %d levels",
                total_points, total_levels);
    float lw = measure_text(13, line);
    draw_text(x + w - lw, y + 3, 13,
              total_levels > 0 && total_points >= total_levels ? kRarity[4]
                                                               : kAccent,
              line);

    draw_text(x, y + 21, 11, kTextFaint,
              "One notch is one mastery level, and a level is kills made with "
              "that weapon. The weapon in your hand gets its own bar on "
              "screen - drag it while this window is open.");

    const float list_y = y + 40;
    const float list_h = h - 40;
    if (list_h < kRowH) return;

    if (rows.empty()) {
        draw_text(x, list_y + 6, 13, kTextDim,
                  "No weapons on this page - the atlas database has none this "
                  "character can equip.");
        return;
    }

    // --- the list -----------------------------------------------------------
    //
    // Whole rows only, the way the Routes page does it: the overlay cannot
    // clip text, so a half-drawn row would paint over the header above or
    // past the window's bottom edge.
    const int per_page = (int)(list_h / kRowH);
    const int max_top = (int)rows.size() - per_page;
    const float max_scroll = max_top > 0 ? max_top * kRowH : 0;
    g_mastery_scroll -= in.wheel * kRowH * 2;
    if (g_mastery_scroll > max_scroll) g_mastery_scroll = max_scroll;
    if (g_mastery_scroll < 0) g_mastery_scroll = 0;

    const float bar_x = x + kRowIcon + 8 + kNameW + 8;
    const float bar_w = w - (kRowIcon + 8 + kNameW + 8) - kReadW - 8;

    const int first = (int)(g_mastery_scroll / kRowH);
    for (int i = first; i < (int)rows.size(); i++) {
        const MasteryRow& r = rows[i];
        const float ry = list_y + (i - first) * kRowH;
        if (ry + kRowH > list_y + list_h) break;

        const bool hot = in.mouse_x >= x && in.mouse_x < x + w &&
                         in.mouse_y >= ry && in.mouse_y < ry + kRowH - 2;
        // The one in your hand, which is the one the on-screen bar is
        // following. The same left-edge stripe the Routes page uses for the
        // route being run, because it answers the same question: which of
        // these is live right now.
        const bool equipped = !snap->active_weapon.empty() &&
                              r.e->id == snap->active_weapon;
        if (equipped) draw_rect(x, ry, w, kRowH - 2, {0.13f, 0.22f, 0.32f, 1.0f});
        else if (hot) draw_rect(x, ry, w, kRowH - 2, {0.12f, 0.14f, 0.20f, 1.0f});
        if (equipped) draw_rect(x, ry, 3, kRowH - 2, kAccent);

        // A weapon can be mastered and not owned - sold, or left on another
        // character - so the icon dims for "not in the vault", never for
        // "no progress". The bar already says that.
        draw_icon_clipped(*r.e, x, ry + (kRowH - 2 - kRowIcon) * 0.5f, kRowIcon,
                          ry, ry + kRowH, r.owned ? kOwnTint : kMissTint);

        draw_text(x + kRowIcon + 8, ry + 8, 13,
                  r.owned ? kRarity[r.e->rarity] : kTextDim,
                  fit_text(r.e->name, 13, kNameW - 6).c_str());

        if (bar_w > 40) draw_mastery_bar(r, bar_x, ry + 8, bar_w, 15);

        if (r.levels <= 0) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "-");
        } else if (r.points < r.levels) {
            // What the next notch costs, which is the number that decides
            // whether it is worth another few minutes with this weapon.
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%d/%d  -  %d to next",
                        r.points, r.levels, r.per - (r.kills % r.per));
        } else {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%d/%d  -  %d kills",
                        r.points, r.levels, r.kills);
        }
        lw = measure_text(12, line);
        draw_text(x + w - lw, ry + 9, 12,
                  r.levels > 0 && r.points >= r.levels ? kRarity[4] : kTextDim,
                  line);
    }

    if (max_scroll > 0) {
        const float track_x = x + w + 4;
        const float total_h = rows.size() * kRowH;
        draw_rect(track_x, list_y, 6, list_h, {0.10f, 0.11f, 0.16f, 1.0f});
        const float thumb_h = list_h * (list_h / total_h);
        const float thumb_y =
            list_y + (list_h - thumb_h) * (g_mastery_scroll / max_scroll);
        draw_rect(track_x, thumb_y, 6, thumb_h, {0.30f, 0.38f, 0.52f, 1.0f});
    }
}

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
    g_tab = (tab >= 0 && tab < kTabs) ? tab : 0;

    // Where the on-screen mastery bar was left.
    g_hud_x = GetPrivateProfileIntW(L"mastery", L"x", kUnplaced,
                                    g_ini_path.c_str());
    g_hud_y = GetPrivateProfileIntW(L"mastery", L"y", kUnplaced,
                                    g_ini_path.c_str());
    // Pinning is gone: which weapon the bar shows is decided by what is in
    // your hand, so there is nothing left to remember but where it sits.
    // Anything an older build wrote under this section is cleared on the
    // next save rather than left to mean nothing.

    g_ready_tick = GetTickCount();
    InterlockedExchange(&g_loaded, 1);
    host_log("atlas_ui: ready (F8 toggles)");
    return true;
}

// Recipes are per-character, so the ones a character knows are written out
// the way inventories are - that is how the atlas can say "known by Emsei"
// while you are playing someone else.
void write_jobs_json(const std::vector<JobState>& jobs,
                     const RuneState& runes, const std::string& character) {
    if ((jobs.empty() && runes.learned.empty()) || character.empty()) return;
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
    // Runes ride in the same file for the same reason crafts do: they are
    // learned per character, so the atlas can only say "learned by Emsay"
    // about a character who is not logged in if it wrote that down.
    out += "\n  ],\n  \"runes\":[";
    for (size_t i = 0; i < runes.learned.size(); i++) {
        if (i) out += ",";
        out += "\"" + runes.learned[i] + "\"";
    }
    out += "]\n}\n";

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
    // Which jobs this character has at all, so the tooltip can tell "nobody
    // has taken that job up" from "taken up, but this one is not learned".
    size_t jp = 0;
    while ((jp = text.find("{\"job\":\"", jp)) != std::string::npos) {
        jp += 8;
        const size_t end = text.find('"', jp);
        if (end == std::string::npos) break;
        const std::string job = text.substr(jp, end - jp);
        int level = 1;
        const size_t lv = text.find("\"level\":", end);
        if (lv != std::string::npos && lv < end + 24)
            level = atoi(text.c_str() + lv + 8);
        if (!job.empty()) {
            int& known = snap->job_level[job];
            if (level > known) known = level;
        }
        jp = end;
    }

    // Crafts and runes are both "this character learned this", read into the
    // same map from the two arrays that carry them.
    for (const char* key : {"\"learned\":[", "\"runes\":["}) {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            pos += strlen(key);
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
}

void atlas_ui_update(const Collection& c, const Inventories& inv,
                     const std::vector<UnitProgress>& unit_progress,
                     const std::vector<JobState>& jobs,
                     const RuneState& runes,
                     const CompletionState& done,
                     const std::vector<WeaponMastery>& mastery) {
    if (!InterlockedCompareExchange(&g_loaded, 0, 0)) return;

    auto snap = std::make_shared<OwnSnap>();
    snap->character = inv.valid ? inv.character : "";
    snap->hero_class = inv.valid ? inv.hero_class : "";
    snap->active_weapon = inv.valid ? inv.active_weapon : "";

    // Weapon mastery is per character and never merged across them: the
    // kills your Warrior made with a sword are not your Priest's, and the
    // page says which character it is describing.
    for (const auto& wm : mastery) snap->weapon_kills[wm.weapon] = wm.kills;

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
    // Runes go into the same map: both answer "which character learned this".
    for (const auto& rune : runes.learned)
        snap->learned_by[rune].insert(snap->character);

    // One-time sources this character has spent. Deliberately *not* merged
    // across characters the way learned crafts are: a chest your Priest
    // opened is still waiting for your Mage, so pooling them would hide
    // places that are genuinely still there.
    for (const auto& id : done.done) snap->done_sources.insert(id);
    write_jobs_json(jobs, runes, snap->character);
    {
        const std::string skip = sanitize_name(snap->character);
        std::wstring dir = exe_dir();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"farever-jobs-*.json").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string text;
                if (!read_file(dir + fd.cFileName, &text)) continue;
                // farever-jobs-<name>.json. The name is ASCII by
                // construction - sanitize_name wrote it - so a wide
                // character here is a hand-placed file, and skipping it
                // beats truncating it into some other character's name.
                std::string who;
                bool ascii = true;
                for (const wchar_t* p = fd.cFileName; *p && ascii; p++) {
                    if (*p < 32 || *p > 126) ascii = false;
                    else who.push_back((char)*p);
                }
                if (!ascii) continue;
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
    for (const auto& up : unit_progress) {
        if (!g_entry_by_id[kCreaturesCat].count(up.unit)) continue;
        Owned& o = snap->byId[kCreaturesCat][up.unit];
        o.unlocked = true;
        o.total = up.kills;
        o.max_level = up.rank;      // the codex rank, reused for display
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
    // A recipe entry IS the craft: its id is the produced item, which is
    // exactly what the game records as learned.
    // A rune is learned exactly the way a craft is - permanently, by one
    // character - so the same map answers both, and a mastery id can never
    // collide with a produced-item id.
    for (int cat : {kRecipesCat, kRunesCat}) {
        for (int i = g_cat_begin[cat]; i < g_cat_begin[cat + 1]; i++) {
            const Entry& e = g_entries[i];
            auto f = snap->learned_by.find(e.id);
            if (f == snap->learned_by.end() || f->second.empty()) continue;
            snap->byId[cat][e.id].unlocked = true;
        }
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

    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_hud_x, 0, 0));
    WritePrivateProfileStringW(L"mastery", L"x", buf, g_ini_path.c_str());
    swprintf_s(buf, L"%d", (int)InterlockedCompareExchange(&g_hud_y, 0, 0));
    WritePrivateProfileStringW(L"mastery", L"y", buf, g_ini_path.c_str());
    // Sweep out what the pin feature used to write. Done once per run rather
    // than every tick: the keys are gone after the first save, and a config
    // file that keeps a setting nothing reads is a puzzle for whoever opens
    // it next.
    static bool swept = false;
    if (!swept) {
        swept = true;
        std::vector<wchar_t> buf(8192, 0);
        const DWORD n = GetPrivateProfileSectionW(L"mastery", buf.data(),
                                                  (DWORD)buf.size(),
                                                  g_ini_path.c_str());
        if (n > 0 && n < buf.size() - 2) {
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
                const wchar_t* eq = wcschr(p, L'=');
                if (!eq) continue;
                std::wstring key(p, eq);
                if (key != L"pin" && key.compare(0, 4, L"pin.") != 0) continue;
                WritePrivateProfileStringW(L"mastery", key.c_str(), nullptr,
                                           g_ini_path.c_str());
            }
        }
    }
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

    // Computed here rather than after the visibility check, because the
    // pinned mastery bar is drawn either way and needs to know about a click
    // to be draggable. Consuming it once, up front, keeps a click from being
    // seen twice by two different widgets.
    const bool clicked = in.clicks != g_seen_clicks;
    g_seen_clicks = in.clicks;

    auto own = own_get();

    // The pinned bar lives over the world, so it draws whether or not the
    // atlas window is open - that is the whole point of pinning one.
    mastery_hud_draw(in, clicked, screen_w, screen_h, own.get());

    if (!in.visible) {
        input_set_ui_rect(0, 0, 0, 0);
        g_dragging = false;
        // Discoverability: a quiet hint for the first minute of a session.
        if (GetTickCount() - g_ready_tick < 60000) {
            draw_text(24, screen_h - 46, 13, {0.7f, 0.75f, 0.85f, 0.65f},
                      "F8 - Collection Atlas");
        }
        return;
    }

    // --- window placement ---------------------------------------------------
    float win_w = kWinW, win_h = kWinH;
    if (win_w > screen_w - 40) win_w = screen_w - 40;
    if (win_h > screen_h - 40) win_h = screen_h - 40;

    float wx = (float)(LONG)InterlockedCompareExchange(&g_win_x, 0, 0);
    float wy = (float)(LONG)InterlockedCompareExchange(&g_win_y, 0, 0);

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
    if (tab < 0 || tab >= kTabs) tab = 0;

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
    char labels[kTabs][64];
    float tab_bx[kTabs], tab_by[kTabs], tab_bw[kTabs];
    int tab_rows = 1;
    {
        float lx = 0, ly = 0;
        for (int c = 0; c < kTabs; c++) {
            if (c == kRoutesTab) {
                sprintf_s(labels[c], "Routes %d", routes_count());
            } else if (c == kPlayersTab) {
                sprintf_s(labels[c], "Players %d", players_count());
            } else if (c == kMasteryTab) {
                // The tab carries the same total the page opens with, so the
                // whole answer is on the strip before you click it.
                if (own) {
                    int pts = 0, lvls = 0;
                    for (const auto& r : mastery_rows(own.get())) {
                        pts += r.points;
                        lvls += r.levels;
                    }
                    sprintf_s(labels[c], "Mastery %d/%d", pts, lvls);
                } else {
                    sprintf_s(labels[c], "Mastery");
                }
            } else {
                const int total = g_cat_begin[c + 1] - g_cat_begin[c];
                if (own)
                    sprintf_s(labels[c], "%s %d/%d", kCatNames[c],
                              own->owned_count[c], total);
                else
                    sprintf_s(labels[c], "%s %d", kCatNames[c], total);
            }
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

    for (int c = 0; c < kTabs; c++) {
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
            // Whatever had the keyboard belonged to the page being left, and
            // a field that keeps capture while off screen swallows movement
            // keys with nothing on screen to explain why.
            g_search_focus = false;
            input_set_text_capture(false);
        }
    }

    // --- mastery ------------------------------------------------------------
    //
    // Like Routes: not a collection page, so no grid, no search and no
    // filters. It reads the weapons page and the live character rather than
    // owning entries of its own.
    if (tab == kMasteryTab) {
        const float mx = wx + kPad;
        const float my = wy + kTitleH + tabs_h + 4;
        mastery_draw(in, mx, my, win_w - 2 * kPad - 10,
                     win_h - (kTitleH + tabs_h + 4) - kPad, own.get());
        return;
    }

    // --- routes -------------------------------------------------------------
    //
    // Not a collection page: no grid, no search, no filters. It takes the
    // whole content band and draws itself.
    if (tab == kRoutesTab) {
        const float rx = wx + kPad;
        const float ry = wy + kTitleH + tabs_h + 4;
        routes_draw(in, clicked, rx, ry, win_w - 2 * kPad,
                    win_h - (kTitleH + tabs_h + 4) - kPad);
        return;
    }

    // --- players ------------------------------------------------------------
    //
    // Same shape as Routes: a list page that takes the whole content band and
    // scrolls itself. It lists the players this client has been sent, which is
    // not the same thing as every player on the shard.
    if (tab == kPlayersTab) {
        const float px = wx + kPad;
        const float py = wy + kTitleH + tabs_h + 4;
        players_draw(in, clicked, px, py, win_w - 2 * kPad,
                     win_h - (kTitleH + tabs_h + 4) - kPad);
        return;
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
                // `craft:` and `mastery:` are lookup keys, not something to
                // filter on - nobody wants a chip that says "8/20".
                if (t.compare(0, 6, "craft:") == 0) continue;
                if (t.compare(0, 8, "mastery:") == 0) continue;
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

            // A click on a cell with known coordinates toggles the tracker;
            // held modifiers add it to the route being followed instead, so
            // a run can be assembled out of the atlas itself.
            if (clicked && !e.targets.empty() &&
                in.click_x >= x && in.click_x < x + kIcon &&
                in.click_y >= by0 && in.click_y < by1) {
                const std::vector<NavTarget> go = live_targets(e, own.get());
                char key[192];
                _snprintf_s(key, sizeof(key), _TRUNCATE, "%s/%s",
                            kCatTsv[ecat], e.id.c_str());
                if (go.empty()) {
                    // Nothing left that can be shown to still be there:
                    // clicking points at nothing rather than at a maybe. But
                    // untracking must still work. Track an item held in one
                    // chest, walk over and open it, and its last source is
                    // now spent - a plain list of alternatives never expires
                    // on its own, so refusing the click here left the arrow
                    // aimed at an emptied chest with no way to call it off
                    // short of Shift+F10.
                    if (nav_is_tracked(key)) nav_untrack();
                } else if (in.click_shift || in.click_ctrl) {
                    nav_add(e.name.c_str(), go.data(), (int)go.size(),
                            in.click_shift);
                } else {
                    nav_track(key, e.name.c_str(), go.data(), (int)go.size());
                }
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

// --- shared with other mods on this host ------------------------------------

bool atlas_ui_lookup(const std::string& id, AtlasItemInfo* out) {
    if (!out || !InterlockedCompareExchange(&g_loaded, 0, 0)) return false;
    // The per-category id maps are built once at load and never touched
    // again, so this needs no lock. An id can appear on more than one page
    // (a recipe's produced item is also a Misc entry); the first hit wins,
    // and the pages are in the order the tab strip shows them, so the more
    // specific page is found first.
    for (int c = 0; c < kCats; c++) {
        auto f = g_entry_by_id[c].find(id);
        if (f == g_entry_by_id[c].end()) continue;
        const Entry& e = g_entries[f->second];
        out->name = e.name;
        out->rarity = e.rarity;
        out->icon = e.icon;
        return true;
    }
    return false;
}

bool atlas_ui_find_by_name(const std::string& name, AtlasItemInfo* out) {
    if (!out || !InterlockedCompareExchange(&g_loaded, 0, 0)) return false;
    std::string want;
    for (char c : name) want += (char)tolower((unsigned char)c);
    while (!want.empty() && want.back() == ' ') want.pop_back();
    if (want.empty()) return false;

    // Exact first, and only then a substring - otherwise "Credence" would
    // lose to "Credence of the Deep" whenever both exist, and the item the
    // player named is the one they meant.
    const Entry* best = nullptr;
    int partial = 0;
    for (const Entry& e : g_entries) {
        std::string lower_name;
        for (char c : e.name) lower_name += (char)tolower((unsigned char)c);
        if (lower_name == want) {
            best = &e;
            partial = 0;
            break;
        }
        if (lower_name.find(want) != std::string::npos) {
            if (!partial) best = &e;
            partial++;
        }
    }
    // An ambiguous substring is a miss. Picking one of four swords for
    // somebody and putting it on their clipboard is worse than saying that
    // the name was not specific enough.
    if (!best || partial > 1) return false;
    out->name = best->name;
    out->rarity = best->rarity;
    out->icon = best->icon;
    return true;
}

void atlas_ui_draw_icon(int icon, float x, float y, float size, float alpha) {
    if (g_atlas < 0 || icon < 0 || g_atlas_w < 64 || g_atlas_h < 64) return;
    float u0, v0, u1, v1;
    icon_uv(icon, &u0, &v0, &u1, &v1);
    draw_image(g_atlas, x, y, size, size, u0, v0, u1, v1,
               {1.0f, 1.0f, 1.0f, alpha});
}

Color atlas_ui_rarity_color(int rarity) {
    return kRarity[rarity >= 0 && rarity <= 4 ? rarity : 0];
}

}  // namespace fmk
