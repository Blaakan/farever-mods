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

## Roadmap

| Stage | What | State |
|---|---|---|
| 1 | dxgi proxy: load, forward, log | **built** |
| 2 | HashLink state reader, driven by the offsets `tools/scan-hltypes.mjs` generates from `hlboot.dat` | not started |
| 3 | D3D12 overlay + Lua runtime exposing the same `farever.*` / `imgui.*` API, so the existing plugins run unmodified | not started |

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
