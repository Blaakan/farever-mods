// ---------------------------------------------------------------------------
// hl_reader.cpp
//
// Walks the game's object graph to produce the data the mods need:
//
//   ent.Hero  +0x4b8 player -> st.Player
//     +0x0e0 accountProgress -> st.player.AccountProgress
//       +0x0a8 collection    -> st.player.Collection
//         mounts / gliders / pets / gears / toys / emotes
//       +0x0b8 bank, +0x0c0 bankEquipment
//
// Every offset comes from offsets.gen.h, generated out of the game's own
// bytecode. Every dereference is validated (see hl_runtime.cpp). The Hero
// pointer is re-validated by class name before each walk, so a stale pointer
// after a zone change degrades to "not found" instead of a wild read.
// ---------------------------------------------------------------------------

#include "hl_reader.h"
#include "hl_runtime.h"
#include "offsets.gen.h"

#include <algorithm>
#include <stdio.h>
#include <share.h>

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

void* g_hero_type = nullptr;
void* g_hero      = nullptr;
void* g_app_type  = nullptr;
void* g_app       = nullptr;

// Collection entries are Haxe values of an unknown-at-compile-time shape:
// a String, a boxed enum, or a CDB-backed object. Try the shapes we know, in
// order, and fall back to the class name so an unhandled shape shows up in the
// log as a name rather than silently producing nothing.
std::string decode_entry(void* elem) {
    if (!elem) return {};

    std::string cls = obj_class_name(elem);
    if (cls == "String") {
        std::string s = read_hx_string(elem);
        if (!s.empty()) return s;
    }

    // Objects that carry an id/kind String field: probe the first few slots
    // for something that reads back as a plausible id.
    for (uint32_t off = 0x08; off <= 0x40; off += 0x08) {
        void* p = read_ptr(elem, off);
        if (!p) continue;
        if (obj_class_name(p) != "String") continue;
        std::string s = read_hx_string(p);
        if (s.size() >= 2 && s.size() <= 64) return s;
    }

    if (!cls.empty()) return "<" + cls + ">";
    return {};
}

std::vector<std::string> decode_proxy_list(void* collection, uint32_t field) {
    std::vector<std::string> out;
    void* proxy = read_ptr(collection, field);
    if (!proxy) return out;

    void* elems = nullptr;
    int32_t count = 0;
    if (!read_proxy_array(proxy, &elems, &count)) return out;

    out.reserve((size_t)count);
    for (int32_t i = 0; i < count; i++) {
        void* e = read_ptr(elems, (uint32_t)(i * 8));
        std::string s = decode_entry(e);
        if (!s.empty()) out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

bool reader_locate_hero(bool force_rescan) {
    if (!g_hero_type) {
        g_hero_type = find_type_by_name("ent.Hero");
        if (!g_hero_type) {
            host_log("reader: ent.Hero type not found (character loaded yet?)");
            return false;
        }
        host_log("reader: ent.Hero type at %p", g_hero_type);
    }
    if (!force_rescan && g_hero && obj_is(g_hero, "ent.Hero")) return true;

    // Fast path: GameApp holds the live hero. Once the app has been found,
    // a zone change costs a pointer read instead of an 8GB sweep - the
    // scan below only runs before the app is known, or if that field ever
    // stops validating.
    if (g_app && obj_is(g_app, "GameApp")) {
        void* h = read_ptr(g_app, off::GameApp::hero);
        if (obj_is(h, "ent.Hero")) {
            void* player = read_ptr(h, off::ent_Hero::player);
            if (obj_is(player, "st.Player")) {
                g_hero = h;
                return true;
            }
        }
    }

    // A rescan is ~8GB of memory traffic. The Hero pointer goes stale exactly
    // when the game is loading or changing zone - the worst possible moment to
    // add that pressure, while it is busy deserialising. Hold off briefly so
    // the load can finish before we sweep.
    static DWORD last_scan = 0;
    DWORD now_ms = GetTickCount();
    if (g_hero_type && last_scan && (now_ms - last_scan) < 15000) {
        return false;
    }
    last_scan = now_ms;

    g_hero = nullptr;
    // The local Hero is one of several ent.Hero instances (party members
    // stream in as Heroes too), and most qwords matching the type pointer are
    // metadata rather than objects. Validate during the scan: accept only a
    // candidate whose player chain resolves all the way to an
    // AccountProgress, which only the local player has.
    g_hero = find_instance_of_type_where(
        g_hero_type,
        [](void* cand, void*) -> bool {
            void* player = read_ptr(cand, off::ent_Hero::player);
            if (!obj_is(player, "st.Player")) return false;
            void* ap = read_ptr(player, off::st_Player::accountProgress);
            return obj_is(ap, "st.player.AccountProgress");
        },
        nullptr);

    if (g_hero) {
        host_log("reader: local hero %p", g_hero);
        return true;
    }

    // Nothing passed. Fall back to a laxer probe purely for diagnosis: find
    // anything that at least looks like a live Hero, and report what its
    // `player` slot actually holds - a wrong offset then surfaces as a wrong
    // class name instead of another silence.
    void* any = find_instance_of_type_where(
        g_hero_type,
        [](void* cand, void*) -> bool { return obj_is(cand, "ent.Hero"); },
        nullptr);
    if (any) {
        void* p0 = read_ptr(any, off::ent_Hero::player);
        std::string cn = obj_class_name(p0);
        host_log("reader: hero-like %p, +0x%x -> %p (%s)", any,
                 off::ent_Hero::player, p0,
                 cn.empty() ? "<not an object>" : cn.c_str());
    } else {
        host_log("reader: no ent.Hero instance yet (in a loading screen?)");
    }
    return false;
}

void* reader_hero() {
    // GameApp is authoritative about whether a hero exists at all. At the
    // main menu, during logout, and between characters it nulls this field,
    // whereas a cached pointer can keep validating against memory the game
    // has simply stopped using - which is how a stale collection lingered
    // on screen after logging out. Reading it here also means a character
    // swap is picked up immediately, with no rescan.
    if (g_app && obj_is(g_app, "GameApp")) {
        void* h = read_ptr(g_app, off::GameApp::hero);
        if (!obj_is(h, "ent.Hero")) {
            g_hero = nullptr;
            return nullptr;
        }
        g_hero = h;
        return h;
    }
    // One local load: the pose thread calls this at 20Hz while the worker
    // may rewrite g_hero during a rescan, and three separate loads of a
    // racing pointer could validate one value and return another.
    void* h = g_hero;
    return (h && obj_is(h, "ent.Hero")) ? h : nullptr;
}

bool reader_read_unit_progress(
    std::vector<std::pair<std::string, int32_t>>* out) {
    out->clear();
    void* hero = reader_hero();
    if (!hero) return false;
    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return false;
    void* progress = read_ptr(player, off::st_Player::progress);
    if (!obj_is(progress, "st.player.Progress")) return false;

    void* data = read_ptr(progress, off::st_player_Progress::unitsProgress);
    if (!obj_is(data, "hxbit.MapData")) return false;

    // MapData.map is typed as the IMap interface, so it holds a vvirtual.
    void* map = deref_virtual(data, off::hxbit_MapData::map);
    if (!obj_is(map, "haxe.ds.StringMap")) {
        static bool once = true;
        if (once) {
            once = false;
            host_log("codex: unitsProgress map is %s, not a StringMap",
                     obj_class_name(map).c_str());
        }
        return false;
    }

    std::vector<MapEntry> entries;
    if (!read_string_map(map, &entries)) return false;

    out->reserve(entries.size());
    for (const auto& e : entries)
        out->push_back({e.key, dyn_as_int(e.value, 0)});

    // One line the first time through. The value type is a generic's erased
    // parameter, so the only way to learn what these numbers mean is to look
    // at what came back - including the runtime tag, which separates "the
    // count really is zero" from "this is not an int and the decoder is
    // handing back its fallback".
    static bool once = true;
    if (once && !entries.empty()) {
        once = false;
        std::string sample;
        int32_t nonzero = 0;
        for (const auto& kv : *out) if (kv.second) nonzero++;
        for (size_t i = 0; i < entries.size() && i < 3; i++) {
            void* v = entries[i].value;
            void* t = v ? read_ptr(v, 0) : nullptr;
            char one[160];
            _snprintf_s(one, sizeof(one), _TRUNCATE, " %s=%d(kind %d,%s)",
                        entries[i].key.c_str(), (*out)[i].second,
                        t ? read_i32(t, hlrt::type_kind) : -1,
                        v ? obj_class_name(v).c_str() : "<null>");
            sample += one;
        }
        host_log("codex: %zu units, %d with a non-zero value:%s", out->size(),
                 nonzero, sample.c_str());
    }
    return true;
}

bool reader_read_collection(Collection* out) {
    *out = {};
    void* hero = reader_hero();
    if (!hero) return false;

    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return false;

    void* ap = read_ptr(player, off::st_Player::accountProgress);
    if (!obj_is(ap, "st.player.AccountProgress")) return false;

    void* col = read_ptr(ap, off::st_player_AccountProgress::collection);
    if (!obj_is(col, "st.player.Collection")) return false;

    out->mounts  = decode_proxy_list(col, off::st_player_Collection::mounts);
    out->gliders = decode_proxy_list(col, off::st_player_Collection::gliders);
    out->pets    = decode_proxy_list(col, off::st_player_Collection::pets);
    out->gears   = decode_proxy_list(col, off::st_player_Collection::gears);
    out->toys    = decode_proxy_list(col, off::st_player_Collection::toys);
    out->emotes  = decode_proxy_list(col, off::st_player_Collection::emotes);
    out->bank_slots = read_i32(ap, off::st_player_AccountProgress::bankNbSlots);
    out->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

namespace {

// Rarity lives only on st.item.Weapon, and the bytecode declares it a String
// ("Common".."Legendary" - the CastleDB rarity sheet ids), not an enum. The
// first version read it as a boxed enum index, which is why every item
// reported -1. Non-weapons have no such field at all: their rarity is a
// static property of the kind, which the atlas data supplies offline.
int32_t rarity_index(void* str_obj) {
    if (!str_obj || obj_class_name(str_obj) != "String") return -1;
    const std::string s = read_hx_string(str_obj);
    static const char* kNames[] = {"Common", "Uncommon", "Rare", "Epic",
                                   "Legendary"};
    for (int i = 0; i < 5; i++)
        if (s == kNames[i]) return i;
    return -1;
}

bool read_item(void* obj, const char* source, Item* out) {
    if (!obj) return false;
    std::string cls = obj_class_name(obj);
    if (cls.empty()) return false;

    void* kind_str = read_ptr(obj, off::st_item_Gear::kind);
    std::string kind = read_hx_string(kind_str);
    if (kind.empty()) return false;

    out->kind = kind;
    out->cls = cls;
    out->source = source;
    out->level = read_i32(obj, off::st_item_Gear::level);
    out->upgrade = read_i32(obj, off::st_item_Gear::upgradeLevel);

    out->rarity = -1;
    if (cls == "st.item.Weapon")
        out->rarity = rarity_index(read_ptr(obj, off::st_item_Weapon::rarity));

    // Uninitialised slots report absurd values; clamp rather than propagate.
    if (out->level < 0 || out->level > 999) out->level = 0;
    if (out->upgrade < 0 || out->upgrade > 99) out->upgrade = 0;
    return true;
}

// Diagnostics are one-shot per source: this walk is several links deep and a
// silent zero says nothing about which link broke.
bool g_item_diag = true;

// Walks an hl.types.ArrayObj of item objects.
void read_item_array(void* array_obj, const char* source,
                     std::vector<Item>* out) {
    if (!array_obj) {
        if (g_item_diag) host_log("items[%s]: array is null", source);
        return;
    }
    std::string acls = obj_class_name(array_obj);
    int32_t len = read_i32(array_obj, off::hl_types_ArrayBase::length);
    void* varr = read_ptr(array_obj, off::hl_types_ArrayObj::array);
    int32_t cap = varr ? read_i32(varr, hlrt::varray_size) : -1;
    if (g_item_diag) {
        host_log("items[%s]: cls=%s len=%d varr=%p cap=%d", source,
                 acls.empty() ? "?" : acls.c_str(), len, varr, cap);
    }
    if (len <= 0 || len > 4096 || !varr) return;
    if (cap >= 0 && cap < len) len = cap;

    void* elems = (uint8_t*)varr + hlrt::varray_data;
    int rejected = 0;
    for (int32_t i = 0; i < len; i++) {
        void* e = read_ptr(elems, (uint32_t)(i * 8));
        Item it;

        // Slots are structural values, not items. The item hangs off a field
        // that the bank calls "it" and inventories call "item" - same shape
        // otherwise, so both names are accepted. `count` carries the stack.
        std::vector<VirtualField> vf;
        if (e && read_virtual_fields(e, &vf)) {
            void* inner = nullptr;
            int32_t count = 1;
            for (const auto& f : vf) {
                if (!f.value_ptr) continue;
                if ((f.name == "item" || f.name == "it") && f.kind == hlrt::HOBJ) {
                    inner = read_ptr(f.value_ptr, 0);
                } else if (f.name == "count" && f.kind == 3 /* HI32 */) {
                    int32_t c = read_i32(f.value_ptr, 0);
                    if (c > 0 && c < 100000) count = c;
                }
            }
            if (inner && read_item(inner, source, &it)) {
                it.count = count;
                out->push_back(std::move(it));
                continue;
            }
            // An empty slot is normal - equipped had 30 slots for 17 items.
            if (!inner) continue;
        }

        if (read_item(e, source, &it)) {
            out->push_back(std::move(it));
        } else if (e) {
            rejected++;
            if (g_item_diag && rejected <= 3) {
                // Report the raw type kind. obj_class_name only accepts
                // HOBJ/HSTRUCT and returns "" for anything else, which hides
                // the actual shape - the elements are clearly *something*.
                // Elements are HVIRTUAL (kind 15): Haxe structural values, not
                // class instances. Enumerate the field table so the shape is
                // named rather than guessed at.
                std::vector<VirtualField> vf;
                if (read_virtual_fields(e, &vf)) {
                    std::string desc;
                    for (size_t k = 0; k < vf.size() && k < 10; k++) {
                        if (!desc.empty()) desc += ", ";
                        desc += vf[k].name + ":k" + std::to_string(vf[k].kind);
                        // Name whatever an object-typed field points at.
                        if (vf[k].kind == hlrt::HOBJ && vf[k].value_ptr) {
                            void* v = read_ptr(vf[k].value_ptr, 0);
                            std::string c = obj_class_name(v);
                            if (!c.empty()) desc += "=" + c;
                        }
                    }
                    host_log("items[%s]:   elem[%d] virtual{%s}", source, i,
                             desc.c_str());
                } else {
                    host_log("items[%s]:   elem[%d]=%p not decodable", source, i, e);
                }
            }
        }
    }
}

// st.Inventory / st.Equipment both hold their items in `content`.
void read_inventory(void* inv, const char* source, std::vector<Item>* out) {
    if (!inv) {
        if (g_item_diag) host_log("items[%s]: inventory is null", source);
        return;
    }
    if (g_item_diag) {
        host_log("items[%s]: inventory cls=%s", source,
                 obj_class_name(inv).c_str());
    }
    void* content = read_ptr(inv, off::st_Inventory::content);
    read_item_array(content, source, out);
}

}  // namespace

bool reader_read_inventories(Inventories* out) {
    *out = {};
    void* hero = reader_hero();
    if (!hero) return false;

    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return false;
    void* ap = read_ptr(player, off::st_Player::accountProgress);
    if (!obj_is(ap, "st.player.AccountProgress")) return false;

    // Account-wide: the bank is shared by every character, so anything here
    // counts as owned regardless of who deposited it.
    read_item_array(read_ptr(ap, off::st_player_AccountProgress::bank),
                    "bank", &out->bank);
    read_item_array(read_ptr(ap, off::st_player_AccountProgress::bankEquipment),
                    "bankEquipment", &out->bank_equipment);
    out->bank_slots = read_i32(ap, off::st_player_AccountProgress::bankNbSlots);

    // Character-scoped: only the logged-in character exists in this process.
    out->character = read_hx_string(read_ptr(player, off::st_Player::name));

    void* loadout = read_ptr(hero, off::ent_Hero::loadout);
    if (g_item_diag) {
        host_log("items: loadout=%p cls=%s", loadout,
                 obj_class_name(loadout).c_str());
    }
    // Accept any class here rather than requiring an exact match: if the
    // runtime type is a subclass the exact-name test would silently skip the
    // whole walk, which is how the first attempt returned four empty lists.
    if (loadout) {
        read_inventory(read_ptr(loadout, off::st_Loadout::equipment),
                       "equipped", &out->equipped);
        read_inventory(read_ptr(loadout, off::st_Loadout::inventory),
                       "bags", &out->bags);
    }

    g_item_diag = false;   // one round of diagnostics is enough
    out->valid = true;
    return true;
}

void write_collection_json(const Collection& c) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;
    wcsncat_s(path, MAX_PATH, L"farever-collection.json", _TRUNCATE);

    FILE* f = _wfsopen(path, L"w", _SH_DENYNO);
    if (!f) return;

    auto list = [&](const char* name, const std::vector<std::string>& v, bool last) {
        fprintf(f, "  \"%s\": [", name);
        for (size_t i = 0; i < v.size(); i++) {
            fprintf(f, "%s\n    \"%s\"", i ? "," : "", v[i].c_str());
        }
        fprintf(f, "%s]%s\n", v.empty() ? "" : "\n  ", last ? "" : ",");
    };

    fprintf(f, "{\n");
    fprintf(f, "  \"bankSlots\": %d,\n", c.bank_slots);
    list("mounts", c.mounts, false);
    list("gliders", c.gliders, false);
    list("pets", c.pets, false);
    list("gears", c.gears, false);
    list("toys", c.toys, false);
    list("emotes", c.emotes, true);
    fprintf(f, "}\n");
    fclose(f);
    host_log("collection: wrote farever-collection.json");
}

void write_inventory_json(const Inventories& inv, const std::string& character) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;

    // Character names come from the game; keep the filename to safe chars.
    std::string safe;
    for (char c : character) {
        if (isalnum((unsigned char)c) || c == '_' || c == '-') safe.push_back(c);
    }
    if (safe.empty()) safe = "unknown";
    std::wstring wname(safe.begin(), safe.end());
    wcsncat_s(path, MAX_PATH, L"farever-inventory-", _TRUNCATE);
    wcsncat_s(path, MAX_PATH, wname.c_str(), _TRUNCATE);
    wcsncat_s(path, MAX_PATH, L".json", _TRUNCATE);

    FILE* f = _wfsopen(path, L"w", _SH_DENYNO);
    if (!f) return;

    auto emit = [&](const char* name, const std::vector<Item>& v, bool last) {
        fprintf(f, "  \"%s\": [", name);
        for (size_t i = 0; i < v.size(); i++) {
            const Item& it = v[i];
            fprintf(f,
                    "%s\n    {\"kind\":\"%s\",\"level\":%d,\"upgrade\":%d,"
                    "\"rarity\":%d,\"count\":%d,\"class\":\"%s\"}",
                    i ? "," : "", it.kind.c_str(), it.level, it.upgrade,
                    it.rarity, it.count, it.cls.c_str());
        }
        fprintf(f, "%s]%s\n", v.empty() ? "" : "\n  ", last ? "" : ",");
    };

    fprintf(f, "{\n");
    fprintf(f, "  \"character\": \"%s\",\n", safe.c_str());
    fprintf(f, "  \"bankSlots\": %d,\n", inv.bank_slots);
    emit("bank", inv.bank, false);
    emit("bankEquipment", inv.bank_equipment, false);
    emit("equipped", inv.equipped, false);
    emit("bags", inv.bags, true);
    fprintf(f, "}\n");
    fclose(f);

    host_log("inventory: %s bank=%zu bankEq=%zu equipped=%zu bags=%zu",
             safe.c_str(), inv.bank.size(), inv.bank_equipment.size(),
             inv.equipped.size(), inv.bags.size());
}

// GameApp is the application singleton: one instance, holding the game
// camera (and the hero, which a later version could use to skip the hero
// scan entirely). Validated during the scan by both of those fields, since
// most qwords matching a type pointer are metadata rather than instances.
// The singleton without a memory sweep: `inst` is a static of App, and a
// Haxe class's statics are fields of the class-value object that
// hl_type_obj.global_value points at. GameApp's type carries its
// superclass, so one type lookup reaches App's statics and the instance
// falls out as a pointer read.
//
// This is what makes startup quick. Scanning for the instance is a full
// pass over ~8GB of private memory; this is four dereferences.
void* find_app_via_statics() {
    if (!g_app_type) return nullptr;

    // GameApp's own statics do not hold `inst` - it is declared on App, the
    // superclass - so the walk steps up one level first.
    void* tobj = read_ptr(g_app_type, hlrt::type_obj);
    void* super = tobj ? read_ptr(tobj, hlrt::obj_super) : nullptr;
    void* super_obj = super ? read_ptr(super, hlrt::type_obj) : nullptr;
    void* slot = super_obj ? read_ptr(super_obj, hlrt::obj_global) : nullptr;
    void* statics = slot ? read_ptr(slot, 0) : nullptr;
    void* inst = statics ? read_ptr(statics, off::_App::inst) : nullptr;
    if (obj_is(inst, "GameApp")) return inst;

    // Name the link that broke rather than silently falling back to a scan
    // that costs ~8GB of reads. Logged once; a null `inst` early on is
    // simply the game not having built its App yet, which is why the caller
    // retries before giving up on this path.
    static bool diag = true;
    if (diag) {
        diag = false;
        host_log("app: statics walk - super=%s global=%p statics=%s inst=%s",
                 super ? obj_class_name_of_type(super).c_str() : "<null>",
                 slot, statics ? obj_class_name(statics).c_str() : "<null>",
                 inst ? obj_class_name(inst).c_str() : "<null>");
    }
    return nullptr;
}

bool reader_locate_app(bool allow_scan) {
    if (!g_app_type) {
        g_app_type = find_type_by_name("GameApp");
        if (!g_app_type) return false;
    }
    if (g_app && obj_is(g_app, "GameApp")) return true;

    g_app = find_app_via_statics();
    if (g_app) {
        host_log("reader: GameApp %p (via App.inst)", g_app);
        return true;
    }
    // App.inst is null until the game constructs its application object, so
    // an early miss is expected; the caller keeps trying this cheap path
    // before permitting the expensive one.
    if (!allow_scan) return false;

    // Fallback for a build where that walk does not hold: find the instance
    // the slow way, validated by one of its own fields.
    host_log("reader: App.inst still unavailable - scanning for GameApp");
    g_app = find_instance_of_type_where(
        g_app_type,
        [](void* cand, void*) -> bool {
            return obj_is(read_ptr(cand, off::GameApp::gameCamera),
                          "client.GameCamera");
        },
        nullptr);

    if (g_app) host_log("reader: GameApp %p (scanned)", g_app);
    else host_log("reader: GameApp not found");
    return g_app != nullptr;
}

bool reader_read_camera(double* px, double* py, double* pz,
                        double* tx, double* ty, double* tz) {
    // This walk is five links deep, and a silent failure here is
    // indistinguishable from "no camera" at the UI - it just quietly draws
    // a hero-relative arrow. Name the broken link, once.
    static bool diag = true;
    auto fail = [&](const char* where, void* p) {
        if (diag) {
            diag = false;
            std::string cls = obj_class_name(p);
            host_log("camera: walk stopped at %s (%p is %s)", where, p,
                     cls.empty() ? "<not an object>" : cls.c_str());
        }
        return false;
    };

    if (!g_app || !obj_is(g_app, "GameApp")) return fail("GameApp", g_app);
    void* ctrl = read_ptr(g_app, off::GameApp::gameCamera);
    if (!obj_is(ctrl, "client.GameCamera")) return fail("gameCamera", ctrl);

    // The controller is not the camera: it drives the scene's h3d.Camera,
    // and only that object knows where the view actually is.
    void* scene = read_ptr(ctrl, off::client_BaseCamera::scene);
    if (!obj_is(scene, "h3d.scene.Scene")) return fail("BaseCamera.scene", scene);
    void* cam = read_ptr(scene, off::h3d_scene_Scene::camera);
    if (!obj_is(cam, "h3d.Camera")) return fail("Scene.camera", cam);

    void* pos = read_ptr(cam, off::h3d_Camera::pos);
    void* target = read_ptr(cam, off::h3d_Camera::target);
    if (!pos || !target) return fail("Camera.pos/target", pos ? target : pos);

    if (diag) {
        diag = false;
        host_log("camera: view chain resolved (h3d.Camera %p)", cam);
    }

    return read(pos, off::h3d_VectorImpl::x, px) &&
           read(pos, off::h3d_VectorImpl::y, py) &&
           read(pos, off::h3d_VectorImpl::z, pz) &&
           read(target, off::h3d_VectorImpl::x, tx) &&
           read(target, off::h3d_VectorImpl::y, ty) &&
           read(target, off::h3d_VectorImpl::z, tz);
}

bool reader_read_hero_pose(double* x, double* y, double* z, double* rot_z) {
    void* hero = reader_hero();
    if (!hero) return false;
    return read(hero, off::ent_GameObject::posx, x) &&
           read(hero, off::ent_GameObject::posy, y) &&
           read(hero, off::ent_GameObject::posz, z) &&
           read(hero, off::ent_GameObject::rotationZ, rot_z);
}

bool reader_read_unit_state(UnitState* out) {
    *out = {};
    void* hero = reader_hero();
    if (!hero) return false;
    uint8_t in_combat = 0;
    read(hero, off::ent_Unit::isInCombat, &in_combat);
    out->in_combat = in_combat != 0;
    out->valid = true;
    return true;
}

}  // namespace fmk
