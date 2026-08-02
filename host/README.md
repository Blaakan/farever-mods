# farever-modkit host

A standalone runtime for Farever mods, so Collection Atlas and AuraForge can
ship without requiring farever-minimap (and without its minimap and DPS
meter).

**Status: running in-game.** The proxy loads, the D3D12 overlay draws, the
reader reads, and the Collection Atlas, the navigator and the Recent Loots
feed all run on it. The chat window is the exception: it is built and compiles
but has never been run in game. The roadmap table at the bottom says which
stage landed when. AuraForge is still a farever-minimap plugin.

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
| 3c | Native Collection Atlas UI (`atlas_ui.cpp` + `input.cpp`) | **working in-game** — 1639 entries across 13 pages |
| 4a | Navigator routes (`navigator.cpp`) + the Routes page (`routes.cpp`, `tools/gen-routes.mjs`) | **working in-game** — 69 routes, 1001 waypoints; the log has a route reached and cleared |
| 4b | Recent Loots (`loot.cpp`) | **built** — draws, but nothing in the log proves a diff landed |
| 4c | Waypoints from the game's map (`mapwatch.cpp`) | **working in-game** — a map click became a waypoint |
| 4d | Chat window and the `!!` command surface (`chat.cpp`, `tools/dis-hlcode.mjs`) | **built, never run in game** — compiles clean against generated offsets; nothing of it has been seen live |
| 5 | Packaging: `install.cmd`, `tools/package.mjs`, CI release | **built** — a release builds on a clean runner |

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

Two of the tabs are lists rather than grids, because what they show is not a
collection: **Mastery** (weapon mastery, below) and **Routes**.

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

**A finished route clears itself**, exactly as Shift+F10 would: the pill
disappears the moment the last waypoint is reached. It happens on the pose
thread rather than in the draw callback, so it does not wait for a frame — a
route finished while alt-tabbed is already gone when you come back.

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
`atlas_ui_lookup` - one copy of 1639 entries, not two.

### Weapon mastery

Farever levels every weapon separately, and levelling one is killing things
with it. The game's own screen shows you the weapon in your hands, one level
at a time. The **Mastery** page shows the whole track for every weapon the
logged-in character can equip, as one bar with a notch per level — so how far
along each weapon is, and how much of the whole thing is left, is one glance
rather than a tour of the inventory.

The save stores one number per weapon, and it is a kill count:

```
st.player.Progress
  +0x0b0  weaponProgress -> hxbit.MapData
            +0x028 map   -> haxe.ds.StringMap    key: the weapon's item id
                              value: hxbit.ObjProxy_Oexp_Int  +0x14 exp
```

Everything else is derived, and the arithmetic is the game's own
(`src/st/player/Progress.hx`, `src/const/HItem.hx`):

```
levels     = min(floor(kills / killsPerPoint), maxLevels)
killsPerPoint = 20, or 26 for a weapon that fits the off-hand slot
maxLevels  = upgradeable skills on the weapon x (WeaponSkill_MaxRank - 1)
```

An upgradeable skill is one of type `AttackCombo`, `WeaponSkill` or
`WeaponPassive`; the rest of a weapon's skills — its basic attacks, its block
— take no points. In this build that puts most weapons on an eight-level
track, the starter weapons on four or six, and the shields on two or four at
26 kills each. The butterfly net has no upgradeable skills at all, and the
page says *no mastery track* rather than drawing an empty bar.

Those are properties of the weapon rather than of the character, so they come
out of the CastleDB at generation time — `tools/gen-atlas.mjs` writes them
into the weapon's row as a `mastery:<levels>/<kills>` tag, reading the three
constants by name so a patch that retunes them is a re-run. The reader is
left with one map walk and no formula.

**Which weapons a character can equip** is the same rule the game applies
(`st.Item.checkAptitudes`): a weapon lists the aptitudes allowed to wield it,
and each of the four player classes carries exactly one — Warrior=Fighter,
Rogue=Assassin, Mage=Wizard, Priest=Cleric.

The class itself is **`ent.Unit.kind` at `+0x258`, read straight off the
Hero** — not `st.player.HeroData.kind`, which was the first attempt and came
back empty in game. `ent.Unit.kind` is provably the unit's id in the unit
sheet: `Unit.set_kind` uses that very string as the key into
`Data.unit.byId` to resolve the unit's own row (`src/ent/Unit.hx:686`), and
for a hero that row is the class. HeroData remains the fallback.

It is checked against those four names and ignored otherwise: an
unrecognised value means the read is wrong, not that there is a fifth class,
and the page then lists every weapon and says why rather than filtering by a
word it does not understand. Either way the log names what it got —
`items: character class is Priest`, or the same line saying it is not one of
the four — because a filter that silently does nothing is the one bug this
page cannot afford.

Per character, and never merged across them: the kills your Warrior made with
a sword are not your Priest's. Only the logged-in character is in the process,
so unlike ownership there is no offline half to this page.

#### The bar on screen

The weapon in your main hand gets its own bar over the world, where it stays
with the atlas closed — the point being to watch a track fill while you
fight, not to open a window to check it. There is nothing to choose:
equipping a weapon is already the act of choosing it, and asking a second
time would only be a way to get it wrong. Swap weapon and the bar follows;
swap character and it follows that too, because it is reading the hand rather
than a setting.

**It disappears when there is nothing left to watch** — an empty hand, a
weapon with no upgradeable skills, or a track already full. A bar sitting at
100% forever tells you nothing you did not know the moment it filled.

The weapon is the game's own `activeWeapon`, which is equipment slot 0.
`ent.Hero.get_activeWeapon` is `get_weapon1`, that is
`Equipment.getSlot("Slot_Weapon1")` (`src/ent/Hero.hx:64`), and `getSlot`
indexes the equipment inventory by the slot's position in
`DataCache.EQUIPMENT_SLOTS` (`src/st/Equipment.hx:159`) — which is the
itemType sheet's `isSlot` rows in order, with `Slot_Weapon1` first. The live
array confirms it: it is exactly 30 long, and the CastleDB has exactly 30
`isSlot` rows.

Deliberately **not** `ent.Hero.weaponInHand`, which is the weapon the *skill
being cast* belongs to and only falls back to the active one
(`src/ent/Hero.hx:1420-1422`). That is a truer answer to "what is swinging
right now" and a worse one to "what am I wielding": it flips to the shield
for the length of a shield skill, and a progress bar that swaps weapon
mid-fight is noise.

The bar is sized to sit alongside the game's own XP bar rather than next to
it looking like a different game, and those numbers are the game's own:
`UI/Style/style.css` in `res.light.pak` says `exp-bar { bar-width: 619;
bar-height: 17 }`. Those are units in the UI scene, not pixels — the map
diagnostics report `scene=2048x1152 frame=2560x1440`, so the scene is a fixed
design size the game letterboxes onto the frame. The bar therefore scales by
`min(width/2048, height/1152)` and comes out the same size as the game's on
any monitor, instead of being right on one and wrong everywhere else.

Dragging works only while the atlas window is open, which is the rule the
waypoint pill already follows and for the same reason: over the world it must
never eat a click. The border and panel that appear while it is movable are
the cue that it can be. Only the position is remembered, under `[mastery]` in
`farever-modkit.ini` — there is no longer anything else to remember.

On the page itself, the equipped weapon carries the same left-edge accent
stripe the Routes page gives the route being run, because it answers the same
question: which of these is on screen right now.

#### Knowing when you are actually in the world

Anything the host draws over the world has to disappear at the main menu and
behind a loading screen, and "can the hero be read" turned out not to answer
that. Two separate things were wrong, and both affected the whole overlay —
the atlas window and the waypoint pill as much as the mastery bar — but the
bar is what made them visible, being the first thing meant to be on screen
without opening anything.

**The main menu.** `App.inst` becomes a `MenuApp` there, and
`find_app_via_statics` returned null for that exactly as it did for a walk
that failed. Null was read as "no news", so the previous session's `GameApp`
stayed cached — and a dead HashLink object keeps answering to its class name
until the collector reuses its block, so it went on yielding a hero. A live
object that is not a `GameApp` is now reported as the answer it is, and the
cached app and hero are dropped.

**Loading screens.** These are a different question and are deliberately
kept separate. The game settles it in one integer: `GameApp.get_isLoading` is
literally `loadingState != 10` (`src/GameApp.hx:50`), so `GameApp.loadingState`
at `+0xf8` is what `reader_is_loading` reads.

It gates **drawing only**, in one place — `host_draw` steps the whole overlay
aside for the frame and clears the input rectangles so nothing goes on
swallowing clicks over a screen with no window on it. Nothing is unloaded and
no state is dropped, because a zone load is not the character leaving: the
collection, the route and the loot feed are all still true on the other side
of it and are simply not drawn for a moment.

Keeping those two apart matters more than it looks. The first attempt folded
the loading check into "is a character in the world", which is what drops the
ownership snapshot — so every zone change threw away the collection and
rebuilt it for a character that had never gone anywhere. The question a
loading screen answers is *should this be on screen*, and that is all it is
allowed to decide.

### Runes

A rune is a one-use, one-pickup item that permanently teaches one character an
upgrade to one skill — Alacrity cuts the cooldown of the Priest's Judgment.

The game calls them **skill masteries** and does not keep them in the item
sheet: `skill.mastery` is an array of them inside the skill they modify, and
the *item* you find is a single generic `Mastery` whose name is literally
`Rune: ::ref_mastery::`, filled in at pickup. So the page is built from the 84
mastery rows — 21 per class, across 28 skills — each with its own portrait
under `UI/Portraits/Items/Masteries/`.

Ownership works exactly like recipes, because the game stores it the same way:
`st.player.Progress.skillMasteriesLearnt` is learned-and-permanent, per
character, so the tooltip says *Learned by Emsay* — including for characters
who are not logged in, via the same `farever-jobs-<character>.json` the crafts
already ride in. (`HeroSpecialization.skillMasteries` is the separate,
changeable list of which ones are currently slotted.)

Descriptions are written against the skill's own numbers — *"::name:: costs
::var1:: less [Rage]"* — so the generator substitutes them: the rune's vars,
then the skill's, then one hop to the status the skill applies. About a third
of the values live in effect blocks rather than a vars map and are rendered
`?`, which reads as "we could not find the number" rather than printing
`val1%` at the player as if it were one.

Two Priest_Miracle runes ship with an empty text block — they exist, with
icons, and are simply unnamed in this build. They show under their id rather
than being hidden.

**There is no such thing as "where does Alacrity drop".** No loot table
anywhere names a specific mastery: the thing that drops is the one generic
`Mastery`, and which rune it becomes is decided when you take it. The tooltip
says that outright, because it is more use than an empty line.

What *is* knowable is where the pickup comes from, and most of it is not in a
loot table either:

- **Eight quests hand one over every time.** An NPC's dialogue grants items on
  a choice — in one of two shapes depending on when the quest was authored,
  `receiveItems: [{kind, amount}]` or `gains.items: [{item, count}]` — and the
  objective beside them names the quest. So the navigator points at *Baywatch
  - Meridion POI*, at the NPC who pays it. Negative amounts are what the
  choice costs rather than what it gives, so only gains count.
- **Two world chests always contain one**, through a `lootItems` line with a
  `dropRate` of 1 sitting on top of the chest's ordinary table.
- The 5% roll from world crates and unique foes, which is the fallback.

All three are general rather than rune-specific: **101 entries across the
atlas gained a quest-reward line**, and grouping chests by the `lootTable`
they roll is the honest answer for anything whose only source is a crate.
Together they took navigator coverage from 577 entries to **695** — the
number `gen-atlas.mjs` prints as `tracker targets on N entries`, which is
where to check it rather than trusting this line after a patch.

Guaranteed sources are pushed first, so a quest that always hands one over
outranks a place it might drop.

### Every source the shipped data actually contains

Loot tables are the obvious source and the smallest one. Auditing every entry
that had no acquisition text, against every CastleDB sheet and every prefab,
turned up these — all of them general, none of them rune-specific:

| Source | Where it lives | Reaches |
|---|---|---|
| Loot tables | `lootTable` | most drops |
| Crafts | `craft` | 190 recipes and what they make |
| Merchant stalls | element `props.shop` | vendors, with their own names |
| **Quest rewards** | NPC dialogue: `receiveItems[{kind,amount}]` (older) or `gains.items[{item,count}]` (newer), with `goal.name` naming the quest | 101 entries |
| **Guaranteed chest contents** | chest `props.lootItems[{item,dropRate}]`, `dropRate: 1` | 19 entries |
| **Chests by the table they roll** | chest `props.lootTable` | anything a crate can hold |
| **Vaults** | the same field — each vault holds Gold and exactly one mount or glider at 100% | 8 collectibles |
| **Achievements** | `ach.reward.items` | 23 mounts and gliders, every one of which had no other source |
| Faction outfits | item `faction` | 92 appearances |
| Summon altars | element `interactible.cost` + `spawnUnit` | 8 demons |
| Spawners, instances | `spawner`, and a level's activity orb for its entrance | 252 creatures |

Two things worth knowing about the shapes:

- **Negative amounts are what a dialogue choice costs**, not what it gives. A
  quest that takes 106 gold and hands over a rune has both in one array.
- **A vault names the exact table it holds.** Matching every vault in the
  region instead - as this once did - gave the Semeruian Dragoon three
  targets, two of them the wrong hidden area.

After all of that, **101 entries still have no source**, and that is now a
statement about the game rather than about the extractor: none of them is
mentioned anywhere outside its own item row - not in a loot table, a craft, an
achievement, a shop, a quest or a prefab. Mostly unreleased or code-granted
(22 mounts, 24 gliders, 22 trinkets). `shopList` exists as a field and is used
by nothing.

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

writes `farever-atlas.tsv` (1639 entries across thirteen categories),
`farever-atlas-icons.dds` (a 2048px BC7 atlas repacked block-for-block from
the game's 256px portrait mips — no image decoding anywhere) and
`farever-routes.txt`, and copies all three next to `Farever.exe`. Re-run both
after a patch, along with `tools/update.mjs`.

## Chat

`chat.cpp` draws a chat window over the game's own message area, with the
session's whole scrollback, timestamps, per-channel filters, an ignore list
the game does not have, item links, and a command language typed into the
game's own chat box.

**Run in game once, and worth being exact about which parts that covers.**

- **Seen working on screen:** the session's scrollback out of
  `ChatClient.history`, the anonymous-structure decode, `st.Channel`
  classification, whisper target resolution (*To Emsey: test whisper*), the
  timestamps the game records and never shows, the channel filter chips, and
  the window drawing over the game's own message area.
- **Failed in that run, fixed since:** the ChatBox lookup, which searched
  `ui.BaseUI.elements` — the box is not there, and the game's own route is
  `gui.gameRoot.hud.chat`; and the message-area rectangle, which was part
  guesswork because `h2d.Flow`'s `calculatedWidth`/`calculatedHeight` were
  believed ungeneratable. They are generated and read now.
- **Not yet confirmed working in game: the `!!` command surface.** It did not
  fire in that session. The cause was found and fixed — see [The command
  surface](#the-command-surface) — but no command has been seen to run.

Read the rest of this section as a description of the code, and only the first
bullet as a live observation.

### Two surfaces, answering different questions

```
ent.Hero
  player           -> st.Player
    chatClient     -> st.player.ChatClient
      history      -> hl.types.ArrayObj      the durable one

GameApp.gui  (ui.GameUI extends ui.BaseUI)
  gameRoot       -> ui.GameUiRoot
    hud          -> ui.Hud
      chat       -> ui.hud.ChatBox           the display; ephemeral
        messages -> h2d.Flow                 absX/absY + calculatedW/H
        messageInput -> ui.comp.InputBox -> input -> h2d.Text.text
  s2d            -> h2d.Scene -> events -> currentFocus
  console        -> h2d.Console -> bg -> visible
```

`ui.BaseUI.elements` is where the box looked like it should be and is not: it
holds no `ui.hud.ChatBox` at all, so the first version of `find_chat_box`
found nothing — which showed up only as the command surface silently never
firing. The route above is the game's own (`ui.GameUI.get_hud`, GameUI.hx:33,
is `gameRoot?.hud`), every hop is validated by class name, and the pointer is
cached keyed on the `ui.GameUI` that owned it.

`history` is **not replicated** — there is no `__net_mark_history` beside
it — and `localReceiveMessage` (ChatClient.hx:25-29) does a bare `push` with
no trim and no ring buffer. So the whole session is there in arrival order,
indices are stable, and tailing it is "read the length, decode what is new".
A history that got *shorter* is not corruption: it is a different
`ChatClient`, which is what a relog or a character swap builds, and the module
drops its index and its ring rather than skipping the next session's messages.

`ui.hud.ChatBox` is read for the two things `history` cannot answer: where the
game's own box is on screen, and what is being typed into it. A third was
intended — whether the last line it drew was one the game generated
locally — and does not work; see below.

**History elements are anonymous structures, not class instances.** Each is
`{ args, channel, localStamp, localTextId, notify, sender, text }`, read with
`read_virtual_fields` and matched **by name** — a structure's field order is
not guaranteed and there is no generated offset for any of them. Decoding
always produces a message even when nothing reads, because a caller tailing
by index cannot have lines silently dropped: an empty message says "this line
was there and would not read", which is the truth, and keeps every later index
right.

**`st.Channel` is an enum, and only its index is safe.** A HashLink enum value
carries its constructor index at a fixed place, which is all six channels need
(`Local | All | AllSystem | Player | Group | System`). The *parameters* of
`Player`/`Group`/`System` live at offsets that come from the enum construct
table, which `gen-offsets.mjs` does not emit — so the far end of a whisper is
a best-effort read validated by `obj_is(p, "st.Player")` and left **empty**
when it does not validate. `Group` is not attempted at all: `st.Group` has no
name field among the generated offsets, only its player list, so there is
nothing to put there that would not be invented.

A sender is named only when it is an `ent.Hero`. A null sender is a system
line; any other `ent.Unit` has no name among the generated offsets, only its
unit id, which is not what anybody is called — so those draw without a name
rather than with a guess.

### The command surface

The mechanism is one `return` in the game's own code.
`ui.hud.ChatBox.processMessage` (ChatBox.hx:132-171) trims what you typed,
prefixes `"!" + <selected channel> + " "` when it does not start with `!`,
splits on spaces and switches on the first token. Four match — `!say`, `!map`,
`!group`, `!to` — and the default case at ChatBox.hx:165 is
`chatError("Unknown chat command " + cmd); return;`. The single call to
`ChatClient.sendMessage` is at ChatBox.hx:169, past that return.

So an unrecognised `!command` draws one local line and never reaches the
network, which is what lets a host that only reads own a command language. The
disassembly is in
[RESEARCH.md](../RESEARCH.md#can-a-mod-be-driven-by-in-game-commands); reproduce
it with `node tools/dis-hlcode.mjs ChatBox.processMessage` rather than taking
this paragraph's word for it.

The prefix is `!!`. Any unmatched `!x` behaves identically; `!!` is harder to
typo into a broadcast, because dropping one `!` still leaves an unknown
command while dropping the only `!` sends what you typed on whatever channel
the dropdown has selected. That remains the one real footgun here.

**Reading it back was designed around two signals, and now uses one.** The
echo carries only the first token (`"Unknown chat command " + cmd`), so it
cannot say what the arguments were; polling the input box alone races with
Enter clearing it and cannot tell a submit from an Escape. So the first
version kept the last non-empty *focused* input and waited for the input going
empty **together with** a new bare `ui.hud.ChatBoxLine` in the `messages`
flow — `ChatBoxMessage` extends `ChatBoxLine`, so "is a line and is not a
message" ought to be exactly a locally generated echo.

**That second signal does not exist to read.** In the live run the flow's
children came back as `ui.UIElement`, neither `ChatBoxLine` nor
`ChatBoxMessage`, so the test never once passed and no command ever ran. It is
also not needed, which is the more useful half: all it bought was declining to
act on a cancelled command, and every `!!` token is unmatched by
`processMessage` and swallowed locally at ChatBox.hx:165 whatever the host
does. So `chat.cpp` now buffers the input while it has text and runs the
command when the input goes empty, gated on nothing but the `!!` prefix. The
worst an over-eager trigger can do is run a local read-only command somebody
meant to abandon, which is a far better failure than a surface that does not
work. `ChatBoxState::line_count` and `last_is_error` are still read and are no
longer acted on.

The gaps that follow, none of them closable without writing to the game:
Escape empties the field exactly as Enter does, so an abandoned command runs;
and what is buffered is the last sample before that, so a command typed and
submitted inside one 100 ms poll can be caught mid-word. Matching is on the
first token, so a truncated sample whose first token is a whole command name
runs with truncated arguments — `!!ignore Emsey` seen as `!!ignore Em` ignores
*Em*. Under the old echo-matching design a half-typed command matched nothing;
under this one it runs, which is worse, and saying so is the point.

What *is* available to a reader is whether the text stood still for a whole
poll interval before the box emptied — *they had stopped typing* versus *no
idea*. `poll_chatbox` counts that (`g_cmd_samples`) and passes it to
`run_command`, which prints the string it is about to act on whenever there is
no such evidence. Waiting two polls before arming would not help: a person
types the rest and presses Enter well inside 200 ms, so it would refuse
ordinary commands without proving the sample complete. Every command is local
and read-only, and that plus the announcement is what makes this tolerable.

The game's own echo, meanwhile, is drawn either way and the host cannot remove
it. Aligned and at the default opacity it lands under the window and is
covered; turn the opacity down and it shows through like everything else, and
in free placement the window is wherever the player put it, so the echo may
not be covered at all.

Focus is read but **not** used to gate any of this: text only appears in that
box because the player put it there, and the focus read is the most fragile on
this path — it reports false when it cannot resolve, which would take the
whole surface with it. What is buffered is only ever acted on if it starts
with `!!`. The read itself is `scene.events.currentFocus == interactive`,
which is what
`h2d.Interactive.hasFocus()` computes (Interactive.hx:311). `h2d.Object` has
no generated `scene` field, so the scene comes down from `GameApp.gui.s2d`
instead and the comparison is the same one the game makes. `currentFocus` is
declared as an interface, so it holds a vvirtual whose value is the object;
both that and a directly stored object are accepted, and if neither shape
resolves it reports **not focused** rather than guessing. That conservative
answer is exactly why nothing depends on it.

### Ten times a second, not twice

The loot feed's half-second is too slow here, because this poll is also how a
typed command is noticed, and a command that takes half a second to do
anything reads as one that was swallowed — so the player types it again. The
cost stays small because only the tail of the history that has not been
decoded is ever read: the length is one integer, and "already up to date" is
one cheap read rather than a re-decode. The first poll after a long load takes
the backlog in slices of 400 so no single hold on the lock is long.

The render thread keeps its own copy of the ring and tops it up incrementally,
the way `routes.cpp` keeps `g_view`. A session's chat is thousands of strings
and copying all of them under the lock every frame would be the most expensive
thing the draw callback does. A wholesale drop of the ring bumps an epoch, so
the incremental copy can tell "nothing new" from "everything you have belongs
to a session that ended".

### Placement

Aligned is the default, and all four edges are the game's own.
`ui.BaseElement` extends `h2d.Flow`, and a Flow records the box its layout
settled on in `calculatedWidth`/`calculatedHeight`, so the `messages` flow
gives its position from `h2d.Object.absX`/`absY` and its size from those two.
That is what keeps the window on the message area and off the footer and the
text field below it.

An earlier version believed a Flow's size could not be generated. It derived
the height from the gap between the messages' top and the footer's, left the
width at 0, and the caller guessed it from the saved value or 560 — which was
visibly the wrong size on screen. Adding the two fields to the WANT list was
one read and the game's own number. The saved width survives only as the
fallback for a `calculatedWidth` that reads as 0 or absurd, which means the
read landed mid-layout or before the flow was ever laid out; no rectangle is
better than a wrong one drawn over the game's own chat.

Those bounds are in the UI scene's own units and the overlay draws in
swap-chain pixels, so they are scaled by the frame size against
`h2d.Scene.width/height` — the same conversion `reader_map_pick` does in the
other direction. A miss keeps the last good rectangle rather than teleporting
the window to the free placement and back on the next frame; a rectangle that
never reads at all falls back to free placement and **says so** on the window,
because a toggle that silently does nothing is worse than one that explains
itself. That notice draws with the chip row, so it is only on screen while the
atlas window is open — during play the fallback is silent.

**It cannot hide the game's own chat box.** That would be a write, so the game
goes on drawing its copy of every line underneath. The window is therefore
**opaque by default** — `[chat] opacity`, `!!opacity <20-100>` — and that is a
correctness matter rather than a preference: at anything below 100 each
message is legible twice, in two sizes, which reads as the mod being broken.

`ui.hud.ChatBox` carries a `minimizeButton` (icon *Cross*), whose handler is a
closure rather than one of the class's eight named methods (`init`, `focus`,
`unfocus`, `hasFocus`, `receiveMessage`, `reloadMessages`, `processMessage`,
`chatError`). It is not a way round this — tried in game, the box does not
stay hidden.

Two things that would replace the game's box outright, both deliberately not
built. Writing `0` to the box's own `visible` (`h2d.Object`, `+0x50`) would do
it in one byte — recorded as weighed and rejected, because one write to a UI
flag is still a write, and the read-only rule is what the safety argument and
the front page's opening claim both rest on. Covering the whole box and
mirroring the input from `messageInput.input.text` plus
`h2d.TextInput.cursorIndex` would do it with no write at all — not built
because it trades a cosmetic win for typing latency, a covered channel
dropdown and no IME composition, and seeing the game's own input is fine.
Enter still opens the game's input, typing
still goes to the game, and sending is still the game sending.

Free placement is dragged and resized only while the atlas window is open,
which is the rule the navigator's pill and the loot feed already follow: over
the world the host must never eat a click. The aligned rectangle is not
draggable at all — it is where the game's box is. Clicks inside the window are
claimed through **aux input rectangle 3** (0 is the navigator's pill, 1 the
loot feed, 2 the atlas HUD panel; sharing one is silent, the module that draws
last simply wins), and only while the atlas is open.

The wheel is the one exception, and it has its own mechanism —
`input_set_wheel_rect` in `input.*`, a single rectangle that claims the wheel
and nothing else while the host's window is **shut**. Scrollback that only
worked with a second window open would not be scrollback: reading what
somebody said a minute ago is a thing you do mid-play. Clicks over the frame
still reach the game's own chat box underneath; only the wheel is taken, and
only while the atlas is not drawn over the frame.

While the developer console is up the module takes no click and no wheel
anywhere. The console is a password-gated admin surface that owns its own
keys — `reader_console_open()` reads `h2d.Console.bg.visible`, which is what
`h2d.Console.isActive` reads (Console.hx:297) — and the host has no business
putting anything into it.

### Files, all next to the game

```
farever-modkit.ini          [chat] - placement, size, filters, options
farever-chat-ignore.txt     one name per line; '#' comments
farever-chat-log.txt        the session log, when [chat] log = 1
```

The ignore list hides a sender in the window **and** keeps them out of the
log — the point of ignoring someone is that they leave no trace. The channel
filters deliberately do not apply to the log: they are a chip you click to
quieten the window for a minute, and a log that silently stopped recording a
channel would be worth less than no log, because the record is the thing you
cannot get back. Hiding is applied when the window draws rather than when a
line arrives, so `!!unignore` brings the backlog back with it.

`!!clear` empties the *view*, not the history. The game's own history is the
only copy of the session, and throwing it away to tidy a window would lose the
thing worth keeping — `!!find` still searches all of it.

Item links resolve through the atlas's own database (`atlas_ui_lookup`), which
indexes by **item id**, matches exactly, and has no name search. `link_lookup`
tries three exact lookups and stops at the first hit: the text as typed, the
text with every non-alphanumeric character removed (`Copper Ore` →
`CopperOre`), and that squashed form re-cased as CamelCase.

That reaches a typed display name for **182 of the 1639** generated atlas
entries — about one in nine. It holds for gathered materials, recipes and
consumables, and for essentially nothing else: not one of the 428
appearances, 64 mounts, 73 pets, 68 gliders or 37 weapons has an id a name
derives, and only 7 of 252 creatures do. *Abyssal Shoulderplates* is
`Shoulders_RManfish_FigAss`. An earlier version of this file said the ids
*were* the display names with the spaces taken out; run the derivation over
`farever-atlas.tsv` and it is false for about 89% of it. For the rest the id
itself is what you type, and the atlas entry's detail panel prints it.

One consequence: `cmd_link` copies `"[" + info.name + "]"`, so a link made
from an id renders through the same three lookups and, outside that one in
nine, will not resolve — it pastes as plain text with no icon even for another
person running this mod.

When no derivation resolves, `!!link` says that plainly — including that a
DLL-only install has no `farever-atlas.tsv` and so will never match
anything — rather than sending someone hunting for a typo that is not there.
Chat is deliberately independent of the atlas otherwise: it is worth having on
an install with no generated database, and the only thing a missing atlas
costs is the icons on links.

Every command and every ini key: **[docs/chat.md](../docs/chat.md)**.

## Build

Needs the MSVC C++ x64 toolset — Visual Studio 2022 Community, or the
standalone Build Tools, with **Desktop development with C++** (MSVC v143 x64
and a Windows 10/11 SDK).

```bash
host/build.cmd
```

The toolset is located through `vswhere.exe`, which every VS 2017+ installer
puts at one fixed path and which knows about every edition, channel and
install drive. Probing for `2022\Community` finds one machine's layout;
asking the installer finds everyone's. Failing that it falls back to the
well-known layouts, and failing *that* it names what to install rather than
saying "not found".

Output: `host/build/dxgi.dll`. The build deliberately does **not** install
into the game — dropping a `dxgi.dll` next to `Farever.exe` changes what
loads at the next launch, so that stays a conscious step. `install.cmd` in
the repository root is the conscious step.

### What it links against

The proxy loads before the game's own renderer, and a proxy that fails to
load takes the game with it — `dx12.hdll` imports `dxgi.dll` statically, so a
missing dependency is a game that will not start rather than a mod that does
not work. So the import list is worth keeping short and boring, and CI prints
it on every build:

```
ADVAPI32.dll  d3d12.dll  D3DCOMPILER_47.dll  dxgi.dll  GDI32.dll
KERNEL32.dll  USER32.dll
```

All of them ship with Windows 10 and 11. There is **no** `VCRUNTIME` or
`MSVCP` in that list, which is the point of `/MT`: a static CRT means the
player does not need the Visual C++ redistributable installed. `dxgi.dll` in
its own import list is this proxy calling `CreateDXGIFactory1` and reaching
its own export, which forwards to the real DLL by absolute path.

## Packaging a release

```bash
node tools/package.mjs --build
```

Writes `dist/farever-modkit-<version>.zip`: the DLL, `install.cmd`,
`uninstall.cmd`, the two generators with the libraries they need, `LICENSE`,
a standalone `README.txt`, and `build-info.json` recording which game build
the DLL's offsets were compiled against — which is what lets the installer
refuse to install into a game it could not read.

Nothing generated from the game goes in. The item names, icons, loot tables
and level data belong to Shiro Games; the generators ship and the haul does
not, which is why installing needs Node and a copy of the game rather than
being a drag and drop.

Tagging `v<version>` runs the same script on a clean Windows runner and
attaches the archive to a GitHub release, so what people download is built
from the tag rather than uploaded from a laptop. The version lives in
`host/src/version.h`, the release workflow refuses a tag that disagrees with
it, and the host logs it on attach.

## Coexistence

`dinput8.dll` (farever-minimap) imports `CreateDXGIFactory1`, so with both
installed its calls route through this proxy before reaching the real DLL.
That part is harmless. But both draw an overlay, and that is not a supported
combination — run one or the other.

## Safety notes

- The real `dxgi.dll` is loaded by **absolute system path**. A bare
  `LoadLibrary("dxgi.dll")` would find this proxy first and recurse.
- `DllMain` does the minimum under the loader lock: no dependent
  `LoadLibrary`, no thread sync. The real DLL resolves lazily on the first
  forwarded call.
- **Nothing in the game's own state is ever written.** Every read goes
  through one function, `mem_read`, which checks the page is readable and
  then does the copy inside `__try`/`__except`; a pointer that has gone stale
  costs a `false`, not a crash. Candidates are validated against the type
  they are supposed to be as they are found, and the whole reader is gated on
  the bytecode hash the offsets were generated from — a patched game means
  every read is disabled, not that stale pointers get walked.
- The only memory the host writes is **three entries in two COM vtables**:
  `Present` and `ResizeBuffers` on the swap chain, and `ExecuteCommandLists`
  on the command queue, so the overlay can draw after the game has. That is
  DXGI's and D3D12's memory, not the game's, and no game code is patched.
- Clicks that fall outside the host's own rectangles are passed straight
  through, so the game still receives them.
