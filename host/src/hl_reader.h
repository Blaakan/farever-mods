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
    // The character's class, as the game's own unit id: Warrior, Rogue, Mage
    // or Priest. Empty when it does not read back as one of those, because
    // the only thing it is used for - deciding which weapons this character
    // can equip - is better left unanswered than answered wrongly.
    std::string hero_class;
    // The item id of the weapon in the main hand, or empty for an empty hand.
    // This is the game's own `activeWeapon`: equipment slot 0.
    std::string active_weapon;
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

// Weapon mastery. The game levels each weapon separately, by kills made with
// it: `weapon` is the weapon's CastleDB item id - the key the game itself
// uses - and `kills` is the raw counter behind it.
//
// The levels are not stored. The game derives them (st/player/Progress.hx):
//
//   points = min(floor(kills / killsPerPoint), maxPoints)
//
// where killsPerPoint is 20, or 26 for a weapon that goes in the off hand,
// and maxPoints is (upgradeable skills on the weapon) x (max rank - 1). Both
// of those are properties of the weapon rather than of the character, so
// they come from the atlas data (tools/gen-atlas.mjs) rather than from here.
//
// Per character, and only the logged-in one: a weapon your Warrior has
// mastered is untouched for your Mage.
struct WeaponMastery {
    std::string weapon;
    int32_t kills = 0;
};
bool reader_read_weapon_mastery(std::vector<WeaponMastery>* out);

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

// True while a loading screen is up. `GameApp.get_isLoading` is literally
// `loadingState != 10` (src/GameApp.hx:50), so this is one int off the app
// the pose thread already re-derives every tick.
//
// Deliberately NOT folded into "is a character in the world". A loading
// screen is a moment when nothing of ours should be *drawn*; it is not a
// moment when the character has gone. Treating it as the latter threw away
// the collection snapshot on every zone change and made the atlas rebuild
// itself for a character that had never left.
bool reader_is_loading();

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

// --- chat -------------------------------------------------------------------
//
// Two separate surfaces, and they answer different questions.
//
// `st.player.ChatClient.history` is the durable one: every message this
// client has received, in arrival order. It is NOT a replicated property -
// there is no `__net_mark_history` beside it - and `localReceiveMessage`
// (ChatClient.hx:25-29) stamps `localStamp` with sys_time() and does a bare
// push. No trim, no ring buffer. So the whole session is there, indices are
// stable, and tailing it is "read the length, decode what is new".
//
// `ui.hud.ChatBox` is the display, and it is ephemeral: `reloadMessages`
// empties the flow outright. It is read for three things history cannot
// answer - where the game's own box sits on screen, what is being typed into
// it, and whether the last line drawn was one the game generated locally.

// Which channel a line arrived on. These are st.Channel's own constructor
// order, which is exactly what a HashLink enum stores in its index field.
enum ChatChannel : int32_t {
    kChatUnknown   = -1,
    kChatLocal     = 0,
    kChatAll       = 1,
    kChatAllSystem = 2,
    kChatPlayer    = 3,   // a whisper; the constructor carries the other end
    kChatGroup     = 4,
    kChatSystem    = 5,
};

// Every string here is UTF-8, decoded properly from the game's UTF-16 rather
// than narrowed to ASCII. This is the one surface carrying text a person
// typed, so an accented name or a sentence in another language has to survive.
// The overlay's font atlas is ASCII-only and will draw blanks where it has no
// glyph, but the data - and farever-chat-log.txt - is right.
struct ChatMessage {
    ChatChannel channel = kChatUnknown;
    std::string sender;   // the speaking character; empty on a system line
    std::string text;
    // The far end of a whisper. The Player and Group constructors carry a
    // payload, and reading an enum's parameters needs the construct table
    // rather than a fixed offset - so this is best-effort and validated by
    // class name. Empty means "could not read it", never a guess.
    std::string other;
    double stamp = 0;     // ChatClient's own sys_time() arrival stamp
    bool mine = false;    // the logged-in character said it
};

// History from `from` onward, at most `max` messages. `total` always receives
// the history length, even when nothing is decoded - so a caller that is
// already up to date learns that from one cheap read rather than by decoding
// the session again several times a second.
bool reader_read_chat(int32_t from, int32_t max, std::vector<ChatMessage>* out,
                      int32_t* total);

// The game's own chat box: where it is, what is in its input, and the newest
// line it drew.
struct ChatBoxState {
    bool found = false;

    // The MESSAGE AREA is being drawn - not merely that the ChatBox object
    // exists. It is the `messages` flow's own visible flag, the same flag on
    // every object up its parent chain (which passes through the ChatBox), and
    // that chain ending at the UI scene. Anything the caller aligns to the
    // bounds below must be gated on this: a collapsed or detached box reports
    // false, and drawing over its last-known bounds puts an opaque window over
    // nothing.
    bool visible = false;
    bool focused = false;      // its input holds keyboard focus
    std::string input;         // what is being typed, right now

    // The `messages` flow, in the UI scene's own units. `scene_w/h` is the
    // ratio that turns those into swap-chain pixels - the same conversion
    // reader_map_pick already undoes for the map, and the reason the overlay
    // can sit exactly over the message area and leave the input box alone.
    //
    // All nought when the rectangle could not be read in full. A partly-read
    // rectangle is not reported: the origin is the caller's window position,
    // and an unread origin defaulting to 0,0 is a placement, not a failure.
    double msg_x = 0, msg_y = 0;
    double msg_w = 0, msg_h = 0;
    int32_t scene_w = 0, scene_h = 0;

    // `line_count` is the flow's own child count, whole. The flow is appended
    // to and never trimmed (ChatBox.hx:126-129; only reloadMessages clears
    // it), so this keeps climbing all session and a caller can watch it for
    // growth. It is not the number of lines inspected: the walk only looks at
    // the tail, because looking at all of them every poll would cost more than
    // the answer is worth.
    //
    // `last_line` is the newest child of that flow that is a line at all. A
    // real message is a `ui.hud.ChatBoxMessage`; a line the game generated
    // locally - including the "Unknown chat command " echo - is a bare
    // `ui.hud.ChatBoxLine`, and `last_is_error` is that distinction.
    int32_t line_count = 0;
    std::string last_line;
    bool last_is_error = false;
};
bool reader_read_chatbox(ChatBoxState* out);

// True while the developer console is open (`h2d.Console.bg.visible`, which
// is what Console.isActive reads at Console.hx:297). The host reads this only
// to stay out of the way: the console owns the `/` key, and it is a
// password-gated admin surface (ui.Console.admin, Console.hx:338) that this
// host has no business putting anything into.
bool reader_console_open();

// --- the layer roster -------------------------------------------------------
//
// Every player st.GameLayer.players holds. The game's own Manage Party window
// walks exactly this array (ui.win.GroupWindow.init, findex 20537,
// src/ui/win/GroupWindow.hx:58-66): it squares Const.UI.GroupWindow_NearDist -
// 100, described in CastleDB as "Other players within this distance are shown
// in the Manage Party window" - and splits the array on that, then draws the
// far bucket only when Config.prefs.admin is set. So the whole roster is
// already in this client's memory and the distance is presentation.
//
// Three things this cannot tell you, which the caller must not paper over:
//
//  * The roster is what the SERVER chose to replicate to this client. It is
//    not provably every player on the shard, and nothing here should be
//    worded as if it were.
//  * st.Player.group is network property bit 12 (st.Player.__net_mark_group,
//    hxbit/Macros.hx:2104) and sits in the conditional visibility mask, so it
//    reads null for everyone except the local player. Whether some other
//    player is already in somebody's party is simply not in this process.
//  * A row without a hero is a row with no position, full stop. That is not
//    the same as "far away" - the game's own window happens to bucket the two
//    together (GroupWindow.hx:62 sends a null hero straight to the far list),
//    and this reader deliberately does not.
struct RosterPlayer {
    std::string name;
    // st.BaseState.__uid, the session-local id hxbit assigns to a replicated
    // state. NOT st.Player.uid, which is a separate replicated String; both
    // offsets exist and reading one for the other would be a pointer read as
    // a number.
    int64_t uid = 0;
    bool me = false;
    bool in_my_group = false;
    // False means this client has no position for that player: either the
    // hero has not been replicated to us, or the coordinate read did not come
    // back. Both are "cannot say", and neither is a distance. Never render
    // this as a number and never render it as "far".
    bool has_hero = false;
    // ent.GameObject posx/posy/posz, which is the same value the game itself
    // compares: Entity.set_posx writes position.x and then posx in the same
    // breath (src/ent/Entity.hx:69-70). Only meaningful when has_hero.
    double x = 0, y = 0, z = 0;
    // The player's class, as ent.Unit.kind reads on their hero: the unit id
    // that Unit.set_kind (Unit.hx:686) uses as the key into Data.unit.byId,
    // which on a hero is the class - Warrior, Rogue, Mage or Priest.
    //
    // Empty means there was no hero to read it off, which is the same absence
    // as has_hero being false and must be shown as an absence. Anything that
    // is not one of the four is passed through verbatim rather than mapped
    // onto one of them: Inventories::hero_class blanks the unrecognised case
    // because it FILTERS by class and a word we do not understand is no basis
    // for hiding items, but a column that merely displays it has nothing to
    // gain from replacing a real read with a blank, and everything to lose
    // from rounding it to the nearest class we do know.
    std::string hero_kind;
};

struct RosterState {
    bool valid = false;
    std::vector<RosterPlayer> players;
    // The local player's own party, which IS readable - st.Player.group is
    // only replicated for oneself.
    std::vector<std::string> group;     // member names, leader first
    bool i_am_leader = false;
};

// Reads the roster in one pass. Returns false rather than a half-filled
// struct: no hero, no GameApp, no players array, or a length that cannot be
// trusted all mean "cannot read", and the caller keeps whatever it had.
//
// Region lookups are cached per read cycle, as everywhere else here, so a
// caller polling this on its own thread wants mem_flush_cache() at the top of
// its cycle the way the worker thread already does.
bool reader_read_roster(RosterState* out);

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
