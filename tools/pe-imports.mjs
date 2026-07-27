#!/usr/bin/env node
// ---------------------------------------------------------------------------
// pe-imports.mjs
//
// Lists the DLLs a PE binary imports, and its exports.
//
// Used to pick a viable proxy-DLL name for the modkit host: a proxy only ever
// loads if the game actually imports that DLL by name from its own directory.
// Guessing here wastes a whole build cycle, so we read the import table.
//
// Usage:  node tools/pe-imports.mjs <file.exe|file.dll> [...]
// ---------------------------------------------------------------------------

import { readFileSync } from 'node:fs';
import { basename } from 'node:path';

function parsePE(path) {
  const b = readFileSync(path);
  if (b.readUInt16LE(0) !== 0x5a4d) throw new Error('not a PE (no MZ)');
  const peOff = b.readUInt32LE(0x3c);
  if (b.readUInt32LE(peOff) !== 0x00004550) throw new Error('not a PE (no PE\\0\\0)');

  const coff = peOff + 4;
  const nSections = b.readUInt16LE(coff + 2);
  const optSize = b.readUInt16LE(coff + 16);
  const opt = coff + 20;
  const magic = b.readUInt16LE(opt);
  const pe32plus = magic === 0x20b;

  // Data directories: import table is index 1, export table index 0.
  const ddOff = opt + (pe32plus ? 112 : 96);
  const impRva = b.readUInt32LE(ddOff + 8 * 1);
  const expRva = b.readUInt32LE(ddOff + 8 * 0);

  // Section headers, for RVA -> file offset.
  const secOff = opt + optSize;
  const sections = [];
  for (let i = 0; i < nSections; i++) {
    const s = secOff + i * 40;
    sections.push({
      name: b.toString('ascii', s, s + 8).replace(/\0+$/, ''),
      vaddr: b.readUInt32LE(s + 12),
      vsize: b.readUInt32LE(s + 8),
      raw: b.readUInt32LE(s + 20),
      rawSize: b.readUInt32LE(s + 16),
    });
  }

  const toOff = (rva) => {
    for (const s of sections) {
      if (rva >= s.vaddr && rva < s.vaddr + Math.max(s.vsize, s.rawSize)) {
        return s.raw + (rva - s.vaddr);
      }
    }
    return -1;
  };

  const cstr = (off) => {
    if (off < 0 || off >= b.length) return '';
    let e = off;
    while (e < b.length && b[e] !== 0) e++;
    return b.toString('ascii', off, e);
  };

  // --- imports (with per-function names from the import name table) ---
  const imports = [];
  if (impRva) {
    let p = toOff(impRva);
    while (p > 0 && p + 20 <= b.length) {
      const intRva = b.readUInt32LE(p); // OriginalFirstThunk
      const nameRva = b.readUInt32LE(p + 12);
      if (nameRva === 0) break;
      const name = cstr(toOff(nameRva));
      if (!name) break;

      const funcs = [];
      let thunk = toOff(intRva || b.readUInt32LE(p + 16)); // fall back to IAT
      while (thunk > 0 && thunk + 8 <= b.length) {
        const lo = b.readUInt32LE(thunk);
        const hi = b.readUInt32LE(thunk + 4);
        if (lo === 0 && hi === 0) break;
        if (hi & 0x80000000) {
          funcs.push(`#${lo & 0xffff}`); // import by ordinal
        } else {
          const off = toOff(lo);
          if (off > 0) funcs.push(cstr(off + 2)); // skip hint word
        }
        thunk += 8;
      }
      imports.push({ name, funcs });
      p += 20;
    }
  }

  // --- exports ---
  const exports = [];
  if (expRva) {
    const p = toOff(expRva);
    if (p > 0) {
      const nNames = b.readUInt32LE(p + 24);
      const namesRva = b.readUInt32LE(p + 32);
      const np = toOff(namesRva);
      for (let i = 0; i < nNames && np > 0; i++) {
        exports.push(cstr(toOff(b.readUInt32LE(np + i * 4))));
      }
    }
  }

  return { pe32plus, imports, exports };
}

const files = process.argv.slice(2);
if (!files.length) {
  console.error('usage: node tools/pe-imports.mjs <file> [...]');
  process.exit(1);
}

for (const f of files) {
  try {
    const { pe32plus, imports, exports } = parsePE(f);
    console.log(`\n${basename(f)}  (${pe32plus ? 'x64' : 'x86'})`);
    console.log(`  imports (${imports.length} modules):`);
    for (const i of imports.sort((a, b) => a.name.localeCompare(b.name))) {
      console.log(`      ${i.name}`);
      if (process.argv.includes('--funcs')) {
        for (const fn of i.funcs) console.log(`          ${fn}`);
      }
    }
    if (exports.length) {
      console.log(`  exports (${exports.length}):`);
      const show = exports.slice(0, 24);
      for (const e of show) console.log(`      ${e}`);
      if (exports.length > show.length) console.log(`      ... ${exports.length - show.length} more`);
    }
  } catch (e) {
    console.log(`\n${basename(f)}  ERROR: ${e.message}`);
  }
}
