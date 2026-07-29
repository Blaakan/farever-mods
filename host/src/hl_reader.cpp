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

    // With a live GameApp there is nothing to scan *for*: the hero is that
    // object's own field, so a null there means "nobody is in the world yet"
    // - the main menu, character select, a loading screen - and the answer
    // arrives for free the moment one is. Sweeping 4.6GB to look for a hero
    // that does not exist cost 16 seconds of every launch.
    if (g_app && obj_is(g_app, "GameApp")) return false;

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

bool reader_read_jobs(std::vector<JobState>* out) {
    out->clear();
    void* hero = reader_hero();
    if (!hero) return false;
    void* spec = read_ptr(hero, off::ent_Hero::specialization);
    if (!obj_is(spec, "st.player.HeroSpecialization")) return false;

    void* jobs = read_ptr(spec, off::st_player_HeroSpecialization::jobs);
    void* elems = nullptr;
    int32_t count = 0;
    if (!jobs || !read_proxy_array(jobs, &elems, &count)) return false;
    if (count < 0 || count > 32) return false;

    namespace job_off = off::hxbit_ObjProxy_3327ea72931d811ba796c031db6ffed0;
    for (int32_t i = 0; i < count; i++) {
        void* entry = read_ptr(elems, (uint32_t)(i * 8));
        if (!entry) continue;

        JobState js;
        js.job = read_hx_string(read_ptr(entry, job_off::job));
        // The job name is the identity check: the proxy's own class name is
        // a hash of the structure's shape and would move with any patch to
        // it, so validating against that would be brittle.
        if (js.job.empty() || js.job.size() > 32) continue;
        js.level = read_i32(entry, job_off::level);
        read(entry, job_off::knowledge, &js.knowledge);
        if (js.level < 0 || js.level > 200) js.level = 0;

        void* learned = read_ptr(entry, job_off::learnedCrafts);
        void* lelems = nullptr;
        int32_t lcount = 0;
        if (learned && read_proxy_array(learned, &lelems, &lcount) &&
            lcount >= 0 && lcount <= 4096) {
            js.learned.reserve((size_t)lcount);
            for (int32_t k = 0; k < lcount; k++) {
                std::string craft = read_hx_string(read_ptr(lelems, (uint32_t)(k * 8)));
                if (!craft.empty()) js.learned.push_back(std::move(craft));
            }
        }
        out->push_back(std::move(js));
    }

    static bool once = true;
    if (once && !out->empty()) {
        once = false;
        std::string s;
        for (const auto& j : *out) {
            char one[96];
            _snprintf_s(one, sizeof(one), _TRUNCATE, " %s(lv%d,%zu crafts)",
                        j.job.c_str(), j.level, j.learned.size());
            s += one;
        }
        host_log("jobs:%s", s.c_str());
    }
    return true;
}

bool reader_read_unit_progress(std::vector<UnitProgress>* out) {
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

    // The value is a record, not a number: the class name reads
    // ObjProxy_OkillCount_Int_rank_Int, which is the shape spelled out.
    namespace prog = off::hxbit_ObjProxy_OkillCount_Int_rank_Int;
    out->reserve(entries.size());
    for (const auto& e : entries) {
        UnitProgress up;
        up.unit = e.key;
        up.kills = read_i32(e.value, prog::killCount);
        up.rank = read_i32(e.value, prog::rank);
        if (up.kills < 0 || up.kills > 1000000) up.kills = 0;
        if (up.rank < 0 || up.rank > 100) up.rank = 0;
        out->push_back(std::move(up));
    }

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
        for (const auto& up : *out) if (up.kills) nonzero++;
        for (size_t i = 0; i < entries.size() && i < 3; i++) {
            void* v = entries[i].value;
            void* t = v ? read_ptr(v, 0) : nullptr;
            char one[160];
            _snprintf_s(one, sizeof(one), _TRUNCATE, " %s=%dkills/rank%d",
                        entries[i].key.c_str(), (*out)[i].kills,
                        (*out)[i].rank);
            (void)t;
            sample += one;
        }
        host_log("codex: %zu units encountered, %d with kills:%s",
                 out->size(), nonzero, sample.c_str());
    }
    return true;
}

bool reader_read_runes(RuneState* out) {
    *out = {};
    void* hero = reader_hero();
    if (!hero) return false;

    // Learned runes hang off Progress, not off the specialization: learning
    // one is permanent, and slotting it is a separate, changeable choice.
    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return false;
    void* progress = read_ptr(player, off::st_Player::progress);
    if (!obj_is(progress, "st.player.Progress")) return false;
    out->learned = decode_proxy_list(
        progress, off::st_player_Progress::skillMasteriesLearnt);

    // The slotted ones are the specialization's business, and a character
    // with none is normal rather than a failed read.
    void* spec = read_ptr(hero, off::ent_Hero::specialization);
    if (obj_is(spec, "st.player.HeroSpecialization"))
        out->slotted = decode_proxy_list(
            spec, off::st_player_HeroSpecialization::skillMasteries);

    static bool once = true;
    if (once) {
        once = false;
        host_log("runes: %zu learned, %zu slotted%s%s", out->learned.size(),
                 out->slotted.size(), out->learned.empty() ? "" : " - e.g. ",
                 out->learned.empty() ? "" : out->learned[0].c_str());
    }
    return true;
}

bool reader_read_completion(CompletionState* out) {
    *out = {};
    void* hero = reader_hero();
    if (!hero) return false;
    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return false;
    void* progress = read_ptr(player, off::st_Player::progress);
    if (!obj_is(progress, "st.player.Progress")) return false;

    auto walk = [&](uint32_t field, std::vector<MapEntry>* entries) {
        void* data = read_ptr(progress, field);
        if (!obj_is(data, "hxbit.MapData")) return false;
        void* map = deref_virtual(data, off::hxbit_MapData::map);
        if (!obj_is(map, "haxe.ds.StringMap")) return false;
        return read_string_map(map, entries);
    };

    // Elements: a chest opened, a secret orb collected. The record is a
    // single float, and the map only gains a key once you have touched the
    // thing - so a non-zero value is "done with it".
    std::vector<MapEntry> entries;
    if (walk(off::st_player_Progress::elements, &entries)) {
        for (const auto& e : entries) {
            double completed = 0;
            read(e.value, off::hxbit_ObjProxy_Ocompleted_Float::completed,
                 &completed);
            if (completed != 0) out->done.push_back(e.key);
        }
    }

    // Activities: a dungeon, a rift, a camp. This one says outright whether
    // it has ever been finished, which is the question a one-time source
    // asks - `lastCompletion` is for the repeatable ones' cooldowns.
    entries.clear();
    if (walk(off::st_player_Progress::activities, &entries)) {
        namespace act = off::hxbit_ObjProxy_OcompletedOnce_Bool_lastCompletion_Float;
        for (const auto& e : entries) {
            uint8_t once = 0;
            read(e.value, act::completedOnce, &once);
            if (once) out->done.push_back(e.key);
        }
    }

    out->valid = true;
    static bool said = false;
    if (!said) {
        said = true;
        // Grouped by the shape of the id, because that answers the question
        // the npcs map could not: an NPC is an element too, so if handing a
        // quest in marks its NPC completed, these ids are already in here
        // and quest filtering needs nothing further.
        int npcish = 0, chest = 0, orb = 0, other = 0;
        std::string sample_npc;
        for (const auto& id : out->done) {
            if (id.find("NPC") != std::string::npos) {
                npcish++;
                if (sample_npc.size() < 90) sample_npc += "  " + id;
            } else if (id.find("Chest") != std::string::npos) chest++;
            else if (id.find("Orb") != std::string::npos) orb++;
            else other++;
        }
        host_log("done: %zu finished - %d npc-ish, %d chests, %d orbs, %d other%s",
                 out->done.size(), npcish, chest, orb, other,
                 sample_npc.c_str());
    }
    return true;
}

void reader_probe_completion() {
    static bool done = false;
    if (done) return;
    void* hero = reader_hero();
    if (!hero) return;
    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return;
    void* progress = read_ptr(player, off::st_Player::progress);
    if (!obj_is(progress, "st.player.Progress")) return;
    done = true;

    // Every activity key, with whether it is finished.
    //
    // The npcs map turned out to hold no completion signal - all ~47 quest
    // NPCs read the same value - so if a quest is recorded anywhere, it is
    // here. Six keys were not enough to tell: they were all world furniture
    // (a rift, a camp, a dungeon). The whole list settles whether a quest
    // has an activity id at all, and what one looks like.
    {
        void* ad = read_ptr(progress, off::st_player_Progress::activities);
        void* am = obj_is(ad, "hxbit.MapData")
            ? deref_virtual(ad, off::hxbit_MapData::map) : nullptr;
        std::vector<MapEntry> ae;
        if (obj_is(am, "haxe.ds.StringMap") && read_string_map(am, &ae)) {
            namespace act =
                off::hxbit_ObjProxy_OcompletedOnce_Bool_lastCompletion_Float;
            std::string line;
            int n = 0;
            for (const auto& e : ae) {
                uint8_t once = 0;
                read(e.value, act::completedOnce, &once);
                line += "  " + e.key + (once ? "=done" : "=open");
                if (++n % 6 == 0) {
                    host_log("act:%s", line.c_str());
                    line.clear();
                }
            }
            if (!line.empty()) host_log("act:%s", line.c_str());
            host_log("act: %zu activities total", ae.size());
        }
    }

    void* data = read_ptr(progress, off::st_player_Progress::npcs);
    if (!obj_is(data, "hxbit.MapData")) return;
    void* map = deref_virtual(data, off::hxbit_MapData::map);
    if (!obj_is(map, "haxe.ds.StringMap")) return;
    std::vector<MapEntry> npcs;
    if (!read_string_map(map, &npcs)) return;

    // Every NPC, compactly. The interesting comparison is between one whose
    // quest is handed in and one whose is not - with 58 of them and most of
    // the map done, both are in here, and whatever distinguishes them is the
    // signal a quest target needs.
    //
    // The value is not an object (its class name came back empty), so its
    // raw type kind is logged instead: that separates a boxed bool from a
    // float from a null, which the class name cannot.
    // The per-NPC record holds more than its goals: a `bit` and a `dialog`
    // array, neither of which has been read. If a finished quest is recorded
    // anywhere on this character, it is one of those - every map on Progress
    // has now been ruled out.
    //
    // NPC_Lora's quest is done and NPC_Beerutus's is not, so whatever
    // differs between those two lines is the answer.
    namespace npcf = off::hxbit_ObjProxy_ad383d83eed03d0e5475cee203565222;
    for (const auto& e : npcs) {
        const int32_t bit = read_i32(e.value, npcf::bit);
        void* dlg = read_ptr(e.value, npcf::dialog);
        void* arr = dlg ? read_ptr(dlg, off::hxbit_ArrayProxyData::array) : nullptr;
        void* base = arr ? read_ptr(arr, off::hl_types_ArrayDyn::array) : nullptr;
        const int32_t len =
            base ? read_i32(base, off::hl_types_ArrayBase::length) : -1;
        std::string seen;
        void* varr = base ? read_ptr(base, off::hl_types_ArrayObj::array) : nullptr;
        if (varr && len > 0) {
            void* elems = (uint8_t*)varr + hlrt::varray_data;
            for (int32_t i = 0; i < len && i < 8; i++) {
                void* v = read_ptr(elems, (uint32_t)(i * 8));
                const std::string vc = obj_class_name(v);
                seen += " ";
                seen += (vc == "String") ? read_hx_string(v)
                      : (vc.empty() ? "?" : vc);
            }
        }
        host_log("npc[%s]: bit=%d dialog=%d%s", e.key.c_str(), bit, len,
                 seen.c_str());
    }

    // The two fields named for what the codex calls an activity - which
    // includes NPC quests, even though a quest has no authored activity row
    // anywhere and never appears in Progress.activities. If a finished quest
    // is recorded at all, it is in one of these.
    {
        void* hd = read_ptr(player, off::st_Player::heroData);
        void* ap = hd ? read_ptr(hd, off::st_player_HeroData::activityProgress)
                      : nullptr;
        int32_t len = ap ? read_i32(ap, off::hl_types_ArrayBase::length) : -1;
        void* varr = ap ? read_ptr(ap, off::hl_types_ArrayObj::array) : nullptr;
        std::string s;
        if (varr && len > 0) {
            void* elems = (uint8_t*)varr + hlrt::varray_data;
            for (int32_t i = 0; i < len && i < 10; i++) {
                void* e = read_ptr(elems, (uint32_t)(i * 8));
                const std::string c = obj_class_name(e);
                s += "  [" + std::to_string(i) + "]=" +
                     (c.empty() ? "?" : c);
                if (c == "String") s += ":" + read_hx_string(e);
                // A record would name its own fields the way the others do.
                std::vector<VirtualField> vf;
                if (c.empty() && e && read_virtual_fields(e, &vf)) {
                    s += "{";
                    for (size_t k = 0; k < vf.size() && k < 6; k++)
                        s += vf[k].name + ",";
                    s += "}";
                }
            }
        }
        host_log("actprog: len=%d%s", len, s.c_str());

        void* ctx = read_ptr(player, off::st_Player::activityCtx);
        void* carr = ctx ? read_ptr(ctx, off::hxbit_ArrayProxyData::array)
                         : nullptr;
        void* cbase = carr ? read_ptr(carr, off::hl_types_ArrayDyn::array)
                           : nullptr;
        const int32_t clen =
            cbase ? read_i32(cbase, off::hl_types_ArrayBase::length) : -1;
        std::string cs;
        void* cvarr = cbase ? read_ptr(cbase, off::hl_types_ArrayObj::array)
                            : nullptr;
        if (cvarr && clen > 0) {
            void* elems = (uint8_t*)cvarr + hlrt::varray_data;
            for (int32_t i = 0; i < clen && i < 10; i++) {
                void* e = read_ptr(elems, (uint32_t)(i * 8));
                const std::string c = obj_class_name(e);
                cs += "  [" + std::to_string(i) + "]=" + (c.empty() ? "?" : c);
                if (c == "String") cs += ":" + read_hx_string(e);
            }
        }
        host_log("actctx: len=%d%s", clen, cs.c_str());
    }

    // The last map on Progress nobody has looked in. Counters are how a game
    // usually records "you have done this N times", which is exactly the
    // shape a completed quest would take if it is not an activity.
    void* counters = read_ptr(progress, off::st_player_Progress::counters);
    if (obj_is(counters, "haxe.ds.StringMap")) {
        std::vector<MapEntry> ce;
        if (read_string_map(counters, &ce)) {
            std::string line;
            int n = 0;
            for (const auto& e : ce) {
                void* v = e.value;
                void* vt = v ? read_ptr(v, 0) : nullptr;
                const int32_t kind = vt ? read_i32(vt, 0) : -1;
                const int32_t val = v ? read_i32(v, hlrt::dyn_payload) : 0;
                char one[128];
                _snprintf_s(one, sizeof(one), _TRUNCATE, "  %s=k%d:%d",
                            e.key.c_str(), kind, val);
                line += one;
                if (++n % 5 == 0) { host_log("ctr:%s", line.c_str()); line.clear(); }
            }
            if (!line.empty()) host_log("ctr:%s", line.c_str());
            host_log("ctr: %zu counters total", ce.size());
        }
    }
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

// ---------------------------------------------------------------------------
// Loot state
//
// A narrow, cheap read meant to run several times a second, because a loot
// feed that samples once a minute is a list of things you have forgotten
// picking up. Nothing here scans, and nothing walks the bank, the codex or
// the collection.
// ---------------------------------------------------------------------------

namespace {

bool g_currency_diag = true;

// The purse. Each element is a structural value with a name and an amount;
// which words the game uses for those two is read rather than assumed, since
// the same shape appears with `kind`/`id` and `count`/`value` elsewhere in
// this file, and getting it wrong would report every balance as zero.
void read_currency_array(void* array_obj, std::vector<Currency>* out) {
    const int32_t len =
        array_obj ? read_i32(array_obj, off::hl_types_ArrayBase::length) : 0;
    void* varr = array_obj ? read_ptr(array_obj, off::hl_types_ArrayObj::array)
                           : nullptr;
    // The array itself gets a line, not only its elements: an empty purse and
    // a walk that stopped one link short both produce no currency lines at
    // all, and only this tells them apart.
    if (g_currency_diag)
        host_log("currencies: array=%p cls=%s len=%d varr=%p", array_obj,
                 obj_class_name(array_obj).c_str(), len, varr);
    if (len <= 0 || len > 256 || !varr) {
        g_currency_diag = false;
        return;
    }
    void* elems = (uint8_t*)varr + hlrt::varray_data;

    for (int32_t i = 0; i < len; i++) {
        void* e = read_ptr(elems, (uint32_t)(i * 8));
        if (!e) continue;

        std::vector<VirtualField> vf;
        if (!read_virtual_fields(e, &vf)) {
            // Not structural: perhaps a plain object with the same fields.
            continue;
        }
        if (g_currency_diag) {
            std::string desc;
            for (size_t k = 0; k < vf.size() && k < 8; k++) {
                if (!desc.empty()) desc += ", ";
                desc += vf[k].name + ":k" + std::to_string(vf[k].kind);
            }
            host_log("currencies: elem[%d] {%s}", i, desc.c_str());
        }

        Currency c;
        for (const auto& f : vf) {
            if (!f.value_ptr) continue;
            if (f.kind == hlrt::HOBJ &&
                (f.name == "kind" || f.name == "id" || f.name == "item" ||
                 f.name == "currency")) {
                c.kind = read_hx_string(read_ptr(f.value_ptr, 0));
            } else if (f.name == "count" || f.name == "value" ||
                       f.name == "amount" || f.name == "nb") {
                if (f.kind == hlrt::HI32) {
                    c.count = read_i32(f.value_ptr, 0);
                } else if (f.kind == hlrt::HF64) {
                    double d = 0;
                    fmk::read(f.value_ptr, 0, &d);
                    c.count = (int64_t)d;
                }
            }
        }
        if (!c.kind.empty()) out->push_back(std::move(c));
    }
    g_currency_diag = false;
}

}  // namespace

bool reader_read_loot_state(LootState* out) {
    *out = {};
    void* hero = reader_hero();
    if (!hero) return false;

    void* player = read_ptr(hero, off::ent_Hero::player);
    if (!obj_is(player, "st.Player")) return false;

    void* hd = read_ptr(player, off::st_Player::heroData);
    if (hd) {
        out->level = read_i32(hd, off::st_player_HeroData::level);
        out->exp = read_i32(hd, off::st_player_HeroData::exp);
        // Uninitialised or mid-write values would read as a huge gain; a
        // level past a few hundred is not a level, it is a bad pointer.
        if (out->level < 0 || out->level > 999) { out->level = 0; out->exp = 0; }
        if (out->exp < 0) out->exp = 0;
        read_currency_array(read_ptr(hd, off::st_player_HeroData::currencies),
                            &out->currencies);
    }

    // The bags, and only the bags: a chest opening puts its contents there,
    // and anything that lands in the bank did not just happen to you.
    //
    // The item decoder's one-shot diagnostics belong to the atlas read, which
    // is where a broken walk needs explaining. This poll runs twice a second
    // and would burn that one shot immediately, then say nothing useful for
    // the rest of the session - so it borrows the flag and puts it back.
    void* loadout = read_ptr(hero, off::ent_Hero::loadout);
    if (loadout) {
        const bool diag = g_item_diag;
        g_item_diag = false;
        read_inventory(read_ptr(loadout, off::st_Loadout::inventory), "loot",
                       &out->bags);
        g_item_diag = diag;
    }

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
    // Re-derive every time rather than trusting the cache.
    //
    // The game *replaces* its GameApp - character select and back builds a
    // new one - and a dead HashLink object keeps its type pointer until the
    // collector reuses the block, so `obj_is(g_app, "GameApp")` goes on
    // saying yes about an object nothing else refers to any more. Everything
    // rooted here then reads that corpse, while everything rooted at the
    // hero (which has its own scan) carries on working. The symptom is not
    // "the mod stopped" but "the arrow fell back to hero facing and the map
    // is never found", which is exactly what farever-modkit.log showed.
    //
    // App.inst is the authority and reaching it is six pointer reads - the
    // whole reason it is the root - so there is nothing to save by caching.
    void* live = find_app_via_statics();
    if (live && live != g_app) {
        host_log("reader: GameApp %p -> %p (App.inst moved)", g_app, live);
        g_app = live;
        // The old app's hero belongs to the old world. Dropping it costs one
        // pointer read on the next call, not a scan.
        g_hero = nullptr;
    }
    if (g_app && obj_is(g_app, "GameApp")) return true;

    if (live) {
        g_app = live;
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

// ---------------------------------------------------------------------------
// The game's own map
// ---------------------------------------------------------------------------

namespace {

// `ui.BaseUI.windows` is the list of windows that are **open**, not of every
// window the UI knows: a live run showed `windows[0] of 1` while the map was
// up, and a different MapWindow pointer on the next open. So presence in that
// list is itself the answer to "is the map open", and there is nothing to
// cache - caching it would only create the one failure this cannot otherwise
// have, a pointer to a closed window that still passes a type check because
// the collector has not reused the block yet.
//
// The walk costs one length, one array pointer and a handful of element
// reads. At the pose thread's 20Hz that is not worth a cache.
void* find_map_window() {
    if (!g_app || !obj_is(g_app, "GameApp")) return nullptr;
    void* gui = read_ptr(g_app, off::GameApp::gui);
    if (!gui) return nullptr;
    void* arr = read_ptr(gui, off::ui_GameUI::windows);
    if (!arr) return nullptr;
    int32_t len = read_i32(arr, off::hl_types_ArrayBase::length);
    void* varr = read_ptr(arr, off::hl_types_ArrayObj::array);
    if (len <= 0 || !varr) return nullptr;
    // Open windows, so the list is short; a length past this is a bad read,
    // not a player with two hundred windows open.
    if (len > 128) len = 128;
    void* elems = (uint8_t*)varr + hlrt::varray_data;
    for (int32_t i = 0; i < len; i++) {
        void* e = read_ptr(elems, (uint32_t)(i * 8));
        if (e && obj_is(e, "ui.win.MapWindow")) return e;
    }
    return nullptr;
}

// A marker's world position. Every marker class inherits worldPos from
// ui.win.map.MapMarker, so the subclass only matters for the label.
bool marker_pos(void* marker, double* x, double* y, double* z) {
    if (!marker) return false;
    void* v = read_ptr(marker, off::ui_win_map_MapMarker::worldPos);
    if (!v) return false;
    return read(v, off::h3d_VectorImpl::x, x) &&
           read(v, off::h3d_VectorImpl::y, y) &&
           read(v, off::h3d_VectorImpl::z, z);
}

// The best name a marker can give for itself. There is no one field for it:
// a text marker carries a description, some markers carry a scene-object
// name, and the rest are only identified by what class they are. Falling
// back through all three beats calling everything "Waypoint".
std::string marker_label(void* marker) {
    if (!marker) return "";
    const std::string cls = obj_class_name(marker);
    if (cls == "ui.win.map.TextMarker") {
        std::string d =
            read_hx_string(read_ptr(marker, off::ui_win_map_TextMarker::desc));
        if (!d.empty()) return d;
    }
    std::string n =
        read_hx_string(read_ptr(marker, off::ui_win_map_MapMarker::name));
    if (!n.empty()) return n;

    // Class name to something a player would recognise. "ui.win.map." is
    // eleven characters of namespace nobody needs on a HUD.
    static const struct { const char* cls; const char* label; } kNames[] = {
        {"ui.win.map.ActivityMarker", "Activity"},
        {"ui.win.map.ObeliskMarker", "Obelisk"},
        {"ui.win.map.PinMarker", "Map pin"},
        {"ui.win.map.TextMarker", "Place"},
        {"ui.win.map.IconMarker", "Point of interest"},
        {"ui.win.map.PlayerMarker", "Player"},
    };
    for (const auto& k : kNames)
        if (cls == k.cls) return k.label;
    return "Map location";
}

// Walks an ArrayObj of markers, calling `fn(marker)` on each live one.
// Bounded: the map holds a marker per point of interest in the loaded
// region, so this is hundreds, not tens - fine for a click, not for 20Hz.
template <typename F>
int for_each_marker(void* array_obj, F fn) {
    if (!array_obj) return 0;
    int32_t len = read_i32(array_obj, off::hl_types_ArrayBase::length);
    void* varr = read_ptr(array_obj, off::hl_types_ArrayObj::array);
    if (len <= 0 || !varr) return 0;
    if (len > 4096) len = 4096;
    void* elems = (uint8_t*)varr + hlrt::varray_data;
    int seen = 0;
    for (int32_t i = 0; i < len; i++) {
        void* e = read_ptr(elems, (uint32_t)(i * 8));
        if (!e) continue;
        seen++;
        fn(e);
    }
    return seen;
}

}  // namespace

bool reader_read_map_state(MapState* out) {
    *out = {};
    void* win = find_map_window();
    if (!win) return false;

    // Being in the open-windows list is the answer. `visible` and `parent`
    // are read anyway and reported, because they are the two fields that
    // would justify a stricter gate if presence ever turns out not to be
    // enough - and one line in the log settles that far better than a guess.
    uint8_t visible = 0;
    read(win, off::ui_win_MapWindow::visible, &visible);
    out->window = win;
    out->visible = visible != 0;
    out->parented = read_ptr(win, off::ui_win_MapWindow::parent) != nullptr;
    out->open = true;

    // Reported, not used. Both were the first attempt at "what is under the
    // cursor" and both stay null with a mouse: they belong to the gamepad
    // crosshair and to the debug readout respectively. Kept in the log so
    // that stays a fact rather than a memory.
    out->near_clickable =
        read_ptr(win, off::ui_win_MapWindow::nearClickableMarker);
    out->mouse_cursor = read_ptr(win, off::ui_win_MapWindow::mouseCursor);

    void* markers = read_ptr(win, off::ui_win_MapWindow::markers);
    out->markers = for_each_marker(markers, [](void*) {});

    void* gui = read_ptr(g_app, off::GameApp::gui);
    void* scene = gui ? read_ptr(gui, off::ui_GameUI::s2d) : nullptr;
    if (scene) {
        out->scene_w = read_i32(scene, off::h2d_Scene::width);
        out->scene_h = read_i32(scene, off::h2d_Scene::height);
    }

    // The player's own pins.
    void* pins = read_ptr(win, off::ui_win_MapWindow::pinMarkers);
    for_each_marker(pins, [&](void* m) {
        MapPin p;
        if (!marker_pos(m, &p.x, &p.y, &p.z)) return;
        p.label = marker_label(m);
        if (out->pins.size() < 64) out->pins.push_back(std::move(p));
    });
    return true;
}

bool reader_map_pick(int client_x, int client_y, float client_w, float client_h,
                     MapPin* out, double* miss_dist) {
    if (miss_dist) *miss_dist = -1;
    if (!out || client_w <= 0 || client_h <= 0) return false;
    *out = {};
    void* win = find_map_window();
    if (!win) return false;

    // Markers report their position in the UI scene's own units, and the
    // mouse arrives in swap-chain pixels. When the UI is scaled those differ,
    // so the scene's own dimensions supply the ratio between them. No scene
    // means no way to compare, and guessing 1:1 would put the hit test
    // somewhere else entirely on a scaled UI.
    void* gui = read_ptr(g_app, off::GameApp::gui);
    void* scene = gui ? read_ptr(gui, off::ui_GameUI::s2d) : nullptr;
    if (!scene) return false;
    const int32_t sw = read_i32(scene, off::h2d_Scene::width);
    const int32_t sh = read_i32(scene, off::h2d_Scene::height);
    if (sw <= 0 || sh <= 0) return false;
    const double mx = client_x * ((double)sw / client_w);
    const double my = client_y * ((double)sh / client_h);

    // How near counts as clicking it, in scene units. Generous: map icons are
    // around 32 units and the player is aiming at the icon, not its origin.
    const double kReach = 26.0;

    void* markers = read_ptr(win, off::ui_win_MapWindow::markers);
    void* best = nullptr;
    double best_d = 1e18;
    for_each_marker(markers, [&](void* m) {
        uint8_t vis = 0;
        read(m, off::ui_win_map_MapMarker::visible, &vis);
        if (!vis) return;
        double ax = 0, ay = 0;
        if (!read(m, off::ui_win_map_MapMarker::absX, &ax) ||
            !read(m, off::ui_win_map_MapMarker::absY, &ay))
            return;
        const double dx = ax - mx, dy = ay - my;
        const double d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = m;
        }
    });
    // Report the nearest either way, then apply the reach.
    if (miss_dist && best) *miss_dist = sqrt(best_d);
    if (!best || best_d > kReach * kReach) return false;
    if (!marker_pos(best, &out->x, &out->y, &out->z)) return false;
    out->label = marker_label(best);
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
