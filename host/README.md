# farever-modkit host

A standalone runtime for Farever mods, so Collection Atlas and AuraForge can
ship without requiring farever-minimap (and without its minimap and DPS
meter).

**Status: stage 1 of 3 — injection vector, built and export-verified, not yet
tested in-game.** It does not render anything or read game state yet. The
plugins still run on farever-minimap today; nothing here changes that until
stage 3 lands.

## Why `dxgi.dll`

`Farever.exe` imports only `libhl.dll` and the CRT — it does **not** import
`dinput8.dll`. That vector (used by farever-minimap) is a dynamic load, almost
certainly SDL3 probing for joystick support.

`dx12.hdll` statically imports `dxgi.dll`, and `dxgi` is **not** in the
KnownDLLs registry list, so a copy in the application directory wins the
loader search. Static import means guaranteed load, early, every launch.

Exact DXGI functions the game's modules import, from
`node tools/pe-imports.mjs --funcs`:

| Module | Imports |
|---|---|
| `dx12.hdll` | `CreateDXGIFactory2` |
| `directx.hdll` | `CreateDXGIFactory` |
| `sl.common.dll` (Streamline/DLSS) | `CreateDXGIFactory` |
| `dinput8.dll` (farever-minimap) | `CreateDXGIFactory1` |

All five documented exports are forwarded anyway.

## What stage 2 unlocks: the real account collection

The plugin API has no collection reader, so `collection_atlas` can only record
items as they pass through your hands — meaning anything you unlocked before
installing it is invisible, and there is no way to ask "which appearances do I
already own?"

The data exists and is fully mapped. From `node tools/scan-hltypes.mjs`:

```
st.player.AccountProgress          extends st.DBState   sizeof=208
  +0x0a8  collection      : st.player.Collection
  +0x0b8  bank            : hl.types.ArrayObj
  +0x0c0  bankEquipment   : hl.types.ArrayObj
  +0x0c8  bankNbSlots     : I32

st.player.Collection               extends st.DBBaseState  sizeof=168
  +0x078  gliders  : hxbit.ArrayProxyData
  +0x080  mounts   : hxbit.ArrayProxyData
  +0x088  toys     : hxbit.ArrayProxyData
  +0x090  emotes   : hxbit.ArrayProxyData
  +0x098  gears    : hxbit.ArrayProxyData      <- armor appearances
  +0x0a0  pets     : hxbit.ArrayProxyData      <- companions
```

That is the authoritative, account-wide collection — the exact six categories
the game's own collection menu shows — reachable as
`st.Player -> AccountProgress.collection -> {mounts, gliders, pets, gears,
toys, emotes}`. `AccountProgress.bank` / `bankEquipment` covers the stored
weapons and trinkets the vault currently has to infer.

So the collection tracker becomes a real "12 / 63 owned" checklist rather than
a discovery log, with zero re-collecting — but only through the host's own
memory reader. Nothing in the plugin sandbox can reach it. This is the
strongest argument for finishing stage 2, and the offsets above mean it is
generation, not reverse-engineering: re-run the scan after a patch and the
addresses regenerate.

### The complete read path

`ent.Hero.player` at `+0x4b8` closes the chain, and it starts from the Hero —
the pointer every Farever mod already locates and tracks continuously. No new
root-finding technique is needed, and in particular no extra `hl_alloc_obj`
hook: this is a pure pointer walk off an anchor that already exists.

```
ent.Hero                        (already tracked)
  +0x4b8  player            -> st.Player
  +0x0e0    accountProgress -> st.player.AccountProgress
  +0x0a8      collection    -> st.player.Collection
                +0x080  mounts   -> hxbit.ArrayProxyData
                +0x078  gliders                +0x028 array -> hl.types.ArrayDyn
                +0x0a0  pets
                +0x098  gears     (armor appearances)
                +0x088  toys
                +0x090  emotes

  AccountProgress +0x0b8 bank / +0x0c0 bankEquipment -> hl.types.ArrayObj
```

`hxbit.ArrayProxyData` is a thin wrapper (`sizeof=48`) whose `array` field at
`+0x28` holds the actual `hl.types.ArrayDyn`.

Every offset above is generated from `hlboot.dat` by
`tools/scan-hltypes.mjs`, so a game patch is a re-run rather than a
re-investigation.

### Two ways to reach it

**Upstream (fast).** The host mod's plugin API is read-only by design, but the
authoring guide invites requests: *"If you find a real-world use case that
needs one of these, open an issue. We can probably expose a safe wrapper for
it."* The maintainer has a track record of doing exactly that — issues #90
(class), #93 (inventory), #94 (weapon skills) and #100 (a currency) were all
API additions, shipped across v1.2.1–v1.2.4. A request for
`farever.player.collection()` carrying the offsets above is a small, safe,
read-only addition on a path the mod already walks.

**In-house (stage 2).** The host's own reader, which is the standalone goal
regardless. The pointer walk above is the whole job for collections; the
remaining work is the generic HashLink array decoding and the safe read
thread.

These are not exclusive: upstream gets the feature working in days, stage 2
removes the dependency.

## Roadmap

| Stage | What | State |
|---|---|---|
| 1 | dxgi proxy: load, forward, log | **built, ran in-game** |
| 2 | HashLink state reader (`hl_runtime`, `hl_scan`, `hl_reader`) + build-hash gate | **built, awaiting live verification** |
| 2b | Post-patch update flow (`tools/update.mjs`) | **built, verified** |
| 3a | Swap-chain observation via the factory wrapper | **built** |
| 3b | D3D12 renderer: Present hook, PSO, font atlas, textured quads | not started |
| 3c | Port the two mods onto the host's draw + state API | not started |

Stage 3b is the remaining bulk. Everything before it is either verified or
waiting only on a game restart.

Stage 3 matters for packaging: because the host reimplements the *same* API
surface (53 entry points, see `docs/`), `collection_atlas.lua` and
`aura_forge.lua` need no changes. Each mod then ships as its own zip bundling
the host plus that one plugin, and the host loads several modules if you
install more than one — so there is never a second D3D12 hook racing the first.

## Build

Needs the MSVC C++ x64 toolset (Visual Studio 2022 Community or Build Tools).

```bash
host/build.cmd
```

Output: `host/build/dxgi.dll`. The build deliberately does **not** install into
the game — dropping a `dxgi.dll` next to `Farever.exe` changes what loads at
the next launch, so that stays a conscious step.

## Trying stage 1

It only writes a log; it draws nothing. Expect no visible change in game.

1. Close Farever.
2. Copy `host/build/dxgi.dll` next to `Farever.exe`.
3. Launch, then read `farever-modkit.log` in the game folder. You should see
   the attach line and one line per module that called through the proxy.
4. To remove: delete that `dxgi.dll`.

If the game fails to start, deleting the file fully reverts it — the proxy is
a single file with no installer and no registry writes.

**Coexistence.** `dinput8.dll` (farever-minimap) imports `CreateDXGIFactory1`,
so with both installed its calls route through this proxy before reaching the
real DLL. Stage 1 only logs and forwards, so that is harmless — but the two are
not a supported combination, and once stage 3 adds an overlay you should run
one or the other.

## Safety notes

- The real `dxgi.dll` is loaded by **absolute system path**. A bare
  `LoadLibrary("dxgi.dll")` would find this proxy first and recurse.
- `DllMain` does the minimum under the loader lock: no dependent
  `LoadLibrary`, no thread sync. The real DLL resolves lazily on the first
  forwarded call.
- Stage 1 does no vtable patching, no memory reads, and no writes to the game.
