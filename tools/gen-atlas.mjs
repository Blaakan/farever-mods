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
import { readHBSON, walkNodes } from './lib/hbson.mjs';

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

// The equippable/collectible categories are decided from the itemType
// inheritance chain; everything else is grouped by what the player would
// call it, since inheritance puts food, recipes and enchants all under
// "Usable" and that makes a useless page.
const BY_TYPE = new Map(Object.entries({
  // Consumables
  Food: 'consumables', Potion: 'consumables', Elixir: 'consumables',
  Consumable: 'consumables', HealthPotion: 'consumables',
  Usable: 'consumables', SkillPointBook: 'consumables', Mastery: 'consumables',
  // Materials
  CraftingComponent: 'materials', Ore: 'materials', Cloth: 'materials',
  Leather: 'materials', Soulstone: 'materials',
  // Recipes
  Recipe: 'recipes',
  // Tools, currency, containers and the leftovers
  Package: 'misc', CompletedPackage: 'misc', Misc: 'misc',
  Currency: 'misc', Bag: 'misc', Prospecting: 'misc',
  LootableContainer: 'misc', Collection: 'misc', Gear: 'misc',
  GearPickaxe: 'misc', GearSickle: 'misc', ToolBlacksmith: 'misc',
  ToolOutfitter: 'misc', ToolAlchemist: 'misc', ToolJeweller: 'misc',
  ToolCook: 'misc', ToolEnchanter: 'misc',
}));

// --- facets, for the in-game filters ------------------------------------
//
// Armour ids end in the classes that can wear them, one or two at a time:
// Fig/Ass/Wiz/Cle, which the game presents as Warrior/Rogue/Mage/Priest.
// (A handful end in Craft/Shop/BaseClothes instead - those are unrestricted.)
const CLASS_CODES = new Map([
  ['Fig', 'Warrior'], ['Ass', 'Rogue'], ['Wiz', 'Mage'], ['Cle', 'Priest'],
]);
const ARMOUR_SLOTS = new Set(['Chest', 'Legs', 'Feet', 'Head', 'Hands',
                              'Waist', 'Back', 'Shoulders']);
// The aptitude each class carries, read off the four player-class units.
const APTITUDE_CLASS = new Map([
  ['Fighter', 'Warrior'], ['Assassin', 'Rogue'],
  ['Wizard', 'Mage'], ['Cleric', 'Priest'],
]);
const TRINKET_LABEL = new Map([
  ['GearTrinket', 'Trinket'], ['GearNeck', 'Necklace'], ['GearFinger', 'Ring'],
]);

// The family in an id like Mount_Wolf_01 or Glider_Owl_Grey.
function familyOf(id, prefix) {
  const m = id.match(new RegExp(`^${prefix}_([A-Za-z]+)`));
  if (m) return m[1];
  return id.replace(/_?\d+$/, '').replace(/_.*$/, '') || 'Other';
}

function tagsFor(item, category) {
  const tags = [];
  switch (category) {
    case 'appearances': {
      if (ARMOUR_SLOTS.has(item.type)) tags.push(`slot:${item.type}`);
      const suffix = (item.id.match(/_([A-Za-z]+)$/) || [])[1] || '';
      for (const [code, name] of CLASS_CODES) {
        if (suffix.includes(code)) tags.push(`class:${name}`);
      }
      // No class in the id means anyone can wear it.
      if (!tags.some((t) => t.startsWith('class:'))) tags.push('class:Any');
      break;
    }
    case 'mounts':   tags.push(`type:${familyOf(item.id, 'Mount')}`); break;
    case 'gliders':  tags.push(`type:${familyOf(item.id, 'Glider')}`); break;
    case 'trinkets':
      tags.push(`type:${TRINKET_LABEL.get(item.type) || item.type}`);
      break;
    case 'weapons': {
      // Who can wield it, not what shape it is. The link is `aptitudes`:
      // each player class has exactly one (Warrior=Fighter, Rogue=Assassin,
      // Mage=Wizard, Priest=Cleric) and a weapon lists the ones it serves.
      for (const a of item.aptitudes || []) {
        const cls = APTITUDE_CLASS.get(a.ref);
        if (cls) tags.push(`class:${cls}`);
      }
      if (!tags.length) tags.push('class:Any');
      break;
    }
    case 'recipes': {
      // Which job can use it, and the craft the game will record as known.
      const craft = craftByRecipe.get(item.id);
      if (craft) {
        if (craft.job) tags.push(`job:${craft.job}`);
        // The craft id is what the game records as known; it is carried for
        // the lookup, not offered as a filter (the UI hides it).
        tags.push(`craft:${craft.item}`);
      } else {
        tags.push('job:Unknown');
      }
      break;
    }
    case 'consumables':
    case 'materials':
    case 'augments':
    case 'misc':
      tags.push(`type:${item.type}`);
      break;
    default: break;
  }
  return tags;
}

function categoryOf(item) {
  const chain = typeChain(item.type || '');
  if (chain.includes('Weapon')) return 'weapons';
  if (chain.includes('Armor')) return 'appearances';
  if (TRINKET_TYPES.has(item.type)) return 'trinkets';
  if (chain.includes('GearGlider')) return 'gliders';
  if (item.type === 'Mount') return 'mounts';
  // Every Augment* variant lands on one page rather than eight.
  if (/^Augment/.test(item.type || '')) return 'augments';
  return BY_TYPE.get(item.type) || null;
}

// Unit flags, in declaration order of the flags column.
const UNIT_NO_CODEX = 1 << 18;
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

// Direct tables, with generic containers climbed one step to whoever
// references them. Shared by the text builder and the tracker targets.
function resolvedTables(itemId) {
  const direct = tablesByItem.get(itemId) || [];
  const out = [];   // {id, proba} - id 'many enemies' for a diluted container
  for (const { table, proba } of direct) {
    let ids = [table];
    if (GENERIC_TABLE.test(table)) {
      const parents = [...(tableParents.get(table) || [])]
        .filter((p) => !GENERIC_TABLE.test(p));
      if (parents.length && parents.length <= 6) ids = parents;
      else if (parents.length) ids = ['many enemies'];
    }
    for (const id of ids) out.push({ id, proba });
  }
  return out;
}

function lootSources(itemId) {
  const labels = new Map();   // label -> best proba
  for (const { id, proba } of resolvedTables(itemId)) {
    const label = id === 'many enemies' ? 'enemy drops' : tableLabel(id);
    const prev = labels.get(label);
    if (prev === undefined || proba > prev) labels.set(label, proba);
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

// A recipe item teaches one craft, and the game records that craft by the
// id of the item it PRODUCES - not by the recipe's own id. `unlockSource`
// is the link: the craft row names the recipe that unlocks it.
const craftByRecipe = new Map();
for (const c of crafts) {
  if (c.unlockSource && c.item) craftByRecipe.set(c.unlockSource, c);
}

// --- the world, parsed --------------------------------------------------
//
// One pass over the map prefabs yields everything positional the atlas
// wants: who sells what and where, where creatures spawn, and where the
// dungeon entrances are. This used to be a raw scan for "@shop" tokens,
// which was wrong in a way worth recording: the '@' it keyed on is really
// HBSON's 0x40 "short string" flag, and the writer only uses it for strings
// of 16 bytes or fewer - so that scan was silently blind to every item id of
// 17 characters or more.

const soldItems = new Set();
const shopPoints = [];      // {item, vendor, x, y, z, zone}
const spawnPoints = [];     // {unit, unitGroup, x, y, z, zone}
const activityOrbs = new Map();   // targetActivity id -> {x, y, z, zone}

function isSpawner(node) {
  return node.props && node.props.$cdbtype === 'spawner';
}

function scanWorld(pak, pathFilter) {
  let parsed = 0, failed = 0;
  for (const f of pak.files) {
    if (!pathFilter(f.path)) continue;
    let doc;
    try {
      doc = readHBSON(pak.read(f));
    } catch (e) {
      failed++;
      continue;
    }
    parsed++;
    walkNodes(doc.root, (node, x, y, z) => {
      const props = node.props;
      if (!props || typeof props !== 'object') return;
      // The cdb-typed object carries the baked zone alongside $cdbtype; the
      // node itself only holds the transform.
      const zone = props.zoneBaked || node.zoneBaked || null;

      if (props.$cdbtype === 'spawner') {
        if (props.unit || props.unitGroup)
          spawnPoints.push({ unit: props.unit || null,
                             unitGroup: props.unitGroup || null, x, y, z, zone });
        return;
      }
      // Shops carry their stock, and the element carries a display name.
      const stock = props.props && props.props.shop;
      if (Array.isArray(stock)) {
        const vendor = cleanText(props.texts?.name || props.id || 'Merchant');
        for (const row of stock) {
          if (!row || !row.item || !itemById.has(row.item)) continue;
          soldItems.add(row.item);
          shopPoints.push({ item: row.item, vendor, x, y, z, zone });
        }
      }
      // An orb that opens an instance: the world-side anchor for everything
      // that only spawns inside that instance.
      const target = props.props && props.props.targetActivity;
      if (target && !activityOrbs.has(target))
        activityOrbs.set(target, { x, y, z, zone });
    });
  }
  return { parsed, failed };
}

const respak = openPak(join(game, 'res.pak'));
let mapPak = null;
try {
  mapPak = openPak(join(game, 'res.map.pak'));
  const r = scanWorld(mapPak, (p) => p.endsWith('.prefab'));
  console.log(`world: ${r.parsed} prefabs parsed${r.failed ? `, ${r.failed} failed` : ''}, ` +
              `${spawnPoints.length} spawn points, ${soldItems.size} items sold`);
} catch (e) {
  console.warn('res.map.pak not scanned:', e.message);
}

function acquisitionOf(itemId) {
  const parts = [];
  // A recipe's own line is about what it teaches, not how it is made.
  const taught = craftByRecipe.get(itemId);
  if (taught) {
    const made = itemById.get(taught.item);
    parts.push(`Teaches: ${cleanText(made?.texts?.name || taught.item)}` +
               `${taught.job ? ` (${taught.job}` : ''}` +
               `${taught.job && taught.level ? ` lvl ${taught.level}` : ''}` +
               `${taught.job ? ')' : ''}`);
  }
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

// --- tracker targets --------------------------------------------------------
//
// World coordinates for sources that have a fixed place: vault chests,
// dungeon bosses and merchants, out of the POI table farever-minimap ships.
// The host shows distance and direction to the nearest target. Sources with
// no fixed place (faction enemies, world-roaming bosses) get none; the
// overrides file can supply hand-curated coordinates for those.

const pois = (() => {
  // The POI table originally ships with farever-minimap; a preserved copy in
  // tools/out keeps the tracker working on installs without that mod.
  const candidates = [
    join(game, 'data', 'pois_W1_Siagarta.json'),
    join(HERE, 'out', 'pois_W1_Siagarta.json'),
  ];
  const p = candidates.find(existsSync);
  if (!p) {
    console.warn('POI table not found - no tracker targets');
    return [];
  }
  try {
    return JSON.parse(readFileSync(p, 'utf8'));
  } catch (e) {
    console.warn(`POI table unreadable: ${e.message}`);
    return [];
  }
})();
const prettyZone = (z) => (z ? z.replace(/^Z\d+_/, '').replace(/_/g, ' ') : 'unknown');
const mkTarget = (label, poi) => ({
  // '@' and ';' are the track column's own separators.
  label: cleanText(label).replace(/[@;]/g, ' ').trim(),
  x: Math.round(poi.x * 10) / 10,
  y: Math.round(poi.y * 10) / 10,
  z: Math.round(poi.z * 10) / 10,
});

const vaultChests = pois.filter((e) => e.name === 'VaultChest');
const merchantPois = pois.filter((e) => e.kind === 'merchant');
const dungeonPois = pois.filter((e) => e.kind === 'dungeon');

// --- where each creature is found ---------------------------------------
//
// Spawners name either a unit outright or a unitGroup; groups are rosters
// with weights, so expanding them is what puts the small critters on the
// map at all - most of them are never named by a spawner directly.

const unitGroups = new Map(sheet('unitGroup').lines.map((g) => [g.id, g]));

function groupMembers(groupId, depth = 0) {
  const group = unitGroups.get(groupId);
  if (!group || depth > 3) return [];
  const out = [];
  for (const comp of group.composition || []) {
    const weight = comp.weight ?? 1;
    for (const entry of comp.group || []) {
      if (entry.unit) out.push({ unit: entry.unit, weight });
      else if (entry.unitGroup)
        for (const nested of groupMembers(entry.unitGroup, depth + 1))
          out.push({ unit: nested.unit, weight: weight * nested.weight });
    }
  }
  return out;
}

// Which released region a zone belongs to. The codex is organised by
// region, and only Z1-Z3 are visible in it - Z4 and the test zone are
// flagged hidden, being unreleased.
const zoneById = new Map(sheet('zone').lines.map((z) => [z.id, z]));
const VISIBLE_REGIONS = new Map([
  ['Z1_Region', 'Z1'], ['Z2_Region', 'Z2'], ['Z3_Region', 'Z3'],
]);
function regionOf(zoneId) {
  let z = zoneById.get(zoneId);
  for (let i = 0; z && i < 10; i++) {
    if (VISIBLE_REGIONS.has(z.id)) return VISIBLE_REGIONS.get(z.id);
    z = z.parent ? zoneById.get(z.parent) : null;
  }
  const m = (zoneId || '').match(/^(Z\d)_/);
  return m && VISIBLE_REGIONS.has(`${m[1]}_Region`) ? m[1] : null;
}

const unitRegions = new Map();  // unit id -> Set of region ids
const noteRegion = (unit, zone) => {
  const r = regionOf(zone);
  if (!unit || !r) return;
  if (!unitRegions.has(unit)) unitRegions.set(unit, new Set());
  unitRegions.get(unit).add(r);
};

const unitPoints = new Map();   // unit id -> [{x, y, z, zone}]
const addPoint = (unit, p) => {
  if (!unit) return;
  if (!unitPoints.has(unit)) unitPoints.set(unit, []);
  const list = unitPoints.get(unit);
  if (list.length < 400) list.push(p);
};
for (const s of spawnPoints) {
  const p = { x: s.x, y: s.y, z: s.z, zone: s.zone };
  if (s.unit) { addPoint(s.unit, p); noteRegion(s.unit, s.zone); }
  if (s.unitGroup)
    for (const m of groupMembers(s.unitGroup)) {
      addPoint(m.unit, p);
      noteRegion(m.unit, s.zone);
    }
}

// Units that only exist inside an instance get the world-side entrance of
// that instance instead - the boss is not standing in the overworld, but
// the door to it is. Instance levels name their activity, and a world orb
// points back at the same id.
try {
  const levels = openPak(join(game, 'res.levels.pak'));
  const byLevel = new Map();   // level dir -> {activity, units:Set}
  for (const f of levels.files) {
    if (!f.path.endsWith('.prefab')) continue;
    // One level is everything under its own "<name>.dat" directory. Slicing
    // a fixed depth instead lumps every level of a region together, which
    // hands several bosses the same entrance.
    const parts = f.path.split('/');
    const datAt = parts.findIndex((s) => s.endsWith('.dat'));
    if (datAt < 0) continue;
    const dir = parts.slice(0, datAt + 1).join('/');
    let doc;
    try { doc = readHBSON(levels.read(f)); } catch (e) { continue; }
    if (!byLevel.has(dir)) byLevel.set(dir, { activity: null, units: new Set() });
    const rec = byLevel.get(dir);
    walkNodes(doc.root, (node) => {
      const props = node.props;
      if (!props || typeof props !== 'object') return;
      if (props.$cdbtype === 'activity' && props.id) rec.activity = props.id;
      if (props.$cdbtype === 'spawner') {
        // A dungeon's monsters belong to the region the dungeon is in,
        // which its own path names (Level/POI/Z1Levels/...) even when the
        // baked zone inside the instance does not resolve.
        const inRegion = props.zoneBaked ||
            ((f.path.match(/Level\/POI\/(Z\d)Levels\//) || [])[1] || '') + '_Region';
        if (props.unit) { rec.units.add(props.unit); noteRegion(props.unit, inRegion); }
        if (props.unitGroup)
          for (const m of groupMembers(props.unitGroup)) {
            rec.units.add(m.unit);
            noteRegion(m.unit, inRegion);
          }
      }
    });
  }
  levels.close();
  let placed = 0;
  for (const rec of byLevel.values()) {
    const orb = rec.activity ? activityOrbs.get(rec.activity) : null;
    if (!orb) continue;
    for (const unit of rec.units) {
      if (unitPoints.has(unit)) continue;      // already out in the world
      addPoint(unit, { ...orb, entrance: true });
      placed++;
    }
  }
  console.log(`instances: ${placed} units placed at their dungeon entrance`);
} catch (e) {
  console.warn('res.levels.pak not scanned:', e.message);
}

// The mean of a unit's spawn points is meaningless when it lives on two
// islands - it lands in the sea between them. Take the densest cluster
// instead: the point with the most neighbours, averaged with them.
function bestCluster(points, radius = 120) {
  if (points.length === 1) return { ...points[0], n: 1 };
  let best = null, bestN = -1;
  for (const a of points) {
    let n = 0;
    for (const b of points) {
      const dx = a.x - b.x, dy = a.y - b.y;
      if (dx * dx + dy * dy <= radius * radius) n++;
    }
    if (n > bestN) { bestN = n; best = a; }
  }
  const near = points.filter((b) => {
    const dx = best.x - b.x, dy = best.y - b.y;
    return dx * dx + dy * dy <= radius * radius;
  });
  const avg = (k) => near.reduce((s, p) => s + p[k], 0) / near.length;
  return { x: avg('x'), y: avg('y'), z: avg('z'), zone: best.zone,
           entrance: best.entrance, n: near.length };
}

// A creature's targets: its best few clusters, so a unit found in two
// regions offers both and the navigator picks whichever is nearer.
function creatureTargets(unitId) {
  const points = unitPoints.get(unitId);
  if (!points || !points.length) return [];
  const targets = [];
  let remaining = points.slice();
  for (let i = 0; i < 3 && remaining.length; i++) {
    const c = bestCluster(remaining);
    const label = c.entrance
      ? `Dungeon entrance - ${prettyZone(c.zone)}`
      : prettyZone(c.zone);
    targets.push(mkTarget(label, c));
    remaining = remaining.filter((p) => {
      const dx = p.x - c.x, dy = p.y - c.y;
      return dx * dx + dy * dy > 120 * 120;
    });
  }
  return targets;
}

function targetsFor(itemId, sold) {
  const targets = [];
  const push = (t) => {
    if (targets.length < 6 && t.label && !targets.some((o) => o.label === t.label))
      targets.push(t);
  };
  for (const { id } of resolvedTables(itemId)) {
    let m;
    if ((m = id.match(/^Vault_Z(\d+)_\d+$/))) {
      for (const c of vaultChests)
        if ((c.zone || '').startsWith(`Z${m[1]}_`))
          push(mkTarget(`Vault - ${prettyZone(c.zone)}`, c));
      continue;
    }
    const unit = unitById.get(id) || unitById.get(id.replace(/_?LT2$/, ''));
    if (unit) {
      // POI names abbreviate ('Nepsid' for Nepsilon), so match on a prefix.
      const frag = unit.id.slice(0, 5).toLowerCase();
      for (const d of dungeonPois)
        if ((d.name || '').toLowerCase().includes(frag))
          push(mkTarget(`${unit.texts?.name || unit.id} - ${prettyZone(d.zone)}`, d));
    }
  }
  // Vendors now come from the prefabs with their own names and positions,
  // which beats pointing at every wandering merchant on the map.
  for (const s of shopPoints) {
    if (s.item === itemId) push(mkTarget(`${s.vendor} - ${prettyZone(s.zone)}`, s));
  }
  if (sold && !targets.length)
    for (const mch of merchantPois)
      push(mkTarget(`Merchant - ${prettyZone(mch.zone)}`, mch));
  return targets;
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
                        'trinkets', 'weapons', 'consumables', 'materials',
                        'recipes', 'augments', 'misc', 'creatures'];

// What a creature drops - the loot table read forwards, for once, since a
// bestiary entry wants "what do I get" rather than "where is this from".
const dropsByUnit = new Map();
for (const t of lootTables) {
  const items = [];
  for (const e of t.loot || []) {
    if (!e.item) continue;
    const item = itemById.get(e.item);
    if (!item || !item.texts?.name) continue;
    items.push({ name: cleanText(item.texts.name), proba: e.proba ?? 1 });
  }
  if (items.length) dropsByUnit.set(t.id, items);
}
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
    track: targetsFor(l.id, soldItems.has(l.id)),
    tags: tagsFor(l, category),
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
  const critterItem = `Critter_${u.id}`;
  const viaItem = itemById.has(critterItem)
    ? acquisitionOf(critterItem).map(cleanText) : [];
  entries.push({
    category: 'pets',
    id: u.id,
    name: cleanText(u.texts?.name) || u.id,
    rarity: 0,
    desc: '',
    acquire: [...viaItem, 'Capture in the wild (Capture Net)'],
    // A pet is a creature first: point at where it actually lives.
    track: creatureTargets(u.id),
    // Pet families read straight off the id: Ladybug_Yellow, DemonDog_Red.
    tags: [`type:${u.id.split('_')[0]}`,
           ...[...(unitRegions.get(u.id) || [])].sort().map((r) => `area:${r}`)],
    gfxFile: gfx.file || '',
    gfxSize: gfx.size || 0,
  });
}

// Creatures: the bestiary. Everything the codex shows, which is the unit
// list minus the entries flagged NoCodex, the *_Base templates and the
// player classes - all of which give themselves away by having no portrait.
for (const u of units) {
  if (u.flags & UNIT_NO_CODEX) continue;
  if (/_Base$/.test(u.id)) continue;
  // Critters are the Pets page; the codex's Monsters category does not
  // include them.
  if (u.type === 'Critter') continue;
  // Only what a player can actually reach: a monster has to spawn in one
  // of the released regions, in the world or inside one of its dungeons.
  // Z4 and the test zone are flagged hidden in the codex for that reason.
  const regions = unitRegions.get(u.id);
  if (!regions || !regions.size) continue;
  const gfx = u.gfx || {};
  if (!gfx.file || !gfx.file.startsWith('UI/Portraits/')) continue;

  const facts = [];
  if (u.lvl) {
    facts.push(u.maxLvl && u.maxLvl !== u.lvl
      ? `Level ${u.lvl}-${u.maxLvl}` : `Level ${u.lvl}`);
  }
  if (u.flags & UNIT_BOSS) facts.push('World boss');
  else if (u.flags & UNIT_MINIBOSS) facts.push('Miniboss');
  const drops = dropsByUnit.get(u.id) || [];
  if (drops.length) {
    const named = drops.sort((a, b) => b.proba - a.proba).slice(0, 5)
      .map((d) => (d.proba <= 0.011 ? `${d.name} (rare)` : d.name));
    facts.push(`Drops: ${named.join(', ')}`);
  }

  const targets = creatureTargets(u.id);
  if (targets.length) {
    // Two clusters can sit in one zone; the navigator still wants both, but
    // naming the place twice in the tooltip reads like a mistake.
    const places = [...new Set(targets.map((t) => t.label))];
    facts.push(`Found in: ${places.join(', ')}`);
  }

  entries.push({
    category: 'creatures',
    id: u.id,
    name: cleanText(u.texts?.name) || u.id,
    rarity: (u.flags & UNIT_BOSS) ? 4 : (u.flags & UNIT_MINIBOSS) ? 3 : 0,
    desc: cleanText(u.texts?.desc || ''),
    acquire: facts.map(cleanText),
    track: targets,
    tags: [`type:${u.type || 'Other'}`,
           ...[...regions].sort().map((r) => `area:${r}`)],
    gfxFile: gfx.file,
    gfxSize: gfx.size || 0,
  });
}

entries.sort((a, b) =>
  CATEGORY_ORDER.indexOf(a.category) - CATEGORY_ORDER.indexOf(b.category) ||
  a.name.localeCompare(b.name) || a.id.localeCompare(b.id));

// --- hand-curated overrides -------------------------------------------------
//
// tools/atlas-overrides.tsv patches whatever the generated data got wrong or
// could not know: sources with no file trail, coordinates for world-roaming
// bosses, better wording. Applied last so it always wins.

const ovPath = join(HERE, 'atlas-overrides.tsv');
if (existsSync(ovPath)) {
  const byId = new Map();
  for (const e of entries) {
    if (!byId.has(e.id)) byId.set(e.id, []);
    byId.get(e.id).push(e);
  }
  // Split on the LAST '@': labels may legitimately contain one
  // ("Boss @ Camp@100,200,300"). Warn on anything that fails to parse
  // rather than silently counting the line as applied.
  const parseTrack = (s, id) => s.split(';').map((t) => {
    const at = t.lastIndexOf('@');
    if (at <= 0) {
      console.warn(`override: unparseable track "${t}" for ${id}`);
      return null;
    }
    const [x, y, z] = t.slice(at + 1).split(',').map(Number);
    const target = {
      label: cleanText(t.slice(0, at)).replace(/[@;]/g, ' ').trim(),
      x, y, z,
    };
    if (!target.label || !Number.isFinite(x) || !Number.isFinite(y) ||
        !Number.isFinite(z)) {
      console.warn(`override: unparseable track "${t}" for ${id}`);
      return null;
    }
    return target;
  }).filter(Boolean);
  let applied = 0;
  for (const line of readFileSync(ovPath, 'utf8').split('\n')) {
    if (!line.trim() || line.startsWith('#')) continue;
    const [id, field, ...rest] = line.replace(/\r$/, '').split('\t');
    const text = rest.join('\t');
    const hits = byId.get(id);
    if (!hits) { console.warn(`override: unknown id "${id}"`); continue; }
    for (const e of hits) {
      if (field === 'acquire') e.acquire = text.split(' | ').map(cleanText).filter(Boolean);
      else if (field === 'acquire+') e.acquire.push(cleanText(text));
      else if (field === 'desc') e.desc = cleanText(text);
      else if (field === 'track') e.track = parseTrack(text, id);
      else if (field === 'track+') e.track.push(...parseTrack(text, id));
      else { console.warn(`override: unknown field "${field}" for ${id}`); continue; }
      applied++;
    }
  }
  // The host reads at most 8 targets per entry; shipping more would be
  // silently dropped there, so trim (and say so) here instead.
  for (const e of entries) {
    if ((e.track || []).length > 8) {
      console.warn(`${e.id}: ${e.track.length} tracker targets, keeping 8`);
      e.track = e.track.slice(0, 8);
    }
  }
  if (applied) console.log(`overrides: ${applied} applied from atlas-overrides.tsv`);
}

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
const tsv = ['# category\tid\tname\trarity\ticon\tdesc\tacquire\ttrack\ttags'];
let tracked = 0;
for (const e of entries) {
  const track = (e.track || [])
    .map((t) => `${t.label}@${t.x},${t.y},${t.z}`).join(';');
  if (track) tracked++;
  tsv.push([e.category, e.id, tsvField(e.name), e.rarity, e.icon,
            tsvField(e.desc), tsvField(e.acquire.join(' | ')),
            tsvField(track), tsvField((e.tags || []).join(','))].join('\t'));
}
writeFileSync(join(OUT, 'farever-atlas.tsv'), tsv.join('\n') + '\n');
writeFileSync(join(OUT, 'farever-atlas-icons.dds'),
              ddsFile(ATLAS_W, ATLAS_H, atlas));

const perCat = {};
for (const e of entries) perCat[e.category] = (perCat[e.category] || 0) + 1;
console.log('entries:', entries.length, perCat);
console.log(`tracker targets on ${tracked} entries`);
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
