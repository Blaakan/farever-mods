#!/usr/bin/env node
// ---------------------------------------------------------------------------
// run-harness.mjs
//
// Runs the plugins against a mock host inside a real Lua VM (fengari), so the
// logic can be exercised without launching Farever. Catches the whole class of
// bugs a syntax check misses: wrong widget return-value order, nil arithmetic,
// broken serialisation, cooldown maths that never counts down.
//
// Usage:  node tools/run-harness.mjs
// ---------------------------------------------------------------------------

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const fengari = require('fengari');
const { lua, lauxlib, lualib, to_luastring } = fengari;

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');

let failures = 0;
let checks = 0;

function ok(cond, label, detail) {
  checks++;
  if (cond) {
    console.log(`    pass  ${label}`);
  } else {
    failures++;
    console.log(`    FAIL  ${label}${detail ? `\n            ${detail}` : ''}`);
  }
}

function newState() {
  const L = lauxlib.luaL_newstate();
  lualib.luaL_openlibs(L);
  return L;
}

function run(L, code, label) {
  const status = lauxlib.luaL_dostring(L, to_luastring(code));
  if (status !== lua.LUA_OK) {
    const err = lua.lua_tojsstring(L, -1);
    lua.lua_pop(L, 1);
    throw new Error(`${label}: ${err}`);
  }
}

function evalLua(L, expr) {
  const status = lauxlib.luaL_dostring(L, to_luastring(`return tostring(${expr})`));
  if (status !== lua.LUA_OK) {
    const err = lua.lua_tojsstring(L, -1);
    lua.lua_pop(L, 1);
    throw new Error(`eval '${expr}': ${err}`);
  }
  const v = lua.lua_tojsstring(L, -1);
  lua.lua_pop(L, 1);
  return v;
}

function num(L, expr) {
  return Number(evalLua(L, expr));
}

const MOCK_SRC = readFileSync(join(HERE, 'harness', 'mock_host.lua'), 'utf8');

function boot(pluginFile) {
  const L = newState();
  run(L, MOCK_SRC, 'mock_host');
  const src = readFileSync(join(ROOT, 'plugins', pluginFile), 'utf8');
  run(L, src, pluginFile);
  return { L, src };
}

// Advance time and render N frames, asserting nothing throws.
function frames(L, n, dt) {
  run(
    L,
    `for i = 1, ${n} do
       MOCK.t = MOCK.t + ${dt}
       on_render()
     end`,
    'frames'
  );
}

// ===========================================================================
console.log('\ncollection_atlas.lua');
// ===========================================================================
try {
  const { L } = boot('collection_atlas.lua');

  run(L, `MOCK.pois = MOCK.make_pois(400)`, 'pois');
  run(
    L,
    `MOCK.inventory = {
       { kind = "Glider_Falcon_Blue",  level = 1,  upgrade = 0, stack = 1 },
       { kind = "Feet_RKobold_FigCle", level = 24, upgrade = 0, stack = 1 },
       { kind = "LavendulaPetal",      level = 0,  upgrade = 0, stack = 10 },
       { kind = "Weird_Thing_XY",      level = 0,  upgrade = 0, stack = 1 },
     }
     MOCK.equipment = {
       { kind = "Staff_Craft_C",       level = 27, upgrade = 3, slot = 1,  slot_name = "Weapon1" },
       { kind = "Mount_Boar_05",       level = 0,  upgrade = 0, slot = 20, slot_name = "" },
       { kind = "StoneOfPower_Trinket",level = 25, upgrade = 1, slot = 14, slot_name = "Trinket" },
       { kind = "Sprout_Garlic_Spark", level = 0,  upgrade = 0, slot = 27, slot_name = "" },
     }
     MOCK.codex = {
       Boar_Z1W_E = { state = "complete", completed = true, progress = 10,
                      max = 10, name = "Boar", path = "beasts/boar" },
       Skunk_Z1W  = { state = "partial",  completed = false, progress = 3,
                      max = 10, name = "Skunk", path = "beasts/skunk" },
     }`,
    'world'
  );

  run(L, `on_init()`, 'on_init');
  ok(num(L, '#MOCK.logs') > 0, 'on_init logs readiness');

  // POI ingest
  frames(L, 3, 0.5);
  const poiLine = evalLua(L, `MOCK.texts[#MOCK.texts]`);
  ok(num(L, '#MOCK.texts') > 0, 'dashboard renders');

  // Collections/vault scan happens on the 1Hz poll
  frames(L, 6, 0.5);
  const toasts = evalLua(L, `table.concat(MOCK.toasts, "|")`);
  ok(toasts.includes('Mount_Boar_05'), 'records mount from the equipped mount slot', toasts);
  ok(toasts.includes('Glider_Falcon_Blue'), 'records glider from the bag');
  ok(toasts.includes('Appearance unlocked: Feet_RKobold_FigCle'), 'armor sighting unlocks its appearance');
  ok(toasts.includes('Vault keeper: Staff_Craft_C'), 'weapon goes to the vault');
  ok(toasts.includes('Vault keeper: StoneOfPower_Trinket'), 'trinket (by slot_name) goes to the vault');
  ok(toasts.includes('New companion recorded: Sprout_Garlic_Spark'), 'companion (Sprout) is recorded');

  // Codex capture via target_changed
  run(L, `on_event("target_changed", { kind = "Boar_Z1W_E" })`, 'codex evt');
  run(L, `on_event("target_changed", { kind = "Skunk_Z1W" })`, 'codex evt2');

  // Walk every tab
  for (let t = 1; t <= 8; t++) {
    run(L, `MOCK.combo["##tab"] = ${t}`, 'tab');
    frames(L, 2, 0.5);
  }
  ok(true, 'all 8 tabs render without error');

  const texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Mount_Boar_05/.test(texts), 'collections tab lists the mount');
  ok(/Appearances \(1\)/.test(texts), 'appearance section counts the armor unlock');
  ok(/Staff_Craft_C\s+lvl 27/.test(texts), 'vault tab shows the weapon with its level');
  ok(/Boar/.test(texts), 'codex tab lists a bestiary entry');
  const acctm = evalLua(L, `tostring(MOCK.store["acct_mounts"])`);
  ok(acctm.includes('Mount_Boar_05'), 'mount record is account-wide (no character suffix)');

  // Route planning + waypoints (tab 4)
  run(L, `MOCK.combo["##tab"] = 4; MOCK.clicks["Plan route"] = true`, 'plan');
  frames(L, 1, 0.1);
  run(L, `MOCK.clicks["Place waypoints"] = true`, 'place');
  frames(L, 1, 0.1);
  const wps = num(L, '#MOCK.waypoints');
  ok(wps > 0, `route places waypoints (got ${wps})`);

  run(L, `MOCK.clicks["Clear waypoints"] = true`, 'clear');
  frames(L, 1, 0.1);
  ok(num(L, '#MOCK.waypoints') === 0, 'clearing removes only our waypoints');

  // Auto-collect: teleport onto a chest and let the proximity tick fire
  run(
    L,
    `local target
     for _, p in ipairs(MOCK.pois) do
       if p.kind == "chest" then target = p break end
     end
     MOCK.x, MOCK.y, MOCK.z = target.x, target.y, target.z
     MOCK.store.auto_collect = true
     ATLAS_TARGET_ID = target.id`,
    'teleport'
  );
  run(L, `on_init()`, 're-init with auto_collect');
  frames(L, 4, 0.5);
  const doneSet = evalLua(L, `tostring(MOCK.store["done__Testchar"])`);
  ok(
    doneSet !== 'nil' && doneSet.length > 0,
    'auto-collect records a collected id',
    doneSet.slice(0, 80)
  );

  // Export
  run(L, `MOCK.combo["##tab"] = 8; MOCK.clicks["Export JSON"] = true`, 'export');
  frames(L, 1, 0.1);
  const exported = evalLua(L, `tostring(MOCK.files["collection-atlas-Testchar.json"])`);
  ok(exported !== 'nil', 'export writes a combat-log file');
  let parsed = null;
  try {
    parsed = JSON.parse(exported);
  } catch (e) {
    /* handled below */
  }
  ok(parsed !== null, 'exported report is valid JSON');
  if (parsed) {
    ok(parsed.plugin === 'collection_atlas', 'export identifies the plugin');
    ok(typeof parsed.categories === 'object', 'export carries category totals');
    ok(Array.isArray(parsed.areas) && parsed.areas.length > 0, 'export carries areas');
    ok(
      parsed.collections && parsed.collections.mount.includes('Mount_Boar_05'),
      'export carries the mount collection'
    );
    ok(
      parsed.collections.appearance.includes('Feet_RKobold_FigCle'),
      'export carries the appearance unlock'
    );
    ok(
      Array.isArray(parsed.vault) &&
        parsed.vault.some((v) => v.kind === 'Staff_Craft_C' && v.level === 27),
      'export carries the vault with levels'
    );
    ok(!exported.includes('LavendulaPetal'), 'materials churn is not tracked');
  }

  ok(num(L, '#MOCK.store_bad') === 0, 'never persists a non-scalar to the store');
} catch (e) {
  failures++;
  console.log(`    FAIL  threw: ${e.message}`);
}

// ===========================================================================
console.log('\naura_forge.lua');
// ===========================================================================
try {
  const { L, src } = boot('aura_forge.lua');

  run(L, `on_init()`, 'on_init');
  const cfgRaw = evalLua(L, `tostring(MOCK.store["config"])`);
  ok(cfgRaw !== 'nil', 'on_init persists a config');

  let cfgJson = null;
  try {
    cfgJson = JSON.parse(cfgRaw);
  } catch (e) {
    /* handled below */
  }
  ok(cfgJson !== null, 'hand-written encoder emits valid JSON');
  if (cfgJson) {
    ok(Array.isArray(cfgJson.auras) && cfgJson.auras.length === 2, 'seeds 2 starter auras');
    ok(typeof cfgJson.buffs === 'object', 'serialises the buff group');
  }

  // --- Permanent + stacked statuses (the live-game trio: duration 0 / -1) --
  run(
    L,
    `MOCK.statuses = {
       { kind = "Staff_SummonDemon_Skill1_Status", duration = 0.0,  stacks = 20, shield_amount = 0 },
       { kind = "Staff_Censer_Passive_Buff",       duration = -1.0, stacks = 5,  shield_amount = 0 },
       { kind = "Mage_ShieldOfSpark_Status",       duration = 8.0,  stacks = 2,  shield_amount = 400 },
     }`,
    'statuses live trio'
  );
  frames(L, 12, 0.3);
  let texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Staff_SummonDemon_Skill1.*x20/.test(texts), 'permanent aura (duration 0) renders with stacks', texts.slice(-400));
  ok(/Staff_Censer_Passive.*x5/.test(texts), 'permanent aura (duration -1) renders with stacks');
  ok(/Mage_ShieldOfSpark.*\d(\.\d)?s/.test(texts), 'timed aura renders a countdown');

  // Permanent auras must not vanish over time (v1 bug: they rendered expired).
  run(L, `MOCK.texts = {}`, 'clear texts');
  frames(L, 40, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Staff_SummonDemon_Skill1.*x20/.test(texts), 'permanent aura still shown 12s later', texts.slice(-300));

  // Status diagnostics tab reports the detected modes
  run(L, `MOCK.checks["settings"] = true`, 'open settings');
  frames(L, 1, 0.1);
  run(L, `MOCK.combo["##tab"] = 1`, 'status tab');
  frames(L, 2, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/\[permanent\]/.test(texts), 'status tab labels permanent mode', texts.slice(-500));
  ok(/\[total\]/.test(texts), 'status tab labels a non-decaying duration as "total"');
  run(L, `MOCK.checks["settings"] = false`, 'close settings');
  frames(L, 1, 0.1);

  // --- StatusTracker: "remaining" semantics (value decays) ----------------
  const { L: L2 } = boot('aura_forge.lua');
  run(L2, `on_init()`, 'init2');
  run(
    L2,
    `MOCK.statuses = { { kind = "Regen_Status", duration = 20.0, stacks = 1, shield_amount = 0 } }`,
    'statuses remaining'
  );
  run(
    L2,
    `for i = 1, 14 do
       MOCK.t = MOCK.t + 0.3
       MOCK.statuses[1].duration = MOCK.statuses[1].duration - 0.3
       on_render()
     end`,
    'decay'
  );
  run(L2, `MOCK.checks["settings"] = true`, 'open settings2');
  run(L2, `MOCK.t = MOCK.t + 0.3; on_render()`, 'frame');
  run(L2, `MOCK.combo["##tab"] = 1; MOCK.t = MOCK.t + 0.3; on_render()`, 'status tab2');
  texts = evalLua(L2, `table.concat(MOCK.texts, "\\n")`);
  ok(
    /\[remaining\]/.test(texts),
    'detects a decaying duration as "remaining"',
    texts.slice(-400)
  );

  // --- CooldownTracker (HUD face) ------------------------------------------
  run(
    L,
    `MOCK.skills = {
       { kind = "Mage_RayOfSpark", cooldown = 8.0,  base_cooldown = 10.0, charges = 1 },
       { kind = "Mage_Blink",      cooldown = 20.0, base_cooldown = 24.0, charges = 2 },
     }`,
    'skills'
  );
  frames(L, 2, 0.3);
  run(L, `MOCK.texts = {}`, 'clear');
  run(L, `on_event("damage_dealt", { skill = "Mage_RayOfSpark", amount = 120, is_crit = false })`, 'cast');
  frames(L, 2, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Mage_RayOfSpark\s+\d(\.\d)?s/.test(texts), 'cooldown row appears on first damage event', texts.slice(-400));

  // Multi-hit must not restart a running cooldown: remaining keeps falling.
  run(L, `on_event("damage_dealt", { skill = "Mage_RayOfSpark", amount = 40 })`, 'multihit');
  run(L, `MOCK.texts = {}`, 'clear');
  frames(L, 10, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  const times = [...texts.matchAll(/Mage_RayOfSpark\s+(\d+(?:\.\d)?)s/g)].map((x) => parseFloat(x[1]));
  ok(
    times.length > 1 && times[times.length - 1] < times[0],
    'multi-hit does not restart the cooldown (remaining keeps falling)',
    JSON.stringify(times)
  );

  // Expire: with show_ready off the row disappears from the HUD.
  frames(L, 30, 0.3);
  run(L, `MOCK.texts = {}`, 'clear');
  frames(L, 2, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(!/Mage_RayOfSpark\s+\d/.test(texts), 'expired cooldown drops off the HUD', texts.slice(-300));

  // --- Alerts ---------------------------------------------------------------
  run(L, `MOCK.health = 200; MOCK.texts = {}`, 'low hp');
  frames(L, 2, 0.2);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/LOW HEALTH/.test(texts), 'low-health starter aura triggers at 20% hp', texts.slice(-300));

  run(
    L,
    `MOCK.target.exists = true
     MOCK.target.casting = true
     MOCK.target.cast_skill = "Boar_Skill1"
     MOCK.target.cast_total = 3.0
     MOCK.target.cast_elapsed = 1.0
     MOCK.texts = {}`,
    'cast'
  );
  frames(L, 2, 0.2);
  ok(
    /TARGET CASTING/.test(evalLua(L, `table.concat(MOCK.texts, "\\n")`)),
    'target-cast starter aura fires'
  );

  // --- JSON round-trip, discriminating test -------------------------------
  // Add a 3rd aura, persist, reload the chunk (fresh locals), add a 4th.
  // If decode were broken the reload would re-seed 2 and we would end at 3.
  run(L, `MOCK.checks["settings"] = true`, 'settings');
  frames(L, 1, 0.1);
  run(L, `MOCK.combo["##tab"] = 4; MOCK.clicks["Add"] = true`, 'add3');
  frames(L, 1, 0.1);
  frames(L, 2, 3.0); // let the 2s save interval elapse
  let after3 = JSON.parse(evalLua(L, `tostring(MOCK.store["config"])`));
  ok(after3.auras.length === 3, `config saved with 3 auras (got ${after3.auras.length})`);

  run(L, src, 'reload chunk');
  run(L, `on_init()`, 'reload init');
  run(L, `MOCK.checks["settings"] = true`, 'settings again');
  frames(L, 1, 0.1);
  run(L, `MOCK.combo["##tab"] = 4; MOCK.clicks["Add"] = true`, 'add4');
  frames(L, 1, 0.1);
  frames(L, 2, 3.0);
  const after4 = JSON.parse(evalLua(L, `tostring(MOCK.store["config"])`));
  ok(
    after4.auras.length === 4,
    `decoder round-trips: reload+add gives 4 auras (got ${after4.auras.length})`
  );

  // --- Import path --------------------------------------------------------
  run(L, `MOCK.combo["##tab"] = 5; MOCK.clicks["Export to box"] = true`, 'export box');
  frames(L, 1, 0.1);
  run(L, `MOCK.clicks["Write to file"] = true`, 'write file');
  frames(L, 1, 0.1);
  const shared = evalLua(L, `tostring(MOCK.files["aura-forge-config.json"])`);
  ok(shared !== 'nil', 'share tab writes a config file');
  let sharedJson = null;
  try {
    sharedJson = JSON.parse(shared);
  } catch (e) {
    /* handled */
  }
  ok(sharedJson !== null && sharedJson.auras.length === 4, 'shared config is valid JSON');

  // Walk every settings tab
  for (let t = 1; t <= 5; t++) {
    run(L, `MOCK.combo["##tab"] = ${t}`, 'tab');
    frames(L, 2, 0.3);
  }
  ok(true, 'all 5 settings tabs render without error');

  ok(num(L, '#MOCK.store_bad') === 0, 'never persists a non-scalar to the store');
} catch (e) {
  failures++;
  console.log(`    FAIL  threw: ${e.message}`);
}

// ===========================================================================
console.log('\nid_scanner.lua');
// ===========================================================================
try {
  const { L } = boot('id_scanner.lua');
  run(L, `MOCK.pois = MOCK.make_pois(80)`, 'pois');
  run(
    L,
    `MOCK.statuses = { { kind = "Mage_ShieldOfSpark_Status", duration = 8, stacks = 1, shield_amount = 100 } }
     MOCK.skills = { { kind = "Mage_RayOfSpark", cooldown = 8, base_cooldown = 10, charges = 1 } }
     MOCK.inventory = { { kind = "Mount_Boar_05", stack = 1 }, { kind = "Glider_Falcon_Blue", stack = 1 } }
     MOCK.equipment = { { kind = "Staff_Craft_C", level = 27, upgrade = 3, slot = 1, slot_name = "Weapon1" } }
     MOCK.currencies = { { kind = "Gold", amount = 10433 } }
     MOCK.target.exists = true
     MOCK.target.name = "Boar_Z1W_E"`,
    'world'
  );

  run(L, `on_init()`, 'on_init');

  // The probe must find real API surface and must not fire mutators.
  const wpBefore = num(L, '#MOCK.waypoints');
  const toastsBefore = num(L, '#MOCK.toasts');
  ok(num(L, '#MOCK.logs') > 0, 'probe logs a result');
  ok(wpBefore === 0, 'probe never called waypoints.add');
  ok(toastsBefore === 0, 'probe never fired a toast');

  frames(L, 4, 0.6);

  // Events, including one the plugin has no prior knowledge of.
  run(L, `on_event("damage_dealt", { skill = "Mage_RayOfSpark", amount = 120, is_crit = true, target = "Boar_Z1W_E" })`, 'dmg');
  run(L, `on_event("target_changed", { kind = "Skunk_Z1W" })`, 'tgt');
  run(L, `on_event("undocumented_future_event", { mystery = 42 })`, 'unknown evt');
  frames(L, 2, 0.6);

  for (let t = 1; t <= 4; t++) {
    run(L, `MOCK.combo["##tab"] = ${t}`, 'tab');
    frames(L, 2, 0.6);
  }
  ok(true, 'all 4 tabs render without error');

  const texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/farever\.player\.health_pct/.test(texts), 'API probe enumerates player getters', texts.slice(0, 200));
  ok(/undocumented_future_event/.test(texts), 'captures an event it was never told about');
  ok(/Mount_Boar_05/.test(texts), 'records item ids from inventory');
  ok(/Boar_Z1W_E/.test(texts), 'records monster ids');

  run(L, `MOCK.combo["##tab"] = 4; MOCK.clicks["Export JSON"] = true`, 'export');
  frames(L, 1, 0.2);
  const dump = evalLua(L, `tostring(MOCK.files["farever-api-scan.json"])`);
  ok(dump !== 'nil', 'exports a scan file');
  let scan = null;
  try {
    scan = JSON.parse(dump);
  } catch (e) {
    /* handled */
  }
  ok(scan !== null, 'scan export is valid JSON');
  if (scan) {
    ok(scan.api.length > 40, `api probe found ${scan.api?.length} entries`);
    ok(!!scan.events.undocumented_future_event, 'unknown event survives into the export');
    ok(
      scan.vocabulary.item?.some((i) => i.id === 'Glider_Falcon_Blue'),
      'glider id lands in the vocabulary'
    );
  }

  ok(num(L, '#MOCK.store_bad') === 0, 'never persists a non-scalar to the store');
} catch (e) {
  failures++;
  console.log(`    FAIL  threw: ${e.message}`);
}

console.log(`\n${checks} check(s), ${failures} failure(s)`);
process.exit(failures > 0 ? 1 : 0);
