#!/usr/bin/env node
// ---------------------------------------------------------------------------
// install.mjs - put the mod in the game, on somebody else's machine.
//
//   node tools/install.mjs              install
//   node tools/install.mjs --uninstall  take it back out
//   node tools/install.mjs --game <dir> point at the install yourself
//   node tools/install.mjs --no-data    just the DLL, keep the data files
//   node tools/install.mjs --purge      (with --uninstall) also remove the
//                                       generated data and your saved routes
//
// It runs from two different places and has to work the same in both:
//
//   a source checkout   the DLL is host/build/dxgi.dll and the offsets it
//                       was built against are in host/src/offsets.gen.h
//   an unpacked release the DLL sits next to this script's parent, and
//                       build-info.json records what it was built against
//
// The one thing it will not do is install a DLL built for a different game
// build. The host refuses to read memory in that case anyway - so it would
// not break anything - but it would sit there doing nothing while looking
// installed, and "installed and silent" is the worst outcome available.
// ---------------------------------------------------------------------------

import { existsSync, readFileSync, copyFileSync, unlinkSync, openSync,
         closeSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { requireGame } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const args = process.argv.slice(2);
const has = (f) => args.includes(f);

const UNINSTALL = has('--uninstall');
const PURGE = has('--purge');
const NO_DATA = has('--no-data');
const FORCE = has('--force');

const sha = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');
const say = (s = '') => console.log(s);

// Files this mod puts next to the game. Named here so uninstalling can be
// complete and honest about what it leaves behind.
const GENERATED = ['farever-atlas.tsv', 'farever-atlas-icons.dds',
                   'farever-routes.txt'];
const RUNTIME = ['farever-modkit.log', 'farever-modkit.ini',
                 'farever-nav-state.txt', 'farever-collection.json'];
const YOURS = ['farever-routes-custom.txt'];

// --- where the pieces are ---------------------------------------------------

// A checkout builds into host/build; a release unpacks the DLL beside the
// tools folder. Whichever exists is the one meant.
function findDll() {
  for (const p of [join(ROOT, 'host', 'build', 'dxgi.dll'),
                   join(ROOT, 'dxgi.dll')]) {
    if (existsSync(p)) return p;
  }
  return null;
}

// What game build this DLL's compiled-in offsets came from. In a release
// that is recorded beside it; in a checkout the header is the record, and
// reading it there means the answer cannot drift from what was compiled.
function buildInfo() {
  const info = join(ROOT, 'build-info.json');
  if (existsSync(info)) {
    try {
      return JSON.parse(readFileSync(info, 'utf8'));
    } catch {
      // Fall through to the header.
    }
  }
  const header = join(ROOT, 'host', 'src', 'offsets.gen.h');
  const version = join(ROOT, 'host', 'src', 'version.h');
  const out = {};
  if (existsSync(header)) {
    const m = readFileSync(header, 'utf8')
      .match(/#define FMK_BUILD_SHA256 "([0-9a-f]+)"/);
    if (m) out.gameSha256 = m[1];
  }
  if (existsSync(version)) {
    const m = readFileSync(version, 'utf8').match(/#define FMK_VERSION "([^"]+)"/);
    if (m) out.version = m[1];
  }
  out.source = true;
  return out;
}

// Whether a dxgi.dll already sitting in the game folder is one of ours. The
// host logs its own name on attach, so the string is in the binary - and
// overwriting somebody else's proxy (or, worse, a real dxgi.dll a user
// copied there) without asking is not ours to do.
function isOurs(path) {
  try {
    return readFileSync(path).includes(Buffer.from('farever-modkit'));
  } catch {
    return false;
  }
}

// Whether the DLL can be replaced at all, asked before anything slow happens.
// Windows locks a loaded DLL, so reinstalling over a running game fails - and
// finding that out after two minutes of generating the item database, with
// the data files already overwritten, is a bad way to learn it. An absent
// file is fine: that is a first install.
function dllIsWritable(target) {
  if (!existsSync(target)) return true;
  try {
    closeSync(openSync(target, 'r+'));
    return true;
  } catch {
    return false;
  }
}

// --- uninstall --------------------------------------------------------------

function uninstall(game) {
  const target = join(game, 'dxgi.dll');
  if (!existsSync(target)) {
    say('dxgi.dll  not installed');
  } else if (!isOurs(target) && !FORCE) {
    say('dxgi.dll  present but NOT ours - left alone.');
    say('          Another mod may own it. Delete it yourself, or re-run');
    say('          with --force if you are sure.');
  } else {
    try {
      unlinkSync(target);
      say('dxgi.dll  removed');
    } catch (e) {
      say(`dxgi.dll  could NOT be removed: ${e.message}`);
      say('          Close the game and try again.');
      return 1;
    }
  }

  const removed = [];
  const kept = [];
  for (const f of [...GENERATED, ...RUNTIME, ...YOURS]) {
    const p = join(game, f);
    if (!existsSync(p)) continue;
    const mine = YOURS.includes(f);
    if (!PURGE) {
      kept.push(f);
      continue;
    }
    if (mine && !FORCE) {
      // Routes somebody recorded by walking them are not "generated data".
      kept.push(f + '  (yours - use --purge --force to remove)');
      continue;
    }
    try {
      unlinkSync(p);
      removed.push(f);
    } catch {
      kept.push(f + '  (in use?)');
    }
  }
  // Per-character files are named after characters, so they are matched
  // rather than listed.
  say('');
  if (removed.length) say(`removed: ${removed.join(', ')}`);
  if (kept.length) {
    say('left in place:');
    for (const k of kept) say(`  ${k}`);
    if (!PURGE) say('  (re-run with --purge to remove the generated data too)');
  }
  say('');
  say('The game is back to stock - the proxy was one file, with no');
  say('installer and no registry writes.');
  return 0;
}

// --- install ----------------------------------------------------------------

function install(game) {
  const dll = findDll();
  if (!dll) {
    say('FAIL  no dxgi.dll to install.');
    say('');
    say('      In a source checkout, build it first:');
    say('        host\\build.cmd');
    say('      In a downloaded release, dxgi.dll should sit next to this');
    say('      installer - if it does not, the archive was unpacked oddly.');
    return 1;
  }

  const info = buildInfo();
  say(`version ${info.version || 'unknown'}`);
  say(`dll     ${dll}`);

  // --- the one check worth stopping for ---
  const boot = join(game, 'hlboot.dat');
  const bootSha = sha(boot);
  say(`game    ${game}`);
  say(`build   ${bootSha.slice(0, 16)}...`);

  if (info.gameSha256 && info.gameSha256 !== bootSha) {
    say('');
    say('STOP  this build of the mod does not match your game.');
    say('');
    say(`      the DLL was built for  ${info.gameSha256.slice(0, 24)}...`);
    say(`      your game is           ${bootSha.slice(0, 24)}...`);
    say('');
    say('      The field offsets the reader uses are compiled into the DLL,');
    say('      so a patched game needs a DLL built against it. Installing');
    say('      this one is safe but pointless: the host would notice the');
    say('      mismatch and disable every memory read, and the atlas would');
    say('      sit there permanently empty.');
    say('');
    if (info.source) {
      say('      You have the source, so this is one command:');
      say('        node tools/update.mjs --fix');
      say('      then run this installer again.');
    } else {
      say('      Check for a release built for the current game version:');
      say('        https://github.com/Blaakan/farever-mods/releases');
      say('      or build from source - see the README.');
    }
    say('');
    say('      --force installs it anyway.');
    if (!FORCE) return 1;
    say('');
    say('      --force given; installing a DLL that will do nothing.');
  }

  // --- can we finish, before we start? ---
  const target = join(game, 'dxgi.dll');
  if (!dllIsWritable(target)) {
    say('');
    say('STOP  the game is running.');
    say('');
    say(`      ${target} is locked, which on Windows means it is loaded into`);
    say('      a running process. Close Farever and run this again.');
    say('');
    say('      Nothing has been changed.');
    return 1;
  }
  if (existsSync(target) && !isOurs(target) && !FORCE) {
    say('');
    say('STOP  there is already a dxgi.dll next to Farever.exe, and it is');
    say('      not one of ours. Overwriting it could break another mod, or');
    say('      lose a file you put there deliberately.');
    say('');
    say(`      ${target}`);
    say('');
    say('      Move it aside, or re-run with --force.');
    say('');
    say('      Nothing has been changed.');
    return 1;
  }

  // --- data files ---
  if (!NO_DATA) {
    for (const [script, what] of [['gen-atlas.mjs', 'the item database and icon atlas'],
                                  ['gen-routes.mjs', 'the generated route set']]) {
      const p = join(HERE, script);
      if (!existsSync(p)) {
        say(`skip    ${script} is not in this package`);
        continue;
      }
      say('');
      say(`building ${what} from your own game files...`);
      try {
        execFileSync(process.execPath, [p, '--game', game], { stdio: 'inherit' });
      } catch {
        say('');
        say(`FAIL  ${script} did not finish.`);
        say('      Nothing has been installed. The mod needs these files:');
        say('      they are built from your copy of the game because the');
        say('      game data is not ours to redistribute.');
        return 1;
      }
    }
  }

  // --- the DLL, last, because it is the part that changes what loads ---
  try {
    copyFileSync(dll, target);
  } catch (e) {
    say('');
    say(`FAIL  could not write ${target}`);
    say(`      ${e.message}`);
    say('');
    say('      If the game is running, close it - Windows holds a loaded');
    say('      DLL open. If the game lives under Program Files, run this');
    say('      from an administrator command prompt.');
    return 1;
  }

  say('');
  say(`installed ${target}`);
  say('');
  say('Launch the game and press F8.');
  say('');
  say('  * the atlas takes about twenty seconds after launch to find its');
  say('    way around, and fills in once a character is in the world');
  say('  * farever-modkit.log, next to the game, narrates what it found -');
  say('    if something looks wrong, that file usually names the reason');
  say('  * to remove it: node tools/install.mjs --uninstall, or just');
  say('    delete that one dxgi.dll');
  say('');
  return 0;
}

// --- go ---------------------------------------------------------------------

say('');

// The generators use `??`, optional chaining and `matchAll` freely, and a
// Node old enough to lack them fails with a SyntaxError halfway through
// rather than saying what is wrong.
const major = Number(process.versions.node.split('.')[0]);
if (major < 18) {
  say(`Node ${process.versions.node} is too old - 18 or newer is needed.`);
  say('  winget install OpenJS.NodeJS.LTS');
  say('  or https://nodejs.org/');
  say('');
  process.exit(1);
}

const game = requireGame(args);
if (!existsSync(join(game, 'hlboot.dat'))) {
  say(`FAIL  ${game} has no hlboot.dat - that is not a Farever install.`);
  process.exit(1);
}

process.exit(UNINSTALL ? uninstall(game) : install(game));
