#!/usr/bin/env node
// ---------------------------------------------------------------------------
// dis-hlcode.mjs
//
// Disassembles a named function out of Farever's HashLink bytecode.
//
// Why this exists: scan-hltypes.mjs says where a field lives and
// scan-hlboot.mjs says which strings exist, but neither says what the game
// *does*. The chat mod needed that answer before a line of it could be
// written. A chat command surface is only safe if the game discards an
// unknown command by itself - the host reads and never writes, so it has no
// way to cancel a send once the game has decided to make one.
//
// This tool settled it. ui.hud.ChatBox.processMessage trims the typed line,
// splits it on spaces, and matches the first token against the only four
// commands it knows - "!group", "!map", "!say" and "!to". Its default case, at
// src/ui/hud/ChatBox.hx:165, builds an "Unknown chat command " string, calls
// chatError() and returns at hx:166. The single call to
// st.player.ChatClient.sendMessage sits at hx:169, past that return, and is
// not reached on that path. So an unrecognised !command is echoed into the
// local history and never leaves the machine - which is what makes reading
// chat a read-only command surface rather than an automation channel. Run it
// and read the jumps rather than trusting this paragraph.
//
// hlboot.dat ships full debug info, so every instruction carries its real
// source file and line. That is the only reason a claim like the one above
// can be verified at all.
//
// Usage:
//   node tools/dis-hlcode.mjs ChatBox.processMessage     dump by name
//   node tools/dis-hlcode.mjs --findex 12236             dump by index
//   node tools/dis-hlcode.mjs --grep 'ChatClient\.'      list matching names
//   node tools/dis-hlcode.mjs --stats                    header summary
//   node tools/dis-hlcode.mjs --game <dir> <name>        a specific install
//
// A bare name matches on the tail, so `ChatBox.processMessage` finds
// `ui.hud.ChatBox.processMessage`. Exits non-zero when the game cannot be
// found, or when nothing matched what was asked for.
// ---------------------------------------------------------------------------

import { readFileSync, existsSync } from 'node:fs';
import { requireBoot } from './lib/game.mjs';

// hl_type_kind, from HashLink's hl.h
const K = {
  VOID: 0, UI8: 1, UI16: 2, I32: 3, I64: 4, F32: 5, F64: 6, BOOL: 7,
  BYTES: 8, DYN: 9, FUN: 10, OBJ: 11, ARRAY: 12, TYPE: 13, REF: 14,
  VIRTUAL: 15, DYNOBJ: 16, ABSTRACT: 17, ENUM: 18, NULL: 19, METHOD: 20,
  STRUCT: 21, PACKED: 22, GUID: 23,
};
const KIND_NAME = Object.fromEntries(
  Object.entries(K).map(([k, v]) => [v, k])
);

// The name and argument count of every opcode, in opcode order, matching
// hl_op_nargs in HashLink's src/opcodes.h. -1 means the operand list is
// variadic and is decoded by hand below.
//
// Opcode 101 is not in that table: stock HashLink stops at Asm = 100 and uses
// 101 as its end marker. Farever's bytecode contains it anyway, so it is
// either a newer or a patched runtime. Its argument count is not documented
// anywhere we can read, and was established by brute force: of the possible
// counts, exactly one - a single operand - lets the whole of hlboot.dat parse
// to its last byte. That is evidence, not knowledge, so the disassembler says
// so out loud the first time it meets one (see UNKNOWN_OPS below) rather than
// quietly pretending to understand it.
const UNKNOWN_OP = 101;
const OPS = [
  ['Mov', 2], ['Int', 2], ['Float', 2], ['Bool', 2], ['Bytes', 2],
  ['String', 2], ['Null', 1],
  ['Add', 3], ['Sub', 3], ['Mul', 3], ['SDiv', 3], ['UDiv', 3],
  ['SMod', 3], ['UMod', 3],
  ['Shl', 3], ['SShr', 3], ['UShr', 3], ['And', 3], ['Or', 3], ['Xor', 3],
  ['Neg', 2], ['Not', 2], ['Incr', 1], ['Decr', 1],
  ['Call0', 2], ['Call1', 3], ['Call2', 4], ['Call3', 5], ['Call4', 6],
  ['CallN', -1], ['CallMethod', -1], ['CallThis', -1], ['CallClosure', -1],
  ['StaticClosure', 2], ['InstanceClosure', 3], ['VirtualClosure', 3],
  ['GetGlobal', 2], ['SetGlobal', 2], ['Field', 3], ['SetField', 3],
  ['GetThis', 2], ['SetThis', 2],
  ['DynGet', 3], ['DynSet', 3],
  ['JTrue', 2], ['JFalse', 2], ['JNull', 2], ['JNotNull', 2],
  ['JSLt', 3], ['JSGte', 3], ['JSGt', 3], ['JSLte', 3], ['JULt', 3],
  ['JUGte', 3], ['JNotLt', 3], ['JNotGte', 3],
  ['JEq', 3], ['JNotEq', 3], ['JAlways', 1],
  ['ToDyn', 2], ['ToSFloat', 2], ['ToUFloat', 2], ['ToInt', 2],
  ['SafeCast', 2], ['UnsafeCast', 2], ['ToVirtual', 2],
  ['Label', 0], ['Ret', 1], ['Throw', 1], ['Rethrow', 1], ['Switch', -1],
  ['NullCheck', 1], ['Trap', 2], ['EndTrap', 1],
  ['GetI8', 3], ['GetI16', 3], ['GetMem', 3], ['GetArray', 3],
  ['SetI8', 3], ['SetI16', 3], ['SetMem', 3], ['SetArray', 3],
  ['New', 1], ['ArraySize', 2], ['Type', 2], ['GetType', 2], ['GetTID', 2],
  ['Ref', 2], ['Unref', 2], ['Setref', 2],
  ['MakeEnum', -1], ['EnumAlloc', 2], ['EnumIndex', 2], ['EnumField', 4],
  ['SetEnumField', 3],
  ['Assert', 0], ['RefData', 2], ['RefOffset', 3], ['Nop', 0],
  ['Prefetch', 3], ['Asm', 3],
  ['Unknown101', 1],
];

// Which operand of a conditional jump holds the relative offset, so targets
// can be printed as absolute instruction numbers instead of deltas.
const JUMP_ARG = {
  JAlways: 0, JTrue: 1, JFalse: 1, JNull: 1, JNotNull: 1, Trap: 1,
  JSLt: 2, JSGte: 2, JSGt: 2, JSLte: 2, JULt: 2, JUGte: 2,
  JNotLt: 2, JNotGte: 2, JEq: 2, JNotEq: 2,
};

// --- reader ----------------------------------------------------------------

class Reader {
  constructor(buf) { this.b = buf; this.p = 0; }
  u8() { return this.b[this.p++]; }
  i32() { const v = this.b.readInt32LE(this.p); this.p += 4; return v; }
  f64() { const v = this.b.readDoubleLE(this.p); this.p += 8; return v; }
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
  uindex() {
    const v = this.index();
    if (v < 0) throw new Error(`negative uindex at 0x${this.p.toString(16)}`);
    return v;
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

// --- parse -----------------------------------------------------------------

// Every unknown opcode met while parsing, so --stats can report them and the
// first one can be complained about on stderr.
const UNKNOWN_OPS = [];

function parse(path) {
  const r = new Reader(readFileSync(path));
  if (r.b.toString('ascii', 0, 3) !== 'HLB')
    throw new Error(`not HashLink bytecode: ${path}`);
  r.p = 3;

  const version = r.u8();
  const flags = r.index();
  const hasDebug = (flags & 1) === 1;
  const nints = r.uindex();
  const nfloats = r.uindex();
  const nstrings = r.uindex();
  const nbytes = version >= 5 ? r.uindex() : 0;
  const ntypes = r.uindex();
  const nglobals = r.uindex();
  const nnatives = r.uindex();
  const nfunctions = r.uindex();
  const nconstants = version >= 4 ? r.uindex() : 0;
  const entrypoint = r.uindex();

  const ints = [];
  for (let i = 0; i < nints; i++) ints.push(r.i32());
  const floats = [];
  for (let i = 0; i < nfloats; i++) floats.push(r.f64());
  const strings = r.strings(nstrings);

  if (version >= 5) {
    const size = r.i32();
    r.p += size;
    for (let i = 0; i < nbytes; i++) r.index();
  }
  let debugFiles = [];
  if (hasDebug) debugFiles = r.strings(r.uindex());

  const str = (i) => (i >= 0 && i < strings.length ? strings[i] : null);

  // --- types ---
  const types = new Array(ntypes);
  for (let i = 0; i < ntypes; i++) {
    const kind = r.u8();
    const t = { idx: i, kind };
    switch (kind) {
      case K.FUN:
      case K.METHOD: {
        const n = r.u8();
        t.args = [];
        for (let a = 0; a < n; a++) t.args.push(r.index());
        t.ret = r.index();
        break;
      }
      case K.OBJ:
      case K.STRUCT: {
        t.name = str(r.index());
        t.super = r.index();
        t.global = r.index();
        const nf = r.uindex(), np = r.uindex(), nb = r.uindex();
        t.fields = [];
        for (let f = 0; f < nf; f++)
          t.fields.push({ name: str(r.index()), type: r.index() });
        t.protos = [];
        for (let p = 0; p < np; p++)
          t.protos.push({
            name: str(r.index()), findex: r.index(), pindex: r.index(),
          });
        t.bindings = [];
        for (let b = 0; b < nb; b++) t.bindings.push([r.index(), r.index()]);
        break;
      }
      case K.REF:
      case K.NULL:
      case K.PACKED:
        t.param = r.index();
        break;
      case K.VIRTUAL: {
        const nf = r.uindex();
        t.fields = [];
        for (let f = 0; f < nf; f++)
          t.fields.push({ name: str(r.index()), type: r.index() });
        break;
      }
      case K.ABSTRACT:
        t.name = str(r.index());
        break;
      case K.ENUM: {
        t.name = str(r.index());
        t.global = r.index();
        const nc = r.uindex();
        t.constructs = [];
        for (let c = 0; c < nc; c++) {
          const name = str(r.index());
          const npar = r.uindex();
          const params = [];
          for (let p = 0; p < npar; p++) params.push(r.index());
          t.constructs.push({ name, params });
        }
        break;
      }
      default:
        break; // primitives carry no payload
    }
    types[i] = t;
  }

  // --- globals / natives ---
  const globals = [];
  for (let i = 0; i < nglobals; i++) globals.push(r.index());
  const natives = [];
  for (let i = 0; i < nnatives; i++)
    natives.push({
      lib: str(r.index()), name: str(r.index()),
      type: r.index(), findex: r.uindex(),
    });

  // --- functions ---
  // The debug table is run-length encoded against the instruction stream:
  // hl_read_debug_infos in HashLink's src/code.c, reproduced exactly. Getting
  // this wrong does not throw, it just silently attaches the wrong line
  // numbers - which is worse than no line numbers at all.
  function readDebug(nops) {
    const out = new Array(nops);
    let curfile = -1, curline = 0, i = 0;
    while (i < nops) {
      const c = r.u8();
      if (c & 1) {
        curfile = ((c >> 1) << 8) | r.u8();
      } else if (c & 2) {
        const delta = c >> 6;
        let count = (c >> 2) & 15;
        while (count-- > 0) out[i++] = [curfile, curline];
        curline += delta;
      } else if (c & 4) {
        curline += c >> 3;
        out[i++] = [curfile, curline];
      } else {
        const b2 = r.u8(), b3 = r.u8();
        curline = (c >> 3) | (b2 << 5) | (b3 << 13);
        out[i++] = [curfile, curline];
      }
    }
    return out;
  }

  const funcs = [];
  for (let i = 0; i < nfunctions; i++) {
    const f = { type: r.index(), findex: r.uindex() };
    const nregs = r.uindex(), nops = r.uindex();
    f.regs = [];
    for (let g = 0; g < nregs; g++) f.regs.push(r.index());
    f.ops = [];
    for (let o = 0; o < nops; o++) {
      const at = r.p;
      const op = r.u8();
      const spec = OPS[op];
      if (!spec) {
        throw new Error(
          `opcode ${op} at byte 0x${at.toString(16)} is not in the table ` +
          `(fn#${f.findex}, instruction ${o} of ${nops}). The parser is ` +
          'out of step with the file and everything after this point would ' +
          'be fiction.'
        );
      }
      if (op === UNKNOWN_OP) UNKNOWN_OPS.push({ at, findex: f.findex, op: o });
      const [name, n] = spec;
      const a = [];
      if (n >= 0) {
        for (let k = 0; k < n; k++) a.push(r.index());
      } else if (name === 'Switch') {
        a.push(r.index());
        const nc = r.uindex();
        const cases = [];
        for (let k = 0; k < nc; k++) cases.push(r.uindex());
        a.push(cases);
        a.push(r.uindex());
      } else {
        // MakeEnum and the four variadic calls share a shape: two operands,
        // a count, then that many more operands.
        a.push(r.index());
        a.push(r.index());
        const nn = r.index();
        const xs = [];
        for (let k = 0; k < nn; k++) xs.push(r.index());
        a.push(xs);
      }
      f.ops.push({ op: name, a });
    }
    if (hasDebug) {
      f.debug = readDebug(nops);
      if (version >= 3) {
        // Local-variable assignments. They name registers, but the meaning of
        // the position field is not documented well enough to print without
        // guessing, so they are skipped rather than half-understood.
        const na = r.uindex();
        for (let k = 0; k < na; k++) { r.uindex(); r.index(); }
      }
    }
    funcs.push(f);
  }

  // --- constants ---
  // Static values baked into globals. Worth reading rather than skipping:
  // this is what turns `GetGlobal r5, 3997` into the actual string literal.
  const constants = [];
  for (let i = 0; i < nconstants; i++) {
    const g = r.uindex();
    const nf = r.uindex();
    const fl = [];
    for (let k = 0; k < nf; k++) fl.push(r.uindex());
    constants.push({ global: g, fields: fl });
  }

  return {
    path, version, flags, hasDebug, entrypoint, ints, floats, strings,
    debugFiles, types, globals, natives, funcs, constants,
    endp: r.p, size: r.b.length,
  };
}

// --- derived tables --------------------------------------------------------

const bootPath = requireBoot(process.argv.slice(2));
let bc;
try {
  bc = parse(bootPath);
} catch (e) {
  console.error('');
  console.error(`Could not read ${bootPath}`);
  console.error(`  ${e.message}`);
  console.error('');
  process.exit(1);
}
const { types, strings, ints, floats, globals, debugFiles } = bc;

const str = (i) => (i >= 0 && i < strings.length ? strings[i] : null);
const isObj = (t) => t && (t.kind === K.OBJ || t.kind === K.STRUCT);

// The runtime field list of a class: the parent's fields first, then its own,
// which is the order hl_get_obj_rt assigns and therefore the order every
// field index in the bytecode is expressed in. The prototype indexed a
// class's *own* field list instead, and that is why it labelled calls with
// names five slots out - $StringTools.trim came out as "rpad".
const flatFieldsCache = new Map();
function flatFields(t) {
  if (!isObj(t)) return [];
  if (flatFieldsCache.has(t.idx)) return flatFieldsCache.get(t.idx);
  flatFieldsCache.set(t.idx, []); // breaks a cycle instead of recursing off
  const sup = t.super >= 0 ? types[t.super] : null;
  const out = isObj(sup) ? [...flatFields(sup)] : [];
  for (const f of t.fields || []) out.push(f);
  flatFieldsCache.set(t.idx, out);
  return out;
}

// The vtable, indexed by proto index, inherited then overridden. CallMethod
// and CallThis name their target this way rather than by findex.
const vtableCache = new Map();
function vtable(t) {
  if (!isObj(t)) return [];
  if (vtableCache.has(t.idx)) return vtableCache.get(t.idx);
  vtableCache.set(t.idx, []);
  const sup = t.super >= 0 ? types[t.super] : null;
  const out = isObj(sup) ? [...vtable(sup)] : [];
  for (const p of t.protos || [])
    if (p.pindex >= 0) out[p.pindex] = { owner: t.name, name: p.name };
  vtableCache.set(t.idx, out);
  return out;
}

const byFindex = new Map();
for (const f of bc.funcs) byFindex.set(f.findex, f);

// findex -> { label, origin }. Protos are the real declared methods and win.
// A binding is a class field that holds a function - which for a `$Foo`
// statics class is exactly the static method - so the label is what the
// bytecode says, not an inference. Where two bindings disagree about a
// findex, neither is trusted.
const nameOf = new Map();
function claim(findex, label, origin) {
  const cur = nameOf.get(findex);
  if (!cur) { nameOf.set(findex, { label, origin }); return; }
  if (cur.origin === 'proto' || cur.origin === 'native') return;
  if (origin === 'proto' || origin === 'native') {
    nameOf.set(findex, { label, origin });
    return;
  }
  if (cur.label !== label) cur.ambiguous = true;
}
for (const t of types) {
  if (!isObj(t)) continue;
  for (const p of t.protos) claim(p.findex, `${t.name}.${p.name}`, 'proto');
}
for (const n of bc.natives) claim(n.findex, `${n.lib}.${n.name}`, 'native');
for (const t of types) {
  if (!isObj(t)) continue;
  const fl = flatFields(t);
  for (const [fid, findex] of t.bindings) {
    const f = fl[fid];
    if (f && f.name) claim(findex, `${t.name}.${f.name}`, 'binding');
  }
}
function labelOf(findex) {
  const e = nameOf.get(findex);
  if (!e) return `fn@${findex}`;
  if (e.ambiguous) return `${e.label}?`;
  return e.origin === 'native' ? `native ${e.label}` : e.label;
}

// Globals whose value is a compile-time constant. hl_module_init decodes
// these by field kind, and this repeats that: a String is its bytes plus its
// length, so a String global resolves to the literal a call actually passes.
const constByGlobal = new Map();
for (const c of bc.constants) constByGlobal.set(c.global, c);
function globalValue(g) {
  const c = constByGlobal.get(g);
  if (!c) return null;
  const gt = types[globals[g]];
  if (!isObj(gt)) return null;
  const fl = flatFields(gt);
  const vals = [];
  for (let i = 0; i < c.fields.length; i++) {
    const ft = fl[i] ? types[fl[i].type] : null;
    const raw = c.fields[i];
    let v;
    switch (ft ? ft.kind : -1) {
      case K.BYTES: {
        const s = str(raw);
        v = s === null ? null : JSON.stringify(s);
        break;
      }
      case K.I32: case K.UI8: case K.UI16: v = ints[raw]; break;
      case K.F32: case K.F64: v = floats[raw]; break;
      case K.BOOL: v = raw ? 'true' : 'false'; break;
      default: v = null; break; // a reference to another global, unresolved
    }
    vals.push({ name: fl[i] ? fl[i].name : `#${i}`, v });
  }
  if (gt.name === 'String' && vals.length && vals[0].v !== null &&
      vals[0].v !== undefined)
    return vals[0].v;
  const body = vals
    .map((x) => `${x.name}=${x.v === null || x.v === undefined ? '?' : x.v}`)
    .join(', ');
  return `${gt.name}{${body}}`;
}

// A readable name for a type. The prototype capped the recursion depth
// because Farever has virtual types that contain themselves, which blows the
// stack; a visited set does the same job without truncating types that are
// merely deep.
function tname(i, seen) {
  const t = types[i];
  if (!t) return `#${i}`;
  if (t.name) return t.name;
  if (!seen) seen = new Set();
  if (seen.has(i)) return '...';
  seen.add(i);
  switch (t.kind) {
    case K.VIRTUAL:
      return `{${t.fields.map((f) => f.name).join(',')}}`;
    case K.NULL:
      return `Null<${tname(t.param, seen)}>`;
    case K.REF:
      return `Ref<${tname(t.param, seen)}>`;
    case K.PACKED:
      return `Packed<${tname(t.param, seen)}>`;
    case K.FUN:
    case K.METHOD:
      return `(${t.args.map((a) => tname(a, seen)).join(', ')}) -> ` +
             tname(t.ret, seen);
    default:
      return KIND_NAME[t.kind] || `#${i}`;
  }
}

// --- rendering -------------------------------------------------------------

// Field indices are relative to the runtime type of a register. Where that
// type carries no field table - a DYN, an ARRAY - the index cannot be turned
// into a name, and the output says so instead of implying the bare number
// means something on its own.
//
// A field can also exist and have no name: hxbit generates the virtual-cache
// slots behind every network object with an empty string for a name, which is
// why ent.Hero has fields 13 to 16 that nothing can call. "Unnamed" and
// "could not be worked out" are different answers and are printed differently.
function fieldNote(regType, fid) {
  const t = types[regType];
  let f = null;
  if (isObj(t)) f = flatFields(t)[fid];
  else if (t && t.kind === K.VIRTUAL) f = (t.fields || [])[fid];
  if (f && f.name) return `.${f.name}`;
  if (f) return `.#${fid} (exists, unnamed in the bytecode)`;
  const on = t ? `, no such field on ${tname(regType)}` : '';
  return `.#${fid} (unresolved${on})`;
}

// CallMethod and CallThis index the vtable of an object. On a virtual there
// is no vtable and the same operand indexes the virtual's own fields, because
// what is being called is a closure held in a field - that is what an
// iterator's hasNext/next calls look like.
function protoNote(regType, pid) {
  const t = types[regType];
  if (isObj(t)) {
    const p = vtable(t)[pid];
    if (p) return `-> ${p.owner}.${p.name}`;
  } else if (t && t.kind === K.VIRTUAL) {
    const f = (t.fields || [])[pid];
    if (f && f.name) return `-> .${f.name} (closure in a virtual field)`;
  }
  return `proto#${pid} (unresolved${t ? `, on ${tname(regType)}` : ''})`;
}

function enumNote(regType, cid) {
  const t = types[regType];
  if (t && t.kind === K.ENUM && t.constructs[cid])
    return `${t.name}.${t.constructs[cid].name}`;
  return `construct#${cid} (unresolved)`;
}

function decorate(f, i, o) {
  const a = o.a;
  const R = (n) => f.regs[n];
  const parts = [];

  switch (o.op) {
    case 'Int': parts.push(`= ${ints[a[1]]}`); break;
    case 'Float': parts.push(`= ${floats[a[1]]}`); break;
    case 'Bool': parts.push(`= ${a[1] ? 'true' : 'false'}`); break;
    case 'String': {
      const s = str(a[1]);
      parts.push(s === null
        ? `str#${a[1]} (out of range)`
        : JSON.stringify(s));
      break;
    }
    case 'Bytes': parts.push(`bytes#${a[1]} (table not decoded)`); break;
    case 'Call0': case 'Call1': case 'Call2': case 'Call3': case 'Call4':
    case 'CallN':
      parts.push(`-> ${labelOf(a[1])}`);
      break;
    case 'CallMethod': parts.push(protoNote(R(a[2][0]), a[1])); break;
    case 'CallThis': parts.push(protoNote(R(0), a[1])); break;
    case 'StaticClosure': parts.push(`-> ${labelOf(a[1])}`); break;
    case 'InstanceClosure': parts.push(`-> ${labelOf(a[1])}`); break;
    case 'VirtualClosure': parts.push(protoNote(R(a[1]), a[2])); break;
    case 'GetGlobal': case 'SetGlobal': {
      const g = a[1];
      const v = globalValue(g);
      parts.push(`g#${g} : ${tname(globals[g])}`);
      if (v !== null) parts.push(`= ${v}`);
      break;
    }
    case 'Field': parts.push(fieldNote(R(a[1]), a[2])); break;
    case 'SetField': parts.push(fieldNote(R(a[0]), a[1])); break;
    case 'GetThis': parts.push(fieldNote(R(0), a[1])); break;
    case 'SetThis': parts.push(fieldNote(R(0), a[0])); break;
    case 'DynGet': case 'DynSet': {
      // The field is named by a string index here, not a field index, so it
      // resolves even on a DYN.
      const s = str(o.op === 'DynGet' ? a[2] : a[1]);
      parts.push(s === null ? 'field name out of range' : `."${s}"`);
      break;
    }
    case 'Type': parts.push(tname(a[1])); break;
    case 'New': parts.push(tname(R(a[0]))); break;
    case 'MakeEnum': case 'EnumAlloc':
      parts.push(enumNote(R(a[0]), a[1]));
      break;
    case 'EnumIndex': break;
    case 'EnumField': parts.push(enumNote(R(a[1]), a[2])); break;
    case 'SetEnumField': break;
    case 'Switch': {
      const targets = a[1].map((d) => `@${i + 1 + d}`).join(' ');
      parts.push(`cases ${targets}  default @${i + 1 + a[2]}`);
      break;
    }
    case 'Unknown101':
      parts.push('opcode 101, meaning unknown - see the header comment');
      break;
    default: break;
  }

  if (o.op in JUMP_ARG && o.op !== 'Switch')
    parts.push(`-> @${i + 1 + a[JUMP_ARG[o.op]]}`);

  return parts.length ? '   ' + parts.join('  ') : '';
}

function dump(f) {
  const e = nameOf.get(f.findex);
  const label = labelOf(f.findex);
  const ft = types[f.type];
  const sig = ft && ft.args
    ? `(${ft.args.map((x) => tname(x)).join(', ')}) -> ${tname(ft.ret)}`
    : '';
  let origin = '';
  if (e && e.ambiguous)
    origin = '   [name from more than one binding, so unreliable]';
  else if (e && e.origin === 'binding')
    origin = '   [name from a field binding, not a declared method]';
  else if (!e) origin = '   [no name in the bytecode]';

  const d0 = f.debug && f.debug[0];
  const where = d0 ? `   ${debugFiles[d0[0]] || '?'}:${d0[1]}` : '';

  console.log('');
  console.log(`===== ${label}   [findex ${f.findex}]  ${sig}${origin}`);
  if (where) console.log(`  from${where}`);
  const regs = f.regs.map((t, i) => `r${i}:${tname(t)}`).join('  ');
  console.log(`  regs: ${regs}`);

  let lastLine = -1, lastFile = -1;
  f.ops.forEach((o, i) => {
    const dbg = f.debug && f.debug[i];
    let src = '';
    if (dbg && (dbg[1] !== lastLine || dbg[0] !== lastFile)) {
      src = `  ; ${debugFiles[dbg[0]] || '?'}:${dbg[1]}`;
      lastLine = dbg[1];
      lastFile = dbg[0];
    }
    const args = o.a
      .map((x) => (Array.isArray(x) ? `[${x.join(',')}]` : x))
      .join(', ');
    console.log(
      `  ${String(i).padStart(4)}  ${o.op.padEnd(16)} ${args}` +
      `${decorate(f, i, o)}${src}`
    );
  });
}

// --- main ------------------------------------------------------------------

const argv = process.argv.slice(2);
function optValue(name) {
  const i = argv.indexOf(`--${name}`);
  if (i >= 0 && argv[i + 1] !== undefined) return argv[i + 1];
  const eq = argv.find((a) => a.startsWith(`--${name}=`));
  return eq ? eq.slice(name.length + 3) : null;
}

const wantStats = argv.includes('--stats');
const grep = optValue('grep');
const findexArg = optValue('findex');
// Values that belong to an option are not function names. Without excluding
// them the tool reports `E:\...\Farever: no such function`, which is a
// confusing way to say the search worked.
const consumed = new Set([grep, findexArg, optValue('game')].filter(Boolean));
const wanted = argv.filter(
  (a) => !a.startsWith('--') && !consumed.has(a) &&
         !/hlboot\.dat$/i.test(a) && !existsSync(a)
);
if (findexArg !== null) wanted.push(findexArg);

if (UNKNOWN_OPS.length) {
  const u = UNKNOWN_OPS[0];
  console.error(
    `warning: opcode ${UNKNOWN_OP} is not in HashLink's table. ` +
    `First seen at byte 0x${u.at.toString(16)} ` +
    `(fn#${u.findex}, instruction ${u.op}); ` +
    `${UNKNOWN_OPS.length} in the file. It is decoded as taking one ` +
    'operand because that is the only count for which the whole file ' +
    'parses to its last byte - which is not the same as knowing what ' +
    'it does.'
  );
}

if (wantStats || (!wanted.length && !grep)) {
  console.log(`reading ${bootPath}`);
  console.log(`  version      ${bc.version}   ` +
              `flags 0x${bc.flags.toString(16)}   ` +
              `debug info ${bc.hasDebug ? 'present' : 'absent'}`);
  console.log(`  ints         ${bc.ints.length.toLocaleString()}`);
  console.log(`  floats       ${bc.floats.length.toLocaleString()}`);
  console.log(`  strings      ${bc.strings.length.toLocaleString()}`);
  console.log(`  debug files  ${bc.debugFiles.length.toLocaleString()}`);
  console.log(`  types        ${bc.types.length.toLocaleString()}`);
  console.log(`  globals      ${bc.globals.length.toLocaleString()}`);
  console.log(`  natives      ${bc.natives.length.toLocaleString()}`);
  console.log(`  functions    ${bc.funcs.length.toLocaleString()}` +
              `   (${nameOf.size.toLocaleString()} named)`);
  console.log(`  constants    ${bc.constants.length.toLocaleString()}`);
  console.log(`  entrypoint   fn#${bc.entrypoint}  ${labelOf(bc.entrypoint)}`);
  // The only honest check that the whole walk is right: a parser that is one
  // byte out anywhere does not land on the last byte of the file.
  console.log(`  parsed to    0x${bc.endp.toString(16)} of ` +
              `0x${bc.size.toString(16)}   ` +
              (bc.endp === bc.size ? 'exact EOF' : 'MISMATCH - do not trust'));
  console.log(`  opcode ${UNKNOWN_OP}   ${UNKNOWN_OPS.length} occurrences, ` +
              'meaning unknown');
  if (!wanted.length && !grep && !wantStats)
    console.log('\nname a function, e.g.  node tools/dis-hlcode.mjs ' +
                'ChatBox.processMessage');
}

let failed = false;

if (grep) {
  let re;
  try {
    re = new RegExp(grep, 'i');
  } catch (err) {
    console.error(`\nnot a regular expression: ${grep}\n  ${err.message}`);
    process.exit(1);
  }
  const hits = [];
  for (const [findex, e] of nameOf)
    if (re.test(e.label) && byFindex.has(findex))
      hits.push([findex, labelOf(findex)]);
  hits.sort((a, b) => (a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0));
  console.log(`\n=== functions matching /${grep}/i ===`);
  for (const [findex, label] of hits)
    console.log(`  ${String(findex).padStart(6)}  ${label}`);
  console.log(`  ${hits.length} match${hits.length === 1 ? '' : 'es'}`);
  if (!hits.length) failed = true;
}

for (const w of wanted) {
  if (/^\d+$/.test(w)) {
    const f = byFindex.get(+w);
    if (f) dump(f);
    else {
      console.error(`\nno function with findex ${w}`);
      failed = true;
    }
    continue;
  }
  let found = 0;
  for (const [findex, e] of nameOf) {
    if (e.label !== w && !e.label.endsWith(`.${w}`)) continue;
    const f = byFindex.get(findex);
    if (f) { dump(f); found++; }
  }
  if (!found) {
    const near = [...nameOf.values()]
      .map((e) => e.label)
      .filter((n) => n.toLowerCase().includes(w.toLowerCase()))
      .slice(0, 12);
    console.error(`\nno function named ${w}` +
                  (near.length ? `. did you mean: ${near.join(', ')}` : ''));
    failed = true;
  }
}

process.exit(failed ? 1 : 0);
