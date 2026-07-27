# farever-mods

Two mods for **Farever** (Shiro Games), written as Lua plugins for the
[farever-minimap](https://github.com/ramisotti13-eng/farever-minimap) plugin
runtime.

| Plugin | What it does |
|---|---|
| **[Collection Atlas](docs/collection-atlas.md)** | Completion tracker: what collectibles exist, which you still need, and where to find them — by category, by area, with routing |
| **[AuraForge](docs/aura-forge.md)** | A WeakAuras-style HUD: movable buff bars, cooldown bars, and rule-driven alerts |
| **[id_scanner](docs/scanning.md)** | Discovery tool: probes the live plugin API, records every event and internal id your build exposes |

Both are read-only and informational. Neither writes to the game, automates
play, or touches the network.

## Why Lua plugins

Farever runs on Shiro Games' own stack — Haxe → HashLink → Heaps.io, rendering
through Direct3D 12. It is **not** a Unity game, so the Unity mod-loader advice
that dominates search results (BepInEx, MelonLoader) does not apply. There is no
official mod API or Workshop.

The one real extension point today is the sandboxed **Lua 5.4 plugin runtime**
inside the farever-minimap overlay mod. That is what these target.

Full write-up, with sources and the claims I could not verify:
**[RESEARCH.md](RESEARCH.md)**.

On permission — Shiro Games, on their official Discord:

> While we won't promote the use of add-ons during the EA (to keep players on
> the intended experience at first), we won't condemn personal use of add-ons
> like minimaps or DPS meter.

Tolerated for personal use, not endorsed. Read
[the risk section](RESEARCH.md#is-it-allowed) before installing anything.

## Install

1. Install [farever-minimap](https://github.com/ramisotti13-eng/farever-minimap)
   (extract into the folder containing `Farever.exe`).
2. Copy either or both files from [`plugins/`](plugins/) into:

   ```
   <Farever>\data\plugins\
   ```

3. That's it — the mod picks up new files about a second later, no restart. Open
   the Plugin Manager (funnel button on the minimap → *Show plugin manager*) to
   confirm they loaded.

Editing a plugin while the game runs hot-reloads it on save.

## First run

**Collection Atlas** works immediately — it reads the POI table the host mod
already ships (1224 entries on Siagarta, including 147 chests and 199 red orbs).
Open your bag once so it can scan your inventory for the discovery log.

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
plugins/
  collection_atlas.lua     the tracker
  aura_forge.lua           the HUD
  id_scanner.lua           API / event / id discovery
docs/
  collection-atlas.md      usage + design notes + limitations
  aura-forge.md            usage + design notes + limitations
  scanning.md              static + runtime discovery, naming conventions
tools/
  check-plugins.mjs        static/sandbox checks
  run-harness.mjs          runtime tests
  scan-hlboot.mjs          HashLink bytecode string-table extractor
  scan-hltypes.mjs         HashLink type/field-offset extractor
  pak-extract.mjs          Shiro/Heaps .pak reader (list + extract)
  gen-atlas.mjs            Collection Atlas data: item TSV + BC7 icon atlas
  lib/pak.mjs              the pak format, as a library
  harness/mock_host.lua    mock of the plugin API
host/                      standalone mod host (dxgi proxy + reader + overlay UI)
RESEARCH.md                how Farever can be modded, with sources
```

## Licence

MIT — see [LICENSE](LICENSE).

Not affiliated with Shiro Games. Farever is © Shiro Games.
