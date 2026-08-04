# DLL source, build, and safety

The installed `dxgi.dll` is built from the C++ sources in `host/src/`.
The proxy exports are declared in `host/dxgi.def`, and `host/build.cmd`
invokes the Microsoft x64 C++ compiler. The installer does not compile source:
it verifies the game build and copies an already-built DLL from
`host/build/dxgi.dll` (or from a release archive).

## Why it is at the game root

Farever's DirectX 12 module loads `dxgi.dll`. Windows resolves the proxy next
to `Farever.exe`, so that one file must stay at the game root. The proxy then
loads the real system DXGI DLL by absolute path and forwards its exports. All
other modkit files live under `mods/farever-mods/`.

## Risk model

This is native code loaded inside the game process. It can read the game's
memory and hook DirectX presentation, so a bug can crash the game. A malicious
binary with the same installation method would have the same access as the
game and the current Windows user. Review the source, build it locally, and
compare SHA-256 hashes when provenance matters.

The current source performs no network communication. It reads the game state
and game archives, and writes only modkit configuration, logs, generated data,
and reports under `mods/farever-mods/`. It also refuses memory reads when the
running `hlboot.dat` hash does not match the offsets compiled into the DLL.

Uninstalling removes the root proxy; without `dxgi.dll`, none of the native
modkit code loads. Generated/user data is preserved unless purge is requested.
