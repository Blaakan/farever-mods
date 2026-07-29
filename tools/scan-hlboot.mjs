#!/usr/bin/env node
// ---------------------------------------------------------------------------
// scan-hlboot.mjs
//
// Extracts the string table out of Farever's HashLink bytecode (hlboot.dat)
// and buckets it into the id vocabularies a plugin author actually needs:
// mounts, gliders, chests, recipes, skills, talents, statuses, monsters, gear.
//
// Why this works: Farever ships as INTERPRETED HashLink bytecode, not HL/C
// compiled to native. hlboot.dat therefore carries a complete string table -
// every string constant, field name and type name in the game - which is the
// authoritative source for the internal ids the plugin API hands you back.
//
// The classification rules below are not guesses. They were derived by
// frequency-analysing the real table (see docs/scanning.md).
//
// This reads your own installed game files, locally, for interoperability.
// It does not modify the game and writes only into tools/out/.
//
// tools/out/ is gitignored on purpose: the dumps are bulk game data belonging
// to Shiro Games. Commit the tool, not the haul.
//
// Usage:
//   node tools/scan-hlboot.mjs [path\to\hlboot.dat]
//   node tools/scan-hlboot.mjs --lua        also emit a Lua id reference
// ---------------------------------------------------------------------------

import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { requireBoot } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out');

const args = process.argv.slice(2);
const wantLua = args.includes('--lua');

const findBoot = () => requireBoot(args);

// --- HashLink bytecode reader ---------------------------------------------

class Reader {
  constructor(buf) {
    this.b = buf;
    this.p = 0;
  }
  u8() {
    return this.b[this.p++];
  }
  i32() {
    const v = this.b.readInt32LE(this.p);
    this.p += 4;
    return v;
  }
  // hl_read_index: 1, 2 or 4 byte variable-length integer with a sign bit.
  index() {
    const b = this.b[this.p++];
    if ((b & 0x80) === 0) return b & 0x7f;
    if ((b & 0x40) === 0) {
      const v = ((b & 0x1f) << 8) | this.b[this.p++];
      return b & 0x20 ? -v : v;
    }
    const c = this.b[this.p++];
    const d = this.b[this.p++];
    const e = this.b[this.p++];
    const v = ((b & 0x1f) << 24) | (c << 16) | (d << 8) | e;
    return b & 0x20 ? -v : v;
  }
  strings(n) {
    const size = this.i32();
    const data = this.b.subarray(this.p, this.p + size);
    this.p += size;
    const out = new Array(n);
    let pos = 0;
    for (let i = 0; i < n; i++) {
      const len = this.index();
      out[i] = data.toString('utf8', pos, pos + len);
      pos += len + 1;
    }
    return out;
  }
}

function parse(path) {
  const buf = readFileSync(path);
  const r = new Reader(buf);
  const magic = buf.toString('ascii', 0, 3);
  if (magic !== 'HLB') throw new Error(`not HashLink bytecode (magic '${magic}')`);
  r.p = 3;
  const version = r.u8();

  const flags = r.index();
  const nints = r.index();
  const nfloats = r.index();
  const nstrings = r.index();
  const nbytes = version >= 5 ? r.index() : 0;
  const ntypes = r.index();
  const nglobals = r.index();
  const nnatives = r.index();
  const nfunctions = r.index();
  const nconstants = version >= 4 ? r.index() : 0;
  const entrypoint = r.index();

  r.p += nints * 4;
  r.p += nfloats * 8;
  const strings = r.strings(nstrings);

  return {
    version,
    hasDebug: (flags & 1) === 1,
    counts: { nints, nfloats, nstrings, nbytes, ntypes, nglobals, nnatives, nfunctions, nconstants, entrypoint },
    strings,
  };
}

// --- Classification --------------------------------------------------------

// Engine, editor and framework namespaces. Filtered out first so the
// game-content buckets stay clean; these are Heaps/Haxe/Shiro internals, not
// anything a plugin can reference.
const ENGINE_NS = /^(domkit|hrt|h2d|h3d|hxd|hxsl|hxbit|haxe|dx|shiro|editor|prefab|shaders|world|lib|ui|sys|cdb|format|js|_?std)\./;

const CLASSES = ['Mage', 'Rogue', 'Priest', 'Warrior', 'Druid', 'Monk'];
const CLASS_RE = CLASSES.join('|');

// Equipment slot prefixes, matching the slot_name values the plugin API
// documents (Head, Neck, Shoulders, Chest, Back, Hands, Waist, Legs, Feet...).
const ARMOR_SLOTS = ['Head', 'Hair', 'Neck', 'Shoulders', 'Back', 'Hands', 'Waist', 'Legs', 'Feet', 'Torso', 'Chest'];
const WEAPONS = ['Sword', 'Staff', 'Bow', 'Daggers', 'Dagger', 'Axe', 'Mace', 'Hammer', 'Spear', 'Wand', 'Book', 'Shield'];

// Role codes seen throughout gear ids. Fig=Fighter/Warrior, Ass=Assassin/Rogue,
// Wiz=Wizard/Mage, Cle=Cleric/Priest; pairs mark gear shared by two classes.
const ROLE_CODES = ['Fig', 'Ass', 'Wiz', 'Cle'];

const RULES = [
  // --- collectibles the tracker cares about ---
  ['mounts', (s) => /^Mount_/.test(s)],
  ['gliders', (s) => /^Glider_/.test(s)],
  ['chests', (s) => /^Chest_/.test(s) && !ARMOR_SLOTS.some((a) => new RegExp(`^Chest_(${ROLE_CODES.join('|')})`).test(s))],
  ['recipes', (s) => /^Recipe_/.test(s)],

  // --- combat vocabulary ---
  ['statuses', (s) => /_Status$/.test(s)],
  ['telegraphs', (s) => /^Telegraph_/.test(s)],
  ['talents', (s) => new RegExp(`^(${CLASS_RE})_Talent_`).test(s)],
  ['skills', (s, fromSkillNs) => fromSkillNs === true],
  ['classSkills', (s) => new RegExp(`^(${CLASS_RE})_[A-Z]`).test(s)],
  ['weaponSkills', (s) => new RegExp(`^(${WEAPONS.join('|')})_[A-Za-z]+_(Skill\\d+|Passive)`).test(s)],
  ['mobSkills', (s) => /^[A-Z][A-Za-z]+_Skill\d+$/.test(s)],

  // --- world ---
  ['monsters', (s) => /^[A-Z][A-Za-z]+_Z\d+[A-Z](_[A-Za-z]+)?$/.test(s)],
  ['zoneContent', (s) => /^Z\d+_/.test(s)],

  // --- gear ---
  ['armor', (s) => new RegExp(`^(${ARMOR_SLOTS.join('|')})_`).test(s)],
  ['weapons', (s) => new RegExp(`^(${WEAPONS.join('|')})_`).test(s)],

  // --- non-content ---
  ['sourceFiles', (s) => /\.hx$/.test(s)],
  ['assets', (s) => /\.(png|jpg|dds|fbx|ogg|wav|mp3|fnt|cdb|prefab|fx|json)$/i.test(s)],
];

// Tuning constants share the Mount_/Glider_ prefixes with the real
// collectibles (Mount_PitchSpeed sits next to Mount_Boar_05). Anything whose
// tail reads like a physics parameter is config, not something you can own.
const PARAM_WORD =
  /(Speed|Multiplier|Tilt|Pitch|Yaw|Roll|Accel|Damp|Offset|Factor|Angle|Height|Radius|Duration|Delay|Threshold|Min|Max|Cooldown|Force|Gravity|Drag|Ratio|Scale|Limit|Time)$/;

function classify(strings) {
  const keys = [...RULES.map(([k]) => k), 'engine', 'other'];
  const buckets = Object.fromEntries(keys.map((k) => [k, []]));

  for (const raw of strings) {
    if (!raw) continue;
    let s = raw.trim();
    if (!s || s.length > 120) continue;

    // script.skills.Foo and script.skills.$Foo both denote the id Foo.
    // Normalise before the rules run so a namespaced status lands in
    // `statuses` under its bare name instead of leaking the prefix, but
    // remember it came from the skill namespace.
    const fromSkillNs = /^script\.skills\./.test(s);
    if (fromSkillNs) s = s.replace(/^script\.skills\.\$?/, '');
    if (!s) continue;

    // Bare prefixes ("Recipe_", "Telegraph_") are format strings, not ids.
    if (/_$/.test(s)) continue;

    if (!fromSkillNs && ENGINE_NS.test(s)) {
      buckets.engine.push(s);
      continue;
    }

    let placed = false;
    for (const [name, test] of RULES) {
      let hit = false;
      try {
        hit = test(s, fromSkillNs);
      } catch {
        hit = false;
      }
      if (hit) {
        buckets[name].push(s);
        placed = true;
        break;
      }
    }
    if (!placed) buckets.other.push(s);
  }

  // Physics tuning constants are not collectibles.
  for (const k of ['mounts', 'gliders']) {
    buckets[k] = buckets[k].filter((s) => !PARAM_WORD.test(s));
  }

  for (const k of Object.keys(buckets)) buckets[k] = [...new Set(buckets[k])].sort();
  return buckets;
}

// --- Lua emit --------------------------------------------------------------

function luaList(name, items, limit) {
  const take = limit ? items.slice(0, limit) : items;
  const body = take.map((s) => `    ${JSON.stringify(s)},`).join('\n');
  return `FAREVER_IDS.${name} = {\n${body}\n}\n`;
}

function emitLua(buckets) {
  const header = `-- Generated by tools/scan-hlboot.mjs from your own Farever install.
-- Internal ids extracted from hlboot.dat (HashLink bytecode string table).
-- Regenerate after a game patch: node tools/scan-hlboot.mjs --lua
--
-- NOT committed to the repo: this is game data. Keep it local.

FAREVER_IDS = {}

`;
  const parts = [
    luaList('mounts', buckets.mounts),
    luaList('gliders', buckets.gliders),
    luaList('chests', buckets.chests),
    luaList('recipes', buckets.recipes),
    luaList('statuses', buckets.statuses),
    luaList('talents', buckets.talents),
    luaList('monsters', buckets.monsters),
  ];
  return header + parts.join('\n') + '\nreturn FAREVER_IDS\n';
}

// --- Main ------------------------------------------------------------------

const path = findBoot();
console.log(`reading ${path}`);
const bc = parse(path);

console.log(`  HashLink bytecode v${bc.version}${bc.hasDebug ? ' (with debug info)' : ''}`);
console.log(`  ${bc.counts.nstrings.toLocaleString()} strings, ${bc.counts.nfunctions.toLocaleString()} functions, ${bc.counts.ntypes.toLocaleString()} types`);

const buckets = classify(bc.strings);

const CONTENT = ['mounts', 'gliders', 'chests', 'recipes', 'statuses', 'telegraphs', 'talents', 'skills', 'classSkills', 'weaponSkills', 'mobSkills', 'monsters', 'zoneContent', 'armor', 'weapons'];

console.log('\ngame content:');
for (const k of CONTENT) console.log(`    ${k.padEnd(14)} ${String(buckets[k].length).padStart(6)}`);
console.log('\nnon-content:');
for (const k of ['sourceFiles', 'assets', 'engine', 'other']) {
  console.log(`    ${k.padEnd(14)} ${String(buckets[k].length).padStart(6)}`);
}

mkdirSync(OUT, { recursive: true });
writeFileSync(join(OUT, 'strings.txt'), bc.strings.join('\n'), 'utf8');
writeFileSync(join(OUT, 'classified.json'), JSON.stringify(buckets, null, 2), 'utf8');
console.log(`\nwrote ${join(OUT, 'strings.txt')}`);
console.log(`wrote ${join(OUT, 'classified.json')}`);

if (wantLua) {
  writeFileSync(join(OUT, 'farever_ids.lua'), emitLua(buckets), 'utf8');
  console.log(`wrote ${join(OUT, 'farever_ids.lua')}`);
}

console.log('\nsamples:');
for (const k of CONTENT) {
  const v = buckets[k];
  if (!v.length) continue;
  const step = Math.max(1, Math.floor(v.length / 6));
  const sample = [];
  for (let i = 0; i < v.length && sample.length < 6; i += step) sample.push(v[i]);
  console.log(`  ${k} (${v.length}):`);
  for (const s of sample) console.log(`      ${s.slice(0, 84)}`);
}
