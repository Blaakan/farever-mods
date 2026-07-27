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
    return (g_hero && obj_is(g_hero, "ent.Hero")) ? g_hero : nullptr;
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

bool reader_read_hero_pos(double* x, double* y, double* z) {
    void* hero = reader_hero();
    if (!hero) return false;
    return read(hero, off::ent_GameObject::posx, x) &&
           read(hero, off::ent_GameObject::posy, y) &&
           read(hero, off::ent_GameObject::posz, z);
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
