# farever-modkit host

A standalone runtime for Farever mods, so Collection Atlas and AuraForge can
ship without requiring farever-minimap (and without its minimap and DPS
meter).

**Status: running in-game.** The proxy loads, the D3D12 overlay draws, the
reader reads, and the Collection Atlas, the navigator and the Recent Loots
feed all run on it. The roadmap table at the bottom says which stage landed
when. AuraForge is still a farever-minimap plugin.

## Why `dxgi.dll`

`Farever.exe` imports only `libhl.dll` and the CRT — it does **not** import
`dinput8.dll`. That vector (used by farever-minimap) is a dynamic load, almost
certainly SDL3 probing for joystick support.

`dx12.hdll` statically imports `dxgi.dll`, and `dxgi` is **not** in the
KnownDLLs registry list, so a copy in the application directory wins the
loader search. Static import means guaranteed load, early, every launch.

Exact DXGI functions the game's modules import, from
`node tools/pe-imports.mjs --funcs`:

| Module | Imports |
|---|---|
| `dx12.hdll` | `CreateDXGIFactory2` |
| `directx.hdll` | `CreateDXGIFactory` |
| `sl.common.dll` (Streamline/DLSS) | `CreateDXGIFactory` |
| `dinput8.dll` (farever-minimap) | `CreateDXGIFactory1` |

All five documented exports are forwarded anyway.

## What stage 2 unlocks: the real account collection

The plugin API has no collection reader, so `collection_atlas` can only record
items as they pass through your hands — meaning anything you unlocked before
installing it is invisible, and there is no way to ask "which appearances do I
already own?"

The data exists and is fully mapped. From `node tools/scan-hltypes.mjs`:

```
st.player.AccountProgress          extends st.DBState   sizeof=208
  +0x0a8  collection      : st.player.Collection
  +0x0b8  bank            : hl.types.ArrayObj
  +0x0c0  bankEquipment   : hl.types.ArrayObj
  +0x0c8  bankNbSlots     : I32

st.player.Collection               extends st.DBBaseState  sizeof=168
  +0x078  gliders  : hxbit.ArrayProxyData
  +0x080  mounts   : hxbit.ArrayProxyData
  +0x088  toys     : hxbit.ArrayProxyData
  +0x090  emotes   : hxbit.ArrayProxyData
  +0x098  gears    : hxbit.ArrayProxyData      <- armor appearances
  +0x0a0  pets     : hxbit.ArrayProxyData      <- companions
```

That is the authoritative, account-wide collection — the exact six categories
the game's own collection menu shows — reachable as
`st.Player -> AccountProgress.collection -> {mounts, gliders, pets, gears,
toys, emotes}`. `AccountProgress.bank` / `bankEquipment` covers the stored
weapons and trinkets the vault currently has to infer.

So the collection tracker becomes a real "12 / 63 owned" checklist rather than
a discovery log, with zero re-collecting — but only through the host's own
memory reader. Nothing in the plugin sandbox can reach it. This is the
strongest argument for finishing stage 2, and the offsets above mean it is
generation, not reverse-engineering: re-run the scan after a patch and the
addresses regenerate.

### Startup: one lookup, no instance sweep

`GameApp` is the root of everything the host reads - it holds the hero, the
camera and the account progress - and it is reached **without scanning for
it**. `App.inst` is a Haxe static, and a class's statics are fields of the
class-value object that `hl_type_obj.global_value` points at, so:

```
find_type_by_name("GameApp")     the one search that remains
  +0x08 hl_type_obj
    +0x18 super          -> App's hl_type
      +0x08 hl_type_obj
        +0x38 global_value -> $App  (the statics object)
          +0x30 inst       -> the GameApp instance
```

Four dereferences instead of a pass over ~8 GB of private memory. Because
`GameApp` exists from application start, the host is ready before the main
menu finishes loading, and the hero is then just `GameApp.hero` - which also
means a zone change or a character swap costs nothing, and that logging out
is noticed immediately (the field goes null). Earlier builds scanned for
`ent.Hero` directly, which meant repeating a full sweep every time the
player was not yet in the world: about two and a half minutes before the
first useful frame.

### The complete read path

`ent.Hero.player` at `+0x4b8` closes the chain, and it starts from the Hero —
the pointer every Farever mod already locates and tracks continuously. No new
root-finding technique is needed, and in particular no extra `hl_alloc_obj`
hook: this is a pure pointer walk off an anchor that already exists.

```
ent.Hero                        (already tracked)
  +0x4b8  player            -> st.Player
  +0x0e0    accountProgress -> st.player.AccountProgress
  +0x0a8      collection    -> st.player.Collection
                +0x080  mounts   -> hxbit.ArrayProxyData
                +0x078  gliders                +0x028 array -> hl.types.ArrayDyn
                +0x0a0  pets
                +0x098  gears     (armor appearances)
                +0x088  toys
                +0x090  emotes

  AccountProgress +0x0b8 bank / +0x0c0 bankEquipment -> hl.types.ArrayObj
```

`hxbit.ArrayProxyData` is a thin wrapper (`sizeof=48`) whose `array` field at
`+0x28` holds the actual `hl.types.ArrayDyn`.

Every offset above is generated from `hlboot.dat` by
`tools/scan-hltypes.mjs`, so a game patch is a re-run rather than a
re-investigation.

### Reader status (verified live)

```
collection: mounts=21 gliders=30 pets=16 gears=185
inventory:  Emsei bank=141 bankEq=6 equipped=20 bags=13
```

Written to `farever-collection.json` and `farever-inventory-<character>.json`
next to the game, on change only.

**Inventory slots are `HVIRTUAL`, not objects.** Both `bank`/`bankEquipment`
and `equipped`/`bags` hold Haxe structural values with fields stored inline
(`value` is null), which is why reading them as class instances rejected all
141 bank entries:

```
bank / bankEquipment   { count:Int, it:st.item.Gear | st.Item, slot:Int }
equipped / bags        { count:Int, item:st.item.Weapon | st.item.Gear }
```

The item field is `it` in the bank and `item` in inventories. Item classes
seen live: `st.Item`, `st.item.Gear`, `st.item.Armor`, `st.item.Weapon`.

**Rarity: solved twice over.** The live read first reported `-1` for
everything because `st.item.Weapon.rarity` is a **String**
(`"Common"`..`"Legendary"`, the CastleDB rarity ids), not a boxed enum - the
bytecode said so all along (`offsets.gen.h`: `OBJ : String`). Weapons now
decode their per-instance rarity live. Everything else has no such field
because rarity is a static property of the kind, and that comes from the
CastleDB itself: **`data.cdb` ships in `res.light.pak`** (the earlier "it is
compiled into hlboot.dat" conclusion was wrong - it just was not in
`res.pak`). `tools/gen-atlas.mjs` extracts it, which also yields the master
"every item that exists" lists, English names and descriptions, loot tables,
crafts and icon references in one pass.

### Two ways to reach it

**Upstream (fast).** The host mod's plugin API is read-only by design, but the
authoring guide invites requests: *"If you find a real-world use case that
needs one of these, open an issue. We can probably expose a safe wrapper for
it."* The maintainer has a track record of doing exactly that — issues #90
(class), #93 (inventory), #94 (weapon skills) and #100 (a currency) were all
API additions, shipped across v1.2.1–v1.2.4. A request for
`farever.player.collection()` carrying the offsets above is a small, safe,
read-only addition on a path the mod already walks.

**In-house (stage 2).** The host's own reader, which is the standalone goal
regardless. The pointer walk above is the whole job for collections; the
remaining work is the generic HashLink array decoding and the safe read
thread.

These are not exclusive: upstream gets the feature working in days, stage 2
removes the dependency.

### Verified

Stage 2 landed. A live run against the game produced:

```
collection: mounts=21 gliders=30 pets=16 gears=179 toys=0 emotes=0 bankSlots=6
  mount  SparkHorse_01
  mount  Mount_Ladybug_Blue
  glider Glider_Dragon_BlueGreen
  ...
```

Authoritative account ownership, with no equipping or observation required.
The offsets derived offline from `hlboot.dat` held exactly against live
memory.

Two lessons worth keeping:

- **Validate during the scan, not after.** Most qwords equal to a type
  pointer are metadata (the type table, proto arrays, type params of other
  types), not instances. Collecting "the first N matches" fills entirely on
  metadata — it hit a 64-item cap in 47ms and never reached a real object.
  Checking each candidate's full chain as it is found took 1,994 candidates
  and 5.2s to land on the right one.
- **Reading beats inferring.** `SparkHorse_01` is a mount with no `Mount_`
  prefix. Any name-prefix heuristic misses it; reading the collection does
  not.

## Roadmap

| Stage | What | State |
|---|---|---|
| 1 | dxgi proxy: load, forward, log | **built, ran in-game** |
| 2 | HashLink state reader (`hl_runtime`, `hl_scan`, `hl_reader`) + build-hash gate | **working in-game** — reads the real account collection |
| 2b | Post-patch update flow (`tools/update.mjs`) | **built, verified** |
| 3a | Swap-chain observation via the factory wrapper | **built** |
| 3b | D3D12 renderer: Present hook, PSO, font atlas, textured quads | **built, text verified in-game** |
| 3c | Native Collection Atlas UI (`atlas_ui.cpp` + `input.cpp`) | **working in-game** — 1547 entries across 12 pages |
| 4a | Navigator routes (`navigator.cpp`) + the Routes page (`routes.cpp`, `tools/gen-routes.mjs`) | **built** — awaiting in-game verification |
| 4b | Recent Loots (`loot.cpp`) | **built** — awaiting in-game verification |
| 4c | Waypoints from the game's map (`mapwatch.cpp`) | **built** — awaiting in-game verification |

Stage 3c replaced the original plan of porting the Lua plugins wholesale: the
tracker UI is now native to the host, driven by the host's own reader and the
generated game database, which the Lua sandbox could never reach anyway.
AuraForge remains a farever-minimap plugin.

## The Collection Atlas UI

One page per category — Appearances, Mounts, Pets, Gliders, Trinkets,
Weapons — every item that exists in the game on the page, owned ones in full
colour with a rarity border (white/green/blue/purple/gold), missing ones
dimmed. Hovering shows name, rarity, level (for levelled gear), the item's
description and how to acquire it (crafts, loot tables, world bosses,
vaults, merchants — inverted from the game's own CastleDB).

- **F8** toggles the window, **Escape** closes it
- drag the title bar to move it; position, and the active tab, persist in
  `farever-modkit.ini` next to the game
- mouse wheel scrolls; clicks over the window never reach the game
- ownership for trinkets/weapons = bank + bank equipment + equipped + bags,
  across characters (offline ones via their `farever-inventory-*.json`),
  and the tooltip lists every stack separately: *Bank x3 - Lv 25 - Rare*,
  *Equipped (Emsei) - Lv 5*, *Bags (other character) - Lv 9*

### The navigator

Sources with a fixed place - vault chests, dungeon bosses, merchants - carry
world coordinates (from the POI table farever-minimap ships). The tooltip
shows the distance and compass direction to the nearest one, and **clicking
the item** toggles tracking: a TomTom-style waypoint frame appears - a
shaded arrow with the distance, the item and the destination stacked under
it - and keeps pointing while you travel, atlas open or not. Click again
(or track something else) to stop. The tracked target survives restarts.

The frame is frameless over the world, the way a waypoint arrow should be.
**Open the atlas (F8) to move it**: a border appears while the window is
open, and you can drag the frame anywhere; its position persists. Tying
dragging to the atlas being open means the frame never swallows a click
during normal play. The arrow itself is two shaded facets split down a
centre crease, which reads as three-dimensional without a mesh, a texture
or a light.

**The arrow is camera-relative**, like the game's own map marker
(`ui.win.map.PlayerMarker` holds both a camera and a hero for exactly that
reason). It is derived as geometry rather than from an angle: a camera is
an `h3d.scene.Object`, so it carries a world position, and what the screen
faces is simply the direction from the camera to the hero. That sidesteps
the question of where a `direction` field's zero is and which way it
winds. The reading is cross-checked against the camera's own `curDistance`
- if the camera does not sit that far from the hero, the field is not what
we think it is and the arrow falls back to the hero's facing
(`ent.GameObject.rotationZ`). Both are sampled at 20Hz by a dedicated pose
thread; `farever-modkit.log` names which path drew the arrow.

**North is `-y`.** Not a guess: averaging POI positions for the zones the
game itself names North and South puts `Z3_CrimsonIsland_North` at y=-743
against `_South` at y=-420, and `Z2_Krisomal_North` at y=1001 against
`_South` at y=1237. Both pairs agree, and x is east either way - which is
why an east/west readout looked correct while north and south were quietly
swapped. `navigator.cpp`'s `bearing()` is the single place that encodes
this, so the compass label and the arrow cannot disagree.

The navigator is its own module (`navigator.cpp`) with a tiny interface, so
any future mod on this host can request tracking the same way the atlas
does. It deliberately does **not** draw inside the game's own map - the host
never writes game memory. For real map pins, the `collection_atlas.lua`
plugin running under farever-minimap places actual waypoints through that
mod's API; the two can run side by side.

### Building a route out of the atlas

**Ctrl+click** an atlas entry to add it to the route being followed;
**Shift+click** puts it at the front. A plain click still tracks it on its
own, which is the common case.

An entry's targets are *alternatives* — three vendors sell the same mount —
so only the nearest is added. Adding all three would send you round every
vendor for one item.

Where a waypoint sits in the list only decides where the arrow goes next in
**In order** mode; nearest-first picks by distance and no list order changes
that. The Following box on the Routes page toggles between the two, which is
what makes "add first" mean "go there next".

### Routes

A tracked thing is always a *list* of waypoints. What changes is what the
list means, which is `NavMode`:

| Mode | Reading | Arriving |
|---|---|---|
| `kNavNearest` | alternatives - "three vendors sell this" | changes nothing |
| `kNavRoute` | a collection - "every chest in Krisomal" | crosses that one off, arrow moves to the next nearest |
| `kNavOrder` | an itinerary someone chose the order of | crosses it off, arrow moves to the next in the list |

Clicking an atlas item still uses the first, which is why walking to the
vendor does not silently stop tracking it. The other two are routes: the pill
grows a `7 / 23 - 16 left` line and a progress bar, and says **Route
complete** with a tick for a few seconds when the last one is crossed off.

Arrival is 15 world units horizontally **and** within 25 vertically. The
vertical gate is what keeps a cave chest from being crossed off while you ride
over the hill above it. It is noticed on the pose thread, so a route advances
whether or not you are looking at the pill.

**A waypoint you just dropped is unarmed.** It sits inside its own arrival
radius the instant it exists, so without this, F9 marked a spot and crossed it
off 60 milliseconds later — recording a route by walking it erased itself as
you went. A dropped waypoint cannot be reached until you have once been 40
units from it. Waypoints from a saved route are armed from the start, because
standing on the first chest of a chest run really does mean you have done that
one.

A finished route lets go of the screen after a few seconds but **not of
itself**: those waypoints are still the thing you might want to save or walk
again, and deleting them just as the last one was reached threw away
recordings at the moment they were complete. The Routes page keeps showing it,
with Restart and Stop next to it.

The active route persists in `farever-nav-state.txt` next to the game -
including which waypoints are already done, so closing the game halfway
through a chest run resumes halfway through it. (Its own file rather than the
INI: a chest route is hundreds of waypoints and `GetPrivateProfileString`
reads into a fixed buffer.)

### The Routes page

A thirteenth tab in the atlas window, listing every route with its waypoint
count, its mode, and the distance to its nearest waypoint - the one number
that says whether it is worth starting from where you stand.

- **Start / Restart / Skip / Stop** for whatever is being followed. Skip and
  stop are also on keys — **F10** skips the waypoint being aimed at,
  **Shift+F10** clears the route — because both are wanted while running, and
  this page is behind F8. The pill names them for the first twenty seconds of
  a new route, and again whenever the atlas is open.
- **F9 drops a waypoint where you stand**, atlas open or not, because
  recording a route is walking it. They queue into one ad-hoc list.
- **Save as route** names that list and writes it to
  `farever-routes-custom.txt`
- **Copy** puts an `FMKR1:` share code on the clipboard; **Import from
  clipboard** reads one back. That is the whole sharing story - a route
  travels through Discord or a forum post without anyone agreeing on where
  files live. The code is base64 over the same plain text the files use, so
  it stays inspectable rather than becoming an opaque blob.

### Waypoints from the game's own map

Open the map, click a point of interest, and it becomes a waypoint. Every
marker on that map carries a world-space `worldPos` in the axes the navigator
already uses, so no projection is involved and the map's zoom and panning
never enter into it — `mapwatch.cpp` reads the marker and calls `nav_queue()`.

**Placing one of the game's own map pins does the same thing.** The navigator
mirrors `pinMarkers`, so a pin you drop the usual way becomes a waypoint with
the arrow already pointing at it. `[map] pins = 0` turns that off.

Finding *which* marker was clicked took a second attempt.
`nearClickableMarker` looks exactly like the answer and is not: it sits with
`crosshair`, `crosshairCheckbox` and a `showCrosshair` static, because it is
the **gamepad** cursor's snap target, and it is null when playing with a
mouse. `mouseCursor` belongs to the debug readout one field over. A live run
with the map open logged both as null, which is why the log now names them on
every open. What works instead: every marker is an `h2d.Object` and knows its
own `absX`/`absY`, so a click is a proximity test against the visible markers
and the winner's `worldPos` is the waypoint. Those are UI-scene units, not
swap-chain pixels, so the mouse is mapped through the ratio between
`GameApp.gui.s2d`'s dimensions and the frame size. The marker list is walked
only on a click — it is hundreds of objects, which is fine once and not at
20Hz.

The click is not taken, either. `input.cpp` counts left-clicks that fall
outside every host rectangle and passes them straight through, so the map
still does whatever it was going to do. A press only counts as a click if it
is released within half a second and six pixels — the map pans with the same
button, and the start of a pan is not a click.

`[map] click` in `farever-modkit.ini` picks which clicks count: `1` any clean
click on a POI (the default), `2` shift-click only, `0` off.

**F9 reads the map too.** It has always meant "drop a waypoint"; over an open
map that is the POI under the cursor, or failing that the ground under the
cursor (`mouseCursor` is a marker the map keeps pinned to the mouse for its
own debug readout). With the map closed it is the ground you are standing on.

**`ui.BaseUI.windows` is the list of windows that are open**, not of every
window the UI knows — a live run logged `windows[0] of 1` with the map up, and
a different `MapWindow` pointer on the next open. So presence in that list is
itself the answer, and the window pointer is deliberately *not* cached:
caching it would create the one failure this cannot otherwise have, a pointer
to a closed window that still passes a type check because the collector has
not reused the block yet. The walk is a length, an array pointer and a handful
of elements, which at 20Hz is not worth a cache.

`visible` and `parent` are read anyway and logged on every open and close.
They gate nothing — one line in the log settles whether presence is really the
whole story far better than a guess does.

Two files, both next to the game:

```
farever-routes.txt          generated, overwritten wholesale
farever-routes-custom.txt   yours; imports and saves land here
```

```
[Primevalley - World chests]
mode = nearest
zone = Primevalley Island, Primevalley Coast +2 more
-0.7, 1093.2, 112.5, World chest lv7 - Primevalley Island
```

`node tools/gen-routes.mjs` builds the generated set out of the game's own
level tiles: **69 routes, 1001 waypoints** across 11 areas - world, recipe,
orb, vault and camp chests, secret orbs, and every ore and herb node - each
grouped by the zone it was baked into. Waypoints come out in a greedy
nearest-neighbour order so the file reads as a circuit; they ship as
`mode = nearest` because you can enter an area from any side and the
navigator then re-derives the circuit from where you actually are.

About half the gathering nodes carry no baked zone, so each takes the zone of
its nearest neighbour that does. Being wrong about a node on a zone border
costs one route boundary and fifty metres.

## Recent Loots

The game's own loot line lasts a second or two. In a fight, or while a chest
chain-opens, that is not long enough to read, and once it is gone there is no
way back to it. `loot.cpp` keeps a feed on screen instead: items with their
own icon and rarity colour, experience, currency and level-ups, newest at the
top, each line fading on its own timer. Open the atlas and every line holds
until you close it again; drag it anywhere, same rule as the navigator's pill.

**There is no loot event to hook**, because the host only reads. So the feed
is a diff: `reader_read_loot_state` samples a deliberately narrow slice -
`ent.Hero.loadout.inventory`, `st.player.HeroData.currencies`, and its `level`
/ `exp` - twice a second, and anything that went up is something you gained.
Items are bucketed by kind + level + upgrade + rarity, so a second Copper Ore
adds to one line while a level 30 sword and a level 5 one stay apart.

One honest consequence: it reports **gains to your bags**, not literally
"loot". Withdrawing from the bank, buying from a merchant and receiving a
trade all read the same, because to a reader of memory they are the same
thing. Losses are never reported, so selling and depositing pass in silence.

Two failure modes are handled rather than believed: a read that comes back
empty against a non-empty baseline is a transient (a zone handover, a pointer
repointed mid-walk) and is skipped, since adopting it would replay the whole
inventory as loot on the next poll; and leaving the world drops the baseline
entirely, so logging back in never arrives as a wall of text.

Names, icons and rarity colours come from the atlas's own database through
`atlas_ui_lookup` - one copy of 1547 entries, not two.

### Where the harder targets come from

Three sources that are not loot tables, because the things they place are not
in loot tables:

- **Boss drops** resolve through the creature, not through a name match. The
  loot table names the unit; the unit's own bestiary entry already knows where
  it is, including "at the door of the dungeon it lives behind". An earlier
  version looked for a dungeon POI whose *name* contained the first five
  letters of the unit id, which found nothing whenever a dungeon is not named
  after its boss — High Inquisitor Chakram is the unit `Phrixes`.
- **Outfit sets** are almost never in a loot table at all (385 of 428
  appearances are in none). The item row still knows: a faction piece carries
  `faction`, so it drops from those enemies and can point at where that
  faction lives; the generic sets carry their rarity and region in the model
  path they load, which names the region but no one place, so they say so and
  offer no target.
- **Summoned monsters** have no spawner anywhere. A soulstone altar is an
  element carrying both `interactible.cost` (the stone it eats) and
  `spawnUnit` (the demon it invokes), which links the two exactly and gives
  the demon a real place — its altar, which is nowhere near the rift the stone
  dropped in. A soulstone therefore has two targets, its altar and its rift,
  labelled so you can tell which is which.

### Fixing wrong or missing acquisition info

`tools/atlas-overrides.tsv` patches the generated data per item id - replace
or append acquisition lines, replace descriptions, or add tracker
coordinates for sources the files cannot know (world-roaming bosses, event
rewards). It survives regeneration; see the comments in the file.

Data files, generated from your own game install:

```bash
node tools/gen-atlas.mjs && node tools/gen-routes.mjs
```

writes `farever-atlas.tsv` (1547 entries across twelve categories),
`farever-atlas-icons.dds` (a 2048px BC7 atlas repacked block-for-block from
the game's 256px portrait mips — no image decoding anywhere) and
`farever-routes.txt`, and copies all three next to `Farever.exe`. Re-run both
after a patch, along with `tools/update.mjs`.

## Build

Needs the MSVC C++ x64 toolset (Visual Studio 2022 Community or Build Tools).

```bash
host/build.cmd
```

Output: `host/build/dxgi.dll`. The build deliberately does **not** install into
the game — dropping a `dxgi.dll` next to `Farever.exe` changes what loads at
the next launch, so that stays a conscious step.

## Trying stage 1

It only writes a log; it draws nothing. Expect no visible change in game.

1. Close Farever.
2. Copy `host/build/dxgi.dll` next to `Farever.exe`.
3. Launch, then read `farever-modkit.log` in the game folder. You should see
   the attach line and one line per module that called through the proxy.
4. To remove: delete that `dxgi.dll`.

If the game fails to start, deleting the file fully reverts it — the proxy is
a single file with no installer and no registry writes.

**Coexistence.** `dinput8.dll` (farever-minimap) imports `CreateDXGIFactory1`,
so with both installed its calls route through this proxy before reaching the
real DLL. Stage 1 only logs and forwards, so that is harmless — but the two are
not a supported combination, and once stage 3 adds an overlay you should run
one or the other.

## Safety notes

- The real `dxgi.dll` is loaded by **absolute system path**. A bare
  `LoadLibrary("dxgi.dll")` would find this proxy first and recurse.
- `DllMain` does the minimum under the loader lock: no dependent
  `LoadLibrary`, no thread sync. The real DLL resolves lazily on the first
  forwarded call.
- Stage 1 does no vtable patching, no memory reads, and no writes to the game.
