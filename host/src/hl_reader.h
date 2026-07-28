// ---------------------------------------------------------------------------
// hl_reader.h - the game-state surface the mods consume.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
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

bool reader_read_collection(Collection* out);
bool reader_read_unit_state(UnitState* out);
bool reader_read_inventories(Inventories* out);

// World position and facing of the local hero, for the navigator's distance
// and arrow readout. Cheap (four validated qword reads), safe to call at
// 20Hz from the pose thread.
bool reader_read_hero_pose(double* x, double* y, double* z, double* rot_z);

// Locates GameApp, the application singleton, which owns the game camera.
// One scan, cached and revalidated; call only after the hero is found, so
// the scan happens in-world rather than during a loading screen.
bool reader_locate_app(bool force_rescan);

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
