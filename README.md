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
  trinkets, weapons, consumables, materials, recipes, augments, misc,
  creatures and runes — each with the game's own icon, name and description
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

## Weapon Mastery

Farever levels every weapon separately, and levelling one is killing things
with it. The game shows you the weapon in your hands, one level at a time.
The **Mastery** tab shows every weapon your character can equip, each with
**one bar for the whole track — a notch per level, not just the level you are
on**. Which weapons you have actually invested in, and how much of each is
left, is one glance.

The bar is the game's own arithmetic, not an estimate: a kill count per
weapon is the only thing the save stores, and 20 kills is a level (26 for a
shield) up to a ceiling set by how many of that weapon's own skills can take
a point. A weapon with no upgradeable skills says *no mastery track* rather
than drawing an empty bar.

**The weapon in your hand gets its own bar on screen**, with the atlas
closed — so you can watch a track fill while you fight instead of opening a
window to check. Nothing to set up and nothing to choose: swap weapon and
the bar follows, swap character and it follows that too. It goes away when
there is nothing left to watch — an empty hand, or a weapon already
mastered.

It is the same size as the game's own XP bar, read out of the game's
stylesheet and scaled the way the game scales it, so it matches on any
monitor. Drag it anywhere while the atlas is open.

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

## Chat

The game's chat box keeps a few lines, shows no timestamp — although it
records one on every message — and has **no ignore list at all**. That last
one is not an oversight of the UI: there is no ignore, mute or block function
anywhere in the client, and no chat command for one. This draws its own
window over the game's message area, out of the history the game already
keeps.

**What has been seen working, and what has not.** This has now been run in a
live session, which is worth being exact about rather than summarising. Seen
on screen: the session's scrollback read out of `ChatClient.history`, the
anonymous-structure decode, the channel classification, whisper targets
resolved to a name (*To Emsey: test whisper*), the timestamps the game records
and never shows, the channel chips, and the window drawing over the game's
message area. Two things failed in that run and have been fixed since — the
chat box was looked for in `ui.BaseUI.elements`, where it does not live (the
game's own route is `gui.gameRoot.hud.chat`), and the message area was sized
by guesswork rather than by reading the `h2d.Flow` fields that hold it.
**The `!!` command surface is the one part still unconfirmed:** it did not
fire in that session, the cause was found and fixed, and no command has yet
been seen to run in game.

- **The whole session's scrollback.** `ChatClient.history` is appended to and
  never trimmed, so everything said since you logged in is still there —
  wheel over the window to scroll back, and a `12 newer` badge says what is
  below you
- **Timestamps**, per line, which the game records and never shows
- **An ignore list** — `!!ignore <name>` and they are gone from the window and
  from the log. The `Ignored N` chip along the bottom shows who is on it and
  removes anyone with a click; it is a plain text file next to the game, so it
  is editable and it survives a reinstall. **The game has none of this** —
  there is no ignore, block, mute or report anywhere in the build
- **Per-channel filters** — Local, All, All system, Whisper, Group, System,
  each a chip you click with the atlas open
- **Item links** — `[Copper Ore]` in anybody's message draws with that item's
  own icon and rarity colour, and `!!link <name>` copies one to the clipboard
  ready to paste. Everyone without the mod sees the plain text they always
  did. This works from a typed name for gathered materials, consumables and
  recipes; most of the atlas is keyed by an internal id instead, and for those
  the id is what you type — see [docs/chat.md](docs/chat.md#item-links)
- **An optional session log**, appended to `farever-chat-log.txt`, flushed per
  line so it can be tailed while you play

**The commands cost the server nothing**, and this is the whole reason they
are typed into the game's own box rather than into a field of ours.
`ui.hud.ChatBox.processMessage` knows exactly four commands — `!say`, `!map`,
`!group`, `!to` — and for anything else prints *Unknown chat command* locally
and returns; the call that actually sends sits past that return. So
`!!ignore Someone` draws one line in your own client and stops there. The
host reads that line rather than writing anything. The disassembly is in
[RESEARCH.md](RESEARCH.md#can-a-mod-be-driven-by-in-game-commands).

**It cannot hide the game's own chat box**, because hiding it would be a
write. What it does instead is align to it: `h2d.Flow` records the box its own
layout settled on, so the message flow gives all four edges of the message
area and the window covers exactly that — leaving the footer and the text
field underneath visible and clickable. Enter still opens the game's input,
typing still goes to the game, and sending a message is still the game doing
it. What the host cannot do is hide the game's own box — that would be a
write — so the game goes on drawing its copy of every line underneath. The
window is **opaque** for that reason rather than as a style choice;
`!!opacity` turns it down if you would rather see the world through it, at the
cost of reading every message twice. `!!size` sets the text size.

The honest limits, all of them consequences of reading only: a sender's name
is read when the sender is a player character and left blank otherwise; the
far end of a whisper is best-effort and blank when it cannot be validated; the
timestamp is when the mod first saw the line, not the game's own arrival
stamp; and a command runs when the chat input goes empty, which Escape does as
well as Enter — so a command abandoned, or typed and submitted inside a tenth
of a second, runs with whatever arguments it had at that moment. Matching is
on the first token, so `!!ignore Emsey` caught as `!!ignore Em` is a valid
command with a shorter argument and it runs on the wrong name. Nothing a
reader can do closes that window, so when the text did not stand still for a
whole poll before the box emptied the window prints the string it is about to
act on first — wrong, but never silently wrong. Every command is local and
read-only, which is what makes that a tolerable failure rather than a
dangerous one.

Full reference — every command, every setting, the file formats:
**[docs/chat.md](docs/chat.md)**.

## Everything else

The first two rows are this repo's own runtime and toolchain, and need nothing
installed but the game. The last three are **Lua plugins**, written before the
host existed: they run inside
[farever-minimap](https://github.com/ramisotti13-eng/farever-minimap)'s sandbox
and only work if you have that mod instead of this one.

| Component | What it does |
|---|---|
| **[host/](host/README.md)** | The standalone mod host: a `dxgi.dll` proxy, a D3D12 overlay, and a HashLink memory reader. The Atlas runs on this |
| **[tools/](docs/scanning.md)** | The extraction toolchain: `.pak` archives, the CastleDB, HashLink type offsets, the world's prefabs |
| **[Collection Atlas (Lua)](docs/collection-atlas.md)** | The original tracker. Sees only what the sandbox exposes — no account collection, no other characters' bags. Still maintained, but the host version above is the one to install |
| **[AuraForge](docs/aura-forge.md)** | A WeakAuras-style HUD: movable buff bars, cooldown bars, rule-driven alerts. Not ported to the host yet |
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

1. Install **[Node.js](https://nodejs.org/)** (LTS) if you do not have it —
   `winget install OpenJS.NodeJS.LTS` does it.
2. Download the latest zip from
   **[Releases](https://github.com/Blaakan/farever-mods/releases)**.
3. Right-click the zip → **Properties** → **Unblock**, then extract it
   anywhere.
4. Run **`install.cmd`**.

You do not have to close the game first. Windows will not let a loaded DLL be
overwritten, but it will let it be *renamed* — so the installer moves the old
one aside, puts the new one in its place, and sweeps up the leftover on its
next run. The session you have open carries on with the DLL it already
loaded; restart the game to pick up the new one.

It finds your install through Steam's own library list, builds the item
database out of your copy of the game, and copies one `dxgi.dll` next to
`Farever.exe`. Launch, and press **F8**.

If it cannot find the game, tell it:

```bash
install.cmd --game "D:\SteamLibrary\steamapps\common\Farever"
```

**Why Node.js.** The atlas is 1639 items with the game's own names, icons,
descriptions and loot tables. That is Shiro Games' data, not ours to put in a
download — so the generators ship and the haul does not, and they run on Node
(18 or newer; no `npm install`, they use only Node's own libraries). It is
used once at install time and never while you play.

**Do not run this alongside farever-minimap.** That mod arrives as
`dinput8.dll` and this one as `dxgi.dll`, so both load, and both install a
D3D12 overlay and a window hook on the same window. Pick one.

**Why Windows will complain.** It is an unsigned DLL, downloaded from the
internet, that loads into a game. So is malware, and a scanner cannot tell the
difference by looking. SmartScreen: *More info* → *Run anyway*. If you would
rather not take anyone's word for it, everything here is source and builds in
one command — see below.

To remove it: run `uninstall.cmd`, or delete that one `dxgi.dll` — the game is
back to stock either way, because there is no installer, no service and no
registry key. `uninstall.cmd --purge` also removes the generated data
(`farever-atlas.tsv`, the icon atlas, `farever-routes.txt`) and the settings
and logs beside them; routes you recorded yourself, in
`farever-routes-custom.txt`, are kept unless you add `--force`.

### From source

Needs Node.js and the MSVC C++ x64 toolset — Visual Studio 2022 Community or
the standalone Build Tools, with **Desktop development with C++**. Nothing
else: the generators use only Node's own libraries, and `npm install` is for
the Lua plugin tests alone.

**Order matters, and only in one place.**

```bash
node tools/gen-offsets.mjs
```

```bash
host\build.cmd
```

```bash
install.cmd
```

`gen-offsets` reads your game's bytecode and writes
`host/src/offsets.gen.h` — the field offsets the reader walks, and the hash of
the bytecode they came from. That header is **compiled into the DLL**, so it
has to be written before the build, not after. Build first and you get a DLL
carrying someone else's offsets, which the host then correctly refuses to use:
`farever-modkit.log` says `build: MISMATCH` and F8 does nothing.

`install.cmd` does the rest — the item database, the routes, and the DLL, all
placed next to `Farever.exe`.

**After a game patch**, one command does the lot:

```bash
node tools/update.mjs --fix
```

It diffs the field offsets so you see exactly what moved, regenerates them,
rebuilds, reinstalls, and rebuilds the atlas and routes as well — a patch can
add an item or move a node, not just shift a field. Until you run it the host
refuses to read memory at all, so a patch degrades to "does nothing", never to
a crash.

### The Lua plugins (legacy, and need farever-minimap instead)

Nothing above requires these. They are the pre-host versions, and because they
need the other mod's `dinput8.dll` you are choosing between the two — see the
conflict note above.

1. Install [farever-minimap](https://github.com/ramisotti13-eng/farever-minimap)
   (extract into the folder containing `Farever.exe`).
2. Copy any of the three files from [`plugins/`](plugins/) into
   `<Farever>\data\plugins\`.
3. The mod picks up new files about a second later, no restart. Editing a
   plugin while the game runs hot-reloads it on save.

## First run

**The Atlas** takes about twenty seconds after launch to find its way around,
then works from the main menu onward; the collection fills in once a
character is in the world. `farever-modkit.log`, next to the game, narrates
what it found. If something looks wrong, that file usually names the reason —
it opens with the mod version and the game build it verified against, which is
the first thing worth quoting in a bug report.

Three things it might say:

| In the log | What it means |
|---|---|
| `build: verified, offsets apply` | working |
| `build: MISMATCH` | the game was patched since this build. Every read is disabled, so the mod does nothing rather than something wrong. Get a newer release, or `node tools/update.mjs --fix` |
| `atlas_ui: ... farever-atlas.tsv missing` | the item database was never built. Run `install.cmd` |

**Collection Atlas (Lua)** works immediately — it reads the POI table
farever-minimap ships, 1224 entries on Siagarta: 313 plants, 283 red orbs,
263 ore nodes, 177 chests, 132 activities and the rest respawn points,
obelisks, dungeons and merchants. Open your bag once so it can scan your
inventory for the discovery log.

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

```bash
node tools/dis-hlcode.mjs ChatBox.processMessage
```

`scan-hltypes.mjs` says where a field lives and `scan-hlboot.mjs` says which
strings exist; neither says what the game *does*. This disassembles one named
function out of the bytecode, and because `hlboot.dat` ships full debug info
every instruction carries its real source file and line — so a claim about the
game's behaviour can be quoted with a line number instead of being asserted.
`--grep` lists matching function names, `--findex` dumps by index, `--stats`
prints the header. This is what settled that the game discards an unknown
`!command` before sending it, which is the whole basis of the chat window's
command surface.

CI runs all of the above plus a build of the DLL on a clean Windows runner
with no game installed, which is what catches "works on my computer". The
house rules that a change has to keep — reads only, generated offsets, and
saying so when the data does not know — are in
**[CONTRIBUTING.md](CONTRIBUTING.md)**, along with the one-line change that
helps most: a missing source filled in in `tools/atlas-overrides.tsv`.

## Layout

```
install.cmd                install into your game - the front door
uninstall.cmd              take it back out
host/                      the standalone mod host - see host/README.md
  build.cmd                  MSVC build, toolset found via vswhere
  src/version.h              the version, in one place
  src/offsets.gen.h          generated, and committed so CI can build
  src/dllmain.cpp            dxgi proxy, worker threads, build-hash gate
  src/dxgi_wrap.*            forwarding the real dxgi.dll's exports
  src/hl_runtime.*           HashLink's own structures; validated reads
  src/hl_scan.cpp            finding objects without hooks
  src/hl_reader.*            the game-state surface: collection, items, codex, jobs
  src/overlay.h              the drawing API the UI code is built out of
  src/overlay_d3d12.cpp      Present hook, font atlas, textured quads
  src/input.*                WndProc subclass: toggle, mouse, text
  src/atlas_ui.*             the Collection Atlas window
  src/navigator.*            the waypoint arrow, and following a route
  src/routes.*               saved routes: files, share codes, the Routes page
  src/mapwatch.*             waypoints from the game's own map, read-only
  src/loot.*                 the Recent Loots feed
  src/chat.*                 the chat window, and the !! command surface
tools/
  install.mjs              what install.cmd runs: find, generate, copy, check
  package.mjs              build the release zip
  gen-offsets.mjs          field offsets -> host/src/offsets.gen.h
  gen-atlas.mjs            the Atlas database: item TSV + BC7 icon atlas
  gen-routes.mjs           the generated route set, out of the world's tiles
  scan-hlboot.mjs          HashLink bytecode string-table extractor
  scan-hltypes.mjs         HashLink type/field-offset extractor
  dis-hlcode.mjs           HashLink disassembler: dump one function by name
  pak-extract.mjs          Shiro/Heaps .pak reader (list + extract)
  pe-imports.mjs           what a PE imports and exports
  update.mjs               post-patch flow
  lib/game.mjs             finding the install, on anyone's machine
  lib/pak.mjs              the .pak format, as a library
  lib/hbson.mjs            the prefab format, as a library
  atlas-overrides.tsv      hand-curated corrections to the generated data
  check-plugins.mjs        static/sandbox checks for the Lua plugins
  run-harness.mjs          runtime tests for the Lua plugins
  harness/mock_host.lua    mock of the plugin API
.github/
  workflows/ci.yml         proves it builds on a machine that is not mine
  workflows/release.yml    tag -> the runner builds the zip -> release
  ISSUE_TEMPLATE/          bug and data reports, both asking for the log
plugins/                   the legacy Lua plugins, for farever-minimap
  collection_atlas.lua     the original tracker
  aura_forge.lua           the HUD
  id_scanner.lua           API / event / id discovery
docs/                      usage, design notes and limitations
RESEARCH.md                how Farever can be modded, with sources
CONTRIBUTING.md            setup, what CI checks, and the read-only house rules
```

## Licence

MIT — see [LICENSE](LICENSE).

Not affiliated with Shiro Games. Farever is © Shiro Games.
