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

// Finds the local Hero. Cheap when the cached pointer still validates;
// falls back to a memory scan when it does not (first call, zone change).
bool reader_locate_hero(bool force_rescan);
void* reader_hero();

bool reader_read_collection(Collection* out);
bool reader_read_unit_state(UnitState* out);

// Writes the collection next to the game as farever-collection.json. Until
// the mods are ported onto this host, that file is the deliverable: it is the
// complete account collection, in a form anything can read.
void write_collection_json(const Collection& c);

}  // namespace fmk
