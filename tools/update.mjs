#!/usr/bin/env node
// ---------------------------------------------------------------------------
// update.mjs - post-patch check and repair.
//
// Run this after Farever updates. It finds the game, works out what actually
// changed, regenerates whatever is stale, and tells you plainly whether a
// rebuild or reinstall is needed.
//
//   node tools/update.mjs            check and report
//   node tools/update.mjs --fix      also regenerate offsets and rebuild
//
// What it checks, and why each one matters:
//
//   hlboot.dat hash   the game's bytecode. If this changed, field offsets may
//                     have moved and the host refuses to read memory until
//                     regenerated - so this is the one that gates everything.
//   field offsets     regenerated and diffed field by field, so you see
//                     exactly what moved rather than just "something did".
//   libhl.dll hash    the HashLink VM itself. Changes here can move the
//                     *native* struct layouts in hl_runtime.h, which are
//                     hand-written and would need a human look.
//   dxgi imports      the proxy only loads if the game still imports the
//                     functions it exports.
//   install state     whether the built DLL is actually deployed and current.
// ---------------------------------------------------------------------------

import { readFileSync, writeFileSync, existsSync, copyFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { findGame } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const STATE = join(HERE, 'out', 'update-state.json');
const FIX = process.argv.includes('--fix');

const sha = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');

function loadState() {
  if (!existsSync(STATE)) return {};
  try {
    return JSON.parse(readFileSync(STATE, 'utf8'));
  } catch {
    return {};
  }
}

// Field offsets currently baked into the host.
function parseHeaderOffsets() {
  const h = join(ROOT, 'host', 'src', 'offsets.gen.h');
  if (!existsSync(h)) return { sha: null, fields: {} };
  const txt = readFileSync(h, 'utf8');
  const m = txt.match(/#define FMK_BUILD_SHA256 "([0-9a-f]+)"/);
  const fields = {};
  let ns = '';
  for (const line of txt.split('\n')) {
    const nsm = line.match(/^namespace (\w+) \{/);
    if (nsm) ns = nsm[1];
    const fm = line.match(/constexpr uint32_t (\w+) = (0x[0-9a-f]+);/);
    if (fm && ns && fm[1] !== 'SIZEOF') fields[`${ns}.${fm[1]}`] = fm[2];
  }
  return { sha: m ? m[1] : null, fields };
}

const say = (s) => console.log(s);
const problems = [];
const actions = [];

say('');
const game = findGame();
if (!game) {
  say('FAIL  Farever install not found.');
  say('      Set FAREVER_DIR=<path to the folder containing Farever.exe>');
  process.exit(1);
}
say(`game    ${game}`);

const bootPath = join(game, 'hlboot.dat');
const libhlPath = join(game, 'libhl.dll');
const bootSha = sha(bootPath);
const libhlSha = existsSync(libhlPath) ? sha(libhlPath) : null;

const prev = loadState();
const header = parseHeaderOffsets();

// --- 1. game bytecode ------------------------------------------------------
const bootChanged = header.sha !== bootSha;
say('');
say(`hlboot.dat  ${bootSha.slice(0, 16)}...`);
if (!header.sha) {
  say('  WARN  host has no generated offsets yet');
  actions.push('generate offsets');
} else if (bootChanged) {
  say(`  CHANGED  host was built for ${header.sha.slice(0, 16)}...`);
  say('  -> the host disables memory reads until offsets are regenerated');
  actions.push('regenerate offsets + rebuild DLL');
} else {
  say('  unchanged - host offsets apply');
}

// --- 2. HashLink VM --------------------------------------------------------
say('');
if (libhlSha) {
  say(`libhl.dll   ${libhlSha.slice(0, 16)}...`);
  if (prev.libhlSha && prev.libhlSha !== libhlSha) {
    say('  CHANGED  the HashLink VM itself was updated.');
    say('  -> hl_runtime.h holds HAND-WRITTEN native struct offsets (hl_type,');
    say('     hl_type_obj, varray). These rarely move, but a VM bump is the');
    say('     one case where they can. Worth a human check.');
    problems.push('libhl.dll changed - review hl_runtime.h native offsets');
  } else if (!prev.libhlSha) {
    say('  (first run - recorded as the baseline)');
  } else {
    say('  unchanged');
  }
}

// --- 3. regenerate + diff --------------------------------------------------
if (bootChanged || !header.sha) {
  say('');
  if (FIX) {
    say('regenerating offsets...');
    const before = header.fields;
    try {
      // force a fresh type dump for the new build
      const dump = join(HERE, 'out', 'hl_types.json');
      if (existsSync(dump)) writeFileSync(dump + '.old', readFileSync(dump));
      execFileSync(process.execPath, [join(HERE, 'scan-hltypes.mjs'), '--json', bootPath], {
        stdio: 'pipe',
      });
      execFileSync(process.execPath, [join(HERE, 'gen-offsets.mjs'), bootPath], {
        stdio: 'inherit',
      });
    } catch (e) {
      say('  FAIL  offset generation failed - a field the host needs is gone.');
      say('        The generator names it above. That field must be re-found');
      say('        in the new build before the reader can be trusted.');
      process.exit(1);
    }

    const after = parseHeaderOffsets().fields;
    const moved = [];
    const gone = [];
    for (const k of Object.keys(before)) {
      if (!(k in after)) gone.push(k);
      else if (before[k] !== after[k]) moved.push(`${k}: ${before[k]} -> ${after[k]}`);
    }
    const added = Object.keys(after).filter((k) => !(k in before));

    say('');
    say(`offset diff: ${moved.length} moved, ${added.length} added, ${gone.length} removed`);
    for (const m of moved) say(`  MOVED    ${m}`);
    for (const g of gone) say(`  REMOVED  ${g}`);
    for (const a of added) say(`  ADDED    ${a}`);
    if (!moved.length && !gone.length) {
      say('  (layout identical - only the build hash changed)');
    }
  } else {
    say('run with --fix to regenerate offsets and rebuild');
  }
}

// --- 4. proxy still viable? ------------------------------------------------
say('');
try {
  const out = execFileSync(
    process.execPath,
    [join(HERE, 'pe-imports.mjs'), '--funcs', join(game, 'dx12.hdll')],
    { encoding: 'utf8' }
  );
  const hasDxgi = /\bdxgi\.dll\b/.test(out);
  say(`dxgi proxy  ${hasDxgi ? 'viable - dx12.hdll still imports dxgi.dll' : 'AT RISK'}`);
  if (!hasDxgi) {
    problems.push('dx12.hdll no longer imports dxgi.dll - the proxy will not load');
  }
} catch {
  say('dxgi proxy  (could not inspect dx12.hdll)');
}

// --- 5. install state ------------------------------------------------------
say('');
const built = join(ROOT, 'host', 'build', 'dxgi.dll');
const installed = join(game, 'dxgi.dll');
if (!existsSync(built)) {
  say('host DLL    not built yet  (host/build.cmd)');
} else if (!existsSync(installed)) {
  say('host DLL    built, NOT installed');
  say(`            copy ${built}`);
  say(`            to   ${installed}`);
} else if (sha(built) !== sha(installed)) {
  say('host DLL    installed copy is STALE');
  if (FIX) {
    // This copy is reached exactly when the installed DLL differs - which is
    // exactly when the game is most likely to be running and holding it
    // open. An unhandled throw here aborted the run before the rebuild
    // below, leaving regenerated offsets and a stale DLL out of step with a
    // raw stack trace as the only explanation.
    if (installDll()) say('            -> updated');
  } else {
    actions.push('reinstall host DLL');
  }
} else {
  say('host DLL    installed and current');
}

function installDll() {
  try {
    copyFileSync(built, installed);
    return true;
  } catch (e) {
    say(`            FAILED to copy ${built}`);
    say(`                          to ${installed}`);
    say(`            ${e.message}`);
    say('            Close the game if it is running - Windows holds a');
    say('            loaded DLL open - or run this from an elevated prompt');
    say('            if the game lives under Program Files.');
    problems.push('host DLL could not be installed');
    return false;
  }
}

// --- rebuild ---------------------------------------------------------------
if (FIX && (bootChanged || !header.sha)) {
  say('');
  say('rebuilding host...');
  let rebuilt = false;
  try {
    // 'inherit', not 'pipe'. build.cmd's most useful output is the one it
    // prints when there is no MSVC toolset - the workload to install and the
    // two download links - and piping it swallowed exactly that, on exactly
    // the machine where it was the answer.
    execFileSync('cmd', ['/c', join(ROOT, 'host', 'build.cmd')], { stdio: 'inherit' });
    say('  built');
    rebuilt = true;
  } catch {
    say('  FAIL  build failed - the compiler output is above');
    problems.push('host rebuild failed');
  }
  // A copy failure is not a build failure, and saying so sent people to look
  // at compiler output that was fine.
  if (rebuilt && existsSync(installed) && installDll()) say('  installed');
}

// --- data files ------------------------------------------------------------
//
// Offsets are not the whole story. A patch that adds or renames an item
// leaves the atlas silently missing it - lookup is by string id - and can
// move the nodes a route points at. Nothing detected that, because nothing
// was checking.
if (bootChanged || !header.sha) {
  say('');
  if (FIX) {
    for (const script of ['gen-atlas.mjs', 'gen-routes.mjs']) {
      say(`regenerating ${script.replace('gen-', '').replace('.mjs', '')}...`);
      try {
        execFileSync(process.execPath, [join(HERE, script), '--game', game],
                     { stdio: 'inherit' });
      } catch {
        say(`  FAIL  ${script} did not finish`);
        problems.push(`${script} failed`);
      }
    }
  } else {
    actions.push('regenerate the atlas and routes (gen-atlas, gen-routes)');
  }
}

// --- summary ---------------------------------------------------------------
mkdirSync(dirname(STATE), { recursive: true });
writeFileSync(STATE, JSON.stringify({ bootSha, libhlSha, when: null }, null, 2));

say('');
say('---');
if (problems.length) {
  say('NEEDS ATTENTION:');
  for (const p of problems) say(`  * ${p}`);
} else if (actions.length && !FIX) {
  say('TODO (re-run with --fix):');
  for (const a of actions) say(`  * ${a}`);
} else {
  say('all good.');
}
say('');
process.exit(problems.length ? 1 : 0);
