# farever-mods

Mods and modding tools for **Farever** (Shiro Games), plus the reverse
engineering they are built on.

Everything here is **read-only**. Nothing writes to the game, automates play,
or touches the network.

## The Collection Atlas

A completion tracker that runs as its own overlay — no other mod required.
It answers "what exists, what do I have, and where do I get the rest" for
every collectible category in the game.

- **1639 entries across 13 pages** — appearances, mounts, pets, gliders,
  trinkets, weapons, consumables, materials, recipes, augments, misc, runes,
  and a bestiary — each with the game's own icon, name and description
- **Ownership read from memory**, per stack: *Bank x3 - Lv 25 - Rare*,
  *Equipped (Emsei) - Lv 5*, including characters who are not logged in
- **How to acquire** each item, inverted from the game's own loot tables,
  crafts and merchant stalls
- **A waypoint arrow** to whatever you are hunting, camera-relative, with
  the distance and direction below it
- **Search across every page**, and per-page filters (weapons by class,
  recipes by job, creatures by type and region, gear by slot)

Press **F8** in game. See **[host/README.md](host/README.md)** for how it
works and how to build it.

## Routes

The waypoint arrow follows a whole list, not one place. **69 routes and 1001
waypoints** are generated straight out of the game's level data — every world,
recipe, orb, vault and camp chest, every secret orb, and every ore and herb
node, grouped by area. Start one and the arrow points at the nearest waypoint
you have not reached yet; walk into it and it is crossed off and the next one
appears, with a `7 / 23` progress bar on the pill.

- **Ctrl+click an atlas entry** to add it to the route, **Shift+click** to put
  it first — a run assembled out of the atlas itself
- **Click a point of interest on the game's own map** and it becomes a
  waypoint — the map already runs the hit test and every marker already knows
  its world position, so this is a read, and the click still reaches the game
- **F9 drops a waypoint where you stand** — recording a route is walking it
- **F10 skips the current waypoint**, **Shift+F10** clears the route
- **Save as route** names what you dropped and keeps it
- **Copy / Import** move a route through the clipboard as one `FMKR1:` line,
  so routes travel through Discord without anyone agreeing on where files live

The **Routes** tab in the atlas window drives all of it.

## Recent Loots

The game's own loot line lasts a second or two — not long enough to read mid
fight, and gone for good once it goes. This keeps a feed on screen: items with
their own icon and rarity colour, experience, currency and level-ups, newest
first, fading on their own timer. Open the atlas and it holds still so you can
read it.

There is no loot event to hook, because the host only reads, so the feed is a
diff of your bags, purse and experience twice a second. It reports **gains to
your bags** — a bank withdrawal reads the same as a chest, and losses are
never reported at all.

## Everything else

| Component | What it does |
|---|---|
| **[host/](host/README.md)** | The standalone mod host: a `dxgi.dll` proxy, a D3D12 overlay, and a HashLink memory reader. The Atlas runs on this |
| **[tools/](docs/scanning.md)** | The extraction toolchain: `.pak` archives, the CastleDB, HashLink type offsets, the world's prefabs |
| **[Collection Atlas (Lua)](docs/collection-atlas.md)** | The earlier version, a plugin for [farever-minimap](https://github.com/ramisotti13-eng/farever-minimap). Superseded by the host, kept because it works without it |
| **[AuraForge](docs/aura-forge.md)** | A WeakAuras-style HUD: movable buff bars, cooldown bars, rule-driven alerts. Still a farever-minimap plugin |
| **[id_scanner](docs/scanning.md)** | Discovery tool: probes the live plugin API, records every event and internal id your build exposes |

## Why any of this was hard

Farever runs on Shiro Games' own stack — Haxe → HashLink → Heaps.io, rendering
through Direct3D 12. It is **not** a Unity game, so the Unity mod-loader advice
that dominates search results (BepInEx, MelonLoader) does not apply. There is no
official mod API or Workshop.

That left two routes: the sandboxed **Lua 5.4 plugin runtime** inside the
farever-minimap overlay (what the plugins target), or **building a host of
your own** (what `host/` is). The second one can see things the sandbox
cannot — the account collection, the codex, what your other characters are
carrying — because it reads the game's own objects directly.

Full write-up, with sources and the claims I could not verify:
**[RESEARCH.md](RESEARCH.md)**.

On permission — Shiro Games, on their official Discord:

> While we won't promote the use of add-ons during the EA (to keep players on
> the intended experience at first), we won't condemn personal use of add-ons
> like minimaps or DPS meter.

Tolerated for personal use, not endorsed. Read
[the risk section](RESEARCH.md#is-it-allowed) before installing anything.

## Install

### The Collection Atlas (standalone)

Needs the MSVC C++ x64 toolset to build, and your own copy of the game to
generate its data from.

```bash
host/build.cmd
```

```bash
node tools/gen-offsets.mjs && node tools/gen-atlas.mjs && node tools/gen-routes.mjs
```

The first reads the game's bytecode for the field offsets the reader needs;
the second builds the item database and icon atlas out of the game's own
files; the third reads the world's level tiles for the starter route set.
All three copy their output next to `Farever.exe`. Then put
`host/build/dxgi.dll` in that same folder and launch. **F8** opens it.

Re-run all three after a game patch. The host refuses to read memory when the
bytecode hash does not match what its offsets were generated from, so a
patch degrades to "does nothing" rather than to a crash.

To remove it, delete that one `dxgi.dll`.

### The plugins (need farever-minimap)

1. Install [farever-minimap](https://github.com/ramisotti13-eng/farever-minimap)
   (extract into the folder containing `Farever.exe`).
2. Copy either file from [`plugins/`](plugins/) into `<Farever>\data\plugins\`.
3. The mod picks up new files about a second later, no restart. Editing a
   plugin while the game runs hot-reloads it on save.

## First run

**The Atlas** takes about twenty seconds after launch to find its way around,
then works from the main menu onward; the collection fills in once a
character is in the world. `farever-modkit.log`, next to the game, narrates
what it found. If something looks wrong, that file usually names the reason.

**Collection Atlas (Lua)** works immediately — it reads the POI table
farever-minimap ships (1224 entries on Siagarta, including 147 chests and 199
red orbs). Open your bag once so it can scan your inventory for the discovery
log.

**AuraForge** needs one setting: go to the **Layout** tab and pick your
resolution. The plugin API cannot query screen size, so anchors depend on it.
Then hit **Unlocked** to see outlines around every HUD element while you place
them.

## Development

The plugins are plain Lua with no build step. The tooling here exists because
the game is a slow feedback loop — a plugin error only shows up in
`farever-mod.log` after you alt-tab.

```bash
cd tools && npm install
```

```bash
node tools/check-plugins.mjs
```

Static checks: parses each plugin, flags anything the sandbox removes (`io`,
`require`, `load`, `debug`, `os.execute`…), catches `imgui.begin`/`imgui.end`
misuse, and reports unknown globals (usually typos).

```bash
node tools/run-harness.mjs
```

Runs both plugins inside a real Lua 5.4 VM ([fengari](https://fengari.io/))
against a mock host in [`tools/harness/mock_host.lua`](tools/harness/mock_host.lua)
that mirrors the documented API — including its constraints, so
`store.set` rejects non-scalars exactly like the real one. It drives frames,
fires events, walks every tab, and asserts on real behaviour: that cooldowns
count down and expire, that the buff tracker resolves both duration semantics,
that the HUD emits draw calls, and that the config survives a
serialise → reload → deserialise round-trip.

That harness earned its keep: it caught a guard that tested
`farever.waypoints` (a table) with a function check, which would have silently
disabled every waypoint feature in game.

```bash
node tools/scan-hlboot.mjs --lua
```

Extracts the internal id vocabulary straight out of the game's HashLink
bytecode — Farever ships interpreted, so `hlboot.dat` carries a full string
table. On the July 2026 build that yields **63 mounts, 70 gliders, 75 chest
types, 74 recipes, 142 statuses, 87 talents, 130 monsters** and more. This is
where `collection_atlas.lua`'s classification prefixes come from, rather than
guesswork.

Output lands in `tools/out/`, which is gitignored — it is bulk game data.
Commit the tool, not the haul. Details and naming conventions:
**[docs/scanning.md](docs/scanning.md)**.

## Layout

```
host/                      the standalone mod host - see host/README.md
  src/dllmain.cpp            dxgi proxy, worker threads, build-hash gate
  src/hl_runtime.*           HashLink's own structures; validated reads
  src/hl_scan.cpp            finding objects without hooks
  src/hl_reader.*            the game-state surface: collection, items, codex, jobs
  src/overlay_d3d12.*        Present hook, font atlas, textured quads
  src/input.*                WndProc subclass: toggle, mouse, text
  src/atlas_ui.*             the Collection Atlas window
  src/navigator.*            the waypoint arrow, and following a route
  src/routes.*               saved routes: files, share codes, the Routes page
  src/mapwatch.*             waypoints from the game's own map, read-only
  src/loot.*                 the Recent Loots feed
tools/
  gen-offsets.mjs          field offsets -> host/src/offsets.gen.h
  gen-atlas.mjs            the Atlas database: item TSV + BC7 icon atlas
  gen-routes.mjs           the generated route set, out of the world's tiles
  scan-hlboot.mjs          HashLink bytecode string-table extractor
  scan-hltypes.mjs         HashLink type/field-offset extractor
  pak-extract.mjs          Shiro/Heaps .pak reader (list + extract)
  update.mjs               post-patch flow
  lib/pak.mjs              the .pak format, as a library
  lib/hbson.mjs            the prefab format, as a library
  atlas-overrides.tsv      hand-curated corrections to the generated data
  check-plugins.mjs        static/sandbox checks for the Lua plugins
  run-harness.mjs          runtime tests for the Lua plugins
  harness/mock_host.lua    mock of the plugin API
plugins/
  collection_atlas.lua     the earlier tracker, for farever-minimap
  aura_forge.lua           the HUD
  id_scanner.lua           API / event / id discovery
docs/                      usage, design notes and limitations
RESEARCH.md                how Farever can be modded, with sources
```

## Licence

MIT — see [LICENSE](LICENSE).

Not affiliated with Shiro Games. Farever is © Shiro Games.
