#!/usr/bin/env node
// ---------------------------------------------------------------------------
// package.mjs - build the archive people actually download.
//
//   node tools/package.mjs           package host/build/dxgi.dll as it is
//   node tools/package.mjs --build   build the DLL first
//
// What goes in, and what deliberately does not:
//
//   dxgi.dll          the whole mod, one file
//   install.cmd       the front door, for someone who has never used a
//   uninstall.cmd     command prompt
//   tools/            only what installing needs: the two generators, the
//                     libraries they read the game's archives with, and the
//                     installer itself
//   build-info.json   which game build this DLL's offsets were compiled
//                     against, so the installer can refuse to install into a
//                     game it cannot read
//   README.txt        install instructions that survive being unzipped on
//                     their own, with the antivirus note, because a
//                     downloaded dxgi.dll will be asked about
//
// NOT in: anything generated from the game. The item names, icons, loot
// tables and level data all belong to Shiro Games. The generators ship, the
// haul does not - which is also why installing needs Node and a copy of the
// game rather than being a drag and drop.
// ---------------------------------------------------------------------------

import { existsSync, readFileSync, writeFileSync, copyFileSync, mkdirSync,
         rmSync, statSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const DIST = join(ROOT, 'dist');

const sha = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');
const read = (p) => readFileSync(p, 'utf8');

function need(path, why) {
  if (existsSync(path)) return path;
  console.error(`missing: ${path}`);
  console.error(`         ${why}`);
  process.exit(1);
}

// --- what we are shipping ---------------------------------------------------

const versionH = need(join(ROOT, 'host', 'src', 'version.h'), 'the version lives here');
const version = (read(versionH).match(/#define FMK_VERSION "([^"]+)"/) || [])[1];
if (!version) {
  console.error('could not read FMK_VERSION from host/src/version.h');
  process.exit(1);
}

if (process.argv.includes('--build')) {
  console.log('building host...');
  execFileSync('cmd', ['/c', join(ROOT, 'host', 'build.cmd')], { stdio: 'inherit' });
}

const dll = need(join(ROOT, 'host', 'build', 'dxgi.dll'),
                 'run host/build.cmd first, or pass --build');
const offsets = need(join(ROOT, 'host', 'src', 'offsets.gen.h'),
                     'run node tools/gen-offsets.mjs first');
const gameSha = (read(offsets).match(/#define FMK_BUILD_SHA256 "([0-9a-f]+)"/) || [])[1];
if (!gameSha) {
  console.error('offsets.gen.h carries no build hash - regenerate it');
  process.exit(1);
}

// A DLL built from different offsets than the ones we are about to advertise
// is the one mistake that would ship silently: `build-info.json` would name a
// game build the binary cannot actually read, and the installer - which
// trusts that file - would happily install it into the game it does not work
// with. Asked of the binary rather than of timestamps, because the header is
// rewritten by every generator run and a build is not stale for being older
// than a file whose contents did not change. `FMK_BUILD_SHA256` is a string
// literal in dllmain.cpp, so the DLL carries it verbatim.
if (!readFileSync(dll).includes(Buffer.from(gameSha, 'ascii'))) {
  console.error('dxgi.dll was not built from the current offsets.gen.h.');
  console.error(`  offsets.gen.h says  ${gameSha.slice(0, 24)}...`);
  console.error('  and the DLL does not contain that hash.');
  console.error('');
  console.error('  Rebuild before packaging:  host\\build.cmd');
  process.exit(1);
}

const name = `farever-modkit-${version}`;
const stage = join(DIST, name);
rmSync(stage, { recursive: true, force: true });
mkdirSync(join(stage, 'tools', 'lib'), { recursive: true });

// The installer needs exactly these. Listing them rather than copying the
// whole tools folder keeps the scanners, the Lua harness and the research
// scripts out of a player's download.
const TOOLS = ['install.mjs', 'gen-atlas.mjs', 'gen-routes.mjs',
               'atlas-overrides.tsv'];
const LIB = ['game.mjs', 'pak.mjs', 'hbson.mjs'];

copyFileSync(dll, join(stage, 'dxgi.dll'));
for (const f of ['install.cmd', 'uninstall.cmd', 'LICENSE'])
  copyFileSync(join(ROOT, f), join(stage, f));
for (const f of TOOLS) copyFileSync(join(HERE, f), join(stage, 'tools', f));
for (const f of LIB) copyFileSync(join(HERE, 'lib', f), join(stage, 'tools', 'lib', f));

writeFileSync(join(stage, 'build-info.json'), JSON.stringify({
  version,
  // The game build these offsets - and therefore this DLL - were compiled
  // against. The installer compares it against the player's hlboot.dat.
  gameSha256: gameSha,
  dllSha256: sha(dll),
}, null, 2) + '\n');

writeFileSync(join(stage, 'README.txt'), readme(version, gameSha));

// --- zip --------------------------------------------------------------------
//
// Compress-Archive rather than a dependency: it is on every Windows machine
// that can build this in the first place.
const zip = join(DIST, `${name}.zip`);
rmSync(zip, { force: true });
execFileSync('powershell', ['-NoProfile', '-NonInteractive', '-Command',
  `Compress-Archive -Path '${stage}\\*' -DestinationPath '${zip}' -Force`],
  { stdio: 'inherit' });

const zipSha = sha(zip);
writeFileSync(join(DIST, `${name}.zip.sha256`), `${zipSha}  ${name}.zip\n`);

console.log('');
console.log(`packaged  ${zip}`);
console.log(`  version     ${version}`);
console.log(`  game build  ${gameSha.slice(0, 16)}...`);
console.log(`  sha256      ${zipSha}`);
console.log(`  size        ${(statSync(zip).size / 1024).toFixed(0)} KB`);

function readme(version, gameSha) {
  return [
    `farever-modkit ${version} - the Collection Atlas for Farever`,
    '',
    'WHAT THIS IS',
    '  A completion tracker that runs as its own overlay: every collectible',
    '  in the game, what you own, and where to get the rest, with a waypoint',
    '  arrow that follows a route. Press F8 in game.',
    '',
    '  It is read-only. It never writes to the game, automates play, or',
    '  touches the network.',
    '',
    'INSTALL',
    '  1. Install Node.js if you do not have it:  https://nodejs.org/  (LTS)',
    '     It is needed because the item database, icons and routes are built',
    '     from YOUR copy of the game - that data belongs to Shiro Games and',
    '     is not ours to put in a download.',
    '  2. Close Farever.',
    '  3. Run install.cmd.',
    '',
    '  It finds the game through Steam, builds the data files, and copies one',
    '  dxgi.dll next to Farever.exe. If it cannot find the game:',
    '',
    '      install.cmd --game "D:\\SteamLibrary\\steamapps\\common\\Farever"',
    '',
    'UNINSTALL',
    '  Run uninstall.cmd, or delete that one dxgi.dll. There is no installer,',
    '  no service and no registry key - the mod is one file.',
    '',
    'IF WINDOWS OR YOUR ANTIVIRUS COMPLAINS',
    '  It will, and the reason is honest: this is an unsigned DLL, downloaded',
    '  from the internet, that loads into a game. That is also the shape of',
    '  things that are not honest, and scanners cannot tell the difference.',
    '',
    '  SmartScreen: "More info" then "Run anyway", or right-click the ZIP ->',
    '  Properties -> Unblock BEFORE extracting it.',
    '',
    '  If you would rather not take our word for it, the whole thing is source',
    '  and builds in one command:',
    '      https://github.com/Blaakan/farever-mods',
    '',
    'GAME UPDATES',
    `  This build reads game bytecode ${gameSha.slice(0, 16)}...`,
    '  The field offsets it walks are compiled in, so when Farever updates,',
    '  this DLL stops reading rather than reading the wrong thing - the atlas',
    '  goes empty and farever-modkit.log says so. Get a newer release, or',
    '  rebuild from source.',
    '',
    'IS THIS ALLOWED',
    '  Shiro Games, on their official Discord: they will not promote add-ons',
    '  during Early Access, but will not condemn personal use of ones like',
    '  minimaps or DPS meters. Tolerated for personal use, not endorsed.',
    '  Read the risk section in the repository before installing.',
    '',
    'LICENCE',
    '  MIT - see LICENSE. Not affiliated with Shiro Games.',
    '  Farever is (c) Shiro Games.',
    '',
  ].join('\r\n');
}
