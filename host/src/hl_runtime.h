// ---------------------------------------------------------------------------
// hl_runtime.h
//
// Layouts of HashLink's *native* runtime structures, plus validated memory
// access. These are the C structs from HashLink's hl.h - they are part of the
// VM, not of Farever, so unlike offsets.gen.h they do not change when the game
// is patched. They only change if the game ships a new HashLink build, which
// tools/update.mjs checks for by hashing libhl.dll.
//
// Everything here is READ-ONLY. Nothing in this host ever writes to the game.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace fmk {

// --- native HashLink layouts (x64) -----------------------------------------
//
// struct hl_type {                     struct hl_type_obj {
//   hl_type_kind kind;   +0x00           int nfields;        +0x00
//   union { ... obj; }   +0x08           int nproto;         +0x04
//   void **vobj_proto;   +0x10           int nbindings;      +0x08
//   unsigned int *mark;  +0x18           const uchar *name;  +0x10
// };                                     hl_type *super;     +0x18
//                                        hl_obj_field *fields; +0x20
// struct varray {                      };
//   hl_type *t;          +0x00
//   hl_type *at;         +0x08        String/uchar is UTF-16.
//   int size;            +0x10
//   int __pad;           +0x14
//   // elements follow   +0x18
// };
namespace hlrt {
constexpr uint32_t type_kind      = 0x00;
constexpr uint32_t type_obj       = 0x08;   // hl_type_obj* when kind == HOBJ
constexpr uint32_t obj_name       = 0x10;   // const uchar* (UTF-16)
constexpr uint32_t obj_super      = 0x18;
// hl_type_obj continues: fields +0x20, proto +0x28, bindings +0x30, then
// global_value - a void** to the slot holding the class value. A Haxe
// class's static vars are fields of that object, which is how a singleton
// like App.inst is reachable without scanning memory for the instance.
constexpr uint32_t obj_global     = 0x38;

constexpr uint32_t varray_size    = 0x10;
constexpr uint32_t varray_data    = 0x18;   // elements start here

constexpr int      HOBJ           = 11;
constexpr int      HVIRTUAL       = 15;
constexpr int      HSTRUCT        = 21;

// hl_type_virtual, reached through hl_type+0x08:
//   hl_obj_field *fields;  +0x00
//   int nfields;           +0x08
//   int dataSize;          +0x0C
//   int *indexes;          +0x10
// hl_obj_field is { const uchar *name; hl_type *t; int hashed_name; } = 24B.
// A vvirtual is { hl_type *t; vdynamic *value; vvirtual *next; } = 24B, and
// when `value` is null the fields live inline: hl_vfields(v) = v + 24 is an
// array of pointers to each field's storage.
constexpr uint32_t vtype_fields   = 0x00;
constexpr uint32_t vtype_nfields  = 0x08;
constexpr uint32_t vfield_stride  = 24;
constexpr uint32_t vfield_name    = 0x00;
constexpr uint32_t vfield_type    = 0x08;
constexpr uint32_t vvirtual_value = 0x08;
constexpr uint32_t vvirtual_data  = 24;
}  // namespace hlrt

// Describes a HVIRTUAL's field table, for decoding structural values.
struct VirtualField {
    std::string name;
    int32_t kind = -1;
    void*   value_ptr = nullptr;   // storage for this field on a given instance
};
bool read_virtual_fields(const void* vobj, std::vector<VirtualField>* out);

// --- validated reads --------------------------------------------------------
//
// The game owns this memory and can free or repoint any of it between our
// reads. Every dereference goes through here: the address is range-checked
// against a committed, readable region, and the read itself runs under SEH so
// a race that slips past the check faults into a `false` return instead of
// taking the game down.

bool mem_readable(const void* addr, size_t len);
bool mem_read(const void* addr, void* out, size_t len);

template <typename T>
inline bool read(const void* base, uint32_t off, T* out) {
    if (!base) return false;
    return mem_read((const uint8_t*)base + off, out, sizeof(T));
}

inline void* read_ptr(const void* base, uint32_t off) {
    void* p = nullptr;
    if (!read(base, off, &p)) return nullptr;
    return p;
}

inline int32_t read_i32(const void* base, uint32_t off) {
    int32_t v = 0;
    read(base, off, &v);
    return v;
}

// Reads a UTF-16 run and narrows it to UTF-8-ish for logging. `max_chars`
// bounds a run with no terminator in mapped memory.
std::string read_utf16(const void* addr, size_t max_chars = 256);

// Class name of a HashLink object: obj -> hl_type* -> hl_type_obj* -> name.
// Empty when the pointer is not a live object, which doubles as the liveness
// check the reader uses before every walk.
std::string obj_class_name(const void* obj);

// True when `obj` is a live object whose class is exactly `name`.
bool obj_is(const void* obj, const char* name);

// Class name straight from an hl_type* rather than from an instance.
std::string obj_class_name_of_type(const void* type);

// --- object discovery (hl_scan.cpp) -----------------------------------------
// Locates objects by class name with a read-only memory scan; deliberately no
// hooks. See hl_scan.cpp for why.
void* find_type_by_name(const char* cls);

// Returns the first object of `type` for which `pred` holds. Validating during
// the scan matters: most qwords equal to a type pointer are metadata
// references, not instances, so a plain "collect the first N" never reaches a
// real object.
using InstancePred = bool (*)(void* candidate, void* ctx);
void* find_instance_of_type_where(void* type, InstancePred pred, void* ctx);

// --- Haxe container helpers -------------------------------------------------

// Decodes a Haxe String object to UTF-8.
std::string read_hx_string(const void* str_obj);

// hxbit.ArrayProxyData -> hl.types.ArrayDyn -> hl.types.ArrayObj -> varray.
// Returns the element pointer block and count. `out_elems` points into game
// memory and is only valid for the duration of the current read.
bool read_proxy_array(const void* proxy, void** out_elems, int32_t* out_count);

}  // namespace fmk
