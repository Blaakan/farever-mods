#!/usr/bin/env node
// ---------------------------------------------------------------------------
// gen-atlas.mjs
//
// Builds the two data files the host's Collection Atlas UI consumes:
//
//   tools/out/atlas/farever-atlas.tsv        every item that exists, by category
//   tools/out/atlas/farever-atlas-icons.dds  one BC7 atlas of 64px icons
//
// Everything comes from the game's own data:
//   * data.cdb (res.light.pak) - the full CastleDB: items, types, rarities,
//     units (pets are Critter units), loot tables, crafts. English names and
//     descriptions live here; only translations ship as lang XML.
//   * res.pak UI/Portraits/**  - one 256px BC7 DDS per item/unit. The 64px
//     mip is copied block-for-block into the atlas, so no image decoding
//     happens at all: BC7 blocks are 4x4-independent.
//   * shop stalls in the map/NPC prefabs, for "sold by" acquisition hints.
//
// Categories match the UI pages: appearances, mounts, pets, gliders,
// trinkets, weapons. Classification is data-driven off the itemType
// inheritance chain (Sword -> OHWeapon -> ... -> Weapon), not name prefixes -
// SparkHorse_01 is a mount with no Mount_ prefix.
//
// With --install (default when the game is found) the outputs are also
// copied next to Farever.exe, where the host looks for them.
// ---------------------------------------------------------------------------

import { existsSync, mkdirSync, writeFileSync, readFileSync, readdirSync,
         copyFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { openPak } from './lib/pak.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out', 'atlas');

const GAME_CANDIDATES = [
  'C:/Program Files (x86)/Steam/steamapps/common/Farever',
  'D:/SteamLibrary/steamapps/common/Farever',
  'E:/SteamLibrary/steamapps/common/Farever',
  'F:/SteamLibrary/steamapps/common/Farever',
];

function findGame() {
  if (process.env.FAREVER_DIR && existsSync(process.env.FAREVER_DIR))
    return process.env.FAREVER_DIR;
  for (const c of GAME_CANDIDATES) if (existsSync(join(c, 'hlboot.dat'))) return c;
  return null;
}

const game = findGame();
if (!game) {
  console.error('game not found; set FAREVER_DIR');
  process.exit(1);
}
const install = !process.argv.includes('--no-install');

// --- load the CastleDB ------------------------------------------------------

const light = openPak(join(game, 'res.light.pak'));
const cdb = JSON.parse(light.read('data.cdb').toString('utf8'));
light.close();
const sheet = (n) => cdb.sheets.find((s) => s.name === n);

const items = sheet('item').lines;
const itemById = new Map(items.map((l) => [l.id, l]));
const itemTypes = new Map(sheet('itemType').lines.map((l) => [l.id, l]));
const units = sheet('unit').lines;
const lootTables = sheet('lootTable').lines;
const crafts = sheet('craft').lines;
const rarities = sheet('rarity').lines.map((l) => l.id);   // Common..Legendary

function typeChain(typeId) {
  const chain = [];
  let cur = itemTypes.get(typeId);
  while (cur && chain.length < 12) {
    chain.push(cur.id);
    cur = cur.inherit ? itemTypes.get(cur.inherit) : null;
  }
  return chain;
}

// --- categories -------------------------------------------------------------

const TRINKET_TYPES = new Set(['GearTrinket', 'GearNeck', 'GearFinger']);

function categoryOf(item) {
  const chain = typeChain(item.type || '');
  if (chain.includes('Weapon')) return 'weapons';
  if (chain.includes('Armor')) return 'appearances';
  if (TRINKET_TYPES.has(item.type)) return 'trinkets';
  if (chain.includes('GearGlider')) return 'gliders';
  if (item.type === 'Mount') return 'mounts';
  return null;
}

// Unit flags, in declaration order of the flags column.
const UNIT_NO_COLLECTION = 1 << 20;

// --- acquisition ------------------------------------------------------------

// Loot-table ids are already meaningful (Manfish, Vault_Z1_2, Nepsilon_LT2),
// but some are generic containers reached from many parents; for those the
// parents are the story. Build item -> direct tables, and table -> parents.
const tablesByItem = new Map();     // item id -> [{table, proba}]
const tableParents = new Map();     // table id -> Set(parent table id)
for (const t of lootTables) {
  for (const e of t.loot || []) {
    if (e.item) {
      if (!tablesByItem.has(e.item)) tablesByItem.set(e.item, []);
      tablesByItem.get(e.item).push({ table: t.id, proba: e.proba ?? 1 });
    }
    if (e.lootTable) {
      if (!tableParents.has(e.lootTable)) tableParents.set(e.lootTable, new Set());
      tableParents.get(e.lootTable).add(t.id);
    }
  }
}

const unitById = new Map(units.map((u) => [u.id, u]));
const UNIT_BOSS = 1 << 4;
const UNIT_MINIBOSS = 1 << 5;

// A table whose id says nothing about *where* - climb to its parents instead.
const GENERIC_TABLE = /^(Humanoid|HumanoidWeights|Clothes|Scrolls|Gems|Bags|Potions|Leather|Debris|Ores|Plants|Soulstone|Scrap|Scrap_Rare|Empty_LootTable|WorldLootTest|.*Weights)$/;

function tableLabel(id) {
  const unit = unitById.get(id) || unitById.get(id.replace(/_?LT2$/, ''));
  if (unit) {
    const name = unit.texts?.name || unit.id;
    if (unit.flags & UNIT_BOSS) return `world boss ${name}`;
    if (unit.flags & UNIT_MINIBOSS) return `miniboss ${name}`;
    return `${name} enemies`;
  }
  let m;
  if ((m = id.match(/^Vault_Z(\d+)_(\d+)$/))) return `Vault of zone ${m[1]} (tier ${m[2]})`;
  if ((m = id.match(/^Ramburg_(\d+)$/))) return 'Ramburg';
  if (/^Rift_/.test(id)) return 'Demonic Rifts';
  if (/^Demon_Soulstone_Z(\d+)/.test(id))
    return `demon soulstones (zone ${id.match(/Z(\d+)/)[1]})`;
  if (/Crate$/.test(id)) return `${id.replace(/Crate$/, '')} crates`.replace(/^ /, '');
  if (/Activity$/.test(id)) return `${id.replace(/Activity$/, '')} activities`.trim();
  return id.replace(/_/g, ' ');
}

function lootSources(itemId) {
  const direct = tablesByItem.get(itemId) || [];
  const labels = new Map();   // label -> best proba
  for (const { table, proba } of direct) {
    let ids = [table];
    // Generic container: one step up to whoever references it.
    if (GENERIC_TABLE.test(table)) {
      const parents = [...(tableParents.get(table) || [])]
        .filter((p) => !GENERIC_TABLE.test(p));
      if (parents.length && parents.length <= 6) ids = parents;
      else if (parents.length) ids = ['many enemies'];
    }
    for (const id of ids) {
      const label = id === 'many enemies' ? 'enemy drops' : tableLabel(id);
      const prev = labels.get(label);
      if (prev === undefined || proba > prev) labels.set(label, proba);
    }
  }
  return [...labels.entries()]
    .sort((a, b) => b[1] - a[1])
    .slice(0, 4)
    .map(([label, proba]) => (proba <= 0.011 ? `${label} (rare)` : label));
}

const craftByItem = new Map();
for (const c of crafts) {
  if (c.item) craftByItem.set(c.item, c);
}

// Shop stalls live in binary prefabs as "@shop" followed by @ItemId tokens.
// The serialization is hide's binary tree, which we do not parse; a windowed
// token scan over the raw bytes is enough to learn *that* an item is sold.
const soldItems = new Set();
function scanShops(pak, pathFilter) {
  for (const f of pak.files) {
    if (!pathFilter(f.path)) continue;
    const buf = pak.read(f);
    const s = buf.toString('latin1');
    let i = -1;
    while ((i = s.indexOf('@shop', i + 1)) !== -1) {
      const win = s.slice(i, i + 4096);
      for (const m of win.matchAll(/@([A-Za-z][A-Za-z0-9_]{2,63})/g)) {
        if (itemById.has(m[1])) soldItems.add(m[1]);
      }
    }
  }
}

const respak = openPak(join(game, 'res.pak'));
scanShops(respak, (p) => p.startsWith('Gameplay/Prefabs/') && p.endsWith('.prefab'));
try {
  const mapPak = openPak(join(game, 'res.map.pak'));
  scanShops(mapPak, (p) => p.endsWith('.prefab'));
  mapPak.close();
} catch (e) {
  console.warn('res.map.pak not scanned:', e.message);
}
console.log(`shops: ${soldItems.size} distinct items seen behind @shop stalls`);

function acquisitionOf(itemId) {
  const parts = [];
  const craft = craftByItem.get(itemId);
  if (craft) {
    let s = `Craft: ${craft.job || '?'}${craft.level ? ` (lvl ${craft.level})` : ''}`;
    if (craft.unlockSource) s += `, recipe from ${craft.unlockSource}`;
    parts.push(s);
  }
  const loot = lootSources(itemId);
  if (loot.length) parts.push(`Drops: ${loot.join(', ')}`);
  if (soldItems.has(itemId)) parts.push('Sold by a merchant');
  return parts;
}

// --- text cleanup -----------------------------------------------------------

// Descriptions carry markup: [GameTerm] links and ::var:: value refs. The
// host's font atlas covers ASCII 32-126 only, so fold typographic characters
// down rather than render them as gaps.
const ASCII_FOLD = {
  '’': "'", '‘': "'", '“': '"', '”': '"',
  '–': '-', '—': '-', '…': '...', ' ': ' ',
};
function cleanText(s) {
  if (!s) return '';
  return s
    .replace(/::([^:]*)::/g, (_, v) => v.replace(/^ref_/, ''))
    .replace(/\[([^\]]*)\]/g, '$1')
    .replace(/[^\x00-\x7f]/g, (c) => ASCII_FOLD[c] ?? '?')
    .replace(/[\t\r\n]+/g, ' ')
    .trim();
}

// --- build the entry list ---------------------------------------------------

const CATEGORY_ORDER = ['appearances', 'mounts', 'pets', 'gliders',
                        'trinkets', 'weapons'];
const entries = [];   // { category, id, name, rarity, desc, acquire, gfxFile }

for (const l of items) {
  const category = categoryOf(l);
  if (!category) continue;
  const gfx = l.gfx || {};
  entries.push({
    category,
    id: l.id,
    name: cleanText(l.texts?.name) || l.id,
    rarity: Math.max(0, rarities.indexOf(l.rarity ?? 'Common')),
    desc: cleanText(l.texts?.desc),
    // Acquisition strings embed unit display names, which can be non-ASCII.
    acquire: acquisitionOf(l.id).map(cleanText),
    gfxFile: gfx.file || '',
    gfxSize: gfx.size || 0,
  });
}

// Pets are Critter units, not items. A pet's acquisition is capture (the net
// works on any critter in the world) plus whatever grants its Critter_<id>
// collection item, when one exists.
for (const u of units) {
  if (u.type !== 'Critter') continue;
  if (u.id === 'Base_Critter') continue;              // template, not a pet
  if (u.flags & UNIT_NO_COLLECTION) continue;
  const gfx = u.gfx || {};
  const viaItem = itemById.has(`Critter_${u.id}`)
    ? acquisitionOf(`Critter_${u.id}`).map(cleanText) : [];
  entries.push({
    category: 'pets',
    id: u.id,
    name: cleanText(u.texts?.name) || u.id,
    rarity: 0,
    desc: '',
    acquire: [...viaItem, 'Capture in the wild (Capture Net)'],
    gfxFile: gfx.file || '',
    gfxSize: gfx.size || 0,
  });
}

entries.sort((a, b) =>
  CATEGORY_ORDER.indexOf(a.category) - CATEGORY_ORDER.indexOf(b.category) ||
  a.name.localeCompare(b.name) || a.id.localeCompare(b.id));

// --- icon atlas: repack 64px BC7 mips, no decoding --------------------------
//
// Every portrait is a 256x256 BC7 DDS with a full mip chain; mip 2 is the
// 64x64 level: 16x16 blocks of 16 bytes. The atlas is 32 cells wide.

const CELL = 64;
const COLS = 32;
const BLOCKS_PER_CELL = CELL / 4;                    // 16
const ATLAS_W = COLS * CELL;                         // 2048
const DDS_HEADER = 148;                              // 4 + 124 + DX10(20)
const MIP2_OFFSET = DDS_HEADER + 65536 + 16384;      // past mips 0 and 1
const MIP2_SIZE = 4096;

// Keyed by file AND declared cell geometry: two entries sharing a file but
// disagreeing on size must not share a cached verdict.
const iconCache = new Map();
let cellCount = 0;
const cellData = [];   // per used cell: Buffer(4096) of BC7 blocks

function iconCellFor(entry) {
  const { gfxFile, gfxSize } = entry;
  if (!gfxFile) return -1;
  const key = `${gfxFile}|${gfxSize}`;
  if (iconCache.has(key)) return iconCache.get(key);

  const f = respak.find(gfxFile);
  if (!f) { console.warn(`  no pak entry for ${gfxFile} (${entry.id})`); return -1; }
  const dds = respak.read(f);
  if (dds.toString('ascii', 0, 4) !== 'DDS ' ||
      dds.toString('ascii', 84, 88) !== 'DX10' ||
      dds.readUInt32LE(128) !== 98 /* BC7_UNORM */ ||
      dds.readUInt32LE(16) !== 256 || dds.readUInt32LE(12) !== 256 ||
      dds.readUInt32LE(28) < 3 || gfxSize !== 256) {
    console.warn(`  unexpected portrait format for ${gfxFile} (${entry.id})`);
    iconCache.set(key, -1);
    return -1;
  }
  const mip = dds.subarray(MIP2_OFFSET, MIP2_OFFSET + MIP2_SIZE);
  const cell = cellCount++;
  cellData.push(Buffer.from(mip));
  iconCache.set(key, cell);
  return cell;
}

for (const e of entries) e.icon = iconCellFor(e);
respak.close();

const rows = Math.ceil(cellCount / COLS);
const ATLAS_H = rows * CELL;
const atlasBlocksPerRow = ATLAS_W / 4;               // 512
const atlas = Buffer.alloc(atlasBlocksPerRow * (ATLAS_H / 4) * 16);
for (let cell = 0; cell < cellCount; cell++) {
  const cx = cell % COLS;
  const cy = (cell / COLS) | 0;
  const src = cellData[cell];
  for (let r = 0; r < BLOCKS_PER_CELL; r++) {
    const dstBlockRow = cy * BLOCKS_PER_CELL + r;
    const dst = (dstBlockRow * atlasBlocksPerRow + cx * BLOCKS_PER_CELL) * 16;
    src.copy(atlas, dst, r * BLOCKS_PER_CELL * 16, (r + 1) * BLOCKS_PER_CELL * 16);
  }
}

// Standard DDS header so ordinary viewers can open the atlas too.
function ddsFile(w, h, payload) {
  const head = Buffer.alloc(DDS_HEADER);
  head.write('DDS ', 0, 'ascii');
  head.writeUInt32LE(124, 4);                        // dwSize
  head.writeUInt32LE(0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, 8);  // caps|h|w|pf|linear
  head.writeUInt32LE(h, 12);
  head.writeUInt32LE(w, 16);
  head.writeUInt32LE(payload.length, 20);            // linear size
  head.writeUInt32LE(1, 28);                         // mip count
  head.writeUInt32LE(32, 76);                        // pf size
  head.writeUInt32LE(0x4, 80);                       // fourcc
  head.write('DX10', 84, 'ascii');
  head.writeUInt32LE(0x1000, 108);                   // caps: texture
  head.writeUInt32LE(98, 128);                       // BC7_UNORM
  head.writeUInt32LE(3, 132);                        // texture2d
  head.writeUInt32LE(0, 136);                        // misc
  head.writeUInt32LE(1, 140);                        // array size
  head.writeUInt32LE(0, 144);                        // misc2
  return Buffer.concat([head, payload]);
}

// --- outputs ----------------------------------------------------------------

mkdirSync(OUT, { recursive: true });

const tsvField = (s) => String(s).replace(/[\t\r\n]+/g, ' ');
const tsv = ['# category\tid\tname\trarity\ticon\tdesc\tacquire'];
for (const e of entries) {
  tsv.push([e.category, e.id, tsvField(e.name), e.rarity, e.icon,
            tsvField(e.desc), tsvField(e.acquire.join(' | '))].join('\t'));
}
writeFileSync(join(OUT, 'farever-atlas.tsv'), tsv.join('\n') + '\n');
writeFileSync(join(OUT, 'farever-atlas-icons.dds'),
              ddsFile(ATLAS_W, ATLAS_H, atlas));

const perCat = {};
for (const e of entries) perCat[e.category] = (perCat[e.category] || 0) + 1;
console.log('entries:', entries.length, perCat);
console.log(`atlas: ${ATLAS_W}x${ATLAS_H}, ${cellCount} icons, ` +
            `${((atlas.length + DDS_HEADER) / 1e6).toFixed(1)} MB`);

// --- verify against live exports, when present ------------------------------
//
// Every id the reader has seen live must classify into the same category
// here; anything unmatched means the classification rules drifted.

const entryIds = new Map(entries.map((e) => [e.category + '/' + e.id, e]));

// The live exports are rewritten by the host while the game runs; a
// half-written file must degrade to "not verified", not abort the build.
function parseJsonFile(path) {
  try {
    return JSON.parse(readFileSync(path, 'utf8'));
  } catch (e) {
    console.warn(`VERIFY: skipping unreadable ${path}: ${e.message}`);
    return null;
  }
}

function verifyLive() {
  const colPath = join(game, 'farever-collection.json');
  if (!existsSync(colPath)) return;
  const col = parseJsonFile(colPath);
  if (!col) return;
  const expect = { mounts: 'mounts', gliders: 'gliders', pets: 'pets',
                   gears: 'appearances' };
  for (const [key, category] of Object.entries(expect)) {
    const miss = (col[key] || []).filter((id) => !entryIds.has(category + '/' + id));
    if (miss.length)
      console.warn(`VERIFY: ${miss.length} live ${key} not in ${category}:`,
                   miss.slice(0, 5).join(', '));
  }
  for (const f of readdirSync(game)) {
    if (!/^farever-inventory-.*\.json$/.test(f)) continue;
    const inv = parseJsonFile(join(game, f));
    if (!inv) continue;
    const all = [...inv.bank || [], ...inv.bankEquipment || [],
                 ...inv.equipped || [], ...inv.bags || []];
    const missW = all.filter((i) => i.class === 'st.item.Weapon' &&
                                    !entryIds.has('weapons/' + i.kind));
    if (missW.length)
      console.warn(`VERIFY: ${f}: weapons not classified:`,
                   [...new Set(missW.map((i) => i.kind))].join(', '));
  }
  console.log('verified against live farever-*.json exports');
}
verifyLive();

if (install) {
  try {
    copyFileSync(join(OUT, 'farever-atlas.tsv'), join(game, 'farever-atlas.tsv'));
    copyFileSync(join(OUT, 'farever-atlas-icons.dds'),
                 join(game, 'farever-atlas-icons.dds'));
    console.log(`installed both files next to ${join(game, 'Farever.exe')}`);
  } catch (e) {
    console.error(`install failed (${e.message}) - copy the two files from ` +
                  `${OUT} into ${game} yourself`);
  }
}
