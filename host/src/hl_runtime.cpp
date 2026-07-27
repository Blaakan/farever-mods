#include "hl_runtime.h"
#include "offsets.gen.h"

#include <vector>

namespace fmk {

// ---------------------------------------------------------------------------
// Validated reads
//
// Two layers, because either alone is insufficient:
//   * VirtualQuery tells us the page is committed and readable, but the game
//     can unmap it between the query and the read.
//   * SEH catches the fault if that happens.
// The VirtualQuery layer still earns its place: it rejects the common cases
// (null, small ints mistaken for pointers, freed regions) without paying for
// an exception, and exceptions inside a game's own render loop are expensive.
// ---------------------------------------------------------------------------

namespace {

constexpr uintptr_t kMinUserAddr = 0x10000;
constexpr uintptr_t kMaxUserAddr = 0x7FFFFFFFFFFFull;

bool page_ok(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & readable) != 0;
}

}  // namespace

bool mem_readable(const void* addr, size_t len) {
    uintptr_t a = (uintptr_t)addr;
    if (a < kMinUserAddr || a > kMaxUserAddr) return false;
    if (len == 0 || len > (1u << 20)) return false;

    uintptr_t end = a + len;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (!page_ok(mbi)) return false;
        uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= a) return false;  // no forward progress
        a = region_end;
    }
    return true;
}

bool mem_read(const void* addr, void* out, size_t len) {
    if (!mem_readable(addr, len)) return false;
    __try {
        memcpy(out, addr, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string read_utf16(const void* addr, size_t max_chars) {
    if (!addr) return {};
    std::string out;
    out.reserve(32);
    const uint16_t* p = (const uint16_t*)addr;
    for (size_t i = 0; i < max_chars; i++) {
        uint16_t c = 0;
        if (!mem_read(p + i, &c, sizeof(c))) break;
        if (c == 0) break;
        if (c < 0x80) {
            out.push_back((char)c);
        } else {
            out.push_back('?');  // ids are ASCII; anything else is display text
        }
    }
    return out;
}

std::string obj_class_name(const void* obj) {
    if (!obj) return {};
    void* type = read_ptr(obj, 0);
    if (!type) return {};

    int32_t kind = read_i32(type, hlrt::type_kind);
    if (kind != hlrt::HOBJ && kind != hlrt::HSTRUCT) return {};

    void* tobj = read_ptr(type, hlrt::type_obj);
    if (!tobj) return {};
    void* name = read_ptr(tobj, hlrt::obj_name);
    if (!name) return {};
    return read_utf16(name, 128);
}

bool obj_is(const void* obj, const char* name) {
    return obj_class_name(obj) == name;
}

std::string read_hx_string(const void* str_obj) {
    if (!str_obj) return {};
    void* bytes = read_ptr(str_obj, off::String::bytes);
    if (!bytes) return {};
    int32_t len = read_i32(str_obj, off::String::length);
    if (len <= 0 || len > 4096) return {};
    return read_utf16(bytes, (size_t)len);
}

bool read_virtual_fields(const void* vobj, std::vector<VirtualField>* out) {
    out->clear();
    if (!vobj) return false;
    void* type = read_ptr(vobj, 0);
    if (!type) return false;
    if (read_i32(type, hlrt::type_kind) != hlrt::HVIRTUAL) return false;

    void* tv = read_ptr(type, hlrt::type_obj);   // same union slot
    if (!tv) return false;
    void* fields = read_ptr(tv, hlrt::vtype_fields);
    int32_t n = read_i32(tv, hlrt::vtype_nfields);
    if (!fields || n <= 0 || n > 64) return false;

    // With value == null the field storage is inline, as an array of pointers
    // immediately after the vvirtual header.
    const uint8_t* vfields = (const uint8_t*)vobj + hlrt::vvirtual_data;

    for (int32_t i = 0; i < n; i++) {
        const uint8_t* f = (const uint8_t*)fields + (size_t)i * hlrt::vfield_stride;
        VirtualField vf;
        vf.name = read_utf16(read_ptr(f, hlrt::vfield_name), 64);
        void* ft = read_ptr(f, hlrt::vfield_type);
        vf.kind = ft ? read_i32(ft, hlrt::type_kind) : -1;
        vf.value_ptr = read_ptr(vfields, (uint32_t)(i * 8));
        out->push_back(std::move(vf));
    }
    return true;
}

bool read_proxy_array(const void* proxy, void** out_elems, int32_t* out_count) {
    *out_elems = nullptr;
    *out_count = 0;
    if (!proxy) return false;

    // hxbit.ArrayProxyData.array -> hl.types.ArrayDyn
    void* dyn = read_ptr(proxy, off::hxbit_ArrayProxyData::array);
    if (!dyn) return false;

    // ArrayDyn.array -> ArrayBase (an ArrayObj in practice)
    void* base = read_ptr(dyn, off::hl_types_ArrayDyn::array);
    if (!base) return false;

    int32_t len = read_i32(base, off::hl_types_ArrayBase::length);
    if (len < 0 || len > 100000) return false;

    // ArrayObj.array -> native varray; elements follow the header
    void* varr = read_ptr(base, off::hl_types_ArrayObj::array);
    if (!varr) return false;
    int32_t cap = read_i32(varr, hlrt::varray_size);
    if (cap < len) len = cap;          // trust the smaller of the two
    if (len <= 0) { *out_count = 0; return true; }

    *out_elems = (uint8_t*)varr + hlrt::varray_data;
    *out_count = len;
    return true;
}

}  // namespace fmk
