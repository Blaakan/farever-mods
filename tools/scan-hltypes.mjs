#!/usr/bin/env node
// ---------------------------------------------------------------------------
// scan-hltypes.mjs
//
// Parses the TYPE table out of Farever's HashLink bytecode and computes the
// runtime memory offset of every field of every class.
//
// Why this matters: a standalone mod host has to read game state out of the
// process - the Hero's health, its active statuses, its equipped weapon. Doing
// that normally means reverse-engineering struct layouts by hand and redoing
// the work after every patch. But Farever ships interpreted bytecode, and the
// bytecode carries the complete type table: every class, its superclass, and
// every field with its type. HashLink then lays objects out deterministically
// (hl_get_obj_rt), so the offsets are *derivable* rather than discoverable.
//
// This replicates that layout algorithm, so `Hero.health` resolves to an exact
// byte offset straight from the game's own metadata - and regenerates in a
// second after a patch.
//
// Usage:
//   node tools/scan-hltypes.mjs                    summary + verification
//   node tools/scan-hltypes.mjs Hero Unit          dump those classes
//   node tools/scan-hltypes.mjs --grep status      find fields by name
//   node tools/scan-hltypes.mjs --json             write offsets to tools/out/
// ---------------------------------------------------------------------------

import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { requireBoot } from './lib/game.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out');
const WSIZE = 8; // x64

// hl_type_kind, from HashLink's hl.h
const K = {
  VOID: 0, UI8: 1, UI16: 2, I32: 3, I64: 4, F32: 5, F64: 6, BOOL: 7,
  BYTES: 8, DYN: 9, FUN: 10, OBJ: 11, ARRAY: 12, TYPE: 13, REF: 14,
  VIRTUAL: 15, DYNOBJ: 16, ABSTRACT: 17, ENUM: 18, NULL: 19, METHOD: 20,
  STRUCT: 21, PACKED: 22, GUID: 23,
};
const KIND_NAME = Object.fromEntries(Object.entries(K).map(([k, v]) => [v, k]));

// hl_type_size
function typeSize(kind) {
  switch (kind) {
    case K.VOID: return 0;
    case K.UI8: case K.BOOL: return 1;
    case K.UI16: return 2;
    case K.I32: case K.F32: return 4;
    case K.I64: case K.F64: return 8;
    default: return WSIZE; // every pointer-ish kind
  }
}

class Reader {
  constructor(buf) { this.b = buf; this.p = 0; }
  u8() { return this.b[this.p++]; }
  i32() { const v = this.b.readInt32LE(this.p); this.p += 4; return v; }
  index() {
    const b = this.b[this.p++];
    if ((b & 0x80) === 0) return b & 0x7f;
    if ((b & 0x40) === 0) {
      const v = ((b & 0x1f) << 8) | this.b[this.p++];
      return b & 0x20 ? -v : v;
    }
    const c = this.b[this.p++], d = this.b[this.p++], e = this.b[this.p++];
    const v = ((b & 0x1f) << 24) | (c << 16) | (d << 8) | e;
    return b & 0x20 ? -v : v;
  }
  strings(n) {
    const size = this.i32();
    const data = this.b.subarray(this.p, this.p + size);
    this.p += size;
    const out = new Array(n);
    let pos = 0;
    for (let i = 0; i < n; i++) {
      const len = this.index();
      out[i] = data.toString('utf8', pos, pos + len);
      pos += len + 1;
    }
    return out;
  }
}

function parse(path) {
  const r = new Reader(readFileSync(path));
  if (r.b.toString('ascii', 0, 3) !== 'HLB') throw new Error('not HashLink bytecode');
  r.p = 3;
  const version = r.u8();
  const flags = r.index();
  const hasDebug = (flags & 1) === 1;
  const nints = r.index();
  const nfloats = r.index();
  const nstrings = r.index();
  const nbytes = version >= 5 ? r.index() : 0;
  const ntypes = r.index();
  const nglobals = r.index();
  const nnatives = r.index();
  const nfunctions = r.index();
  const nconstants = version >= 4 ? r.index() : 0;
  r.index(); // entrypoint

  r.p += nints * 4;
  r.p += nfloats * 8;
  const strings = r.strings(nstrings);

  if (version >= 5) {
    const size = r.i32();
    r.p += size;
    for (let i = 0; i < nbytes; i++) r.index();
  }
  if (hasDebug) {
    const ndebug = r.index();
    r.strings(ndebug);
  }

  const str = (i) => (i >= 0 && i < strings.length ? strings[i] : `<str#${i}>`);

  // --- types ---
  const types = new Array(ntypes);
  for (let i = 0; i < ntypes; i++) {
    const kind = r.u8();
    const t = { idx: i, kind };
    switch (kind) {
      case K.FUN:
      case K.METHOD: {
        const nargs = r.u8();
        t.args = [];
        for (let a = 0; a < nargs; a++) t.args.push(r.index());
        t.ret = r.index();
        break;
      }
      case K.OBJ:
      case K.STRUCT: {
        t.name = str(r.index());
        t.super = r.index();
        t.global = r.index();
        const nfields = r.index();
        const nproto = r.index();
        const nbindings = r.index();
        t.fields = [];
        for (let f = 0; f < nfields; f++) {
          t.fields.push({ name: str(r.index()), type: r.index() });
        }
        t.protos = [];
        for (let p = 0; p < nproto; p++) {
          const name = str(r.index());
          const findex = r.index();
          const pindex = r.index();
          t.protos.push({ name, findex, pindex });
        }
        t.bindings = [];
        for (let bIdx = 0; bIdx < nbindings; bIdx++) {
          t.bindings.push([r.index(), r.index()]);
        }
        break;
      }
      case K.REF:
      case K.NULL:
      case K.PACKED:
        t.param = r.index();
        break;
      case K.VIRTUAL: {
        const nfields = r.index();
        t.fields = [];
        for (let f = 0; f < nfields; f++) {
          t.fields.push({ name: str(r.index()), type: r.index() });
        }
        break;
      }
      case K.ABSTRACT:
        t.name = str(r.index());
        break;
      case K.ENUM: {
        t.name = str(r.index());
        t.global = r.index();
        const nconstructs = r.index();
        t.constructs = [];
        for (let c = 0; c < nconstructs; c++) {
          const name = str(r.index());
          const nparams = r.index();
          const params = [];
          for (let pI = 0; pI < nparams; pI++) params.push(r.index());
          t.constructs.push({ name, params });
        }
        break;
      }
      default:
        break; // primitives carry no payload
    }
    types[i] = t;
  }

  return { version, hasDebug, strings, types };
}

// --- layout, replicating hl_get_obj_rt --------------------------------------

function pad(pos, size) {
  if (size === 0) return 0;
  const d = pos % size;
  return d === 0 ? 0 : size - d;
}

function layout(types, t, cache = new Map()) {
  if (cache.has(t.idx)) return cache.get(t.idx);

  const parent = t.super >= 0 ? types[t.super] : null;
  const parentLayout = parent && (parent.kind === K.OBJ || parent.kind === K.STRUCT)
    ? layout(types, parent, cache)
    : null;

  // A plain object begins with its hl_type* header; a struct does not.
  let start = parentLayout ? parentLayout.size : t.kind === K.STRUCT ? 0 : WSIZE;

  const fields = parentLayout ? [...parentLayout.fields] : [];
  for (const f of t.fields || []) {
    const ft = types[f.type];
    const size = typeSize(ft ? ft.kind : K.DYN);
    start += pad(start, size);
    fields.push({
      name: f.name,
      offset: start,
      size,
      kind: ft ? KIND_NAME[ft.kind] || String(ft.kind) : '?',
      typeName: ft && ft.name ? ft.name : undefined,
      inherited: false,
    });
    start += size;
  }
  if (parentLayout) {
    for (let i = 0; i < parentLayout.fields.length; i++) fields[i] = { ...fields[i], inherited: true };
  }

  const res = { name: t.name, size: start, fields, superName: parent ? parent.name : null };
  cache.set(t.idx, res);
  return res;
}

// --- main ------------------------------------------------------------------

const args = process.argv.slice(2);
const wantJson = args.includes('--json');
const grepIdx = args.indexOf('--grep');
const grep = grepIdx >= 0 ? args[grepIdx + 1] : null;
const explicit = args.find((a) => !a.startsWith('--') && a !== grep && !existsSync(a));

const bootPath = requireBoot(args);

console.log(`reading ${bootPath}`);
const bc = parse(bootPath);
const classes = bc.types.filter((t) => t.kind === K.OBJ || t.kind === K.STRUCT);
console.log(`  ${bc.types.length.toLocaleString()} types, ${classes.length.toLocaleString()} classes/structs`);

const cache = new Map();
const byName = new Map();
for (const c of classes) if (c.name && !byName.has(c.name)) byName.set(c.name, c);

function dump(name) {
  const t = byName.get(name);
  if (!t) {
    const near = [...byName.keys()].filter((k) => k.toLowerCase().includes(name.toLowerCase())).slice(0, 12);
    console.log(`\n${name}: not found${near.length ? `. did you mean: ${near.join(', ')}` : ''}`);
    return;
  }
  const l = layout(bc.types, t, cache);
  console.log(`\n${l.name}${l.superName ? `  extends ${l.superName}` : ''}   sizeof=${l.size}`);
  for (const f of l.fields) {
    console.log(
      `  +0x${f.offset.toString(16).padStart(3, '0')}  ${String(f.size).padStart(2)}  ${f.kind.padEnd(9)} ${f.name}${f.typeName ? `  : ${f.typeName}` : ''}${f.inherited ? '   (inherited)' : ''}`
    );
  }
}

// Fields the farever-minimap plugin API is documented to read. If the parser
// and the layout algorithm are right, these all resolve.
// Class names are package-qualified, matching the src/ layout (src/ent/ ->
// ent.Hero). instigatedStatuses is declared on GameObject and reaches Unit by
// inheritance, which also exercises the parent-layout path.
const EXPECTED = [
  ['ent.Hero', 'lockedTarget'], ['ent.Hero', 'autoTarget'],
  ['ent.Hero', 'weaponInHand'], ['ent.Hero', 'loadout'],
  ['ent.Unit', 'isInCombat'], ['ent.Unit', 'instigatedStatuses'],
];

if (!explicit && !grep) {
  console.log('\n=== verification: fields the plugin API is documented to read ===');
  let okCount = 0;
  for (const [cls, field] of EXPECTED) {
    const t = byName.get(cls);
    if (!t) { console.log(`  MISS  ${cls}.${field}   (class not found)`); continue; }
    const l = layout(bc.types, t, cache);
    const f = l.fields.find((x) => x.name === field);
    if (f) {
      okCount++;
      console.log(`  ok    ${cls}.${field}`.padEnd(34) + `+0x${f.offset.toString(16)}  ${f.kind}`);
    } else {
      console.log(`  MISS  ${cls}.${field}`);
    }
  }
  console.log(`  ${okCount}/${EXPECTED.length} resolved`);

  for (const n of ['Hero', 'Unit']) {
    const t = byName.get(n);
    if (t) {
      const l = layout(bc.types, t, cache);
      console.log(`\n${n}: ${l.fields.length} fields, sizeof=${l.size}`);
    }
  }
  console.log('\nrun with a class name to dump it, e.g.  node tools/scan-hltypes.mjs Hero');
}

for (const a of args) {
  if (!a.startsWith('--') && a !== grep && !a.endsWith('.dat')) dump(a);
}

if (grep) {
  console.log(`\n=== fields matching /${grep}/i ===`);
  const re = new RegExp(grep, 'i');
  let n = 0;
  for (const c of classes) {
    if (!c.name || !c.fields?.length) continue;
    const l = layout(bc.types, c, cache);
    for (const f of l.fields) {
      if (!f.inherited && re.test(f.name)) {
        console.log(`  ${c.name}.${f.name}`.padEnd(52) + `+0x${f.offset.toString(16)}  ${f.kind}`);
        if (++n >= 60) { console.log('  ...'); break; }
      }
    }
    if (n >= 60) break;
  }
}

if (wantJson) {
  mkdirSync(OUT, { recursive: true });
  // Which bytecode this dump describes. Without it a dump left over from the
  // build before a patch is indistinguishable from a fresh one, and offsets
  // generated from it would be wrong in the one way that matters - plausible.
  // `__source` cannot collide with a class name: Haxe has no such type.
  const out = {
    __source: {
      file: bootPath,
      sha256: createHash('sha256').update(readFileSync(bootPath)).digest('hex'),
    },
  };
  for (const [name, t] of byName) {
    const l = layout(bc.types, t, cache);
    out[name] = { size: l.size, super: l.superName, fields: l.fields };
  }
  writeFileSync(join(OUT, 'hl_types.json'), JSON.stringify(out), 'utf8');
  console.log(`\nwrote ${join(OUT, 'hl_types.json')} (${byName.size} classes)`);
}
