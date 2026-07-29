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
//   u32   headerSize          (prologue + entry tree + "DATA" marker)
//   u32   dataSize            (low 32 bits of the data section size)
//   entry root                (tree; the root has an empty name)
//   "DATA"                    (at headerSize-4)
//   ...file data...           (at headerSize + entry.dataPos)
//
//   entry := u8 nameLen, char name[nameLen], u8 flags
//     flags & 1 : directory -> u32 childCount, entry children[childCount]
//     else      : dataPos (f64 LE if flags & 2, else u32), u32 size,
//                 u32 checksum
//
// The f64 position is not a guess: hxd.fmt.pak.Data stores dataPosition as
// Float, and the writer emits it as a double once the archive passes 2^31.
// Decoding those 8 bytes as two u32s is how the first version of this tool
// extracted float soup from every large archive.
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
import { findGame } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out', 'pak');

const args = process.argv.slice(2);
const argOf = (flag) => {
  const i = args.indexOf(flag);
  return i >= 0 ? args[i + 1] : null;
};

const game = findGame(args);
const pakPath = argOf('--pak') || (game ? join(game, 'res.pak') : null);
if (!pakPath || !existsSync(pakPath)) {
  console.error('pak not found; pass --pak <file>, --game <install dir>,');
  console.error('or set FAREVER_DIR');
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

// headerSize spans prologue, tree and the trailing "DATA" marker; the tree
// itself is the middle headerSize-16 bytes.
const header = Buffer.alloc(headerSize - 16);
readSync(fd, header, 0, headerSize - 16, 12);
const dataStart = headerSize;

const files = [];   // { path, pos, size }
let p = 0;

function readEntry(prefix) {
  const nameLen = header[p++];
  const name = header.toString('utf8', p, p + nameLen);
  p += nameLen;
  const flags = header[p++];
  const full = prefix ? `${prefix}/${name}` : name;

  if (flags & 1) {
    const n = header.readUInt32LE(p);
    p += 4;
    for (let i = 0; i < n; i++) readEntry(full);
  } else {
    let pos;
    if (flags & 2) {
      pos = header.readDoubleLE(p);   // 64-bit position, stored as f64
      p += 8;
    } else {
      pos = header.readUInt32LE(p);
      p += 4;
    }
    const size = header.readUInt32LE(p);
    p += 8;   // size + checksum
    files.push({ path: full, pos, size, wide: (flags & 2) !== 0 });
  }
}

readEntry('');

{
  const marker = Buffer.alloc(4);
  readSync(fd, marker, 0, 4, headerSize - 4);
  if (marker.toString('ascii') !== 'DATA') {
    console.error(`  WARNING: no DATA marker at ${headerSize - 4} - ` +
                  `positions may be wrong`);
  }
}

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
  // `--list --get x` means "list everything", not "filter by --get".
  const rawFilter = argOf('--list');
  const filter = (rawFilter && !rawFilter.startsWith('--') ? rawFilter : '')
      .toLowerCase();
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
