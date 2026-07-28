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

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT_H = join(HERE, '..', 'host', 'src', 'offsets.gen.h');

const CANDIDATES = [
  'C:/Program Files (x86)/Steam/steamapps/common/Farever/hlboot.dat',
  'D:/SteamLibrary/steamapps/common/Farever/hlboot.dat',
  'E:/SteamLibrary/steamapps/common/Farever/hlboot.dat',
  'F:/SteamLibrary/steamapps/common/Farever/hlboot.dat',
];

const bootPath = process.argv[2] || CANDIDATES.find((c) => existsSync(c));
if (!bootPath) {
  console.error('hlboot.dat not found; pass its path');
  process.exit(1);
}

// Reuse the type parser by asking scan-hltypes for its JSON dump.
const typesPath = join(HERE, 'out', 'hl_types.json');
if (!existsSync(typesPath)) {
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
  ['ent.Hero', ['player', 'lockedTarget', 'autoTarget', 'weaponInHand', 'loadout']],
  ['ent.Unit', ['isInCombat', 'instigatedStatuses', 'skin']],
  // World position and facing, for the loot tracker's distance/arrow readout.
  ['ent.GameObject', ['posx', 'posy', 'posz', 'rotationZ']],
  // The application singleton: reaches the camera, and holds the hero too.
  ['GameApp', ['gameCamera', 'camera', 'hero', 'world']],
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
  ['st.Player', ['accountProgress', 'progress', 'name']],
  // Codex progress. Every one of these is a hxbit.MapData wrapping a Haxe
  // map behind an interface, so reading them needs the virtual hop.
  ['st.player.Progress', ['counters', 'unitsProgress', 'itemProgress',
                          'zones', 'achievements', 'pets']],
  ['hxbit.MapData', ['map']],
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

const hash = createHash('sha256').update(readFileSync(bootPath)).digest('hex');

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

mkdirSync(dirname(OUT_H), { recursive: true });
writeFileSync(OUT_H, lines.join('\n'), 'utf8');

console.log(`wrote ${OUT_H}`);
console.log(`  build sha256 ${hash.slice(0, 16)}...`);
let n = 0;
for (const [, fields] of WANT) n += fields.length;
console.log(`  ${WANT.length} classes, ${n} fields, 0 missing`);
