// ---------------------------------------------------------------------------
// hl_scan.cpp
//
// Locates live HashLink objects by class name, without hooking anything.
//
// The usual technique for finding the player object is to detour
// hl_alloc_obj and watch allocations go by. This host deliberately does not:
// that detour puts our code on the game's allocation path, inside the GC's
// blast radius, and the game already has a documented crash in exactly that
// area (Type.createEmptyInstance during network deserialisation). A scan is
// slower but is pure observation - it never executes on a game thread, never
// intercepts a call, and cannot perturb allocation timing.
//
// Three phases, each narrowing the next:
//   1. find the UTF-16 class name "ent.Hero" in memory
//   2. find the hl_type_obj whose `name` field points at it, then the
//      hl_type whose `obj` field points at that
//   3. find objects whose first qword is that hl_type - those are instances
//
// Phase 1-2 run once per process (a type pointer is stable for the process
// lifetime). Phase 3 re-runs only when the cached instance stops validating,
// e.g. after a zone change.
// ---------------------------------------------------------------------------

#include "hl_runtime.h"

#include <vector>
#include <algorithm>
#include <unordered_set>

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

struct Region {
    uint8_t* base;
    size_t   size;
};

// Only private committed read/write memory: that is where the Haxe heap
// lives. Skipping images and mapped files cuts the search enormously and
// avoids paging in file-backed data we have no interest in.
std::vector<Region> heap_regions() {
    std::vector<Region> out;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* max_addr = (uint8_t*)si.lpMaximumApplicationAddress;

    while (addr < max_addr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;

        const bool usable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));

        // Skip absurdly large regions; the heap arrives in many smaller ones
        // and a multi-GB reservation is not where objects live.
        if (usable && mbi.RegionSize <= (size_t)512 * 1024 * 1024) {
            out.push_back({(uint8_t*)mbi.BaseAddress, mbi.RegionSize});
        }
        addr = next;
    }
    return out;
}

// Copies a region in bounded chunks so one unmapped page does not lose the
// whole region, and so peak scratch stays small.
//
// The buffer is hoisted out of the loop: a real session has ~10k regions, and
// allocating a megabyte per region was pure churn. Scan-only, single-threaded,
// so a function-local static is safe here.
template <typename Fn>
void for_each_chunk(const Region& r, Fn&& fn) {
    constexpr size_t kChunk = 1 << 20;
    static std::vector<uint8_t> buf(kChunk + 16);
    for (size_t off = 0; off < r.size; off += kChunk) {
        size_t n = (std::min)(kChunk, r.size - off);
        if (!mem_read(r.base + off, buf.data(), n)) continue;
        fn(r.base + off, buf.data(), n);
    }
}

}  // namespace

// Phase 1+2: class name -> hl_type*
//
// Each phase is one pass over the heap with O(1) set lookups. An earlier
// version used std::find over a candidate vector inside the inner loop, which
// turned a ~250M-iteration pass into a ~64x slower one and meant the scan
// never finished inside a play session - the first in-game run produced no
// reader output at all. Hence both the hash sets and the timing logs: a scan
// this size must be able to report on itself.
void* find_type_by_name(const char* cls) {
    // widen the needle
    std::vector<uint16_t> needle;
    for (const char* p = cls; *p; ++p) needle.push_back((uint16_t)*p);
    needle.push_back(0);
    const size_t nbytes = needle.size() * 2;

    const DWORD t0 = GetTickCount();
    auto regions = heap_regions();
    size_t total = 0;
    for (const auto& r : regions) total += r.size;
    host_log("scan: %s - %zu regions, %.1f MB", cls, regions.size(),
             total / (1024.0 * 1024.0));

    // --- phase 1: the name string -------------------------------------------
    std::unordered_set<uintptr_t> names;
    for (const auto& r : regions) {
        for_each_chunk(r, [&](uint8_t* base, uint8_t* buf, size_t n) {
            if (n < nbytes) return;
            for (size_t i = 0; i + nbytes <= n; i += 2) {
                if (buf[i] == (uint8_t)cls[0] &&
                    memcmp(buf + i, needle.data(), nbytes) == 0) {
                    names.insert((uintptr_t)(base + i));
                }
            }
        });
        if (names.size() > 256) break;
    }
    host_log("scan: phase1 names=%zu (%lums)", names.size(), GetTickCount() - t0);
    if (names.empty()) return nullptr;

    // --- phase 2a: hl_type_obj whose +0x10 name == one of those -------------
    std::unordered_set<uintptr_t> tobjs;
    for (const auto& r : regions) {
        for_each_chunk(r, [&](uint8_t* base, uint8_t* buf, size_t n) {
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t v;
                memcpy(&v, buf + i, 8);
                if (v && names.count(v)) {
                    tobjs.insert((uintptr_t)(base + i) - hlrt::obj_name);
                }
            }
        });
        if (tobjs.size() > 256) break;
    }
    host_log("scan: phase2a type_objs=%zu (%lums)", tobjs.size(), GetTickCount() - t0);
    if (tobjs.empty()) return nullptr;

    // --- phase 2b: hl_type whose +0x08 obj == one of those, kind == HOBJ ----
    void* found = nullptr;
    for (const auto& r : regions) {
        if (found) break;
        for_each_chunk(r, [&](uint8_t* base, uint8_t* buf, size_t n) {
            if (found) return;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t v;
                memcpy(&v, buf + i, 8);
                if (!v || !tobjs.count(v)) continue;
                uintptr_t cand = (uintptr_t)(base + i) - hlrt::type_obj;
                int32_t kind = read_i32((void*)cand, hlrt::type_kind);
                if (kind != hlrt::HOBJ && kind != hlrt::HSTRUCT) continue;
                // confirm the round trip resolves back to the same name
                if (obj_class_name_of_type((void*)cand) == cls) {
                    found = (void*)cand;
                    return;
                }
            }
        });
    }
    host_log("scan: phase2b type=%p (%lums)", found, GetTickCount() - t0);
    return found;
}

// Resolves a class name straight from an hl_type* (as opposed to an instance).
std::string obj_class_name_of_type(const void* type) {
    if (!type) return {};
    void* tobj = read_ptr(type, hlrt::type_obj);
    if (!tobj) return {};
    void* name = read_ptr(tobj, hlrt::obj_name);
    if (!name) return {};
    return read_utf16(name, 128);
}

// Phase 3: find an instance whose first qword is `type` AND which satisfies
// `pred`.
//
// A qword equal to the type pointer is NOT necessarily an object: the type
// table, proto arrays and type parameters of other types all reference it too.
// An earlier version simply collected the first 64 matches and stopped - it
// filled its cap inside the first region in 47ms, entirely on metadata, and
// never reached a real instance. So candidates are now validated as they are
// found, and the scan runs until something passes rather than until an
// arbitrary count is reached.
void* find_instance_of_type_where(void* type, InstancePred pred, void* ctx) {
    if (!type) return nullptr;
    const uintptr_t want = (uintptr_t)type;
    const DWORD t0 = GetTickCount();

    size_t candidates = 0;
    void* found = nullptr;

    for (const auto& r : heap_regions()) {
        if (found) break;
        for_each_chunk(r, [&](uint8_t* base, uint8_t* buf, size_t n) {
            if (found) return;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t v;
                memcpy(&v, buf + i, 8);
                if (v != want) continue;
                candidates++;
                void* cand = base + i;
                if (pred(cand, ctx)) {
                    found = cand;
                    return;
                }
            }
        });
    }
    host_log("scan: instances checked=%zu match=%p (%lums)", candidates, found,
             GetTickCount() - t0);
    return found;
}

}  // namespace fmk
