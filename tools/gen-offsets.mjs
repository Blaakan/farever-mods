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
  ['ent.Hero', ['player', 'lockedTarget', 'autoTarget', 'weaponInHand',
                'loadout', 'specialization']],
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
  ['ent.Unit', ['isInCombat', 'instigatedStatuses', 'skin']],
  // World position and facing, for the loot tracker's distance/arrow readout.
  ['ent.GameObject', ['posx', 'posy', 'posz', 'rotationZ']],
  // The application singleton: reaches the camera, and holds the hero too.
  ['GameApp', ['gameCamera', 'camera', 'hero', 'world', 'gui']],
  // The game's own map, read while it is open so a click on a POI can become
  // a waypoint. `windows` is the short list of window instances the UI holds;
  // finding the map means walking it for the right class, not scanning
  // memory. `visible` is h2d.Object's, so it says whether the map is up.
  ['ui.GameUI', ['windows', 'root', 's2d']],
  // The 2D scene the whole UI lives in. Its width/height are the units
  // markers report their screen position in, which is not the same as the
  // pixels the mouse arrives in when the UI is scaled - so the ratio between
  // this and the swap chain's size is what maps one onto the other.
  ['h2d.Scene', ['width', 'height']],
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
  ['st.Player', ['accountProgress', 'progress', 'name', 'heroData',
                 'activityCtx']],
  // What the Recent Loots feed watches. There is no loot event to hook - the
  // host never calls into the game - so the feed is a diff of these between
  // polls: experience and level tick up, `currencies` gains entries, and
  // `inventory` is the same list the atlas already reads for the bags.
  // `activityProgress` and `activityCtx` are the two fields named for the
  // thing the codex calls an activity - which includes NPC quests, even
  // though a quest has no authored activity row anywhere.
  ['st.player.HeroData', ['level', 'exp', 'currencies', 'inventory', 'name',
                          'worldLootLog', 'activityProgress']],
  // Codex progress. Every one of these is a hxbit.MapData wrapping a Haxe
  // map behind an interface, so reading them needs the virtual hop.
  // `activities`, `elements` and `npcs` are what this character has already
  // done: a quest handed in, a chest opened. They are what makes a one-time
  // source disappear from the atlas once it is spent.
  ['st.player.Progress', ['counters', 'unitsProgress', 'itemProgress',
                          'zones', 'achievements', 'pets',
                          'skillMasteriesLearnt', 'activities', 'elements',
                          'npcs']],
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
