// ============================================================================
// farever-modkit host - stage 1: injection vector
//
// A dxgi.dll proxy that loads into Farever, forwards every DXGI export to the
// real system DLL, and reports what it sees. This is the foundation for a
// standalone mod host: once we are inside the process with the swap chain in
// sight, the overlay and the HashLink state reader can be built on top.
//
// WHY dxgi.dll
//   Farever.exe imports only libhl.dll and the CRT - it does NOT import
//   dinput8.dll (that vector, used by farever-minimap, is a dynamic load from
//   SDL3). But dx12.hdll statically imports dxgi.dll, and dxgi is not in the
//   KnownDLLs registry list, so a copy in the application directory wins the
//   loader search. Static import means guaranteed load, early, every launch.
//
//   Confirmed DXGI imports across the game's own modules (tools/pe-imports.mjs
//   --funcs): CreateDXGIFactory (directx.hdll, sl.common.dll),
//   CreateDXGIFactory1 (dinput8.dll), CreateDXGIFactory2 (dx12.hdll).
//   All five documented exports are forwarded regardless, so any module that
//   asks for one it needs still gets it.
//
// STAGE 1 SCOPE - deliberately conservative
//   Load + forward + log only. No vtable patching, no memory reads, no
//   rendering. The point is to prove the vector and leave a diagnostic trail,
//   at near-zero crash risk. Stages 2 (HashLink reader driven by the offsets
//   from tools/scan-hltypes.mjs) and 3 (overlay + Lua runtime) build on this.
//
// SAFETY
//   * The real dxgi.dll is loaded from the SYSTEM directory by absolute path.
//     Using a bare LoadLibrary("dxgi.dll") would find THIS file first and
//     recurse into itself.
//   * DllMain does the bare minimum: no LoadLibrary of anything else, no
//     synchronisation with other threads. The real DLL is resolved lazily on
//     the first forwarded call, outside loader lock.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>  // build-hash verification (excluded by LEAN_AND_MEAN)
#include <intrin.h>    // _ReturnAddress
#include <stdarg.h>
#include <stdio.h>
#include <share.h>   // _SH_DENYNO

#include "atlas_ui.h"
#include "hl_reader.h"
#include "hl_runtime.h"
#include "dxgi_wrap.h"
#include "input.h"
#include "navigator.h"
#include "overlay.h"
#include "offsets.gen.h"

#pragma intrinsic(_ReturnAddress)

namespace {

CRITICAL_SECTION g_lock;
HMODULE          g_real = nullptr;
FILE*            g_log  = nullptr;
bool             g_init = false;

void log_line(const char* fmt, ...);

}  // namespace

// Exposed so the overlay can log without a header for one function.
namespace fmk {
void host_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line("%s", buf);
}
}  // namespace fmk

namespace {

void log_line(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}

// Open the log next to the game executable, matching where the game and other
// mods put theirs.
void open_log() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;
    wcsncat_s(path, MAX_PATH, L"farever-modkit.log", _TRUNCATE);
    // Shared read: the log is the only way to see what the reader is doing, so
    // it must stay readable (tail, editors, tools/update.mjs) while the game
    // runs. Plain _wfopen takes an exclusive lock and makes it unreadable.
    g_log = _wfsopen(path, L"w", _SH_DENYNO);
}

// Resolve the genuine dxgi.dll by absolute system path. Never by bare name -
// that would resolve to this proxy and recurse.
HMODULE real_dxgi() {
    if (g_real) return g_real;
    EnterCriticalSection(&g_lock);
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wcsncat_s(path, MAX_PATH, L"\\dxgi.dll", _TRUNCATE);
            g_real = LoadLibraryW(path);
            log_line("proxy: real dxgi.dll %s (%ls)",
                     g_real ? "loaded" : "FAILED TO LOAD", path);
        }
    }
    LeaveCriticalSection(&g_lock);
    return g_real;
}

FARPROC forward(const char* name) {
    HMODULE m = real_dxgi();
    if (!m) return nullptr;
    FARPROC p = GetProcAddress(m, name);
    if (!p) log_line("proxy: real dxgi.dll has no export '%s'", name);
    return p;
}

// One line per distinct caller module, so the log shows which of the game's
// modules came through us and in what order.
void log_caller(const char* api, void* ret_addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ret_addr, &mod)) {
        log_line("%s <- (unknown caller)", api);
        return;
    }
    wchar_t name[MAX_PATH] = L"?";
    GetModuleFileNameW(mod, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    log_line("%s <- %ls", api, base ? base + 1 : name);
}

// ---------------------------------------------------------------------------
// Worker: everything that touches game memory runs here, on our own thread,
// never on a game thread. It only ever reads.
// ---------------------------------------------------------------------------

volatile LONG g_stop = 0;

// The host composes its modules' draw callbacks; each module stays unaware
// of the others. The navigator draws first so the atlas window stacks above
// its pill.
void host_draw(float w, float h) {
    fmk::nav_draw(w, h);
    fmk::atlas_ui_draw(w, h);
}

// One-second sleep slice shared by every wait in the worker: keeps shutdown
// responsive and gives the UI its once-a-second persistence tick.
void worker_sleep(int seconds) {
    for (int i = 0; i < seconds && !g_stop; i++) {
        Sleep(1000);
        fmk::atlas_ui_tick();
        fmk::nav_tick();
    }
}

// The navigator's arrow rotates with the hero; at the worker's cadence it
// would visibly lag every camera turn. This thread does nothing but four
// validated qword reads every 50ms - it never scans, never walks arrays,
// and reuses whatever hero pointer the worker last validated.
DWORD WINAPI pose_worker(LPVOID) {
    while (!g_stop) {
        double x = 0, y = 0, z = 0, rz = 0;
        if (fmk::reader_read_hero_pose(&x, &y, &z, &rz)) {
            fmk::nav_set_hero_pose(true, x, y, z, rz);
            // Hero first: the camera's diagnostic distance measures against
            // it, though the view vector itself stands alone.
            double px = 0, py = 0, pz = 0, tx = 0, ty = 0, tz = 0;
            if (fmk::reader_read_camera(&px, &py, &pz, &tx, &ty, &tz))
                fmk::nav_set_camera(true, px, py, pz, tx, ty, tz);
            else
                fmk::nav_set_camera(false, 0, 0, 0, 0, 0, 0);
        } else {
            fmk::nav_set_hero_pose(false, 0, 0, 0, 0);
            fmk::nav_set_camera(false, 0, 0, 0, 0, 0, 0);
        }
        Sleep(50);
    }
    return 0;
}

// Refuse to walk pointers unless the running game is the build the offsets
// were generated from. A patched game with stale offsets is exactly how a
// reader turns into a crash.
bool verify_build() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    wcsncat_s(path, MAX_PATH, L"hlboot.dat", _TRUNCATE);

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        log_line("build: cannot open hlboot.dat");
        return false;
    }

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    char hex[65] = {0};

    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        BYTE buf[64 * 1024];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
            CryptHashData(hash, buf, got, 0);
        }
        BYTE digest[32];
        DWORD dlen = sizeof(digest);
        if (CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) {
            for (int i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
            ok = (strcmp(hex, FMK_BUILD_SHA256) == 0);
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(f);

    log_line("build: hlboot.dat sha256=%s", hex);
    if (!ok) {
        log_line("build: MISMATCH - offsets were generated for %s", FMK_BUILD_SHA256);
        log_line("build: memory reads DISABLED. Run: node tools/update.mjs");
    } else {
        log_line("build: verified, offsets apply");
    }
    return ok;
}

DWORD WINAPI worker(LPVOID) {
    log_line("worker: started");

    // Let the game get through its own startup before we touch anything.
    // 20s is enough for the renderer to exist; the reader retries on its own
    // until a character is actually in the world, so waiting longer here only
    // delays the overlay appearing.
    for (int i = 0; i < 20 && !g_stop; i++) Sleep(1000);
    if (g_stop) return 0;

    const bool build_ok = verify_build();
    if (!build_ok) {
        log_line("worker: idling (build mismatch)");
        return 0;
    }

    // The navigator must exist before the pose thread's first tick - its
    // critical section is created here, and nav_set_hero_pose's g_cs_init
    // guard is not a substitute for ordering. Pose updates only make sense
    // once the build is verified; the thread stays a no-op until the worker
    // has located a hero.
    fmk::nav_init();
    HANDLE pose = CreateThread(nullptr, 0, pose_worker, nullptr, 0, nullptr);
    if (pose) CloseHandle(pose);   // fire-and-forget; g_stop ends it

    bool reported = false;
    bool overlay_tried = false;
    bool ui_ready = false;
    bool input_ready = false;
    bool app_found = false;
    int  app_tries = 0;
    while (!g_stop) {
        // Install the render hook once the game has actually made a swap
        // chain. Doing it from here keeps DllMain and the render thread clean.
        // Install unconditionally: the hook is now placed on the shared
        // vtables via throwaway objects, so it does not depend on having
        // observed the game's swap chain first.
        if (!overlay_tried) {
            overlay_tried = true;
            fmk::overlay_set_draw(&host_draw);
            fmk::overlay_install();
        }
        // The UI needs the device (atlas upload) and the window (input), both
        // of which exist only after the first frame went through the hook.
        // Each half retries independently: the HWND can publish a beat after
        // overlay_ready() flips, and a one-shot attempt would leave the UI
        // permanently deaf.
        if (!ui_ready && fmk::overlay_ready()) {
            ui_ready = fmk::atlas_ui_init();
        }
        if (ui_ready && !input_ready) {
            input_ready = fmk::input_install(fmk::overlay_game_hwnd());
        }
        if (fmk::reader_locate_hero(false)) {
            if (!reported) {
                log_line("reader: hero located at %p", fmk::reader_hero());
                reported = true;
            }
            // The camera lives off GameApp, which costs a scan to find, so
            // do it once the game is demonstrably in-world (hero found) and
            // give up after a few attempts - the navigator degrades to the
            // hero's facing rather than rescanning forever.
            if (!app_found && app_tries < 3) {
                app_tries++;
                app_found = fmk::reader_locate_app(false);
            }
            fmk::Collection c;
            fmk::Inventories inv;
            if (fmk::reader_read_collection(&c) && c.valid) {
                // Only speak when something actually changed. The collection
                // is near-static, so re-dumping it every cycle is noise that
                // buries the lines that matter.
                static size_t prev_sig = 0;
                size_t sig = c.mounts.size() * 1000003 + c.gliders.size() * 10007 +
                             c.pets.size() * 101 + c.gears.size() * 7 +
                             c.toys.size() + c.emotes.size() * 31;
                if (sig != prev_sig) {
                    prev_sig = sig;
                    log_line("collection: mounts=%zu gliders=%zu pets=%zu "
                             "gears=%zu toys=%zu emotes=%zu bankSlots=%d",
                             c.mounts.size(), c.gliders.size(), c.pets.size(),
                             c.gears.size(), c.toys.size(), c.emotes.size(),
                             c.bank_slots);
                    fmk::write_collection_json(c);
                }

                // Bank, bags and equipped gear: what "owned" means for
                // weapons and trinkets. Written per character, since only the
                // logged-in one is in this process.
                if (fmk::reader_read_inventories(&inv) && inv.valid) {
                    static std::string prev_who;
                    static size_t prev_isig = 0;
                    size_t isig = inv.bank.size() * 1000003 +
                                  inv.bank_equipment.size() * 10007 +
                                  inv.equipped.size() * 101 + inv.bags.size();
                    if (isig != prev_isig || inv.character != prev_who) {
                        prev_isig = isig;
                        prev_who = inv.character;
                        fmk::write_inventory_json(inv, inv.character);
                    }
                }

                // Hand both reads to the UI; it swaps in a fresh ownership
                // snapshot for the render thread.
                fmk::atlas_ui_update(c, inv);
            } else {
                log_line("collection: hero found but collection walk failed");
            }
            // Steady state: this host does not need to re-read often.
            worker_sleep(30);
        } else {
            reported = false;
            // Back off on repeated failure. A full scan is ~40s of memory
            // traffic; retrying it every 10s while the player sits at a
            // loading screen is pure waste and floods the log.
            static int misses = 0;
            if (misses < 30) misses++;
            worker_sleep(10 + misses * 10);   // 20s .. 5min
        }
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Forwarded exports. Names must match dxgi.dll exactly; see host/dxgi.def.
// ---------------------------------------------------------------------------

extern "C" {

// Each creator forwards, then wraps the result so the swap chain the game
// builds from it becomes observable. Wrapping is best-effort: if we cannot
// fully stand in for the requested interface the original is returned
// untouched, and the game is none the wiser.

HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** out) {
    log_caller("CreateDXGIFactory", _ReturnAddress());
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = (Fn)forward("CreateDXGIFactory");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, out);
    if (SUCCEEDED(hr) && out && *out) *out = fmk::dxgi_wrap_factory(*out, riid);
    return hr;
}

HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** out) {
    log_caller("CreateDXGIFactory1", _ReturnAddress());
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = (Fn)forward("CreateDXGIFactory1");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, out);
    if (SUCCEEDED(hr) && out && *out) *out = fmk::dxgi_wrap_factory(*out, riid);
    return hr;
}

HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    log_caller("CreateDXGIFactory2", _ReturnAddress());
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = (Fn)forward("CreateDXGIFactory2");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) *out = fmk::dxgi_wrap_factory(*out, riid);
    return hr;
}

HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = (Fn)forward("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)forward("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : E_FAIL;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Entry point. Keep this minimal - it runs under the loader lock.
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(self);
            InitializeCriticalSection(&g_lock);
            open_log();
            g_init = true;
            wchar_t exe[MAX_PATH] = L"?";
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            log_line("# farever-modkit host (dxgi.dll proxy) stage 2");
            log_line("attach: pid=%lu exe=%ls", GetCurrentProcessId(), exe);
            log_line("libhl.dll present in process: %s",
                     GetModuleHandleW(L"libhl.dll") ? "yes" : "not yet");
            // Observing the swap chain costs nothing until it appears.
            fmk::dxgi_set_swapchain_cb([](IDXGISwapChain* sc) {
                log_line("dxgi: swap chain observed at %p", (void*)sc);
            });
            // Reads happen on our own thread; DllMain stays minimal.
            CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            break;
        }
        case DLL_PROCESS_DETACH:
            InterlockedExchange(&g_stop, 1);
            fmk::input_uninstall();
            log_line("detach");
            if (g_log) { fclose(g_log); g_log = nullptr; }
            if (g_init) DeleteCriticalSection(&g_lock);
            break;
    }
    return TRUE;
}
