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

#include "hl_reader.h"
#include "hl_runtime.h"
#include "offsets.gen.h"

#pragma intrinsic(_ReturnAddress)

namespace {

CRITICAL_SECTION g_lock;
HMODULE          g_real = nullptr;
FILE*            g_log  = nullptr;
bool             g_init = false;

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
    _wfopen_s(&g_log, path, L"w");
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

    // Give the game time to boot and load a character.
    for (int i = 0; i < 60 && !g_stop; i++) Sleep(1000);
    if (g_stop) return 0;

    const bool build_ok = verify_build();
    if (!build_ok) {
        log_line("worker: idling (build mismatch)");
        return 0;
    }

    bool reported = false;
    while (!g_stop) {
        if (fmk::reader_locate_hero(false)) {
            if (!reported) {
                log_line("reader: hero located at %p", fmk::reader_hero());
                reported = true;
            }
            fmk::Collection c;
            if (fmk::reader_read_collection(&c) && c.valid) {
                log_line("collection: mounts=%zu gliders=%zu pets=%zu gears=%zu "
                         "toys=%zu emotes=%zu bankSlots=%d",
                         c.mounts.size(), c.gliders.size(), c.pets.size(),
                         c.gears.size(), c.toys.size(), c.emotes.size(),
                         c.bank_slots);
                size_t shown = 0;
                for (const auto& m : c.mounts) {
                    log_line("  mount  %s", m.c_str());
                    if (++shown >= 10) break;
                }
                shown = 0;
                for (const auto& g : c.gliders) {
                    log_line("  glider %s", g.c_str());
                    if (++shown >= 10) break;
                }
            } else {
                log_line("collection: hero found but collection walk failed");
            }
            // Steady state: this host does not need to re-read often.
            for (int i = 0; i < 30 && !g_stop; i++) Sleep(1000);
        } else {
            reported = false;
            for (int i = 0; i < 10 && !g_stop; i++) Sleep(1000);
        }
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Forwarded exports. Names must match dxgi.dll exactly; see host/dxgi.def.
// ---------------------------------------------------------------------------

extern "C" {

HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** out) {
    log_caller("CreateDXGIFactory", _ReturnAddress());
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = (Fn)forward("CreateDXGIFactory");
    return fn ? fn(riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** out) {
    log_caller("CreateDXGIFactory1", _ReturnAddress());
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = (Fn)forward("CreateDXGIFactory1");
    return fn ? fn(riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    log_caller("CreateDXGIFactory2", _ReturnAddress());
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = (Fn)forward("CreateDXGIFactory2");
    return fn ? fn(flags, riid, out) : E_FAIL;
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
            // Reads happen on our own thread; DllMain stays minimal.
            CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            break;
        }
        case DLL_PROCESS_DETACH:
            InterlockedExchange(&g_stop, 1);
            log_line("detach");
            if (g_log) { fclose(g_log); g_log = nullptr; }
            if (g_init) DeleteCriticalSection(&g_lock);
            break;
    }
    return TRUE;
}
