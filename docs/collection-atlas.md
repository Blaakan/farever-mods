# Collection Atlas

A completion tracker for Farever: what collectibles exist, which ones you still
need, and where to find them.

Install: copy [`plugins/collection_atlas.lua`](../plugins/collection_atlas.lua)
into `<Farever>\data\plugins\`.

> There is now also a **native Collection Atlas UI** in the standalone host
> ([`host/`](../host/README.md#the-collection-atlas-ui)): every item that
> exists per category with icons, owned/missing state, rarity, and
> how-to-acquire tooltips, read from the account collection in memory. This
> plugin remains the farever-minimap flavour, limited to what the sandbox
> can observe.

## What it adds over the built-in minimap

The host mod already draws collectible markers and lets you right-click one to
dim it. Collection Atlas is the bookkeeping layer on top of that data:

- **Completion percentages** per category, and a combined total.
- **Completion by area** — POIs are clustered on a grid and each cluster is
  named after the nearest landmark POI, so the answer to "where are the rest"
  reads as *"7 chests left near \<dungeon name\>"* rather than raw coordinates.
- **Nearest-uncollected list** with distance, a heading glyph, and altitude
  delta (`dz`), which matters a lot in a game with gliding and vertical terrain.
- **Route planner** — greedy nearest-neighbour over the closest N uncollected,
  pushed out as real map waypoints.
- **Discovery log** for the things the POI table does *not* cover: mounts,
  gliders, gear, materials — harvested from your inventory.
- **Bestiary progress** via the codex API.
- **JSON export** of the whole picture.

## Tabs

### Dashboard
Per-category progress bars for one-shot collectibles (chests, red orbs), plus a
combined total. Respawning nodes (plants, ore) are listed with spawn-point
counts but no completion — they come back, so a percentage would be meaningless.
Landmarks are counted separately.

### Nearby
The closest uncollected entries in the selected category. Each row shows a
heading glyph (`^` ahead, `>` right, `v` behind…), the 3D distance, and the
vertical offset. **Waypoint** drops a pin; **Mark done** ticks it off.

### Areas
Completion per region, worst-first, so the top of the list is where your
remaining collectibles actually are. **Waypoint** pins the area centroid.

Area size is the grid cell, default 500m (Settings). Bigger cells give fewer,
broader regions; smaller cells give more precise ones.

### Route
Plans a collection run: takes the nearest uncollected in the current category
and orders them greedily by travel distance. **Place waypoints** pushes them as
numbered pins; **Clear waypoints** removes only the ones this plugin created,
never your own.

### Collections
Account-wide unlock records, mirroring how Farever actually treats these
items — they live in collection lists, not in your bag:

- **Mounts** and **gliders**, shown against the known totals for this build
  (63 and 70, extracted from the game's bytecode by
  [`tools/scan-hlboot.mjs`](../tools/scan-hlboot.mjs)).
- **Appearances**: armor unlocks its appearance account-wide when obtained and
  is then recycle/sell fodder. Every armor piece seen in your gear or bag is
  recorded as an unlocked appearance — the record survives selling the item,
  which is the whole point.

The plugin API has no account-collections getter, so ownership is **observed**:
an item is recorded the first time it passes through your equipment or bag.
Equip each mount and glider once and the collection fills in. These records are
stored account-wide (no character suffix), so every character sees the same
list.

### Vault
Weapons and trinkets — the items that unlock nothing and are usually worth
keeping in the bank. Each records the best level/upgrade observed and which
character last improved it.

Routing is by equipment `slot_name` where available (`Weapon1`, `Trinket`,
`Neck`, the finger slots…), with id-prefix fallbacks for bag items. Materials
and consumables are deliberately **not tracked** — they are churn.

The bank itself is not readable through the plugin API; the vault records what
passed through your hands.

### Codex
Bestiary completion, recorded whenever you change target. Shows progress per
monster and an overall completion bar across everything you have encountered.

### Settings
Auto-mark, alerts, area grid size, export, and a reset.

## Marking things collected

**This plugin keeps its own collected-set, separate from the host mod's.** The
mod stores yours in `poi_done__<name>.json` but does not expose it to plugins,
so there is no way to read or write it from Lua. The two are independent by
design.

In practice: tick things off here (or use auto-mark), and treat the minimap's
own dimming as a separate view.

Progress is keyed **per character**, matching how the host mod does it, so a new
alt starts clean.

### Auto-mark

Off by default. When on, walking within *N* metres (default 6) of an uncollected
chest or orb marks it collected.

It is a **proximity heuristic, not a pickup event** — the API exposes no
"container opened" signal. Running past an unopened chest will mark it. Keep the
range tight, or leave it off and use the *Mark done* buttons.

Auto-mark writes to disk immediately rather than waiting for the batch window,
so a crash never costs you a pickup.

## Limitations

- **Separate collected-set** from the host mod's, as above.
- **Auto-mark is proximity-based** and can produce false positives.
- **Ownership is observed, not queried.** A mount you own but never equip while
  the plugin runs is not recorded. Totals (x / 63) are known from the bytecode
  scan; the per-id checklist fills in as you cycle your collection once.
- **Appearance unlocks are inferred** from armor sightings; armor obtained and
  recycled before the plugin ever saw it will be missing until it appears again.
- **`farever.store` holds scalars only**, so the collected set is packed into a
  comma-joined string. A POI id containing a comma cannot round-trip; those are
  skipped with a warning in `farever-mod.log` rather than corrupting the set.
  No id in the shipped POI data has one.
- **POI data comes from the host mod**, not the game — it ships
  `data/pois_<world>.json`. New content appears when that file is updated.
- **Respawning nodes are excluded from the "(all one-shot)" category** unless you
  opt in; selecting `plant` or `ore` explicitly always shows them.

## Performance

`farever.pois()` returns a full snapshot per call (~1224 entries), so it is
polled at 1 Hz and cached, never per frame. Store writes are batched to at most
one every 3 seconds — except auto-mark, which flushes immediately. The proximity
scan is a single pass over the POI list per poll.

## Export

**Settings → Export JSON** writes to
`%LOCALAPPDATA%\farever-minimap\combatlogs\collection-atlas-<character>.json`
(the only file destination the sandbox allows). Contains category totals, area
completion with centroids, the discovery log with categories, and codex state.
