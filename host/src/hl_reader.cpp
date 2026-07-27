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

    g_hero = nullptr;
    // The local Hero is one of possibly several ent.Hero instances (party
    // members stream in as Heroes too). Prefer one whose `player` chain
    // actually resolves to an AccountProgress - only the local player has one.
    auto hits = find_instances_of_type(g_hero_type, 64);
    host_log("reader: %zu ent.Hero instance(s)", hits.size());

    int with_player = 0;
    for (void* h : hits) {
        void* player = read_ptr(h, off::ent_Hero::player);
        if (!obj_is(player, "st.Player")) continue;
        with_player++;
        void* ap = read_ptr(player, off::st_Player::accountProgress);
        if (!obj_is(ap, "st.player.AccountProgress")) continue;
        g_hero = h;
        host_log("reader: local hero %p (player %p)", h, player);
        return true;
    }
    // Report how far the chain got: which link broke is the whole diagnosis.
    if (!hits.empty()) {
        host_log("reader: no local hero - %d/%zu had an st.Player, none had "
                 "accountProgress", with_player, hits.size());
        // Name whatever the first instance's `player` slot actually holds, so
        // a wrong offset shows up as a wrong class name rather than silence.
        void* p0 = read_ptr(hits[0], off::ent_Hero::player);
        std::string cn = obj_class_name(p0);
        host_log("reader:   hero[0]+0x%x -> %p (%s)", off::ent_Hero::player, p0,
                 cn.empty() ? "<not an object>" : cn.c_str());
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
