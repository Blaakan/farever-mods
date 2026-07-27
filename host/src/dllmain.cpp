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
#include <intrin.h>   // _ReturnAddress
#include <stdarg.h>
#include <stdio.h>

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
            log_line("# farever-modkit host (dxgi.dll proxy) stage 1");
            log_line("attach: pid=%lu exe=%ls", GetCurrentProcessId(), exe);
            log_line("libhl.dll present in process: %s",
                     GetModuleHandleW(L"libhl.dll") ? "yes" : "not yet");
            break;
        }
        case DLL_PROCESS_DETACH:
            log_line("detach");
            if (g_log) { fclose(g_log); g_log = nullptr; }
            if (g_init) DeleteCriticalSection(&g_lock);
            break;
    }
    return TRUE;
}
