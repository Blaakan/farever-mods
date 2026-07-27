# Collection Atlas

A completion tracker for Farever: what collectibles exist, which ones you still
need, and where to find them.

Install: copy [`plugins/collection_atlas.lua`](../plugins/collection_atlas.lua)
into `<Farever>\data\plugins\`.

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

### Discoveries
Every distinct item kind seen in your bag or on your character, bucketed into
categories.

It is a **discovery log, not a checklist**: it records what you have found as
you find it. New items fire a toast when they first appear.

Categories are matched by anchored prefixes in `ITEM_CATEGORIES` at the top of
the plugin. Those prefixes are **not guesses** — they were read out of the
game's own HashLink bytecode by
[`tools/scan-hlboot.mjs`](../tools/scan-hlboot.mjs), which finds 63 `Mount_*`
ids, 70 `Glider_*`, 74 `Recipe_*` and the full armour slot set. See
[docs/scanning.md](scanning.md).

Prefixes are tried before loose substrings, so `Chest_Z1U2_Cle` is correctly
armour rather than being pulled into another bucket. Anything unmatched lands
in **Unclassified**, where you can read the raw ids and add a rule — editing
the table and saving hot-reloads instantly:

```lua
{ cat = "Mounts", prefixes = { "mount_" } },
```

If you want a true *checklist* — "you own 12 of 63 mounts" — run
`node tools/scan-hlboot.mjs --lua` to generate the full id list from your own
install and paste it in. It is left out of the shipped plugin deliberately:
those lists are game data, not ours to redistribute.

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
- **Discovery, not completion**, for mounts/gliders/gear by default. The full id
  lists *can* be extracted (see above) but are not shipped, so out of the box
  the plugin counts what you have rather than what you lack.
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
