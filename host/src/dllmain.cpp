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
#include "chat.h"
#include "players.h"
#include "hl_reader.h"
#include "hl_runtime.h"
#include "dxgi_wrap.h"
#include "input.h"
#include "loot.h"
#include "mapwatch.h"
#include "navigator.h"
#include "overlay.h"
#include "routes.h"
#include "offsets.gen.h"
#include "version.h"

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

// `[debug] probe = 1` in farever-modkit.ini. Read once: the reader's probes
// are one-shot anyway, and a setting that can change halfway through a
// session only makes the log harder to read.
bool probe_enabled() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    wcsncat_s(path, MAX_PATH, L"farever-modkit.ini", _TRUNCATE);
    cached = GetPrivateProfileIntW(L"debug", L"probe", 0, path) ? 1 : 0;
    if (cached) log_line("debug: [debug] probe = 1 - dumping raw progress state");
    return cached != 0;
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

// Set when the world state flips, so the worker stops waiting out its
// 30-second cycle and re-reads as soon as a character is in play.
volatile LONG g_world_changed = 0;

// True while a loading screen is up. Stamped by the pose thread, read by the
// render thread - a flag rather than a read, because the draw callback never
// walks game memory.
volatile LONG g_loading = 0;

// The host composes its modules' draw callbacks; each module stays unaware
// of the others. The navigator draws first so the atlas window stacks above
// its pill.
void host_draw(float w, float h) {
    // Behind a loading screen the whole overlay steps aside - and only that.
    // Nothing is unloaded and no state is dropped: the character has not
    // gone anywhere, so the collection, the route and the loot feed are all
    // still true and are simply not drawn for a moment. Clearing the input
    // rectangles is part of stepping aside; leaving them set would go on
    // swallowing clicks over a screen that has no window on it.
    if (InterlockedCompareExchange(&g_loading, 0, 0)) {
        fmk::input_set_ui_rect(0, 0, 0, 0);
        for (int i = 0; i < fmk::kAuxRects; i++)
            fmk::input_set_aux_rect(i, 0, 0, 0, 0);
        // The chat window's wheel claim goes with them: it is the one
        // rectangle that survives the atlas being shut, so it is also the one
        // that would go on eating the wheel over a loading screen.
        fmk::input_set_wheel_rect(0, 0, 0, 0);
        return;
    }
    fmk::nav_draw(w, h);
    fmk::loot_draw(w, h);
    fmk::chat_draw(w, h);
    fmk::atlas_ui_draw(w, h);
}

// One-second sleep slice shared by every wait in the worker: keeps shutdown
// responsive and gives the UI its once-a-second persistence tick.
void worker_sleep(int seconds) {
    for (int i = 0; i < seconds && !g_stop; i++) {
        Sleep(1000);
        fmk::atlas_ui_tick();
        fmk::nav_tick();
        fmk::routes_tick();
        fmk::loot_tick();
        fmk::chat_tick();
        // The player roster, on the same once-a-second beat. It is the only
        // one of these that is a read rather than a persist, and it belongs
        // here rather than on the render thread for the reason host_draw
        // gives: the draw callback never walks game memory. Its own timer
        // decides whether a second has been long enough.
        fmk::players_poll();
        // Entering or leaving the world cuts the wait short, so stepping
        // into the world refreshes the atlas at once instead of showing
        // nothing until the cycle happens to come round.
        if (InterlockedExchange(&g_world_changed, 0)) return;
    }
}

// Whether a character is in play, published by the pose thread so the loot
// thread does not have to ask the reader the same question twice.
volatile LONG g_in_world = 0;

// The navigator's arrow rotates with the camera; at the worker's cadence it
// would visibly lag every turn. This thread does nothing but a handful of
// validated qword reads every 50ms - it never scans and never walks arrays.
// It doubles as the liveness check: GameApp.hero going null is how logging
// out, a character swap or the main menu is noticed within a frame or two.

DWORD WINAPI pose_worker(LPVOID) {
    bool prev_in_world = false;
    while (!g_stop) {
        // Re-derive GameApp from App.inst before reading anything rooted at
        // it. The game replaces that object across a character select, and
        // the cached pointer cannot be told apart from a live one - so
        // without this the camera and the map window silently read a dead
        // app for the rest of the session. Never with a scan: this runs at
        // 20Hz, and the cheap path is the only one that belongs here.
        fmk::reader_locate_app(false);

        // Purely a drawing question, kept apart from the one below: a zone
        // load is not the character leaving, so it must not invalidate
        // anything the atlas has read.
        InterlockedExchange(&g_loading, fmk::reader_is_loading() ? 1 : 0);

        double x = 0, y = 0, z = 0, rz = 0;
        const bool in_world = fmk::reader_read_hero_pose(&x, &y, &z, &rz);
        if (in_world != prev_in_world) {
            prev_in_world = in_world;
            fmk::atlas_ui_set_in_world(in_world);
            InterlockedExchange(&g_in_world, in_world ? 1 : 0);
            InterlockedExchange(&g_world_changed, 1);
        }
        if (in_world) {
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
        // After the pose, so a waypoint dropped on this tick measures against
        // a position stamped on this tick. Called even out of the world, so
        // its click and key counters stay synchronised rather than firing a
        // backlog on the next zone-in.
        fmk::mapwatch_poll(in_world);

        // The navigator's two live controls. Consumed unconditionally so a
        // press at the main menu is dropped rather than queued up to fire on
        // the next zone-in, and handled here rather than in mapwatch because
        // neither has anything to do with the map.
        for (int i = fmk::input_take_skip_presses(); i > 0; i--)
            fmk::nav_skip();
        for (int i = fmk::input_take_clear_presses(); i > 0; i--)
            fmk::nav_untrack();
        Sleep(50);
    }
    return 0;
}

// The Recent Loots feed has no event to hook - the host only reads - so it
// diffs a small slice of state against its last reading. Twice a second: fast
// enough that a pickup lands on screen while you are still looking at the
// chest, cheap enough that it is a rounding error against a frame.
DWORD WINAPI loot_worker(LPVOID) {
    while (!g_stop) {
        fmk::loot_poll(InterlockedCompareExchange(&g_in_world, 0, 0) != 0);
        Sleep(500);
    }
    return 0;
}

// Chat cannot ride the loot thread's half-second, because this poll is also
// how a command typed into the game's own box is noticed - and a command that
// takes half a second to do anything reads as one that was swallowed, so the
// player types it again. Ten times a second is the difference between "it
// ran" and "did that work?". The cost stays small because only the tail of
// the history the module has not already decoded is ever read.
DWORD WINAPI chat_worker(LPVOID) {
    while (!g_stop) {
        fmk::chat_poll(InterlockedCompareExchange(&g_in_world, 0, 0) != 0);
        Sleep(100);
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
        // The offsets are compiled in, so this is not something the player
        // can fix by re-running a generator: the DLL itself has to be built
        // against the new bytecode. Say that, rather than naming a command
        // that will not help - a stranger reading this log has no idea which
        // of the two situations they are in.
        log_line("build: MISMATCH - this dxgi.dll was built for %s",
                 FMK_BUILD_SHA256);
        log_line("build: the game has been patched since. Memory reads are "
                 "DISABLED, so the mod does nothing at all - it will not "
                 "misbehave.");
        log_line("build: fix it by installing a release built for this game "
                 "version, or from a source checkout:");
        log_line("build:   node tools/update.mjs --fix");
    } else {
        log_line("build: verified, offsets apply");
    }
    return ok;
}

DWORD WINAPI worker(LPVOID) {
    log_line("worker: started");
    const DWORD worker_started = GetTickCount();

    // Let the game get through the earliest part of its own startup. This
    // used to be 20s, from when finding anything meant a memory sweep that
    // was wasted if the game was not ready. GameApp now comes from a static
    // and exists from application start - long before the main menu - so
    // there is nothing to wait for beyond the HashLink runtime being up.
    for (int i = 0; i < 6 && !g_stop; i++) Sleep(1000);
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
    // Routes are only files on disk until something starts one, so they load
    // beside the navigator and before anything can ask for them.
    fmk::routes_init();
    fmk::loot_init();
    fmk::mapwatch_init();
    fmk::chat_init();
    fmk::players_init();
    HANDLE pose = CreateThread(nullptr, 0, pose_worker, nullptr, 0, nullptr);
    if (pose) CloseHandle(pose);   // fire-and-forget; g_stop ends it
    // The loot feed needs its own cadence: twice a second is fast enough that
    // a pickup feels immediate, and slow enough that walking the bags costs
    // nothing measurable. It cannot ride the pose thread, whose whole point
    // is that it does a handful of qword reads and never touches an array.
    HANDLE loot = CreateThread(nullptr, 0, loot_worker, nullptr, 0, nullptr);
    if (loot) CloseHandle(loot);
    // Separate again rather than folded into the loot thread: the two want
    // different cadences, and sharing one would mean either the loot diff
    // running five times as often as it needs to or a command answering five
    // times slower than it should.
    HANDLE chat = CreateThread(nullptr, 0, chat_worker, nullptr, 0, nullptr);
    if (chat) CloseHandle(chat);

    bool reported = false;
    bool overlay_tried = false;
    bool ui_ready = false;
    bool input_ready = false;
    bool app_found = false;
    int  app_tries = 0;
    while (!g_stop) {
        // The reader can start early, but the shared Present vtable must not
        // be patched while a slower localised build is still in DX12Driver
        // setup. Give graphics its own conservative delay; the probe objects
        // still make the hook independent of observing the game's swap chain.
        // Doing this here also keeps DllMain and the render thread clean.
        if (!overlay_tried && GetTickCount() - worker_started >= 20000) {
            overlay_tried = true;
            log_line("overlay: startup delay elapsed; installing hooks");
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
            // Once, not every two seconds for the rest of the session. The
            // retry is right - the device and the window arrive on their own
            // schedule - but a missing item database never fixes itself
            // mid-run, and repeating it fills the log that is supposed to
            // explain it.
            static bool said = false;
            if (!ui_ready && !said) {
                said = true;
                log_line("atlas: not ready yet (see the line above for why); "
                         "waypoints and the loot feed still work");
            }
        }
        // **Not** gated on the atlas. `input_install` is what F8, F9, F10,
        // map clicks and dragging the pill all arrive through, and the atlas
        // needs a generated item database that a DLL-only install does not
        // have. Behind `ui_ready`, someone who copied one file lost every
        // key on the mod at once - including the F8 that would have shown
        // them nothing was loaded - which reads as "the mod does not work"
        // rather than "one file is missing".
        if (!input_ready && fmk::overlay_ready()) {
            input_ready = fmk::input_install(fmk::overlay_game_hwnd());
        }
        // GameApp first, and it is the only thing worth searching for: it
        // exists from application start, holds the hero and the camera, and
        // is reached through App.inst rather than by scanning. Everything
        // downstream is then a pointer walk - including finding the hero
        // again after a zone change or a character swap.
        if (!app_found && app_tries < 40) {
            app_tries++;
            // App.inst is null until the game builds its application, which
            // happens well before the main menu but not instantly. Keep to
            // the free path; only then pay for a sweep.
            //
            // Three minutes, not thirty seconds. The pose thread now
            // re-derives App.inst every tick, so the moment the game builds
            // its application object one of the two threads has it - which
            // makes the sweep pure redundancy rather than a fallback. Being
            // impatient cost four 6-second scans on a cold launch, one of
            // which finished *after* the pose thread had already found it.
            const bool allow_scan = app_tries > 60;
            app_found = fmk::reader_locate_app(allow_scan);
            if (!app_found) {
                worker_sleep(3);
                continue;
            }
        }
        if (fmk::reader_locate_hero(false)) {
            if (!reported) {
                log_line("reader: hero located at %p", fmk::reader_hero());
                reported = true;
            }
            // Region lookups are cached for the length of one cycle; drop
            // them first so a cycle never trusts last cycle's layout.
            fmk::mem_flush_cache();
            const DWORD cycle_start = GetTickCount();

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

                // The bestiary, read from the codex map. Failing here is not
                // fatal to the rest: the creatures page simply shows nothing
                // encountered.
                std::vector<fmk::UnitProgress> units;
                fmk::reader_read_unit_progress(&units);

                // Crafting jobs, for which recipes this character knows.
                std::vector<fmk::JobState> jobs;
                fmk::reader_read_jobs(&jobs);

                // Skill runes, which are learned the same per-character way.
                fmk::RuneState runes;
                fmk::reader_read_runes(&runes);

                // The one-time sources this character has already spent, so
                // the atlas stops pointing at them.
                fmk::CompletionState done;
                fmk::reader_read_completion(&done);

                // Weapon mastery: the kills this character has made with
                // each weapon, which is what the game levels weapons by.
                std::vector<fmk::WeaponMastery> mastery;
                fmk::reader_read_weapon_mastery(&mastery);

                // The one shape still unsettled: an NPC's per-quest goals.
                // Off unless asked for. It dumps every NPC, activity and
                // counter this character has - seventy-odd lines, a third of
                // the log - and the log's job is to name the reason when
                // something looks wrong to somebody who did not write this.
                // Research output buried that. `[debug] probe = 1` in
                // farever-modkit.ini brings it back.
                if (probe_enabled()) fmk::reader_probe_completion();

                // Hand the reads to the UI; it swaps in a fresh ownership
                // snapshot for the render thread.
                fmk::atlas_ui_update(c, inv, units, jobs, runes, done, mastery);
            } else {
                log_line("collection: hero found but collection walk failed");
            }

            // How long a cycle costs, once, so the polling interval below is
            // a measured choice rather than a hopeful one.
            static bool timed = false;
            if (!timed) {
                timed = true;
                log_line("reader: full cycle took %lums",
                         GetTickCount() - cycle_start);
            }

            // Poll often enough that picking something up, catching a
            // critter or killing a mob shows up while you are still looking
            // at the window. True event hooks would mean patching the game's
            // own code, which this host does not do - it only ever reads -
            // so a short poll is the honest way to feel immediate.
            worker_sleep(2);
        } else {
            reported = false;
            // No hero: the main menu, a loading screen, or between
            // characters. This used to back off to five minutes because
            // every retry was a ~40s memory sweep; now it is a single
            // pointer read off GameApp, so it can simply check again
            // shortly - and the pose thread cuts even this short the
            // moment a character is actually in the world.
            worker_sleep(5);
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
            // The version leads, because it is the first question about any
            // report from a machine that is not this one, and nobody can
            // answer it from memory.
            log_line("# farever-modkit " FMK_VERSION " (dxgi.dll proxy)");
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
