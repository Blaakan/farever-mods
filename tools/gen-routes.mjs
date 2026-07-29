#!/usr/bin/env node
// ---------------------------------------------------------------------------
// gen-routes.mjs
//
// Builds the navigator's starter route set out of the game's own world:
//
//   tools/out/routes/farever-routes.txt
//
// Every chest, secret orb and gathering node in the world is placed by a
// prefab reference in the level tiles inside res.map.pak, and each of those
// carries the zone it was baked into. That is enough to answer "give me every
// world chest in Primevalley" without anyone hand-marking a map.
//
// Grouping is by *area* - the middle part of a zone id, so `Z1_Primevalley_Col`
// and `Z1_Primevalley_Island` land in one Primevalley route. Zones are small
// enough that a per-zone chest route would often hold two chests, and areas
// are the unit players actually talk in ("chest run in Krisomal").
//
// Waypoints come out in a greedy nearest-neighbour order, so the file reads as
// a sensible circuit and `mode = order` would follow one. The routes ship as
// `mode = nearest` regardless, because you can enter an area from any side and
// the navigator's nearest-first rule then re-derives the circuit from wherever
// you actually are.
//
// With --install (the default when the game is found) the file is also copied
// next to Farever.exe, where the host reads it. Your own routes live in
// farever-routes-custom.txt and are never touched by this.
// ---------------------------------------------------------------------------

import { existsSync, mkdirSync, writeFileSync, copyFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { openPak } from './lib/pak.mjs';
import { readHBSON, walkNodes } from './lib/hbson.mjs';
import { requireGame } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out', 'routes');

const game = requireGame();
const install = !process.argv.includes('--no-install');

// --- what counts as worth walking to ----------------------------------------
//
// Keyed on the prefab a node references, which is how the level tiles name
// what a thing is. Ordered: the first pattern that matches wins, so the
// specific chests come before the general gatherable folders.
//
// `group` is the route a node joins; `label` names the single waypoint. A
// null group means "known, and deliberately not a route" - grapple points and
// bumpers are traversal furniture, not something you collect.

const KINDS = [
  { re: /Elements\/WorldChest\.prefab$/,               group: 'World chests',   label: 'World chest' },
  { re: /Elements\/Recipe_Chest\.prefab$/,             group: 'Recipe chests',  label: 'Recipe chest' },
  { re: /Activities\/OrbChest\.prefab$/,               group: 'Orb chests',     label: 'Orb chest' },
  { re: /Activities\/VaultChest\.prefab$/,             group: 'Vault chests',   label: 'Vault chest' },
  { re: /Activities\/CampChest\.prefab$/,              group: 'Camp chests',    label: 'Camp chest' },
  { re: /Secret_World\/RedOrb_World\.prefab$/,         group: 'Secret orbs',    label: 'Secret orb' },
  // The folder says ore or plant; the file's basename says which one, with an
  // optional _Small/_Large suffix for the node's size. Both parts of the path
  // before the basename vary (Ores/Z1/, Ores/), so they are skipped rather
  // than matched - a greedy `.*` here silently ate the basename and grouped
  // every ore in the game under the letter "l".
  { re: /Gatherables\/Ores\/(?:.*\/)?([A-Za-z0-9]+?)(?:_(Small|Large))?\.prefab$/,
    group: (m) => `${spaced(m[1].replace(/Ore$/, ''))} Ore`,
    label: (m) => sized(spaced(m[1].replace(/Ore$/, '')) + ' Ore', m[2]) },
  { re: /Gatherables\/Plants\/(?:.*\/)?([A-Za-z0-9]+?)(?:_(Small|Large))?\.prefab$/,
    group: (m) => spaced(m[1]),
    label: (m) => sized(spaced(m[1]), m[2]) },
];

// AncientThyme -> "Ancient Thyme"; R2PlantRare -> "R2 Plant Rare".
const spaced = (s) => s.replace(/([a-z0-9])([A-Z])/g, '$1 $2').replace(/_/g, ' ');
const sized = (base, size) => (size ? `${base} (${size.toLowerCase()})` : base);

function classify(source, node) {
  for (const k of KINDS) {
    const m = source.match(k.re);
    if (!m) continue;
    const group = typeof k.group === 'function' ? k.group(m, node) : k.group;
    const label = typeof k.label === 'function' ? k.label(m, node) : k.label;
    return group ? { group, label } : null;
  }
  return null;
}

// Z1_Primevalley_Island -> { area: 'Primevalley', zone: 'Primevalley Island' }.
// A zone with no area part (Z4_Ebral) is its own area.
function splitZone(zone) {
  if (!zone) return null;
  const parts = zone.split('_');
  if (parts.length < 2) return { area: zone, zone };
  const area = parts[1];
  return { area, zone: parts.slice(1).join(' ') };
}

// --- scan the world ---------------------------------------------------------

const pak = openPak(join(game, 'res.map.pak'));
const worlds = new Map();
for (const f of pak.files) {
  const m = f.path.match(/^Level\/World\/([^/]+)\.dat\/gameplayData\/.*\.prefab$/);
  if (!m) continue;
  if (!worlds.has(m[1])) worlds.set(m[1], []);
  worlds.get(m[1]).push(f);
}
if (!worlds.size) {
  console.error('no world tiles in res.map.pak - has the layout changed?');
  process.exit(1);
}

const points = [];    // {group, label, area, zone, level, x, y, z}
const unzoned = [];   // collectibles with no baked zone, filed in a second pass
let tiles = 0, unreadable = 0;

for (const [, files] of worlds) {
  for (const f of files) {
    let doc;
    try {
      doc = readHBSON(pak.read(f));
    } catch (e) {
      unreadable++;
      continue;
    }
    tiles++;
    walkNodes(doc.root, (node, x, y, z) => {
      const source = node.source;
      if (typeof source !== 'string') return;
      const hit = classify(source, node);
      if (!hit) return;
      const props = node.props || {};
      const where = splitZone(props.zoneBaked || node.zoneBaked || null);
      if (!where) { unzoned.push({ hit, x, y, z }); return; }
      points.push({
        group: hit.group,
        label: hit.label,
        area: where.area,
        zone: where.zone,
        level: (props.props && props.props.level) || null,
        x, y, z,
      });
    });
  }
}
pak.close();

// Plenty of gathering nodes carry no baked zone - they sit on terrain the
// zone shapes do not cover, or inside a prefab that was placed before the
// zone was drawn. Dropping them would lose most of the herb and ore map, so
// each takes the zone of the nearest node that does have one. That is exactly
// the question being asked ("which area is this in"), and being wrong about a
// node on a zone border costs nothing: it lands in the neighbouring route,
// fifty metres from where it would otherwise have been.
let adopted = 0;
if (points.length) {
  // Snapshot first: an adopted node must not itself become a source of
  // zoning, or one mistake at the edge of the map propagates inward.
  const zoned = points.slice();
  for (const u of unzoned) {
    let best = null, bestD = Infinity;
    for (const p of zoned) {
      const dx = p.x - u.x, dy = p.y - u.y, dz = p.z - u.z;
      const d = dx * dx + dy * dy + dz * dz;
      if (d < bestD) { bestD = d; best = p; }
    }
    if (!best) continue;
    adopted++;
    points.push({ group: u.hit.group, label: u.hit.label, area: best.area,
                  zone: best.zone, level: null, x: u.x, y: u.y, z: u.z });
  }
}
console.log(`${tiles} tiles read (${unreadable} unreadable), ` +
            `${points.length} collectible nodes ` +
            `(${adopted} took a neighbour's zone)`);

// --- one route per (group, area) --------------------------------------------

const MIN_POINTS = 3;    // two chests is a pair, not a route

const routes = new Map();
for (const p of points) {
  const key = `${p.area}|${p.group}`;
  if (!routes.has(key))
    routes.set(key, { name: `${p.area} - ${p.group}`, area: p.area,
                      group: p.group, points: [] });
  routes.get(key).points.push(p);
}

// Greedy nearest-neighbour from the westernmost point. Not an optimal tour and
// not trying to be: it exists so the file reads as a path rather than as tile
// order, and the navigator re-picks the nearest one live anyway.
function order(points) {
  if (points.length < 3) return points.slice();
  const left = points.slice();
  let cur = left.reduce((a, b) => (a.x <= b.x ? a : b));
  left.splice(left.indexOf(cur), 1);
  const out = [cur];
  while (left.length) {
    let best = 0, bestD = Infinity;
    for (let i = 0; i < left.length; i++) {
      const dx = left[i].x - cur.x, dy = left[i].y - cur.y;
      const d = dx * dx + dy * dy;
      if (d < bestD) { bestD = d; best = i; }
    }
    cur = left.splice(best, 1)[0];
    out.push(cur);
  }
  return out;
}

const kept = [...routes.values()]
  .filter((r) => r.points.length >= MIN_POINTS)
  .sort((a, b) => a.name.localeCompare(b.name));

const dropped = routes.size - kept.length;

let text =
  '# farever-modkit routes, generated by tools/gen-routes.mjs from the\n' +
  "# game's own level data. Regenerate after a patch; this file is\n" +
  '# overwritten wholesale, so put anything of your own in\n' +
  '# farever-routes-custom.txt instead (the Routes page writes there).\n' +
  '#\n' +
  '# Format: [name], then `mode = nearest|order`, then one waypoint per\n' +
  '# line as `x, y, z, label`.\n';

for (const r of kept) {
  const pts = order(r.points);
  const zones = [...new Set(pts.map((p) => p.zone))];
  text += `\n[${r.name}]\nmode = nearest\n`;
  text += `zone = ${zones.slice(0, 3).join(', ')}` +
          (zones.length > 3 ? ` +${zones.length - 3} more` : '') + '\n';
  for (const p of pts) {
    const lvl = p.level ? ` lv${p.level}` : '';
    text += `${p.x.toFixed(1)}, ${p.y.toFixed(1)}, ${p.z.toFixed(1)}, ` +
            `${p.label}${lvl} - ${p.zone}\n`;
  }
}

mkdirSync(OUT, { recursive: true });
const outPath = join(OUT, 'farever-routes.txt');
writeFileSync(outPath, text);

const total = kept.reduce((n, r) => n + r.points.length, 0);
console.log(`${kept.length} routes, ${total} waypoints -> ${outPath}` +
            (dropped ? ` (${dropped} groups under ${MIN_POINTS} points dropped)` : ''));
for (const r of kept.slice(0, 8))
  console.log(`  ${r.name} (${r.points.length})`);
if (kept.length > 8) console.log(`  ... and ${kept.length - 8} more`);

if (install) {
  try {
    copyFileSync(outPath, join(game, 'farever-routes.txt'));
    console.log(`installed next to ${join(game, 'Farever.exe')}`);
  } catch (e) {
    console.error(`install failed (${e.message}) - copy ${outPath} into ` +
                  `${game} yourself`);
  }
}
