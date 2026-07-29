# How Farever can be modded, as of July 2026

Research notes behind the two plugins in this repo. Written to be checkable:
every load-bearing claim says where it came from, and the things I could not
confirm are marked as such rather than smoothed over.

## The game

| | |
|---|---|
| Title | Farever |
| Developer / publisher | Shiro Games (Bordeaux, France) |
| Steam appid | 3672400 |
| Released | Early Access, 6 May 2026, ~$19.99 |
| Genre | Online multiplayer / co-op action RPG, open world ("Siagarta") |
| EA length | ~1 year, per the studio |

Shiro Games was co-founded by **Nicolas Cannasse** — the author of the Haxe
language and the Heaps.io engine. That single fact determines everything about
the modding surface.

## Engine and runtime — confirmed

Farever runs on Shiro's in-house stack, not on a commercial engine:

```
Haxe (language)  ->  HashLink (VM)  ->  Heaps.io (engine)
                     CastleDB (data.cdb)   res.pak (assets)
```

Evidence:

- Shiro's stack is documented publicly by the studio and the Haxe project:
  [haxe.org/blog/shirogames-stack](https://haxe.org/blog/shirogames-stack/) and
  [heaps.io/documentation/fullstack.html](https://heaps.io/documentation/fullstack.html).
- **Direct corroboration for Farever specifically**, from the working mod:
  - It reads **`hlboot.dat`** — the HashLink bytecode bootstrap — to SHA-256 the
    game build. A `hlboot.dat` next to the executable means Farever ships as
    *interpreted HashLink bytecode*, not HL/C compiled to native. That is the
    friendlier of the two options for inspection.
  - Its troubleshooting notes name the crash symbol **`h3d.impl.DX12Driver.present`**.
    `h3d` is the Heaps 3D package; `DX12Driver` is its Direct3D 12 backend.
    That is a Heaps class path in a Farever stack trace.
- Renderer is **Direct3D 12**.

So: a Heaps/HashLink game rendering through D3D12, with assets in Shiro's
`res.pak` format and structured data in CastleDB. Same family as Northgard,
Wartales and Dune: Spice Wars.

## What "modding Farever" means in practice today

There is no official mod API, no Steam Workshop, and no SDK. What exists is one
community project, and it is good:

**[ramisotti13-eng/farever-minimap](https://github.com/ramisotti13-eng/farever-minimap)**
— minimap, camera compass, party display, DPS/HPS meter, boss timer, and, most
importantly for us, **a sandboxed Lua 5.4 plugin runtime**.

How it attaches:

- Ships as **`dinput8.dll`**, dropped next to `Farever.exe`. No injector: Windows
  resolves `dinput8.dll` from the application directory before `System32`, so the
  game loads it itself. The DLL forwards all five real DirectInput exports so
  input keeps working.
- Renders with **Dear ImGui**, either into the game's own D3D12 swap chain
  ("Fast") or via a DirectComposition layer ("Compatibility").
- Reads game memory **read-only**. It states it never writes to game memory,
  makes no network connections (the binary imports no networking APIs), and
  touches no registry.
- **Refuses to inject at all if it detects an anti-cheat process.**

That plugin runtime is the modding surface this repo targets. Writing a Lua
plugin is the only way to extend Farever right now that does not involve
reverse-engineering the client yourself.

## Is it allowed?

Shiro Games addressed add-ons on their official Discord, quoted in the mod's
README:

> While we won't promote the use of add-ons during the EA (to keep players on
> the intended experience at first), we won't condemn personal use of add-ons
> like minimaps or DPS meter.

So: tolerated for personal use, not endorsed. Both plugins here are read-only
and informational, which is the category that quote covers. Neither writes to
the game, automates play, or touches the network.

I found **no evidence of a shipped anti-cheat** — no Steam anti-cheat
disclosure block, and the mod's own anti-cheat detector is a precaution rather
than a response to a known one. That could change; it is Early Access.

**Honest risk statement.** This is an online game and the client is not the
authority, so an informational overlay cannot give you power the server would
honour. But "read-only and tolerated" is not "guaranteed safe": any third-party
DLL in the game process is a decision you make about your own account. The
plugins here add no risk beyond the host mod they run inside — but they do not
subtract any either.

## The modding surface, safest first

| Approach | What it gets you | Verdict |
|---|---|---|
| **Lua plugin in the farever-minimap runtime** | Player/target/party state, POIs, waypoints, ImGui + absolute drawing, events | **What this repo uses.** Sandboxed, read-only, hot-reloadable, no game files touched |
| Host mod itself (`dinput8.dll`) | D3D12 overlay, read-only memory | Already solved; no reason to rebuild it |
| Unpacking `res.pak` (QuickBMS + [Shiro_Games_PAK_script.bms](https://github.com/Sviat/qbms_shirogames)) | Assets, `data.cdb` game data | Fine for *offline inspection*. Editing game files for an online game is a bad idea |
| Editing CastleDB `data.cdb` | Balance/content changes | Server-authoritative game — changes are cosmetic at best, ban-shaped at worst |
| **Reading** `hlboot.dat`'s string table | The complete internal id vocabulary | **Used here.** Read-only static analysis of a file you own; changes nothing. See [docs/scanning.md](docs/scanning.md) |
| Patching HashLink bytecode (`hlboot.dat`) | Arbitrary client changes | Deep water. Breaks on every patch, defeats the build-hash check, and is squarely "modifying the client" |
| Memory writes / automation | — | Cheating. Not covered by the dev quote above, and not what this repo does |

## The plugin API, in one page

Everything below is confirmed from
[`data/plugins/README.md`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/data/plugins/README.md)
in the host mod (v1.2.4 surface).

**Lifecycle** — define any of `on_init()`, `on_render()`, `on_event(name, data)`.
Files hot-reload about a second after you save. Errors are caught by `pcall` and
land in `farever-mod.log` and the in-game Plugin Manager; a broken plugin does
not take the mod down.

**Reads** — `farever.player.*` (position, heading, health, shield, energy, the
five primaries, crit/armor/mastery, class resources: rage, spark, focus, combo
points, poise, oxygen, `glide_speed`), `farever.player.statuses()` (active
buffs), `farever.player.skills()` (cooldown values + icons),
`farever.player.equipment() / inventory() / currencies()`,
`farever.player.codex(kind)` (**bestiary only** — see below),
`farever.target.*` (incl. a learned
cast bar), `farever.party.*`, `farever.compass.*`, `farever.pois()`,
`farever.waypoints.*`.

**`farever.player.codex()` is not a collection reader**, which was worth
finding out because it looked like one. The bytecode carries a whole codex
tree — `data.CodexNode`, `ProgressCalcMode`, both `CodexUnitSheet` *and*
`CodexActivitySheet` — and the API returns a codex tree path per entry, so if
mounts, gliders and appearances were nodes in that tree it would have been
account-wide ownership through the sandbox. A generated probe plugin called it
against 428 ids across eight buckets, including fifteen taken from a live
equipment dump so that a miss could not be blamed on not owning the thing.
Monsters hit, as the control predicted; **every mount, glider, armour, weapon,
companion, chest and recipe id missed**. The codex is the bestiary. That is
what makes the host's own reader the only route to the collection, and it is
why `st.player.Collection` is walked directly instead.

**Events** — `hero_locked`, `fight_start`, `fight_end`, `damage_dealt`,
`heal_dealt`, `shield_applied`, `target_changed`, `cast_start`, `cast_end`,
`weapon_changed`.

**Draw** — flow widgets (`imgui.text/button/checkbox/slider_float/drag_float/
input_text/combo/color_edit/progress/icon`) plus absolute screen-space
primitives (`imgui.draw_rect_filled/draw_rect/draw_circle/draw_line/draw_text/
draw_triangle_filled`), `farever.now()`, `imgui.font_scale`, `imgui.cursor_pos`,
`imgui.dummy`.

**Persist** — `farever.store.get/set` (scalars only), plus
`farever.write_combatlog(name, text)` for a controlled file write into
`%LOCALAPPDATA%\farever-minimap\combatlogs\`.

**Sandbox removes** — `io`, `require`, `dofile`, `loadfile`, `load`, `debug`,
`os.execute/remove/exit`, arbitrary audio, other players' positions, any write
to game state.

### Four API limits that shaped both plugins

1. **No mouse API.** No cursor position, no click/drag state. Anything
   "movable" must be moved with numeric controls, not by dragging it.
2. **No screen-size query.** Nothing reports the back-buffer dimensions, so
   screen-relative anchoring needs the resolution as a user setting.
3. **Icons are flow widgets.** `imgui.icon` draws at the ImGui cursor inside the
   plugin's window. The absolute `draw_*` primitives have no image variant, so a
   free-floating HUD can use shapes and text but not the game's icons.
4. **The store holds scalars only** — no tables. Any collection has to be
   serialised into a string yourself.

Each of these has a workaround, documented in the two plugin docs.

## Content facts, confirmed from the mod's shipped data

Useful for seeding a tracker, and notable because these come from the game's own
files rather than from a wiki:

- **1224** POI entries on the `W1_Siagarta` world.
- **821** collectible spawn points: **147 chests**, **199 red orbs** (the
  secret-world ones), **311 plants**, **264 ore nodes**. Plants and ores respawn
  and therefore have no completion counter; chests and red orbs are one-shot.
- POI `kind` values seen in the API docs: `chest`, `ore`, `plant`, `red_orb`,
  `activity`, `dungeon`, `merchant`, and others.
- **Four Early Access classes**: Rogue, Mage, Priest, Warrior (from
  `farever.player.class()`).
- **Mounts and gliders are enumerable after all.** An earlier draft of this
  document said no public list existed. That was true of the web; it is not true
  of the game files. Extracting the HashLink string table
  ([`tools/scan-hlboot.mjs`](tools/scan-hlboot.mjs), see
  [docs/scanning.md](docs/scanning.md)) yields **63 mounts** (`Mount_Boar_05`,
  `Mount_Croco_02`, `Mount_Ladybug_Yellow`…) and **70 gliders**
  (`Glider_Falcon_Blue`, `Glider_Butterfly_Pink`…), alongside 75 chest types,
  74 recipes, 142 statuses, 87 talents and 130 monsters.
- Gliding is confirmed as a mechanic independently — `farever.player.glide_speed()`
  is a real getter.
- Zone naming is `Z1`/`Z2`/`Z3`, with real place names in the strings:
  `Z1_Meridion_Shore`, `Z2_Azuram_Road`, `Z2_Nescent_Hive`.
- Roadmap for 2026 (from press coverage): two more regions, more dungeons,
  factions and reputation, world events, level cap to 50, talent trees, Druid
  and Monk classes, fishing, archaeology, guilds, an auction hall, PvP.

## Can a mod be driven by in-game commands?

Short answer: it can be driven by *typing*, but not by the game's own command
system, and the difference matters.

The build ships two text surfaces, both fully mapped in
`tools/out/hl_types.json`:

| Class | What it is | Fields that matter |
|---|---|---|
| `h2d.Console` / `ui.Console` | Heaps' developer console, with the game's own commands registered into it | `commands : haxe.ds.StringMap` `+0xe0`, `tf : h2d.TextInput` `+0xc0`, `curCmd : String` `+0x108` |
| `ui.hud.ChatBox` | The player-facing chat | `messageInput : ui.comp.InputBox` `+0x480`, `messages : ui.BaseElement` `+0x468` |

`ui.Console`'s static fields name the dev commands still compiled in —
`loadLevel`, `tpElement`, `lootDrops`, `physTree`, `highlightObject`,
`allFxs`. Registering a command into that map, or pushing text into either
input, means **writing to game memory and calling into the game** — which is
the one thing this host does not do, and in the console's case it would also
be a cheat surface. Reading the chat box's input as you type is possible
read-only, but a `/whatever` typed there is still sent to the server as a chat
message, so it is not free either.

The route taken instead: the host already owns a `WndProc` subclass
(`input.cpp`), so **it can own keys and text of its own** without the game
being involved at all. That is what F8 already was; **F9** (drop a waypoint)
and the Routes page's text field are the same mechanism. A full `/`-style
command line for the host would be the next step on that same path, and would
need nothing new from the game.

## Can fareverdb.com fill in missing locations?

It could, and its coordinates are *exactly* ours — but it is not needed, and
taking it would be taking someone else's work.

`https://fareverdb.com/data/world_markers.json` is a plain static file, 840 KB,
3108 markers for `W1_Siagarta`: 1155 creature, 877 poi, 576 gatherable, 445
chest, 23 orb-chest, 18 npc, 12 dungeon, 2 rift. Each carries raw `x/y/z` plus
a map-projected `lng/lat`, a zone id and a level.

The raw coordinates are the same world space this repo already uses, confirmed
on a shared object rather than assumed: `Madrigold_Small` reads
`-9.70, 1011.68, 83.26` in their file and `-9.6961, 1011.6821, 83.2592` in
`tools/out/pois_W1_Siagarta.json`.

But the site says plainly that its data is "extracted directly from the game
files", and so is ours — `tools/gen-routes.mjs` now reads the same 829 world
tiles out of `res.map.pak` and gets 1051 collectible nodes with the zone each
was baked into. The two overlap almost exactly where they overlap at all (both
find 576 gatherables). What their file adds is creature spawn markers and
per-marker levels, and both of those are derivable from the same tiles.

So: **no dependency**. Their `robots.txt` reserves rights over the content
(`Content-Signal: ai-train=no, use=reference`), redistributing their file in
this repo is not ours to do, and a scrape breaks on their next redesign while a
pak read breaks only on a game patch — which is a thing this toolchain already
handles. FareverDB is a good site and worth linking to; it is not worth
depending on.

## Reading the game's own map, read-only

`ui.win.MapWindow` turns out to be the best-instrumented object in the client
for this purpose:

```
ui.win.MapWindow
  +0x530  mouseCursor         : ui.win.map.MapMarker   <- world pos under the cursor
  +0x568  nearClickableMarker : ui.win.map.MapMarker   <- the POI being hovered
  +0x570  pinMarkers          : hl.types.ArrayObj      <- the player's own pins
  +0x548  markers / +0x550 memoMarkers / +0x4f8 activities
  +0x4e8  obelisks / +0x4f0 respawnPoints / +0x4c8 allZones
  +0x620  debugCursorWorldPos : ui.comp.FmtText

ui.win.map.MapMarker
  +0x448  worldPos : h3d.VectorImpl
```

`MapMarker.worldPos` is a world-space vector, so "click a POI on the map to
make it a waypoint" is a **read**. No writes, no hooks. This is what
`host/src/mapwatch.cpp` does.

**But not through `nearClickableMarker`**, which is the obvious-looking field
and the wrong one. It sits with `crosshair`, `crosshairCheckbox` and a
`showCrosshair` static: it is the *gamepad* cursor's snap target, and it stays
null when playing with a mouse. `mouseCursor` is the same story one field
over — it belongs to the debug readout (`debugCursor2DPos`,
`debugCursorWorldPos`). A live run with the map open logged both as null.

What works is the marker list plus each marker's own screen position, since
every marker is an `h2d.Object`:

```
ui.win.MapWindow +0x548  markers : hl.types.ArrayObj   (all markers on screen)
ui.win.MapWindow +0x570  pinMarkers                    (the player's own pins)
h2d.Object       +0x098  absX : F64
h2d.Object       +0x0a0  absY : F64
```

So a click becomes a proximity test between the mouse and every visible
marker's `absX`/`absY`, and the winner's `worldPos` is the waypoint. Those
are UI-scene units rather than swap-chain pixels, so the mouse is mapped
through the ratio between `GameApp.gui.s2d`'s `width`/`height` and the frame
size — assuming 1:1 would put the hit test somewhere else entirely on a
scaled UI.

The window itself is found without a memory scan: `GameApp.gui` is a
`ui.GameUI`, which extends `ui.BaseUI`, whose `windows` at `+0x90` is a short
`ArrayObj` of window instances — walk it once for the right class and cache
the pointer, re-validating it by class name the way `reader_hero()` does.

Two gotchas, both found the hard way:

- **`windows` holds only the windows that are open.** A live run logged
  `windows[0] of 1` with the map up, and a *different* `MapWindow` pointer on
  the next open — so presence in that list is itself "the map is open", and
  the first attempt's extra `visible && parent` gate was what silently
  rejected every map click. Do not cache the pointer either: a closed
  window's block still passes a type check until the collector reuses it.
- **`near` is a `windef.h` macro.** `void* near = ...` fails to compile with
  the error reported on the *following* line.

## Debunked: the "Farever modding" content farms

A large share of search results for Farever modding are auto-generated SEO
pages. They are confidently written and technically wrong. The most common
false claim:

> "Farever is a Unity game — install BepInEx / use a Unity mod loader."

Farever is not a Unity game. Shiro Games ships on Haxe/HashLink/Heaps, as shown
above. BepInEx, MelonLoader and Unity asset tooling are all irrelevant here, and
following that advice will waste your time.

Domains that produced fabricated or unverifiable technical claims in my
searches: `wemod.com`, `xmodhub.com`, `darkprogame.itch.io`, and several
"Farever wiki"/`farever.org`/`farever.co` fan-SEO sites. Some also assert
"Farever is online-only with anti-cheat and mods will flag your account" —
which contradicts both the Steam page (no anti-cheat disclosure, singleplayer
listed) and the developers' own Discord statement quoted above.

Treat anything from those sources as unverified.

## Sources

- [Farever on Steam (appid 3672400)](https://store.steampowered.com/app/3672400/Farever/)
- [Shiro Games' technology stack — Haxe blog](https://haxe.org/blog/shirogames-stack/)
- [Heaps.io — Shiro Games full stack](https://heaps.io/documentation/fullstack.html)
- [Heaps.io](https://heaps.io/) / [CastleDB](https://castledb.org/)
- [ramisotti13-eng/farever-minimap](https://github.com/ramisotti13-eng/farever-minimap) —
  [plugin API](https://github.com/ramisotti13-eng/farever-minimap/blob/main/data/plugins/README.md),
  [capabilities](https://github.com/ramisotti13-eng/farever-minimap/blob/main/CAPABILITIES.md)
- [Sviat/qbms_shirogames](https://github.com/Sviat/qbms_shirogames) — Shiro `res.pak` QuickBMS script
- [Wartales modding guides on Nexus](https://www.nexusmods.com/wartales/articles/11)
- [Massively OP — Farever early access launch](https://massivelyop.com/2026/05/06/shiros-easygoing-mmo-farever-officially-rolls-into-early-access-today-with-an-ambitious-roadmap/)
- [FareverDB](https://www.fareverdb.com/) — fan-made database and world map.
  Nothing here depends on it, but it is the best cross-check going for
  anything extracted from the game files.
- [RPG Site — Farever EA launch and roadmap](https://www.rpgsite.net/news/20313-farever-steam-early-access-discount-now-available-full-content-roadmap)
