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
       { kind = "Mount_DesertRaptor", level = 1, upgrade = 0, stack = 1 },
       { kind = "Glider_LinenWing",   level = 1, upgrade = 0, stack = 1 },
       { kind = "LavendulaPetal",     level = 0, upgrade = 0, stack = 10 },
       { kind = "Weird_Thing_XY",     level = 0, upgrade = 0, stack = 1 },
     }
     MOCK.equipment = { { kind = "Staff_Craft_C", level = 27, upgrade = 3 } }
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

  // Discovery: inventory scan happens on the 1Hz poll
  frames(L, 6, 0.5);
  const toasts = evalLua(L, `table.concat(MOCK.toasts, "|")`);
  ok(toasts.includes('Mount_DesertRaptor'), 'discovers mount from inventory', toasts);
  ok(toasts.includes('Glider_LinenWing'), 'discovers glider from inventory');

  // Codex capture via target_changed
  run(L, `on_event("target_changed", { kind = "Boar_Z1W_E" })`, 'codex evt');
  run(L, `on_event("target_changed", { kind = "Skunk_Z1W" })`, 'codex evt2');

  // Walk every tab
  for (let t = 1; t <= 7; t++) {
    run(L, `MOCK.combo["##tab"] = ${t}`, 'tab');
    frames(L, 2, 0.5);
  }
  ok(true, 'all 7 tabs render without error');

  const texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Mount_DesertRaptor/.test(texts), 'discoveries tab lists the mount');
  ok(/Unclassified/.test(texts), 'unmatched item falls into Unclassified');
  ok(/Boar/.test(texts), 'codex tab lists a bestiary entry');

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
  run(L, `MOCK.combo["##tab"] = 7; MOCK.clicks["Export JSON"] = true`, 'export');
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
      Array.isArray(parsed.discovered) &&
        parsed.discovered.some((d) => d.category === 'Mounts'),
      'export classifies the mount'
    );
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
    ok(cfgJson.screen_w === 1920, 'defaults to 1920 wide');
    ok(typeof cfgJson.buffs === 'object', 'serialises the buff group');
  }

  // --- StatusTracker: "total" semantics (value holds steady) --------------
  run(
    L,
    `MOCK.statuses = {
       { kind = "Mage_ShieldOfSpark_Status", duration = 12.0, stacks = 2, shield_amount = 400 },
     }`,
    'statuses total'
  );
  frames(L, 12, 0.3);
  run(L, `MOCK.combo["##tab"] = 1`, 'status tab');
  frames(L, 2, 0.3);
  let texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/\[total\]/.test(texts), 'detects a non-decaying duration as "total"', texts.slice(-400));

  // --- StatusTracker: "remaining" semantics (value decays) ----------------
  const { L: L2 } = boot('aura_forge.lua');
  run(L2, `on_init()`, 'init2');
  run(
    L2,
    `MOCK.statuses = { { kind = "Regen_Status", duration = 20.0, stacks = 1, shield_amount = 0 } }
     MOCK.combo["##tab"] = 1`,
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
  texts = evalLua(L2, `table.concat(MOCK.texts, "\\n")`);
  ok(
    /\[remaining\]/.test(texts),
    'detects a decaying duration as "remaining"',
    texts.slice(-400)
  );

  // --- CooldownTracker ----------------------------------------------------
  run(
    L,
    `MOCK.skills = {
       { kind = "Mage_RayOfSpark", cooldown = 8.0,  base_cooldown = 10.0, charges = 1 },
       { kind = "Mage_Blink",      cooldown = 20.0, base_cooldown = 24.0, charges = 2 },
     }`,
    'skills'
  );
  frames(L, 2, 0.3);
  run(L, `on_event("damage_dealt", { skill = "Mage_RayOfSpark", amount = 120, is_crit = false })`, 'cast');
  frames(L, 2, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Mage_RayOfSpark\s+[\d.]+\s*\/\s*8s/.test(texts), 'cooldown starts on first damage event', texts.slice(-500));

  // Multi-hit must not restart a running cooldown.
  const before = num(L, 'MOCK.t');
  run(L, `on_event("damage_dealt", { skill = "Mage_RayOfSpark", amount = 40 })`, 'multihit');
  frames(L, 2, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  const m = texts.match(/Mage_RayOfSpark\s+([\d.]+)\s*\/\s*8s/g);
  ok(m !== null && m.length > 0, 'cooldown still tracking after a second hit');

  // Let it expire, then confirm it reports ready.
  frames(L, 40, 0.3);
  texts = evalLua(L, `table.concat(MOCK.texts, "\\n")`);
  ok(/Mage_RayOfSpark\s+ready/.test(texts), 'cooldown expires back to ready', texts.slice(-400));

  // --- HUD actually draws -------------------------------------------------
  run(L, `MOCK.draws = 0; MOCK.draw_texts = {}`, 'reset draws');
  run(
    L,
    `MOCK.statuses = {
       { kind = "Mage_ShieldOfSpark_Status", duration = 12.0, stacks = 3, shield_amount = 400 },
       { kind = "Haste_Status", duration = 6.0, stacks = 1, shield_amount = 0 },
     }
     MOCK.health = 200`,
    'hud state'
  );
  frames(L, 3, 0.2);
  ok(num(L, 'MOCK.draws') > 0, 'HUD issues absolute draw primitives');
  const drawn = evalLua(L, `table.concat(MOCK.draw_texts, "|")`);
  ok(/Mage_ShieldOfSpark/.test(drawn), 'buff bar draws the status name', drawn.slice(0, 200));
  ok(/LOW HEALTH/.test(drawn), 'low-health starter aura triggers at 20% hp', drawn.slice(0, 200));

  // Target-casting starter aura
  run(
    L,
    `MOCK.target.exists = true
     MOCK.target.casting = true
     MOCK.target.cast_skill = "Boar_Skill1"
     MOCK.target.cast_total = 3.0
     MOCK.target.cast_elapsed = 1.0
     MOCK.draw_texts = {}`,
    'cast'
  );
  frames(L, 2, 0.2);
  ok(
    /TARGET CASTING/.test(evalLua(L, `table.concat(MOCK.draw_texts, "|")`)),
    'target-cast starter aura fires'
  );

  // --- JSON round-trip, discriminating test -------------------------------
  // Add a 3rd aura, persist, reload the chunk (fresh locals), add a 4th.
  // If decode were broken the reload would re-seed 2 and we would end at 3.
  run(L, `MOCK.combo["##tab"] = 4; MOCK.clicks["Add"] = true`, 'add3');
  frames(L, 1, 0.1);
  frames(L, 2, 3.0); // let the 2s save interval elapse
  let after3 = JSON.parse(evalLua(L, `tostring(MOCK.store["config"])`));
  ok(after3.auras.length === 3, `config saved with 3 auras (got ${after3.auras.length})`);

  run(L, src, 'reload chunk');
  run(L, `on_init()`, 'reload init');
  run(L, `MOCK.combo["##tab"] = 4; MOCK.clicks["Add"] = true`, 'add4');
  frames(L, 1, 0.1);
  frames(L, 2, 3.0);
  const after4 = JSON.parse(evalLua(L, `tostring(MOCK.store["config"])`));
  ok(
    after4.auras.length === 4,
    `decoder round-trips: reload+add gives 4 auras (got ${after4.auras.length})`
  );

  // --- Import path --------------------------------------------------------
  run(L, `MOCK.combo["##tab"] = 6; MOCK.clicks["Export to box"] = true`, 'export box');
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

  // Walk every tab
  for (let t = 1; t <= 6; t++) {
    run(L, `MOCK.combo["##tab"] = ${t}`, 'tab');
    frames(L, 2, 0.3);
  }
  ok(true, 'all 6 tabs render without error');

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
