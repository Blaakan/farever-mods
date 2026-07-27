// ---------------------------------------------------------------------------
// overlay_d3d12.cpp
//
// Draws into the game's own D3D12 swap chain.
//
// How it gets there: dxgi_wrap hands us the swap chain the game created, so
// hooking Present is one vtable entry on an object we hold - no signature
// scanning, nothing to re-find after a patch.
//
// The renderer is intentionally small. It draws exactly what the two mods
// need - solid quads and textured quads - through a single pipeline state
// with one orthographic transform. Text is drawn as textured quads from a
// font atlas built at startup. There is no widget toolkit and no depth
// buffer; configuration lives in a hot-reloaded file instead of in-game
// panels, which is what keeps this file a few hundred lines instead of
// several thousand.
//
// Threading: Present runs on the game's render thread. Everything here is
// therefore on that thread, and must be cheap and must never block. Game
// state is read on the worker thread and published through a snapshot the
// draw callback copies - the render thread never walks the game's heap.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

#include <vector>
#include <mutex>

#include "overlay.h"
#include "dxgi_wrap.h"

namespace fmk {

// Logging lives in dllmain; declared here to avoid a header for one function.
void host_log(const char* fmt, ...);

namespace {

// --- vertex format ---------------------------------------------------------
struct Vertex {
    float x, y;      // pixels; the shader converts to clip space
    float u, v;      // atlas coords; (-1,-1) marks an untextured quad
    float r, g, b, a;
};

// --- state -----------------------------------------------------------------
struct Frame {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12Resource*         rtv_res = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12Resource*         vbuf = nullptr;
    void*                   vbuf_cpu = nullptr;
    UINT                    vbuf_cap = 0;
};

ID3D12Device*              g_dev = nullptr;
ID3D12CommandQueue*        g_queue = nullptr;
ID3D12GraphicsCommandList* g_cmdlist = nullptr;
ID3D12DescriptorHeap*      g_rtv_heap = nullptr;
ID3D12DescriptorHeap*      g_srv_heap = nullptr;
ID3D12RootSignature*       g_root = nullptr;
ID3D12PipelineState*       g_pso = nullptr;
std::vector<Frame>         g_frames;
UINT                       g_rtv_stride = 0;

bool g_ready = false;
bool g_failed = false;
DrawFn g_draw = nullptr;

std::vector<Vertex> g_verts;   // built each frame by the draw callback
float g_w = 0, g_h = 0;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present_orig = nullptr;

// ---------------------------------------------------------------------------
// The command queue is not reachable from the swap chain in D3D12, so it is
// captured from ID3D12CommandQueue::ExecuteCommandLists. That is the one place
// a vtable hook is genuinely required; everything else comes to us.
// ---------------------------------------------------------------------------
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                        ID3D12CommandList* const*);
ExecFn g_exec_orig = nullptr;

void STDMETHODCALLTYPE hooked_exec(ID3D12CommandQueue* q, UINT n,
                                   ID3D12CommandList* const* lists) {
    if (!g_queue && q) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queue = q;
            host_log("overlay: command queue captured %p", (void*)q);
        }
    }
    g_exec_orig(q, n, lists);
}

// --- vtable patching --------------------------------------------------------
bool patch_vtable(void* obj, int index, void* fn, void** out_orig) {
    if (!obj) return false;
    void** vtbl = *(void***)obj;
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *out_orig = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return true;
}

// --- shaders ----------------------------------------------------------------
// Compiled at runtime with D3DCompile from d3dcompiler_47.dll, which the game
// already ships next to its executable, so there is no extra dependency and
// no precompiled blobs to keep in sync.
const char kShaderSrc[] = R"(
cbuffer C : register(b0) { float2 inv_size; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
VSOut VSMain(VSIn i) {
    VSOut o;
    // pixels -> clip space, y down
    o.pos = float4(i.pos.x * inv_size.x * 2.0 - 1.0,
                   1.0 - i.pos.y * inv_size.y * 2.0, 0, 1);
    o.uv = i.uv; o.col = i.col;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    if (i.uv.x < 0) return i.col;              // untextured quad
    return tex.Sample(smp, i.uv) * i.col;
}
)";

}  // namespace

// ---------------------------------------------------------------------------
// Draw API - appends geometry; nothing is submitted until Present.
// ---------------------------------------------------------------------------

static void push_quad(float x, float y, float w, float h, float u0, float v0,
                      float u1, float v1, Color c) {
    const Vertex a{x,     y,     u0, v0, c.r, c.g, c.b, c.a};
    const Vertex b{x + w, y,     u1, v0, c.r, c.g, c.b, c.a};
    const Vertex d{x,     y + h, u0, v1, c.r, c.g, c.b, c.a};
    const Vertex e{x + w, y + h, u1, v1, c.r, c.g, c.b, c.a};
    g_verts.push_back(a); g_verts.push_back(b); g_verts.push_back(d);
    g_verts.push_back(b); g_verts.push_back(e); g_verts.push_back(d);
}

void draw_rect(float x, float y, float w, float h, Color c) {
    push_quad(x, y, w, h, -1, -1, -1, -1, c);
}

void draw_rect_outline(float x, float y, float w, float h, float t, Color c) {
    draw_rect(x, y, w, t, c);
    draw_rect(x, y + h - t, w, t, c);
    draw_rect(x, y + t, t, h - 2 * t, c);
    draw_rect(x + w - t, y + t, t, h - 2 * t, c);
}

bool overlay_ready() { return g_ready; }
void overlay_set_draw(DrawFn fn) { g_draw = fn; }

// ---------------------------------------------------------------------------
// Present hook
// ---------------------------------------------------------------------------

namespace {

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT interval,
                                         UINT flags) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        host_log("overlay: first Present seen; queue=%s",
                 g_queue ? "captured" : "not yet");
    }
    // Rendering is wired up in the next step; for now the hook proves the
    // seam and stays a pure pass-through so it cannot destabilise the game.
    return g_present_orig(sc, interval, flags);
}

}  // namespace

bool overlay_install() {
    if (g_ready || g_failed) return g_ready;

    IDXGISwapChain* sc = dxgi_swapchain();
    if (!sc) return false;

    // Present is vtable slot 8 on IDXGISwapChain
    // (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, Present 8).
    if (!patch_vtable(sc, 8, (void*)&hooked_present, (void**)&g_present_orig)) {
        host_log("overlay: failed to hook Present");
        g_failed = true;
        return false;
    }
    host_log("overlay: Present hooked on swap chain %p", (void*)sc);

    // Capture the queue via ExecuteCommandLists (vtable slot 10 on
    // ID3D12CommandQueue) if a device is reachable from the swap chain.
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) && dev) {
        g_dev = dev;
        host_log("overlay: D3D12 device %p", (void*)dev);
    }
    return true;
}

void overlay_shutdown() {
    g_ready = false;
}

}  // namespace fmk
