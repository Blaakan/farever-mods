#!/usr/bin/env node
// ---------------------------------------------------------------------------
// pak-extract.mjs
//
// Reads Shiro/Heaps .pak archives - the format Farever ships its assets in -
// and extracts individual files.
//
// Format, reversed from the bytes of res.pak:
//
//   "PAK" u8:version
//   u32   headerSize          (size of the entry tree that follows)
//   u32   unknown             (not needed to locate files)
//   entry root                (tree; the root has an empty name)
//   ...file data...           (at dataStart + entry.dataPos)
//
//   entry := u8 nameLen, char name[nameLen], u8 isDir,
//            isDir ? u32 childCount, entry children[childCount]
//                  : u32 dataPos, u32 size, u32 checksum
//
// Only the header is read to list or locate; file bodies are pulled with a
// targeted seek, so a 4.8 GB archive costs a ~680 KB read to browse.
//
// Reads your own installed game files locally. Extracted assets are game data
// and stay in tools/out/ (gitignored) - they are not ours to redistribute.
//
// Usage:
//   node tools/pak-extract.mjs --list [substring]     list matching entries
//   node tools/pak-extract.mjs --get <path> [...]     extract to tools/out/pak/
//   node tools/pak-extract.mjs --pak <file>           choose the archive
// ---------------------------------------------------------------------------

import { openSync, readSync, closeSync, existsSync, mkdirSync, writeFileSync,
         statSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out', 'pak');

const GAME_CANDIDATES = [
  'C:/Program Files (x86)/Steam/steamapps/common/Farever',
  'D:/SteamLibrary/steamapps/common/Farever',
  'E:/SteamLibrary/steamapps/common/Farever',
  'F:/SteamLibrary/steamapps/common/Farever',
];

const args = process.argv.slice(2);
const argOf = (flag) => {
  const i = args.indexOf(flag);
  return i >= 0 ? args[i + 1] : null;
};

function findGame() {
  if (process.env.FAREVER_DIR && existsSync(process.env.FAREVER_DIR))
    return process.env.FAREVER_DIR;
  for (const c of GAME_CANDIDATES) if (existsSync(join(c, 'hlboot.dat'))) return c;
  return null;
}

const game = findGame();
const pakPath = argOf('--pak') || (game ? join(game, 'res.pak') : null);
if (!pakPath || !existsSync(pakPath)) {
  console.error('pak not found; pass --pak <file> or set FAREVER_DIR');
  process.exit(1);
}

// --- header parse -----------------------------------------------------------

const fd = openSync(pakPath, 'r');
const head8 = Buffer.alloc(12);
readSync(fd, head8, 0, 12, 0);
if (head8.toString('ascii', 0, 3) !== 'PAK') {
  console.error('not a PAK archive');
  process.exit(1);
}
const version = head8[3];
const headerSize = head8.readUInt32LE(4);

const header = Buffer.alloc(headerSize);
readSync(fd, header, 0, headerSize, 12);

// Data begins immediately after the 12-byte prologue plus the entry tree.
const dataStart = 12 + headerSize;

const files = [];   // { path, pos, size }
let p = 0;

// The third byte is a FLAG, not a boolean:
//   0 = file, 12 bytes follow (pos, size, checksum)
//   1 = directory, u32 child count then that many entries
//   2 = file, 16 bytes follow - appears once the archive grows past 2^31,
//       so it carries a wider position. The exact field order is still
//       being pinned down; sizes decode correctly, positions do not yet.
function readEntry(prefix) {
  const nameLen = header[p++];
  const name = header.toString('utf8', p, p + nameLen);
  p += nameLen;
  const flag = header[p++];
  const full = prefix ? `${prefix}/${name}` : name;

  if (flag === 1) {
    const n = header.readUInt32LE(p);
    p += 4;
    for (let i = 0; i < n; i++) readEntry(full);
  } else if (flag === 0) {
    const pos = header.readUInt32LE(p);
    const size = header.readUInt32LE(p + 4);
    p += 12;
    files.push({ path: full, pos, size, wide: false });
  } else if (flag === 2) {
    const w = [header.readUInt32LE(p), header.readUInt32LE(p + 4),
               header.readUInt32LE(p + 8), header.readUInt32LE(p + 12)];
    p += 16;
    files.push({ path: full, pos: w[1], size: w[2], wide: true, words: w });
  } else {
    throw new Error(`unknown entry flag ${flag} at ${p - 1} (${full})`);
  }
}

readEntry('');

const pakSize = statSync(pakPath).size;
console.log(`${basename(pakPath)}  v${version}  ${(pakSize / 1e9).toFixed(2)} GB`);
console.log(`  header ${headerSize.toLocaleString()} bytes, dataStart ${dataStart.toLocaleString()}`);
console.log(`  ${files.length.toLocaleString()} files`);

// Sanity: the first entry's body should land inside the archive.
if (files.length) {
  const f0 = files[0];
  const end = dataStart + f0.pos + f0.size;
  if (end > pakSize) {
    console.error(`  WARNING: first entry runs past EOF (${end} > ${pakSize})`);
    console.error('  the dataStart assumption is wrong');
  }
}

// --- commands ---------------------------------------------------------------

function extract(entry) {
  const buf = Buffer.alloc(entry.size);
  readSync(fd, buf, 0, entry.size, dataStart + entry.pos);
  const dest = join(OUT, entry.path);
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, buf);
  const sig = buf.toString('ascii', 0, 4).replace(/[^\x20-\x7e]/g, '.');
  console.log(`  extracted ${entry.path}  ${entry.size.toLocaleString()} bytes  sig="${sig}"`);
  return dest;
}

if (args.includes('--list')) {
  const filter = (argOf('--list') || '').toLowerCase();
  const hits = filter
    ? files.filter((f) => f.path.toLowerCase().includes(filter))
    : files;
  console.log(`\n${hits.length} match(es)${filter ? ` for "${filter}"` : ''}:`);
  for (const f of hits.slice(0, 200)) {
    console.log(`  ${String(f.size).padStart(10)}  ${f.path}`);
  }
  if (hits.length > 200) console.log(`  ... ${hits.length - 200} more`);
}

const wanted = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--get') {
    for (let j = i + 1; j < args.length && !args[j].startsWith('--'); j++)
      wanted.push(args[j]);
  }
}
if (wanted.length) {
  mkdirSync(OUT, { recursive: true });
  console.log('');
  for (const w of wanted) {
    const e = files.find((f) => f.path === w) ||
              files.find((f) => f.path.toLowerCase().endsWith(w.toLowerCase()));
    if (!e) {
      console.log(`  MISSING ${w}`);
      continue;
    }
    extract(e);
  }
}

closeSync(fd);
