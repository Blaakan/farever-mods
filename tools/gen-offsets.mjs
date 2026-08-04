#!/usr/bin/env node
// ---------------------------------------------------------------------------
// gen-offsets.mjs
//
// Emits host/src/offsets.gen.h - the field offsets the standalone host's
// HashLink reader compiles against, derived from the game's own bytecode
// type table rather than hand-found.
//
// This is the bridge between tools/scan-hltypes.mjs and the C++ host. After a
// game patch, re-run it and rebuild: offsets regenerate instead of needing
// rediscovery, which is normally what kills a mod like this.
//
// The header also carries the build hash of the hlboot.dat it came from, so
// the host can refuse to read memory when the game has been patched out from
// under its offsets rather than walking stale pointers.
//
// Usage:  node tools/gen-offsets.mjs [path\to\hlboot.dat]
// ---------------------------------------------------------------------------

import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { requireBoot } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT_H = join(HERE, '..', 'host', 'src', 'offsets.gen.h');

const bootPath = requireBoot();
const bootSha = createHash('sha256').update(readFileSync(bootPath)).digest('hex');

// Reuse the type parser by asking scan-hltypes for its JSON dump - but only
// when the dump on disk describes *this* bytecode. A dump left over from
// before a patch parses perfectly and yields offsets that are wrong in the
// worst way available: plausible. Regenerating on a hash mismatch is what
// makes "re-run the generators after a patch" actually sufficient.
const typesPath = join(HERE, 'out', 'hl_types.json');
function dumpMatches() {
  if (!existsSync(typesPath)) return false;
  try {
    const src = JSON.parse(readFileSync(typesPath, 'utf8')).__source;
    return !!src && src.sha256 === bootSha;
  } catch {
    return false;   // unreadable, or written before __source existed
  }
}
if (!dumpMatches()) {
  console.log('generating tools/out/hl_types.json ...');
  execFileSync(process.execPath, [join(HERE, 'scan-hltypes.mjs'), '--json', bootPath], {
    stdio: 'inherit',
  });
}
const types = JSON.parse(readFileSync(typesPath, 'utf8'));

// What the host actually needs to walk. Keep this list tight: every entry is
// a promise the reader depends on, and a missing one should fail the build
// rather than surface as a wild pointer at runtime.
const WANT = [
  // `name` is the character name, and it is what a chat line's sender
  // resolves to: a message's `sender` is an ent.Unit, and for player chat
  // that unit is the speaker's Hero.
  ['ent.Hero', ['player', 'lockedTarget', 'autoTarget', 'weaponInHand',
                'loadout', 'specialization', 'name', '_level']],
  // Crafting. Jobs are per-character, and each carries the crafts that
  // character knows. The proxy class name is a hash of the anonymous
  // structure's shape, so a patch that changes a field of that struct
  // renames the class - at which point this generator fails loudly rather
  // than the reader walking a stale layout.
  // `skillMasteries` is which runes are *slotted*; the learned ones live on
  // Progress, because learning is permanent and slotting is a choice.
  ['st.player.HeroSpecialization', ['jobs', 'talents', 'skillMasteries']],
  ['hxbit.ObjProxy_3327ea72931d811ba796c031db6ffed0',
   ['job', 'level', 'knowledge', 'learnedCrafts', 'completedCrafts']],
  // `kind` is the unit's id in the unit sheet, and provably so: Unit.set_kind
  // does `inf = Data.unit.byId.get(kind)` (Unit.hx:686), so it is the key that
  // resolves the unit's own CastleDB row. On a hero that is the class -
  // Warrior / Rogue / Mage / Priest.
  ['ent.Unit', ['isInCombat', 'instigatedStatuses', 'skin', 'kind']],
  // World position and facing, for the loot tracker's distance/arrow readout.
  ['ent.GameObject', ['posx', 'posy', 'posz', 'rotationZ']],
  // The application singleton: reaches the camera, and holds the hero too.
  // `loadingState` is how the game itself answers "is a loading screen up":
  // GameApp.get_isLoading is literally `loadingState != 10` (GameApp.hx:50),
  // so 10 is the one value that means in the world and playing.
  // `layer` reaches the whole player roster - see the st.GameLayer note below.
  ['GameApp', ['gameCamera', 'camera', 'hero', 'world', 'gui', 'loadingState',
               'layer']],
  // The game's own map, read while it is open so a click on a POI can become
  // a waypoint. `windows` is the short list of window instances the UI holds;
  // finding the map means walking it for the right class, not scanning
  // memory. `visible` is h2d.Object's, so it says whether the map is up.
  // `gameRoot` is the way to the HUD, and it is the game's own route rather
  // than a guess: ui.GameUI.get_hud (GameUI.hx:33) is literally
  // `gameRoot?.hud`. The chat box hangs off that and NOT off `elements` -
  // walking `elements` for a ui.hud.ChatBox finds nothing, which cost a
  // whole test cycle to learn.
  ['ui.GameUI', ['windows', 'root', 's2d', 'gameRoot']],
  ['ui.GameUiRoot', ['hud']],
  ['ui.Hud', ['chat']],
  // A Flow's laid-out size. Every ui.BaseElement is one, so this is what
  // gives the chat message area its real width and height instead of a
  // guess derived from where the footer starts. `calculated*` is the box the
  // layout settled on; `content*` is what is inside it before padding.
  ['h2d.Flow', ['calculatedWidth', 'calculatedHeight', 'contentWidth',
                'contentHeight']],
  // The 2D scene the whole UI lives in. Its width/height are the units
  // markers report their screen position in, which is not the same as the
  // pixels the mouse arrives in when the UI is scaled - so the ratio between
  // this and the swap chain's size is what maps one onto the other.
  // `events` reaches hxd.SceneEvents, whose currentFocus is how "is the
  // player typing in the game's own box" is answered without guessing.
  ['h2d.Scene', ['width', 'height', 'events']],
  // `visible` alone is not "on screen": a closed window can keep the flag and
  // its last hit-test result, and acting on that would drop a waypoint at
  // whatever the player last hovered, days ago. Heaps detaches a closed
  // window from the scene, so `parent` going null is the second signal.
  ['ui.win.MapWindow', ['mouseCursor', 'nearClickableMarker', 'pinMarkers',
                        'markers', 'visible', 'parent', 'zoom']],
  // A marker's worldPos is a world-space vector - the same space the
  // navigator already works in - so no projection maths is involved at all.
  // absX/absY are the marker's own place on screen, which is what makes a
  // hit test possible at all: the mouse and the marker can be compared
  // directly and the map's zoom and panning never enter into it.
  ['ui.win.map.MapMarker', ['worldPos', 'visible', 'name', 'absX', 'absY']],
  ['ui.win.map.TextMarker', ['desc']],
  // `$App` is the class-value object holding App's statics, and `inst` is
  // the singleton itself - the whole reason startup needs no instance scan.
  ['$App', ['inst']],
  // Camera orientation. `direction` is where the camera is heading,
  // `curDirection` the smoothed value actually rendered - the game's own map
  // marker (ui.win.map.PlayerMarker) holds a camera for exactly this reason.
  ['client.BaseCamera', ['direction', 'curDirection', 'pitch', 'curPitch',
                         'curDistance', 'enabled', 'scene']],
  // The render camera itself, reached through the controller's scene. Its
  // pos and target ARE where the view sits and what it looks at, so the
  // screen's forward direction is target-minus-pos - no angle convention to
  // guess at, and no dependence on the hero. (The controller's own
  // h3d.scene.Object x/y/z are not the camera's world position: they read
  // as origin in game, which is what sent the first attempt to the
  // fallback path.)
  ['h3d.scene.Scene', ['camera']],
  ['h3d.Camera', ['pos', 'target']],
  ['h3d.VectorImpl', ['x', 'y', 'z']],
  // `chatClient` is the chat history's owner, and `isMe` is what tells the
  // local player apart when walking a group. `group` carries the party, which
  // is the payload of the st.Channel.Group constructor.
  // `hero` is how a player gets a world position and therefore a distance. It
  // is null for anyone whose hero has not been replicated to this client,
  // which is not the same thing as them being far away and must never be
  // reported as one.
  //
  // There are two fields here that both look like an identity and are not the
  // same thing. `uid` is a String and a replicated property (there is a
  // __net_mark_uid beside it); `__uid` is the I64 that st.BaseState carries
  // for every replicated state, assigned by the local host, and it is the one
  // that identifies a roster row within this session. Both are taken so that
  // a caller has to pick, rather than reading the String slot as a number.
  //
  // `removed` is st.BaseState's own tombstone flag, and it is what the game's
  // Manage Party window tests first (GroupWindow.hx:62) before it looks at a
  // roster entry at all. Reading the roster without it lists players the
  // client has already been told are gone.
  ['st.Player', ['accountProgress', 'progress', 'name', 'heroData',
                 'activityCtx', 'chatClient', 'isMe', 'group', 'hero',
                 'uid', '__uid', 'removed']],
  // What the Recent Loots feed watches. There is no loot event to hook - the
  // host never calls into the game - so the feed is a diff of these between
  // polls: experience and level tick up, `currencies` gains entries, and
  // `inventory` is the same list the atlas already reads for the bags.
  // `activityProgress` and `activityCtx` are the two fields named for the
  // thing the codex calls an activity - which includes NPC quests, even
  // though a quest has no authored activity row anywhere.
  // `kind` is the character's unit id - Warrior / Rogue / Mage / Priest - and
  // the game treats it as one: Hero.hx passes it straight to Unit.isInfElite
  // where a unit id is expected. It is what says which weapons this character
  // can equip, since each of those four units carries exactly one aptitude.
  ['st.player.HeroData', ['level', 'exp', 'currencies', 'inventory', 'name',
                          'worldLootLog', 'activityProgress', 'kind']],
  // Codex progress. Every one of these is a hxbit.MapData wrapping a Haxe
  // map behind an interface, so reading them needs the virtual hop.
  // `activities`, `elements` and `npcs` are what this character has already
  // done: a quest handed in, a chest opened. They are what makes a one-time
  // source disappear from the atlas once it is spent.
  // `weaponProgress` is weapon mastery: keyed by the weapon's CastleDB item
  // id, and the value counts kills made with it (Progress.hx:489).
  ['st.player.Progress', ['counters', 'unitsProgress', 'itemProgress',
                          'zones', 'achievements', 'pets',
                          'skillMasteriesLearnt', 'activities', 'elements',
                          'npcs', 'weaponProgress']],
  ['hxbit.MapData', ['map']],
  // The value in the codex map: not a bare count but a small record, whose
  // class name spells out its own shape.
  ['hxbit.ObjProxy_OkillCount_Int_rank_Int', ['killCount', 'rank']],
  // What this character has finished. Like the codex proxy above, these
  // class names spell out their own shape, so a patch that changes the
  // record renames the class and this generator fails loudly rather than the
  // reader quietly deciding nothing is done.
  ['hxbit.ObjProxy_Ocompleted_Float', ['completed']],
  ['hxbit.ObjProxy_OcompletedOnce_Bool_lastCompletion_Float',
   ['completedOnce', 'lastCompletion']],
  ['hxbit.ObjProxy_ad383d83eed03d0e5475cee203565222',
   ['goalsMap', 'dialog', 'bit']],
  // The weapon mastery record. One field, and the class name says which:
  // `exp` is a kill count, which the game divides by a per-weapon constant
  // to get mastery levels (Progress.hx:507).
  ['hxbit.ObjProxy_Oexp_Int', ['exp']],
  ['haxe.ds.StringMap', ['h']],
  ['st.player.AccountProgress', ['collection', 'bank', 'bankEquipment', 'bankNbSlots']],
  ['st.player.Collection', ['mounts', 'gliders', 'pets', 'gears', 'toys', 'emotes']],
  ['hxbit.ArrayProxyData', ['array']],
  // Ownership beyond the collection: bank, bags and equipped gear.
  ['st.Loadout', ['equipment', 'appearance', 'inventory']],
  ['st.Inventory', ['content', 'baseSize', 'addSize']],
  // Item identity and quality. kind/level/upgradeLevel are on the Gear base;
  // rarity is declared on Weapon.
  ['st.item.Gear', ['kind', 'level', 'upgradeLevel', 'slots']],
  ['st.item.Weapon', ['rarity']],
  // Containers the decoder walks. ArrayDyn wraps an ArrayBase (in practice an
  // ArrayObj); ArrayObj's `array` is a native varray whose elements start
  // immediately after the varray header.
  ['hl.types.ArrayDyn', ['array']],
  ['hl.types.ArrayObj', ['length', 'array']],
  ['hl.types.ArrayBase', ['length']],
  ['String', ['bytes', 'length']],

  // --- chat -----------------------------------------------------------------
  //
  // `history` is where every message the client has received lands, and it is
  // NOT a replicated property - there is no __net_mark_history beside it.
  // ChatClient.localReceiveMessage (ChatClient.hx:25-29) stamps localStamp
  // with sys_time() and does a bare push, with no trim and no ring buffer. So
  // this array is the whole session's chat, in order, and it is the durable
  // source; the ChatBox's own `messages` flow is only what is currently drawn
  // and gets emptied by reloadMessages.
  ['st.player.ChatClient', ['history', 'player', 'chat']],
  // The elements of that array are Haxe anonymous structures, so their fields
  // are found by name at runtime through read_virtual_fields rather than by
  // offset. The shape is
  //   { args, channel, localStamp, localTextId, notify, sender, text }
  // with channel an st.Channel enum: Local | All | AllSystem | Player(Player)
  // | Group(Group) | System(Player).
  //
  // `messages` is the flow the game draws lines into, and it holds two
  // classes: ChatBoxMessage for a real message and a bare ChatBoxLine for a
  // locally generated error. Reading it is what makes a custom command
  // possible - see chat.cpp for why an unknown `!command` never leaves the
  // client.
  ['ui.hud.ChatBox', ['messages', 'messageInput', 'channelDropdown',
                      'chatClient', 'messageIndex', 'footer']],
  ['ui.hud.ChatBoxLine', ['msgText']],
  ['ui.hud.ChatBoxMessage', ['message']],
  ['ui.comp.InputBox', ['input', 'hintText']],
  // What the player is typing, live. `interactive` is how focus is answered:
  // h2d.Interactive.hasFocus is `scene.events.currentFocus == this`
  // (Interactive.hx:311), which is three validated reads and no guessing.
  ['h2d.TextInput', ['interactive', 'cursorIndex']],
  ['h2d.Text', ['text']],
  ['hxd.SceneEvents', ['currentFocus']],
  // absX/absY put the game's own chat box on screen in scene units, which is
  // what lets the overlay cover exactly the message area and leave the input
  // box below it alone. Same scene-units-to-pixels ratio the map hit test
  // already undoes.
  ['h2d.Object', ['absX', 'absY', 'visible', 'parent', 'children', 'alpha']],
  // The developer console, so the host can stay out of its way. `bg.visible`
  // is what h2d.Console.isActive reads (Console.hx:297). The host never puts
  // anything into it - it is a password-gated admin surface (ui.Console.admin,
  // Console.hx:338) and reading whether it is open is the entire interest.
  ['ui.BaseUI', ['elements', 'console']],
  ['h2d.Console', ['bg', 'tf']],
  ['st.Group', ['players']],

  // --- the layer roster -----------------------------------------------------
  //
  // Every player the client knows about, which is more than the game will
  // show you. ui.win.GroupWindow.init (GroupWindow.hx:58-63) walks exactly
  // this array, splits it on a squared distance against
  // Const.UI.GroupWindow_NearDist - 100, whose own CastleDB description reads
  // "Other players within this distance are shown in the Manage Party window"
  // - and then draws the far bucket only when Config.prefs.admin is set,
  // under a header reading "(ADMIN) Other loaded players (". So the roster is
  // already in memory in full and the distance is presentation, not a rule.
  //
  // `hero` is how a player gets a world position, and so a distance; it is
  // null for anyone whose hero has not been replicated to this client, which
  // is a different thing from them being far away and must not be reported as
  // one.
  ['st.GameLayer', ['players']],
];

const hash = bootSha;

const lines = [];
const miss = [];

lines.push('// GENERATED by tools/gen-offsets.mjs - do not edit.');
lines.push('//');
lines.push('// Field offsets read out of Farever\'s HashLink bytecode type table and');
lines.push('// laid out with HashLink\'s own hl_get_obj_rt algorithm. Regenerate after a');
lines.push('// game patch:  node tools/gen-offsets.mjs');
lines.push('#pragma once');
lines.push('');
lines.push('#include <stdint.h>');
lines.push('');
lines.push(`// SHA-256 of the hlboot.dat these offsets came from. The reader must refuse`);
lines.push(`// to walk pointers when the running game does not match.`);
lines.push(`#define FMK_BUILD_SHA256 "${hash}"`);
lines.push('');
lines.push('namespace fmk {');
lines.push('namespace off {');
lines.push('');

for (const [cls, fields] of WANT) {
  const t = types[cls];
  if (!t) {
    miss.push(`${cls} (class not found)`);
    continue;
  }
  const ident = cls.replace(/[.$]/g, '_');
  lines.push(`// ${cls}  (sizeof=${t.size}${t.super ? `, extends ${t.super}` : ''})`);
  lines.push(`namespace ${ident} {`);
  lines.push(`    constexpr uint32_t SIZEOF = ${t.size};`);
  for (const f of fields) {
    const fd = t.fields.find((x) => x.name === f);
    if (!fd) {
      miss.push(`${cls}.${f}`);
      continue;
    }
    lines.push(
      `    constexpr uint32_t ${f} = 0x${fd.offset.toString(16)};` +
        `  // ${fd.kind}${fd.typeName ? ` : ${fd.typeName}` : ''}`
    );
  }
  lines.push(`}  // namespace ${ident}`);
  lines.push('');
}

lines.push('}  // namespace off');
lines.push('}  // namespace fmk');
lines.push('');

if (miss.length) {
  console.error('MISSING - the game layout changed, reader would be unsafe:');
  for (const m of miss) console.error(`  ${m}`);
  process.exit(1);
}

const text = lines.join('\n');
const changed = !existsSync(OUT_H) || readFileSync(OUT_H, 'utf8') !== text;

// Only write when something actually differs. Rewriting an identical header
// bumps its timestamp, and anything downstream that compares it against the
// built DLL then believes the build is stale when it is not.
mkdirSync(dirname(OUT_H), { recursive: true });
if (changed) writeFileSync(OUT_H, text, 'utf8');

console.log(`wrote ${OUT_H}`);
console.log(`  build sha256 ${hash.slice(0, 16)}...`);
let n = 0;
for (const [, fields] of WANT) n += fields.length;
console.log(`  ${WANT.length} classes, ${n} fields, 0 missing`);

// This header is compiled into the DLL, so writing it changes nothing until
// something rebuilds. Saying so here is the difference between "I re-ran the
// generator and it still does not work" and one more command - the
// requirement was only ever stated in a comment at the top of this file.
if (changed) {
  console.log('');
  console.log('  the offsets CHANGED - the DLL must be rebuilt to use them:');
  console.log('    host\\build.cmd   (then install.cmd)');
  console.log('  or let node tools/update.mjs --fix do the whole sequence.');
}
