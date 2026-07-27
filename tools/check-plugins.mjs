#!/usr/bin/env node
// ---------------------------------------------------------------------------
// check-plugins.mjs
//
// Static checks for farever-minimap Lua plugins. The mod's plugin runtime is a
// sandboxed Lua 5.4 state, and a plugin that throws only reports into
// farever-mod.log at runtime - so it is worth catching what we can offline.
//
// Checks performed:
//   1. Syntax          - full parse via luaparse.
//   2. Sandbox         - use of anything the runtime removes (io, require,
//                        load, dofile, loadfile, debug, os.execute/remove/exit).
//   3. Unknown globals - reads of globals outside the documented API and the
//                        Lua standard subset, which are almost always typos.
//   4. Forbidden imgui - imgui.begin / imgui.end, which the docs call out as
//                        the most common plugin error.
//
// Usage:  node tools/check-plugins.mjs [file.lua ...]     (default: plugins/*.lua)
// ---------------------------------------------------------------------------

import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const luaparse = require('luaparse');

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');

// Globals the plugin runtime provides.
const HOST_GLOBALS = new Set(['farever', 'imgui']);

// Lifecycle hooks the mod calls; assigning them is the whole point.
const LIFECYCLE = new Set(['on_init', 'on_render', 'on_event']);

// Lua standard library that survives the sandbox.
const LUA_STD = new Set([
  'assert', 'error', 'ipairs', 'next', 'pairs', 'pcall', 'xpcall', 'print',
  'select', 'tonumber', 'tostring', 'type', 'unpack', 'rawget', 'rawset',
  'rawequal', 'rawlen', 'setmetatable', 'getmetatable', 'collectgarbage',
  'string', 'table', 'math', 'os', 'coroutine', 'utf8', '_G', '_VERSION',
]);

// Removed by the sandbox - using these is a guaranteed runtime failure.
const BANNED_GLOBALS = new Set([
  'io', 'require', 'dofile', 'loadfile', 'load', 'loadstring', 'debug',
]);
const BANNED_OS_FIELDS = new Set(['execute', 'remove', 'exit', 'rename', 'tmpname']);

function walk(node, visit, parent = null) {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) {
    for (const n of node) walk(n, visit, parent);
    return;
  }
  if (node.type) visit(node, parent);
  for (const key of Object.keys(node)) {
    if (key === 'type' || key === 'loc' || key === 'range') continue;
    walk(node[key], visit, node);
  }
}

function checkFile(path) {
  const src = readFileSync(path, 'utf8');
  const problems = [];
  let ast;

  try {
    ast = luaparse.parse(src, {
      luaVersion: '5.3',
      locations: true,
      scope: true,
      comments: false,
    });
  } catch (e) {
    const line = e.line ?? '?';
    return [{ sev: 'error', line, msg: `syntax: ${e.message}` }];
  }

  // Collect globals that are assigned at top level, so a plugin defining its
  // own helper as a global does not get flagged as unknown.
  const declaredGlobals = new Set(LIFECYCLE);
  walk(ast, (n) => {
    if (n.type === 'AssignmentStatement') {
      for (const v of n.variables ?? []) {
        if (v.type === 'Identifier' && v.isLocal === false) declaredGlobals.add(v.name);
      }
    }
    if (n.type === 'FunctionDeclaration' && n.identifier?.type === 'Identifier') {
      if (n.identifier.isLocal === false) declaredGlobals.add(n.identifier.name);
    }
  });

  const seen = new Set();
  walk(ast, (n, parent) => {
    const line = n.loc?.start?.line ?? '?';

    if (n.type === 'Identifier' && n.isLocal === false) {
      // Skip the property side of `a.b` - only `a` is a global read.
      if (parent?.type === 'MemberExpression' && parent.identifier === n) return;

      const name = n.name;
      if (BANNED_GLOBALS.has(name)) {
        problems.push({ sev: 'error', line, msg: `'${name}' is removed by the plugin sandbox` });
        return;
      }
      if (
        !HOST_GLOBALS.has(name) &&
        !LUA_STD.has(name) &&
        !declaredGlobals.has(name)
      ) {
        const key = `${name}`;
        if (!seen.has(key)) {
          seen.add(key);
          problems.push({ sev: 'warn', line, msg: `unknown global '${name}' (typo?)` });
        }
      }
    }

    // os.execute / os.remove / os.exit
    if (
      n.type === 'MemberExpression' &&
      n.base?.type === 'Identifier' &&
      n.base.name === 'os' &&
      n.base.isLocal === false &&
      BANNED_OS_FIELDS.has(n.identifier?.name)
    ) {
      problems.push({
        sev: 'error',
        line,
        msg: `'os.${n.identifier.name}' is removed by the plugin sandbox`,
      });
    }

    // imgui.begin / imgui.end - the mod opens and closes the window for you.
    if (
      n.type === 'MemberExpression' &&
      n.base?.type === 'Identifier' &&
      n.base.name === 'imgui' &&
      ['begin', 'end', 'begin_window', 'end_window'].includes(n.identifier?.name)
    ) {
      problems.push({
        sev: 'error',
        line,
        msg: `imgui.${n.identifier.name}() is not exposed - the mod owns the window`,
      });
    }
  });

  return problems;
}

const args = process.argv.slice(2);
const files =
  args.length > 0
    ? args
    : readdirSync(join(ROOT, 'plugins'))
        .filter((f) => f.endsWith('.lua'))
        .map((f) => join(ROOT, 'plugins', f));

let errors = 0;
let warns = 0;

for (const f of files) {
  const problems = checkFile(f);
  const errs = problems.filter((p) => p.sev === 'error');
  const wrns = problems.filter((p) => p.sev === 'warn');
  errors += errs.length;
  warns += wrns.length;

  if (problems.length === 0) {
    console.log(`  ok    ${basename(f)}`);
  } else {
    console.log(`  ${errs.length ? 'FAIL' : 'warn'}  ${basename(f)}`);
    for (const p of problems) {
      console.log(`          ${p.sev === 'error' ? 'E' : 'W'} line ${p.line}: ${p.msg}`);
    }
  }
}

console.log(`\n${files.length} file(s), ${errors} error(s), ${warns} warning(s)`);
process.exit(errors > 0 ? 1 : 0);
