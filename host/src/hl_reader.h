// ---------------------------------------------------------------------------
// hl_reader.h - the game-state surface the mods consume.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fmk {

// The account-wide collection: exactly the six lists the game's own
// collection menu shows. This is authoritative ownership, not observation -
// it is what the account has, whether or not you ever equipped it while the
// mod was running.
struct Collection {
    bool valid = false;
    std::vector<std::string> mounts;
    std::vector<std::string> gliders;
    std::vector<std::string> pets;    // companions
    std::vector<std::string> gears;   // armor appearances
    std::vector<std::string> toys;
    std::vector<std::string> emotes;
    int32_t bank_slots = 0;
};

struct UnitState {
    bool valid = false;
    bool in_combat = false;
};

// A single owned item. `rarity` is 0..4 = common/uncommon/rare/epic/legendary
// decoded from the String field on st.item.Weapon (the CastleDB rarity ids).
// Everything that is not a weapon has no per-instance rarity: it is a static
// property of the kind, which the atlas data (tools/gen-atlas.mjs) supplies.
struct Item {
    std::string kind;
    int32_t level = 0;
    int32_t upgrade = 0;
    int32_t rarity = -1;      // -1 when the item carries no rarity field
    int32_t count = 1;        // stack size, from the slot's `count` field
    std::string cls;          // runtime class, e.g. st.item.Weapon
    std::string source;       // bank / bankEquipment / equipped / bags
};

// Everything the tracker needs to call a weapon or trinket "owned": the
// account bank plus, for the character currently logged in, their equipped
// gear and bags. Other characters' bags are not in this process at all, so
// those are accumulated across sessions by the layer above.
struct Inventories {
    bool valid = false;
    std::vector<Item> bank;
    std::vector<Item> bank_equipment;
    std::vector<Item> equipped;
    std::vector<Item> bags;
    std::string character;
    int32_t bank_slots = 0;
};

// Finds the local Hero. Cheap when the cached pointer still validates;
// falls back to a memory scan when it does not (first call, zone change).
bool reader_locate_hero(bool force_rescan);
void* reader_hero();

// One crafting job as the logged-in character has it. `learned` holds craft
// ids, which are the *produced* item ("HonedCopperPlate"), not the recipe
// item that taught it. The list mixes crafts unlocked automatically by job
// level with those learned from a recipe; telling them apart is the atlas
// data's job, since only it knows which crafts are recipe-gated.
struct JobState {
    std::string job;
    int32_t level = 0;
    double knowledge = 0;
    std::vector<std::string> learned;
};

// The crafting jobs of the character currently logged in. Per-character:
// another character on the same account knows different recipes.
bool reader_read_jobs(std::vector<JobState>* out);

// Skill runes. A rune is a one-use pickup that permanently teaches this
// character an upgrade to one skill, which they may then slot or not - so
// there are two lists and they answer different questions. `learned` is what
// the character owns and is what the atlas ticks off; `slotted` is the few
// currently in effect.
struct RuneState {
    std::vector<std::string> learned;
    std::vector<std::string> slotted;
};
bool reader_read_runes(RuneState* out);

// What this character has already finished. The ids are the game's own
// element and activity ids - `W1_Siagarta_WorldChest_16`, `POI_Rift_01` -
// which are exactly the ids the atlas records against a one-time source, so
// membership here is what retires a chest or a quest from a target list.
//
// Per character: a chest your Priest opened is still there for your Mage.
struct CompletionState {
    bool valid = false;
    std::vector<std::string> done;
};
bool reader_read_completion(CompletionState* out);

// One-shot diagnostic over the parts of that state whose shape is not yet
// settled. Writes to the log and nothing else. It exists because these are
// generic maps whose value type is erased, so what a key looks like and what
// a value means cannot be read off the bytecode - guessing at that is what
// made the map hit test read the gamepad's cursor.
void reader_probe_completion();

// What the codex records for one creature. A creature absent from the map
// has never been encountered at all.
struct UnitProgress {
    std::string unit;
    int32_t kills = 0;
    int32_t rank = 0;
};

// Codex progress per creature. Empty when the walk cannot be trusted.
bool reader_read_unit_progress(std::vector<UnitProgress>* out);

bool reader_read_collection(Collection* out);
bool reader_read_unit_state(UnitState* out);
bool reader_read_inventories(Inventories* out);

// One currency the character holds, e.g. {"Gold", 12045}.
struct Currency {
    std::string kind;
    int64_t count = 0;
};

// Everything the Recent Loots feed compares against its last reading. There
// is no loot event to hook - the host never calls into the game - so what a
// feed of "you just picked this up" really is, is the difference between two
// of these. Deliberately narrow so it can be polled several times a second:
// the bags and the currency purse, not the bank, the codex or the collection.
struct LootState {
    bool valid = false;
    int32_t level = 0;
    int32_t exp = 0;
    std::vector<Item> bags;
    std::vector<Currency> currencies;
};

bool reader_read_loot_state(LootState* out);

// The game's own map, read while it is open. Every marker on it carries its
// world position in the same axes the navigator already works in, so turning
// one into a waypoint needs no projection and no writes.
//
// The map's own `nearClickableMarker` looked like the obvious source and is
// not: it belongs to the gamepad crosshair (`crosshair`, `crosshairCheckbox`,
// a `showCrosshair` static) and stays null with a mouse. `mouseCursor` is
// likewise part of the debug readout. What does work is the marker list plus
// each marker's own screen position.

struct MapPin {
    double x = 0, y = 0, z = 0;
    std::string label;
};

struct MapState {
    bool open = false;      // the map window is in the UI's open-window list
    // The player's own pins. Placing one is a perfectly good way to say
    // "take me there", so the navigator mirrors them.
    std::vector<MapPin> pins;
    // Diagnostics, logged on every open and close. If a mechanism here ever
    // stops working, one line says which of these went empty.
    void* window = nullptr;
    bool visible = false;
    bool parented = false;
    int markers = 0;
    int scene_w = 0, scene_h = 0;   // against the swap chain, this is the
                                    // scale the hit test has to undo
    void* mouse_cursor = nullptr;
    void* near_clickable = nullptr;
};

bool reader_read_map_state(MapState* out);

// The marker nearest a point on screen, for turning a click into a waypoint.
// `client_*` are swap-chain pixels - the space the mouse arrives in - which
// this maps onto the UI scene's own units before comparing. Returns false
// when the map is closed or nothing is within reach of that point.
// `miss_dist`, when given, receives how far the nearest visible marker was in
// scene units even on a failure - which is the only way to tell "you clicked
// empty map" from "the reach is too tight".
bool reader_map_pick(int client_x, int client_y, float client_w, float client_h,
                     MapPin* out, double* miss_dist = nullptr);

// World position and facing of the local hero, for the navigator's distance
// and arrow readout. Cheap (four validated qword reads), safe to call at
// 20Hz from the pose thread.
bool reader_read_hero_pose(double* x, double* y, double* z, double* rot_z);

// Locates GameApp, the application singleton, which owns the game camera
// and the hero. Cheap when it works: App.inst is a static, so no scan is
// involved. `allow_scan` permits the ~8GB fallback sweep - pass false while
// the game is still starting, since App.inst is simply not set yet and
// waiting a moment costs nothing.
bool reader_locate_app(bool allow_scan);

// Where the render camera sits and what it looks at, in world space:
// GameApp.gameCamera -> BaseCamera.scene -> Scene.camera -> h3d.Camera's
// own pos/target vectors. The screen's forward direction is target minus
// pos, which needs no angle convention and no hero. Six validated qword
// reads behind a five-link pointer walk; any broken link returns false and
// the navigator falls back to the hero's facing.
bool reader_read_camera(double* px, double* py, double* pz,
                        double* tx, double* ty, double* tz);

// Writes the collection next to the game as farever-collection.json. Until
// the mods are ported onto this host, that file is the deliverable: it is the
// complete account collection, in a form anything can read.
void write_collection_json(const Collection& c);

// Writes farever-inventory-<character>.json next to the game. One file per
// character: the account bank repeats in each, but bags and equipped gear are
// character-scoped and offline characters are not in this process, so the
// union across files is how "owned on any character" gets answered.
void write_inventory_json(const Inventories& inv, const std::string& character);

}  // namespace fmk
