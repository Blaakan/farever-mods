# Contributing

## The fastest useful contribution

**A line in [`tools/atlas-overrides.tsv`](tools/atlas-overrides.tsv).** The
atlas is generated from the game's own files, and about a hundred entries have
no source recorded in them at all — a mount that only comes from an event, a
vendor the files do not describe. That file patches the generated data per
item id and survives regeneration, so a one-line pull request permanently
fixes an entry for everyone. The format is in the file's own header.

## Setting up

```bash
git clone https://github.com/Blaakan/farever-mods
cd farever-mods
host/build.cmd
node tools/gen-offsets.mjs
install.cmd
```

You need the MSVC C++ x64 toolset (Visual Studio 2022 Community or the
standalone Build Tools, **Desktop development with C++**), Node.js, and your
own copy of the game. Nothing else — the generators use only Node's own
libraries. `npm install` in `tools/` is needed for the Lua plugin harness and
nothing else.

If any tool cannot find your install, `--game "D:\path\to\Farever"` works
everywhere, and is remembered.

## After a game patch

```bash
node tools/update.mjs --fix
```

It diffs the field offsets so you see exactly what moved, regenerates them,
rebuilds and reinstalls. If a field the reader depends on is *gone*, the
generator fails loudly and names it — that field has to be re-found in the new
build before the reader can be trusted, and that is deliberate: a reader that
guesses is worse than one that stops.

## What is checked

```bash
node tools/check-plugins.mjs   # static + sandbox checks for the Lua plugins
node tools/run-harness.mjs     # the plugins in a real Lua 5.4 VM
node tools/package.mjs         # the release archive builds
```

CI runs all of these plus a clean-machine build of the DLL on every push. The
clean-machine part is the one that matters: it has no game install, no
`tools/out`, and whichever Visual Studio GitHub ships this month, so it
catches the whole class of "works on my computer" before anyone downloads it.

## House rules

**The host reads and never writes.** No writes to the game's memory, no input
synthesis, no automation of play, no network. Every read goes through
`mem_read`, which checks the page and wraps the copy in SEH. The reader is
gated on the bytecode hash its offsets came from, so a patched game disables
every read rather than walking a stale pointer. A change that breaks any of
this is not in scope, regardless of how useful it would be.

**Offsets are generated, never hand-written.** If you need a new field, add it
to the `WANT` list in `tools/gen-offsets.mjs` and re-run. Hand-editing
`host/src/offsets.gen.h` works exactly until the next patch. The one exception
is `hl_runtime.h`, which describes HashLink's own native structs — those are
hand-written because they come from the VM rather than from the bytecode, and
`update.mjs` flags a `libhl.dll` change so a human looks at them.

**Say what the data does not say.** Where a source is unknown, the atlas says
so rather than offering a plausible guess — "we could not find the number"
reads as missing, `val1%` reads as a bug, and a waypoint to a place that is
not there is worse than no waypoint. Keep that.

**Comments explain why, not what.** The ones already in the source are mostly
records of something that cost time to find out: which field looked like the
answer and was not, why a cache would be wrong. Those are the valuable ones.

## Releasing

The version lives in `host/src/version.h`. Bump it, commit, tag `v<version>`,
push the tag. CI builds the archive on a clean runner and attaches it to the
release; it refuses a tag that disagrees with the header. Nothing is uploaded
by hand, which is the only claim worth making about an unsigned DLL.

## Licence

MIT. By contributing you agree your changes ship under it.
