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
import { requireGame } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out', 'atlas');

const game = requireGame();
const install = !process.argv.includes('--no-install');
const modDir = join(game, 'mods', 'farever-mods');
const argOf = (flag) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : null;
};

// Which archives are actually there, said once and up front. Three of the
// four are optional in the sense that the run completes without them - and
// completes *quietly*, with fewer creatures and no shop or dungeon sources,
// which looks like the atlas being wrong rather than the game being half
// downloaded. Steam mid-download or mid-verify is the ordinary way to end up
// here.
{
  const paks = [
    ['res.light.pak', 'the CastleDB - names, descriptions, loot tables', true],
    ['res.pak', 'item and creature portraits', true],
    ['res.map.pak', 'the world: shops, chests, spawns, quest rewards', false],
    ['res.levels.pak', 'dungeon interiors: where instanced bosses live', false],
  ];
  const missing = paks.filter(([f]) => !existsSync(join(game, f)));
  if (missing.length) {
    console.warn('');
    for (const [f, what, required] of missing)
      console.warn(`  ${required ? 'MISSING' : 'absent '}  ${f}  - ${what}`);
    if (missing.some(([, , required]) => required)) {
      console.error('');
      console.error('A required archive is not there. If Steam is still');
      console.error('downloading or verifying, let it finish and re-run.');
      process.exit(1);
    }
    console.warn('');
    console.warn('  The atlas will build without them, and will be missing');
    console.warn('  what they carry. If Steam is still downloading or');
    console.warn('  verifying, let it finish and re-run.');
    console.warn('');
  }
}

// --- load the CastleDB ------------------------------------------------------

const light = openPak(join(game, 'res.light.pak'));
const cdb = JSON.parse(light.read('data.cdb').toString('utf8'));
light.close();
const sheet = (n) => cdb.sheets.find((s) => s.name === n);

// English is embedded in data.cdb. Every other language is an XML overlay in
// res.pak, keyed by the same sheet and row ids. Follow the game's own setting
// unless --lang explicitly asks for one; keeping this here, before any maps
// are built from the CDB, means every consumer below sees the same language.
function configuredLanguage() {
  const forced = argOf('--lang');
  if (forced) return forced.trim();
  try {
    const ini = readFileSync(join(game, 'options.ini'), 'utf8');
    const m = ini.match(/^\s*Language\s*=\s*["']?([^\s"']+)/mi);
    if (m) return m[1];
  } catch { /* a fresh install may not have written options.ini yet */ }
  return 'en';
}

function xmlText(s) {
  return s
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, '$1')
    .replace(/&#x([0-9a-f]+);/gi, (_, n) => String.fromCodePoint(parseInt(n, 16)))
    .replace(/&#([0-9]+);/g, (_, n) => String.fromCodePoint(parseInt(n, 10)))
    .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, '&');
}

function setPath(row, path, value) {
  const parts = path.split('.');
  let dst = row;
  for (let i = 0; i + 1 < parts.length; i++) {
    if (!dst[parts[i]] || typeof dst[parts[i]] !== 'object') dst[parts[i]] = {};
    dst = dst[parts[i]];
  }
  dst[parts.at(-1)] = value;
}

function applyTranslations(xml) {
  const sheets = new Map(cdb.sheets.map((s) => [s.name, s]));
  let translated = 0;
  const sheetRe = /<sheet\s+name="([^"]+)"[^>]*>([\s\S]*?)<\/sheet>/g;
  for (const sm of xml.matchAll(sheetRe)) {
    const target = sheets.get(xmlText(sm[1]));
    if (!target || !Array.isArray(target.lines)) continue;
    const rows = new Map(target.lines.map((r) => [String(r.id), r]));
    const rowRe = /<([A-Za-z_][\w.-]*)>([\s\S]*?)<\/\1>/g;
    for (const rm of sm[2].matchAll(rowRe)) {
      const row = rows.get(xmlText(rm[1]));
      if (!row) continue;
      const fieldRe = /<([A-Za-z_][\w.-]*)>([\s\S]*?)<\/\1>/g;
      for (const fm of rm[2].matchAll(fieldRe)) {
        if (/<[A-Za-z_]/.test(fm[2])) continue;
        setPath(row, fm[1], xmlText(fm[2]));
        translated++;
      }
    }
  }
  return translated;
}

const language = configuredLanguage();
if (language !== 'en') {
  const respakForLang = openPak(join(game, 'res.pak'));
  const langPath = `lang/export_${language}.xml`;
  const langEntry = respakForLang.find(langPath);
  if (!langEntry) {
    respakForLang.close();
    console.warn(`language: ${langPath} not found; using English`);
  } else {
    const count = applyTranslations(respakForLang.read(langEntry).toString('utf8'));
    respakForLang.close();
    console.log(`language: ${language} (${count.toLocaleString()} translated fields)`);
  }
} else {
  console.log('language: en (CastleDB source text)');
}

const items = sheet('item').lines;
const itemById = new Map(items.map((l) => [l.id, l]));
const itemTypes = new Map(sheet('itemType').lines.map((l) => [l.id, l]));
const units = sheet('unit').lines;
const lootTables = sheet('lootTable').lines;
const crafts = sheet('craft').lines;
// Runes live inside the skills they upgrade: `skill.mastery` is an array of
// them, each with its own id, name, description and portrait.
const skills = sheet('skill').lines;
// Achievements hand out mounts and gliders, and are the *only* source for
// the ones they hand out - 23 items whose acquisition is otherwise blank.
const achievements = sheet('ach').lines;
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

// --- weapon mastery ---------------------------------------------------------
//
// Every weapon levels separately, and levelling one is killing things with
// it. Nothing about the track is stored in the save: the game keeps a kill
// count per weapon and derives the rest, so the shape of the track is a
// property of the weapon and belongs here rather than in the reader.
//
//   points = min(floor(kills / killsPerPoint), maxPoints)    Progress.hx:507
//   maxPoints = upgradeableSkills * (WeaponSkill_MaxRank - 1)  HItem.hx:308
//
// `killsPerPoint` is the off-hand constant for anything that can go in the
// off-hand slot (Progress.hx:475), which in practice is the shields.
const constants = new Map(sheet('constant').lines.map((l) => [l.id, l.v]));
function constNumber(id) {
  const v = constants.get(id) || {};
  const n = v.int !== undefined ? v.int : v.float;
  return typeof n === 'number' ? n : null;
}
const WEAPON_MAX_RANK = constNumber('WeaponSkill_MaxRank');
const KILLS_PER_POINT = constNumber('WeaponKills_PerSkillRankPoint');
const KILLS_PER_POINT_OFFHAND = constNumber('WeaponKills_PerSkillRankPoint_OffHand');

// Which skills a point can be spent on. Taken by name rather than by the
// numbers the game compares against, because those are positions in the
// enum and a patch that inserts a type ahead of them shifts every one.
const SKILL_TYPES = (sheet('skill').columns.find((c) => c.name === 'type') || {})
  .typeStr?.split(':').slice(1).join(':').split(',') || [];
const UPGRADEABLE_SKILLS = new Set(
  ['AttackCombo', 'WeaponSkill', 'WeaponPassive']
    .map((n) => SKILL_TYPES.indexOf(n))
    .filter((i) => i >= 0));

const skillById = new Map(skills.map((s) => [s.id, s]));

const masteryKnown =
  WEAPON_MAX_RANK !== null && KILLS_PER_POINT !== null &&
  KILLS_PER_POINT_OFFHAND !== null && UPGRADEABLE_SKILLS.size === 3;
if (!masteryKnown) {
  console.warn('WARNING: the weapon mastery constants moved - the Mastery');
  console.warn('  page will say it does not know rather than guess. Check');
  console.warn('  WeaponSkill_MaxRank / WeaponKills_PerSkillRankPoint and');
  console.warn('  the AttackCombo/WeaponSkill/WeaponPassive skill types.');
}

// "<levels>/<kills per level>", or null when the shape above stopped
// holding. A weapon with no upgradeable skills has no track at all, and
// says so with a zero rather than by going missing.
function masteryTrack(item) {
  if (!masteryKnown) return null;
  let upgradeable = 0;
  for (const s of item.skills || []) {
    const sk = skillById.get(s.skill);
    if (sk && UPGRADEABLE_SKILLS.has(sk.type)) upgradeable++;
  }
  const offhand = typeChain(item.type || '').includes('OffhandWeapon');
  const perPoint = offhand ? KILLS_PER_POINT_OFFHAND : KILLS_PER_POINT;
  return `${upgradeable * (WEAPON_MAX_RANK - 1)}/${perPoint}`;
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
  // The Recipe_* items are pieces of paper you carry, so they belong with
  // the other oddments. The Recipes page is about the crafts themselves.
  Recipe: 'misc',
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
      // How long this weapon's mastery track is, for the Mastery page. Not
      // a facet: it is a lookup key, like `craft:`, and the atlas keeps it
      // out of the filter row.
      const track = masteryTrack(item);
      if (track) tags.push(`mastery:${track}`);
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
// Soulstone altars. Each one names the stone it consumes and the demon it
// invokes, so the link between the two - and the exact place you go to use
// one - is in the world data rather than in a naming convention.
const invocationSites = [];       // {unit, item, x, y, z, zone}
// Every chest in the world, with the table it rolls on.
const lootChests = [];            // {table, x, y, z, zone}
// Items handed over outright rather than rolled for: a quest's reward, and a
// chest with a guaranteed line in it. These are the 100% sources, and they
// are the ones worth walking to.
const itemGrants = [];            // {item, quest, from, x, y, z, zone}
// Named chest contents that are *not* certain, kept for the text only.
const chanceDrops = [];           // {item, pct, id, zone}

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
      // A chest names the table it rolls on, which is the other half of
      // "where does this come from": the loot tables say what a world crate
      // can contain, and these say where the world crates are.
      const rolls = props.props && props.props.lootTable;
      if (typeof rolls === 'string' && rolls)
        lootChests.push({ table: rolls, x, y, z, zone });

      const from = cleanText(props.texts?.name || props.id || '');
      // The element's own id, which is what the game records against it once
      // the chest is opened or the quest handed in.
      const eid = props.id || '';

      // A chest can carry named contents on top of its table. **dropRate is
      // a percentage, not a probability**: the values in the world are 1 and
      // 100, and reading 1 as "always" is how a 1% chest came to be offered
      // as a guaranteed rune. Only 100 is a certainty; the rest are recorded
      // with their odds and given no waypoint, because a 1% chest is a worse
      // place to send someone than the crate clusters they already have.
      for (const e of (props.props && props.props.lootItems) || []) {
        if (!e || !e.item) continue;
        const pct = e.dropRate ?? 0;
        if (pct <= 0) continue;
        chanceDrops.push({ item: e.item, pct, id: eid, zone });
        if (pct >= 100)
          itemGrants.push({ item: e.item, quest: null, from: 'Chest',
                            once: eid, x, y, z, zone });
      }

      // Quest rewards. An NPC's dialogue tree hands items over on a choice,
      // in one of two shapes depending on when the quest was authored, and
      // the objective sitting beside them names the quest. Negative amounts
      // are what the choice *costs*, so only gains count.
      const grants = (node_props, quest) => {
        if (!node_props || typeof node_props !== 'object') return;
        if (Array.isArray(node_props)) {
          for (const v of node_props) grants(v, quest);
          return;
        }
        const here = cleanText(node_props.goal?.name || '') || quest;
        for (const e of node_props.receiveItems || [])
          if (e && e.kind && (e.amount ?? 0) > 0)
            itemGrants.push({ item: e.kind, quest: here, from, once: eid,
                             x, y, z, zone });
        for (const e of node_props.gains?.items || [])
          if (e && e.item && (e.count ?? 0) > 0)
            itemGrants.push({ item: e.item, quest: here, from, once: eid,
                             x, y, z, zone });
        for (const k of Object.keys(node_props)) grants(node_props[k], here);
      };
      if (props.dialog) grants(props.dialog, null);

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
      // An altar that trades an item for a monster. This is where a rift
      // soulstone is actually used, which is nowhere near the rift that
      // dropped it.
      const spawn = props.props && props.props.spawnUnit;
      if (spawn && spawn.unit) {
        const cost = ((props.props.interactible || {}).cost || [])[0];
        invocationSites.push({ unit: spawn.unit, item: cost ? cost.item : null,
                               x, y, z, zone });
      }
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

// Item -> the achievement that awards it. An achievement is not a place, so
// this never produces a navigator target - but "collect all the secret orbs
// in Skover Island" is the complete answer to how you get that glider, and
// it points at a route the atlas already ships.
const achById = new Map(achievements.map((a) => [a.id, a]));
const achFor = new Map();
for (const a of achievements) {
  for (const r of a.reward?.items || []) {
    if (!r.item) continue;
    if (!achFor.has(r.item)) achFor.set(r.item, []);
    achFor.get(r.item).push(a);
  }
}

// What to call an achievement. The tiered collection ones ("Collect 25
// Mounts") have no name of their own and inherit their wording from the tier
// below, with the number kept in the objective rather than in the text - so
// the name has to be rebuilt rather than read.
function achTitle(a) {
  let desc = a.desc;
  for (let up = achById.get(a.parent), i = 0; !desc && up && i < 6; i++) {
    desc = up.desc;
    up = achById.get(up.parent);
  }
  const value = a.objectives?.[0]?.value?.v;
  const fill = (s) => (s || '')
    .replace(/\[(Z\d)_Region\]/g, (_, z) => regionDisplay(z))
    .replace(/::targetValue::/g, value !== undefined ? String(value) : 'some');
  const name = cleanText(fill(a.name));
  const text = cleanText(fill(desc));
  if (name && text) return `${name} - ${text}`;
  return name || text || a.id;
}

function acquisitionOf(itemId) {
  const parts = [];
  for (const a of achFor.get(itemId) || [])
    parts.push(`Achievement: ${achTitle(a)}`);
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
  // Quests hand their reward over every time, so they lead - and naming them
  // is what turns "somewhere" into somewhere you can go.
  // With the zone, because the navigator deliberately will not walk you to a
  // quest - the game does not record which you have finished - so this line
  // is the only thing that tells you where to look.
  const quests = [...new Set((grantsByItem.get(itemId) || [])
    .filter((g) => g.quest)
    .map((g) => `${g.quest} (${prettyZone(g.zone)})`))];
  if (quests.length)
    parts.push(`Quest reward: ${quests.slice(0, 3).join(', ')}` +
               (quests.length > 3 ? ` +${quests.length - 3} more` : ''));
  const chestGrants = (grantsByItem.get(itemId) || []).filter((g) => !g.quest);
  if (chestGrants.length)
    parts.push(`Always in ${chestGrants.length} chest` +
               (chestGrants.length === 1 ? '' : 's'));
  // Named in a specific chest, but not certainly there. Saying the odds is
  // the difference between "go here" and "this is a lottery ticket".
  const chances = chanceDrops.filter((c) => c.item === itemId);
  if (chances.length) {
    const pct = Math.max(...chances.map((c) => c.pct));
    parts.push(`Named in ${chances.length} chest` +
               (chances.length === 1 ? '' : 's') + ` at ${pct}%`);
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
const mkTarget = (label, poi, once) => ({
  // '@' and ';' are the track column's own separators.
  label: cleanText(label).replace(/[@;]/g, ' ').trim(),
  x: Math.round(poi.x * 10) / 10,
  y: Math.round(poi.y * 10) / 10,
  z: Math.round(poi.z * 10) / 10,
  // The id of the one-time thing this target *is* - the NPC whose quest pays
  // out, or the chest that holds it. A source you have already spent is no
  // longer somewhere to go, and only this id can say which one it was.
  // Absent for the repeatable ones: a vendor, a spawn cluster, a crate.
  once: once ? String(once).replace(/[@;,]/g, '') : '',
});

const vaultChests = pois.filter((e) => e.name === 'VaultChest');

// Rift doors. A rift is an instance like a dungeon is, so the world side of
// it is the activity orb that opens it - `POI_Rift_01` sits at a real place
// in Krisomal North. This is where a soulstone *drops*.
const riftEntrances = [...activityOrbs.entries()]
  .filter(([id]) => /rift/i.test(id))
  .map(([, o]) => o);

// ...and this is where one is *used*, which is somewhere else entirely: an
// altar out in the world, one per demon. Indexed both ways, because the
// demon's entry wants "where do I summon it" and the stone's wants "where
// does this get spent".
const sitesByUnit = new Map();
const sitesByStone = new Map();
const stoneForUnit = new Map();
for (const s of invocationSites) {
  if (!sitesByUnit.has(s.unit)) sitesByUnit.set(s.unit, []);
  sitesByUnit.get(s.unit).push(s);
  if (!s.item) continue;
  stoneForUnit.set(s.unit, s.item);
  if (!sitesByStone.has(s.item)) sitesByStone.set(s.item, []);
  sitesByStone.get(s.item).push(s);
}
const invocationTargets = (sites) =>
  clusterTargets(sites, (c) => `Invocation site - ${prettyZone(c.zone)}`);

// Chests grouped by what they roll. An item that only ever comes out of a
// world crate has no single place, but it does have a few hundred places,
// and the nearest handful of those is a real answer to "where do I look".
const chestsByTable = new Map();
for (const c of lootChests) {
  if (!chestsByTable.has(c.table)) chestsByTable.set(c.table, []);
  chestsByTable.get(c.table).push(c);
}
const chestTargets = (table) =>
  clusterTargets(chestsByTable.get(table),
                 (c) => `Chests - ${prettyZone(c.zone)}`);

// The guaranteed sources, by item. These outrank everything: a quest that
// always hands one over beats a table that rolls 5% for it.
const grantsByItem = new Map();
for (const g of itemGrants) {
  if (!grantsByItem.has(g.item)) grantsByItem.set(g.item, []);
  grantsByItem.get(g.item).push(g);
}
const grantLabel = (g) =>
  g.quest ? `${g.quest} - ${prettyZone(g.zone)}`
          : `${g.from} - ${prettyZone(g.zone)}`;
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

// The best few clusters of a set of points, so something found in two
// regions offers both and the navigator picks whichever is nearer.
function clusterTargets(points, label, max = 3) {
  if (!points || !points.length) return [];
  const targets = [];
  let remaining = points.slice();
  for (let i = 0; i < max && remaining.length; i++) {
    const c = bestCluster(remaining);
    targets.push(mkTarget(label(c), c));
    remaining = remaining.filter((p) => {
      const dx = p.x - c.x, dy = p.y - c.y;
      return dx * dx + dy * dy > 120 * 120;
    });
  }
  return targets;
}

// A creature's targets: where that unit stands, or the door to the dungeon
// it lives behind.
function creatureTargets(unitId) {
  return clusterTargets(unitPoints.get(unitId), (c) =>
    c.entrance ? `Dungeon entrance - ${prettyZone(c.zone)}`
               : prettyZone(c.zone));
}

// Where a whole faction is. An outfit set is the gear of the enemies it was
// taken from, and the item rows say which faction each piece belongs to - so
// "where do I farm this appearance" is "where does that faction live", which
// is the union of its units' spawn points.
const factionPoints = new Map();
for (const u of units) {
  if (!u.faction) continue;
  const pts = unitPoints.get(u.id);
  if (!pts || !pts.length) continue;
  if (!factionPoints.has(u.faction)) factionPoints.set(u.faction, []);
  factionPoints.get(u.faction).push(...pts);
}

function factionTargets(faction) {
  return clusterTargets(factionPoints.get(faction),
                        (c) => `${faction} - ${prettyZone(c.zone)}`);
}

function targetsFor(itemId, sold) {
  const targets = [];
  const push = (t) => {
    if (targets.length < 6 && t.label && !targets.some((o) => o.label === t.label))
      targets.push(t);
  };
  // Certain, checkable sources first: a chest the game records you opening.
  //
  // Quest grants are held back to the very end. The game records a chest
  // opened and an activity finished, but *nothing anywhere* records a quest
  // handed in - every map on Progress and HeroData was checked - so a quest
  // target can never be retired, and must not fill the six slots ahead of a
  // source that can be. They carry a `q:` prefix so the host knows never to
  // navigate to one.
  const grants = grantsByItem.get(itemId) || [];
  for (const g of grants)
    if (!g.quest) push(mkTarget(grantLabel(g), g, g.once));

  // A soulstone has two places, and they are nowhere near each other: the
  // rift it drops in, and the altar it is spent at. The altar goes first
  // because a stone you are holding is one you want to use.
  for (const t of invocationTargets(sitesByStone.get(itemId))) push(t);
  for (const { id } of resolvedTables(itemId)) {
    let m;
    // Anything that only comes out of a rift: the tiered rift tables, and
    // the Soulstone table that the rift chests roll on. There is no id
    // linking a loot table to an activity, so the table's own name is the
    // link - which is fine, since it is the game's naming and not ours.
    if (/^Rift/.test(id) || id === 'Soulstone') {
      for (const r of riftEntrances)
        push(mkTarget(`Rift - ${prettyZone(r.zone)}`, r));
      continue;
    }
    // A vault holds Gold and exactly one mount or glider, at 100%, and the
    // chest itself names the table it holds - so point at *that* chest.
    // Matching every vault in the region instead, as this used to, gave the
    // Semeruian Dragoon three targets of which two were the wrong hidden
    // area entirely.
    if (/^Vault_Z\d+_\d+$/.test(id)) {
      const exact = chestsByTable.get(id) || [];
      for (const c of exact) push(mkTarget(`Vault - ${prettyZone(c.zone)}`, c));
      // Only if no chest in the world claims that table - a new tier the
      // world has not caught up with yet.
      if (!exact.length && (m = id.match(/^Vault_Z(\d+)_\d+$/)))
        for (const c of vaultChests)
          if ((c.zone || '').startsWith(`Z${m[1]}_`))
            push(mkTarget(`Vault - ${prettyZone(c.zone)}`, c));
      continue;
    }

    // Ordinary chests that roll this table. After the specific branches, so
    // a vault is named as a vault rather than twice over - and only when
    // nothing better turned up, since "in a world crate somewhere" should
    // never crowd out "this vendor sells it".
    if (!targets.length)
      for (const t of chestTargets(id)) push(t);

    const unit = unitById.get(id) || unitById.get(id.replace(/_?LT2$/, ''));
    if (unit) {
      // Where that creature actually is - the same answer its own bestiary
      // entry gives, including "at the entrance of the dungeon it lives in"
      // for a boss that has no world position of its own.
      //
      // This used to look only for a dungeon POI whose *name* contained the
      // first five letters of the unit id, which finds nothing whenever a
      // dungeon is not named after its boss. High Inquisitor Chakram is the
      // unit `Phrixes` and no POI is called anything like it, so the mount
      // it drops had no target at all while the bestiary entry beside it had
      // one all along.
      const who = cleanText(unit.texts?.name || unit.id);
      const found = creatureTargets(unit.id);
      for (const t of found) push({ ...t, label: `${who} - ${t.label}` });
      // The name match stays as a fallback: it can still name an entrance
      // for a unit the world walk never placed.
      if (!found.length) {
        const frag = unit.id.slice(0, 5).toLowerCase();
        for (const d of dungeonPois)
          if ((d.name || '').toLowerCase().includes(frag))
            push(mkTarget(`${who} - ${prettyZone(d.zone)}`, d));
      }
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

  // Quests last, and only in whatever room is left: they are shown, never
  // navigated to.
  for (const g of grants)
    if (g.quest) push(mkTarget(grantLabel(g), g, 'q:' + g.once));
  return targets;
}

// --- text cleanup -----------------------------------------------------------

// Descriptions carry markup: [GameTerm] links and ::var:: value refs.
// Fold punctuation and ligatures that are outside the Latin-1 font atlas
// down rather than render them as gaps.
const ASCII_FOLD = {
  '\u0153': 'oe', '\u0152': 'OE',
  '’': "'", '‘': "'", '“': '"', '”': '"',
  '–': '-', '—': '-', '…': '...', ' ': ' ',
};
function cleanText(s) {
  if (!s) return '';
  return s
    .replace(/::([^:]*)::/g, (_, v) => v.replace(/^ref_/, ''))
    .replace(/\[([^\]]*)\]/g, '$1')
    .replace(/[^\x00-\xff]/g, (c) => ASCII_FOLD[c] ?? '?')
    .replace(/[\t\r\n]+/g, ' ')
    .trim();
}

// --- build the entry list ---------------------------------------------------

const CATEGORY_ORDER = ['appearances', 'mounts', 'pets', 'gliders',
                        'trinkets', 'weapons', 'consumables', 'materials',
                        'recipes', 'augments', 'misc', 'creatures', 'runes'];

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
// Where an outfit piece comes from when no loot table mentions it - which is
// 385 of the 428 appearances, because cosmetic armour is not placed in tables
// at all. The item's own row still knows: a faction set is the gear of the
// enemies it was taken from, and the generic sets carry their rarity and zone
// in the model path they load.
const regionDisplay = (z) =>
  cleanText(zoneById.get(`${z}_Region`)?.texts?.name || '') || z;

function outfitOrigin(l) {
  const model = l.visuals?.modelPath || '';
  // Trinkets carry their region in the id rather than the model path
  // (`Necklace_Z2_Mp`), and the four faction ones fall through to the
  // faction branch below like any outfit piece.
  const inId = /_Z(\d)_/.exec(l.id);
  // Starter and World are not places you can go; Craft is already covered by
  // the craft lookup.
  if (l.faction && !['Starter', 'World', 'Craft'].includes(l.faction)) {
    return { lines: [`Drops from ${l.faction} enemies`],
             targets: factionTargets(l.faction) };
  }
  if (/\/Starter\//.test(model) || /_Starter_/.test(l.id))
    return { lines: ['Starting outfit'], targets: [] };
  if (/\/BaseClothes\//.test(model))
    return { lines: ['Base clothing'], targets: [] };
  // Outfit/Uncommon/Z2/FigCle/... - a tier and a region, and no one place:
  // it drops from anything in that region, so saying where would be a lie.
  const m = model.match(/Outfit\/(\w+)\/Z(\d)\//);
  if (m)
    return { lines: [`${m[1]} gear from ${regionDisplay(`Z${m[2]}`)} enemies`],
             targets: [] };
  if (inId)
    return { lines: [`${l.rarity || 'Uncommon'} drop from ` +
                     `${regionDisplay(`Z${inId[1]}`)} enemies`], targets: [] };
  return { lines: [], targets: [] };
}

const entries = [];   // { category, id, name, rarity, desc, acquire, gfxFile }

for (const l of items) {
  const category = categoryOf(l);
  if (!category) continue;
  const gfx = l.gfx || {};
  // Acquisition strings embed unit display names, which can be non-ASCII.
  let acquire = acquisitionOf(l.id).map(cleanText);
  let track = targetsFor(l.id, soldItems.has(l.id));
  // Trinkets are placed the same way outfits are - a faction set, or a
  // generic drop for a region - so they take the same fallback.
  if ((category === 'appearances' || category === 'trinkets') &&
      !acquire.length) {
    const o = outfitOrigin(l);
    acquire = o.lines.map(cleanText);
    if (!track.length) track = o.targets;
  }
  entries.push({
    category,
    id: l.id,
    name: cleanText(l.texts?.name) || l.id,
    rarity: Math.max(0, rarities.indexOf(l.rarity ?? 'Common')),
    desc: cleanText(l.texts?.desc),
    acquire,
    track,
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

// Recipes: the crafts themselves - what you can make - rather than the
// scraps of paper that teach them. The game records a learned craft under
// the id of the item it produces, so that id is the entry's id too, and
// "known" becomes a direct lookup.
for (const c of crafts) {
  const made = itemById.get(c.item);
  if (!made) continue;
  const gfx = made.gfx || {};

  const facts = [];
  facts.push(`${c.job || 'Craft'}${c.level ? ` level ${c.level}` : ''}`);
  if (c.input && c.input.length) {
    const parts = c.input.slice(0, 6).map((i) => {
      const ing = itemById.get(i.item);
      return `${i.count || 1}x ${cleanText(ing?.texts?.name || i.item)}`;
    });
    facts.push(`Needs: ${parts.join(', ')}`);
  }
  if (c.cost) facts.push(`Cost: ${c.cost} gold`);

  // A recipe-gated craft has to be unlocked by finding its recipe item, so
  // the useful "how to get" is that item's own story, and the navigator
  // should point at wherever it drops or is sold.
  let targets = [];
  if (c.unlockSource) {
    const paper = itemById.get(c.unlockSource);
    const named = cleanText(paper?.texts?.name || '');
    facts.push(named ? `Unlocked by: ${named}` : 'Unlocked by a recipe you must find');
    // Its own acquisition, minus the line saying what it teaches - which on
    // this page would only repeat the entry's own name back at you.
    for (const line of acquisitionOf(c.unlockSource))
      if (!/^Teaches:/.test(line)) facts.push(line);
    targets = targetsFor(c.unlockSource, soldItems.has(c.unlockSource));
  } else {
    facts.push('Known automatically at that job level');
  }

  entries.push({
    category: 'recipes',
    id: c.item,
    name: cleanText(made.texts?.name) || c.item,
    rarity: Math.max(0, rarities.indexOf(made.rarity ?? 'Common')),
    desc: cleanText(made.texts?.desc),
    acquire: facts.map(cleanText),
    track: targets,
    tags: [`job:${c.job || 'Other'}`,
           c.unlockSource ? 'source:Recipe needed' : 'source:Automatic'],
    gfxFile: gfx.file || '',
    gfxSize: gfx.size || 0,
  });
}

console.log(`summons: ${invocationSites.length} altars, ` +
            `${sitesByUnit.size} units invoked from an item`);

// Runes: one-use pickups that permanently teach one upgrade to one skill.
//
// The game calls them skill masteries and keeps them inside the skill they
// modify rather than in the item sheet - the *item* you find is a single
// generic `Mastery` whose name is literally "Rune: ::ref_mastery::", filled
// in at runtime. So the 84 runes that exist are the mastery rows, and where
// they come from is that one item's own story.
//
// A rune's description is written against the skill's own numbers
// ("::name:: costs ::var1:: less [Rage]"), and the mastery carries those
// numbers next to it. Substituting them here is the difference between
// "name costs var1 less Rage" and something a player can read.
{
  // Every rune arrives through the same generic pickup, and **no loot table
  // anywhere names a specific mastery** - which rune a `Mastery` becomes is
  // decided when you take it. So there is no such thing as "where does
  // Alacrity drop", and saying so is more use than an empty tooltip. What is
  // knowable is where the pickup itself comes from, and the world-chest half
  // of that is hundreds of known positions.
  const runePickup = [
    ...acquisitionOf('Mastery').map(cleanText),
    'Which rune a pickup becomes is decided when you take it',
  ];
  const runeTrack = targetsFor('Mastery', soldItems.has('Mastery'));
  const skillById = new Map(skills.map((s) => [s.id, s]));
  let count = 0, unresolved = 0;

  // `::ref_x::` and `::ref2_x::` read a value off the status the skill
  // applies rather than off the skill itself, and a skill names its statuses
  // in its steps. One hop is enough to catch the ones that are stored as
  // plain vars; the rest are computed out of effect blocks, which is a long
  // walk for a number the game shows you in the tooltip anyway.
  const statusesOf = (s) => (s.steps || [])
    .map((st) => st.props && st.props.status && st.props.status.ref)
    .filter(Boolean)
    .map((id) => skillById.get(id))
    .filter(Boolean);

  for (const s of skills) {
    const refs = (s.mastery || []).length ? statusesOf(s) : [];
    for (const m of s.mastery || []) {
      if (!m.id) continue;
      const skillName = cleanText(s.texts?.name || s.id);
      // Most specific first: the rune's own numbers, then the skill's.
      const own = { ...(m.props || {}), ...(s.vars || {}), ...(m.vars || {}) };

      const desc = (m.text?.desc || '').replace(/::([^:]+)::/g, (_, tok) => {
        const hop = /^ref(\d*)_/.exec(tok);
        let key = tok.replace(/^ref\d*_/, '');
        const pct = key.endsWith('%');
        if (pct) key = key.slice(0, -1);
        // A referenced status has its own name, which is what the sentence
        // is naming when it says ref_name.
        // `::ref_x::` is the first status the skill applies, `::ref2_x::` the
        // second, and so on. Treating everything that was not `ref2` as the
        // first one did not degrade to `?` - it silently read a *different*
        // status's variable and printed it as fact. `Rogue_UrgeToKill_M1`
        // came out as "100%" from a `var1` that means "generate 1 combo
        // point", telling players the rune doubled their damage. An index we
        // do not have is undefined here, which falls through to `?`.
        const ref = hop ? refs[Math.max(0, parseInt(hop[1] || '1', 10) - 1)]
                        : null;
        if (key === 'name')
          return cleanText(ref?.texts?.name || skillName);
        const v = (ref && ref.vars ? ref.vars[key] : undefined) ?? own[key];
        if (v === undefined || typeof v === 'object') {
          // Say "a number we could not read" rather than print the variable
          // name at the player - `val1%` reads as a bug, `?%` reads as
          // missing, which is what it is.
          unresolved++;
          return '?';
        }
        return pct ? `${Math.round(v * 100)}%` : String(v);
      });

      // Class comes off the skill id, which is prefixed with it - and the
      // count comes out even at 21 runes each, so the convention holds.
      const cls = s.id.split('_')[0];
      const gfx = m.gfx || {};
      entries.push({
        category: 'runes',
        id: m.id,
        // Two Priest_Miracle runes ship with an empty text block - they
        // exist, with icons, and are simply unnamed in this build. Showing
        // the id is how the rest of the atlas handles a nameless row, and
        // beats hiding a rune you can actually pick up.
        name: cleanText(m.text?.name) || m.id,
        // The pickup is Epic, and every rune arrives through it.
        rarity: Math.max(0, rarities.indexOf('Epic')),
        desc: cleanText(desc),
        acquire: [`Upgrades: ${skillName} (${cls})`, ...runePickup],
        // A copy per rune. Every other page builds a fresh array; this one
        // computed the pickup's targets once and handed the same array to
        // all 84, so one `track+` line in atlas-overrides.tsv would append
        // to every rune at once and trip the eight-target trim eighty-four
        // times over.
        track: runeTrack.slice(),
        tags: [`class:${cls}`, `skill:${skillName}`],
        gfxFile: gfx.file || '',
        gfxSize: gfx.size || 0,
      });
      count++;
    }
  }
  console.log(`runes: ${count} skill masteries ` +
              `(${unresolved} description values not in the data)`);
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
  // of the released regions, in the world or inside one of its dungeons -
  // or be summonable, which is reaching it by another route entirely.
  // Z4 and the test zone are flagged hidden in the codex for that reason.
  const regions = unitRegions.get(u.id);
  const sites = sitesByUnit.get(u.id);
  const summon = itemById.get(stoneForUnit.get(u.id) || '');
  if ((!regions || !regions.size) && !sites) continue;
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

  let targets = creatureTargets(u.id);
  if (targets.length) {
    // Two clusters can sit in one zone; the navigator still wants both, but
    // naming the place twice in the tooltip reads like a mistake.
    const places = [...new Set(targets.map((t) => t.label))];
    facts.push(`Found in: ${places.join(', ')}`);
  }
  // A summoned demon has no spawn point of its own: it appears at the altar
  // where its stone is spent, and that - not the rift the stone came out of -
  // is the place to walk to.
  if (sites) {
    const where = invocationTargets(sites);
    const places = [...new Set(where.map((t) => t.label))];
    facts.push(summon
      ? `Summoned with the ${cleanText(summon.texts?.name || summon.id)}`
      : 'Summoned at an altar');
    if (places.length) facts.push(`Invoked at: ${places.join(', ')}`);
    if (summon) {
      // Where the stone itself comes from, since that is the step before.
      for (const line of acquisitionOf(summon.id))
        facts.push(`Stone: ${cleanText(line)}`);
    }
    if (!targets.length) targets = where;
  }

  // A summoned unit's region comes from the altar it appears at, since it
  // has no spawn point to read one from.
  const areas = regions && regions.size
    ? [...regions]
    : [...new Set((sites || []).map((s) => regionOf(s.zone)).filter(Boolean))];

  entries.push({
    category: 'creatures',
    id: u.id,
    name: cleanText(u.texts?.name) || u.id,
    rarity: (u.flags & UNIT_BOSS) ? 4 : (u.flags & UNIT_MINIBOSS) ? 3 : 0,
    desc: cleanText(u.texts?.desc || ''),
    acquire: facts.map(cleanText),
    track: targets,
    tags: [`type:${u.type || 'Other'}`,
           ...areas.sort().map((r) => `area:${r}`)],
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
  // `label@x,y,z`, with the one-time source's id appended when there is one.
  // A reader that stops after three numbers still gets the position.
  const track = (e.track || [])
    .map((t) => `${t.label}@${t.x},${t.y},${t.z}${t.once ? ',' + t.once : ''}`)
    .join(';');
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
  const colPath = join(modDir, 'farever-collection.json');
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
  for (const f of readdirSync(modDir)) {
    if (!/^farever-inventory-.*\.json$/.test(f)) continue;
    const inv = parseJsonFile(join(modDir, f));
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
    mkdirSync(modDir, { recursive: true });
    copyFileSync(join(OUT, 'farever-atlas.tsv'), join(modDir, 'farever-atlas.tsv'));
    copyFileSync(join(OUT, 'farever-atlas-icons.dds'),
                 join(modDir, 'farever-atlas-icons.dds'));
    console.log(`installed both files in ${modDir}`);
  } catch (e) {
    console.error(`install failed (${e.message}) - copy the two files from ` +
                  `${OUT} into ${modDir} yourself`);
  }
}
