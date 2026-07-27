# Scanning Farever for what's available

Two complementary ways to discover Farever's internal vocabulary instead of
guessing at it. Use both: static tells you everything that *exists*, runtime
tells you what your build actually *exposes* and what things are called when
they fire.

| | [`tools/scan-hlboot.mjs`](../tools/scan-hlboot.mjs) | [`plugins/id_scanner.lua`](../plugins/id_scanner.lua) |
|---|---|---|
| Reads | `hlboot.dat` on disk | the live plugin API |
| Needs the game running | no | yes |
| Coverage | everything in the build | only what you encounter |
| Finds | complete id lists | API surface, event payloads, live values |

## Static: extract the bytecode string table

Farever ships as **interpreted HashLink bytecode**, not HL/C compiled to
native. You can tell from the install directory:

```
hlboot.dat     13.9 MB    HashLink bytecode  <- the string table lives here
Farever.exe    290 KB     thin launcher, not the game
libhl.dll                 HashLink VM
fmt/sdl/ui/openal/steam/directx.hdll
res.pak        5.15 GB    Shiro asset pack (contains res/data.cdb)
```

A 290 KB executable next to a 14 MB `.dat` is the giveaway. Because the
bytecode is interpreted, it carries a full string table — every string
constant, field name and type name in the game.

```bash
node tools/scan-hlboot.mjs --lua
```

It auto-probes the usual Steam library paths, or takes an explicit one:

```bash
node tools/scan-hlboot.mjs "E:/SteamLibrary/steamapps/common/Farever/hlboot.dat"
```

### What came out (build `7c3ca4dd…`, July 2026)

Header: HashLink **v4**, with debug info — 68,494 strings, 47,195 functions,
45,696 types.

| Bucket | Count | Real examples |
|---|---:|---|
| mounts | 63 | `Mount_Boar_05`, `Mount_Croco_02`, `Mount_Ladybug_Yellow` |
| gliders | 70 | `Glider_Falcon_Blue`, `Glider_Butterfly_Pink`, `Glider_Demon_Purple` |
| chests | 75 | `Chest_Box_Locked`, `Chest_Orb_Unlocked`, `Chest_Z2U1_FigAss` |
| recipes | 74 | `Recipe_InvisibilityPotion`, `Recipe_SanctifiedEmbroidery` |
| statuses | 142 | `Mage_ShieldOfSpark_Status`, `OreAffix_Fire_Status` |
| telegraphs | 80 | `Telegraph_Circle_Spark`, `Telegraph_Cone_Physical` |
| talents | 87 | `Mage_Talent_HighVoltage`, `Warrior_Talent_Bloodfeast` |
| skills | 300 | from the `script.skills.*` namespace |
| classSkills | 187 | `Mage_Blink`, `Warrior_Charge` |
| weaponSkills | 67 | `Axe_Boomerang_Skill1`, `Shield_Craft_Passive` |
| monsters | 130 | `Boar_Z1W_E`, `FaerieBee_Z2W_Tank`, `Manfish_Z1W_Caster` |
| zoneContent | 228 | `Z1_Meridion_Shore`, `Z2_Nescent_Hive` |
| armor | 422 | `Feet_RKobold_FigCle_Craft`, `Hands_Z1U2_FigWiz` |
| weapons | 95 | `Staff_Craft`, `Daggers_Base_Attack3` |

These line up exactly with the ids the plugin API documents returning
(`Mage_RayOfSpark`, `Mage_ShieldOfSpark_Status`, `Boar_Skill1`, `Boar_Z1W_E`,
`Staff_Craft_C`), which is the cross-check that the extraction is correct.

### Naming conventions worth knowing

- **Zones** are `Z1`/`Z2`/`Z3`. Monsters carry a zone tag plus a terrain letter
  and an optional role: `Boar_Z1W_E`, `FaerieBee_Z2W_Tank`, `Manfish_Z1W_Caster`.
- **Role codes** appear all over gear: `Fig` (Warrior), `Ass` (Rogue), `Wiz`
  (Mage), `Cle` (Priest). Pairs like `FigAss` or `WizCle` mark gear shared by
  two classes — `Chest_Z1U1_AssCle` is a chest piece for Rogue and Priest.
- **Skills** live under `script.skills.*`, sometimes with a `$` (`script.skills.$Foo`);
  both denote the id `Foo`.
- **Statuses** end in `_Status`. Telegraphs are `Telegraph_<Shape>_<Element>`.
- **Slot prefixes** match the API's `slot_name` values: `Head_`, `Shoulders_`,
  `Back_`, `Hands_`, `Waist_`, `Legs_`, `Feet_`, `Chest_`, `Neck_`.
  Note `Chest_` is both an armour slot and a container prefix.
- Debug info is present, so source paths are in there too: `src/ent/`, `src/st/`,
  `src/ui/`, `src/world/`, `src/client/`, `src/server/`.

### Output and copyright

Everything lands in `tools/out/`, which is **gitignored on purpose**:

```
tools/out/strings.txt        all 68,494 strings
tools/out/classified.json    bucketed
tools/out/farever_ids.lua    Lua tables (with --lua)
```

These are bulk game data belonging to Shiro Games. Generating them locally from
your own installed copy for interoperability is ordinary modding practice;
redistributing the dumps is not. **Commit the tool, not the haul.**

What this repo *does* commit is the derived *naming conventions* — the prefix
list in `collection_atlas.lua`'s `ITEM_CATEGORIES`. A handful of prefixes is a
fact about how ids are spelled, not a copy of the content.

### Regenerating after a patch

Game updates change ids. Re-run the scan after each one; the mod's own build
allowlist will tell you the build changed anyway.

## Runtime: probe the live API

Drop [`plugins/id_scanner.lua`](../plugins/id_scanner.lua) into
`<Farever>\data\plugins\`.

### API tab
Reflectively walks the `farever` and `imgui` tables and lists **every function
that exists on your DLL**, calling the read-only ones to show a live sample
value:

```
farever.player
  farever.player.health_pct                0.812
  farever.player.spark                     3
  farever.player.statuses                  table[2]
  farever.player.codex                     (needs args)
```

This is the honest answer to "what does my build support" — better than a
changelog, because it is your actual binary. It also surfaces getters the docs
never mentioned.

Mutating functions (`add`, `remove`, `set`, `write`, `toast`, `sound`, `log`…)
are **never auto-invoked**; they are listed and skipped. The test harness
asserts this — that the probe places no waypoints and fires no toasts.

### Events tab
Every event that fires, with its payload field names, types, a sample value and
a count — plus a list of documented events *not yet seen*, so you know what you
still need to go trigger.

It records events by name without a whitelist, so an event added in a future
mod version shows up the first time it fires. The harness covers this by
feeding it a deliberately unknown event.

### Vocabulary tab
Every distinct id observed: skills, statuses, items, slots, currencies,
monsters, cast skills, POI kinds and subkinds, with sighting counts. Persists
across sessions, so it accumulates as you play.

### Export tab
Writes the whole thing to
`%LOCALAPPDATA%\farever-minimap\combatlogs\farever-api-scan.json`.

## Not attempted: unpacking `res.pak`

`res.pak` is 5.15 GB and contains `res/data.cdb` — the CastleDB file holding
Farever's structured game data. That is the *authoritative* source: real
display names, stats, drop tables, and the mapping from internal id to the
name you see in game.

Extracting it needs QuickBMS with
[Shiro_Games_PAK_script.bms](https://github.com/Sviat/qbms_shirogames), the
same route used for Northgard and Wartales. I did not do it here — it needs a
third-party binary and a 5 GB extraction, and the bytecode string table already
answers "what ids exist", which is what the plugins needed.

It is the obvious next step if you want localized display names rather than
internal ids.
