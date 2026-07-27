// ---------------------------------------------------------------------------
// overlay_d3d12.cpp
//
// Draws into the game's own D3D12 swap chain.
//
// How we get there without pattern scanning:
//   * dxgi_wrap owns CreateDXGIFactory*, so it sees the factory the game
//     builds and records the swap chain that comes out of it.
//   * Present is then one vtable entry on an object we legitimately hold.
//   * The command queue is the one thing D3D12 will not hand back from a swap
//     chain, so it is captured from ID3D12CommandQueue::ExecuteCommandLists.
//     That is the only unavoidable hook in the whole host.
//
// The renderer is deliberately minimal: one pipeline state, one vertex format,
// solid and textured quads, alpha blended, no depth. Text is glyph quads from
// an atlas rasterised once at startup with GDI, so no font file ships and no
// font data is embedded. That is everything the two mods need - icon strips,
// bars, countdowns, stack counts.
//
// Failure policy: if anything in init fails, g_failed latches and Present
// stays a pure pass-through forever. A broken overlay must never be worse
// than no overlay.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "overlay.h"
#include "dxgi_wrap.h"

namespace fmk {

void host_log(const char* fmt, ...);

namespace {

// --- font atlas geometry ----------------------------------------------------
constexpr int kFirstChar = 32;
constexpr int kLastChar  = 126;
constexpr int kNumChars  = kLastChar - kFirstChar + 1;
constexpr int kGlyphCols = 16;
constexpr int kGlyphRows = (kNumChars + kGlyphCols - 1) / kGlyphCols;
constexpr int kCell      = 32;                    // px per glyph cell
constexpr int kAtlasW    = kGlyphCols * kCell;
constexpr int kAtlasH    = kGlyphRows * kCell;
constexpr float kFontPx  = 24.0f;                 // rasterised size

struct Glyph {
    float u0, v0, u1, v1;
    float w;    // advance in atlas pixels
};
Glyph g_glyphs[kNumChars]{};

struct Vertex {
    float x, y;
    float u, v;      // u < 0 marks an untextured quad
    float r, g, b, a;
};

// A full item-grid page plus tooltips is far past the old 8K-quad budget.
constexpr UINT kMaxVerts = 96 * 1024;

// Descriptor slots: 0 is the font, the rest are atlases handed out by
// overlay_load_atlas.
constexpr int kMaxTextures = 8;

// --- device objects ---------------------------------------------------------
ID3D12Device*              g_dev = nullptr;
ID3D12CommandQueue*        g_queue = nullptr;
ID3D12RootSignature*       g_root = nullptr;
ID3D12PipelineState*       g_pso = nullptr;
ID3D12DescriptorHeap*      g_rtv_heap = nullptr;
ID3D12DescriptorHeap*      g_srv_heap = nullptr;
ID3D12GraphicsCommandList* g_cmd = nullptr;
ID3D12Resource*            g_textures[kMaxTextures]{};   // [0] = font
LONG                       g_texture_count = 1;
UINT                       g_rtv_stride = 0;
UINT                       g_srv_stride = 0;

struct FrameCtx {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12Resource*         rt = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12Resource*         vb = nullptr;
    Vertex*                 vb_cpu = nullptr;
    UINT64                  fence_val = 0;   // last submission using this ctx
};
std::vector<FrameCtx> g_frames;

// Guards allocator/vertex-buffer reuse: a frame context is only reset once
// the GPU has retired its previous submission. Back-buffer index reuse does
// not imply that on its own when the GPU is running behind.
ID3D12Fence* g_frame_fence = nullptr;
HANDLE       g_frame_fence_ev = nullptr;
UINT64       g_fence_next = 1;
DXGI_FORMAT  g_rt_format = DXGI_FORMAT_UNKNOWN;

bool   g_ready = false;
bool   g_failed = false;
DrawFn g_draw = nullptr;
std::vector<Vertex> g_verts;

// Quads are batched into runs that share a texture, so mixing text, panels
// and icon draws still costs one DrawInstanced per run rather than per quad.
// tex == -1 means "only solid quads so far" and binds the font at flush.
struct DrawSeg {
    int  tex;
    UINT start, count;
};
std::vector<DrawSeg> g_segs;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present_orig = nullptr;

using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                             DXGI_FORMAT, UINT);
ResizeFn g_resize_orig = nullptr;

using ExecFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                        ID3D12CommandList* const*);
ExecFn g_exec_orig = nullptr;

// ---------------------------------------------------------------------------
const char kShader[] = R"(
cbuffer C : register(b0) { float2 inv_size; float2 pad; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.x * inv_size.x * 2.0 - 1.0,
                   1.0 - i.pos.y * inv_size.y * 2.0, 0, 1);
    o.uv = i.uv; o.col = i.col;
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    if (i.uv.x < 0) return i.col;
    // One path for every texture: the font atlas is uploaded as white RGBA
    // with coverage in alpha, so glyphs and icon atlases both come out of a
    // straight modulate. The vertex colour is the tint.
    return tex.Sample(smp, i.uv) * i.col;
}
)";

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

// --- font atlas -------------------------------------------------------------
//
// Rasterised once with GDI into a 32bpp DIB, then uploaded as R8 alpha. Using
// the OS rasteriser keeps this file free of embedded font data and gives
// properly hinted glyphs at the size we actually draw.
bool build_font_atlas(std::vector<uint8_t>* out) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kAtlasW;
    bi.bmiHeader.biHeight = -kAtlasH;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return false; }
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)kAtlasW * kAtlasH * 4);

    HFONT font = CreateFontW(-(int)kFontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldfont = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);

    for (int i = 0; i < kNumChars; i++) {
        wchar_t ch = (wchar_t)(kFirstChar + i);
        int cx = (i % kGlyphCols) * kCell;
        int cy = (i / kGlyphCols) * kCell;
        TextOutW(dc, cx + 1, cy + 1, &ch, 1);

        SIZE sz{};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        g_glyphs[i].u0 = (float)cx / kAtlasW;
        g_glyphs[i].v0 = (float)cy / kAtlasH;
        g_glyphs[i].u1 = (float)(cx + kCell) / kAtlasW;
        g_glyphs[i].v1 = (float)(cy + kCell) / kAtlasH;
        g_glyphs[i].w  = (float)sz.cx + 1.0f;
    }

    // GDI drew white-on-black; any channel is the coverage. Emit white RGBA
    // with coverage in alpha so the shader treats the font like any texture.
    out->resize((size_t)kAtlasW * kAtlasH * 4);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)kAtlasW * kAtlasH; i++) {
        (*out)[i * 4 + 0] = 255;
        (*out)[i * 4 + 1] = 255;
        (*out)[i * 4 + 2] = 255;
        (*out)[i * 4 + 3] = src[i * 4];
    }

    SelectObject(dc, oldfont);
    if (font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(font);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}

// Creates a texture, uploads one mip of pixel data, transitions it for
// sampling and writes its SRV into the given descriptor slot. `src_pitch` is
// the tight source pitch of one copy row; block-compressed formats copy H/4
// rows of 4x4 blocks instead of H rows of texels.
bool upload_texture(UINT w, UINT h, DXGI_FORMAT format, const uint8_t* data,
                    UINT src_pitch, UINT copy_rows, int slot) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* tex = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&tex))))
        return false;

    const UINT row_pitch = (src_pitch + 255) & ~255u;
    const UINT upload_size = row_pitch * copy_rows;

    ID3D12Resource* upload = nullptr;
    D3D12_HEAP_PROPERTIES uhp{};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC urd{};
    urd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    urd.Width = upload_size;
    urd.Height = 1;
    urd.DepthOrArraySize = 1;
    urd.MipLevels = 1;
    urd.Format = DXGI_FORMAT_UNKNOWN;
    urd.SampleDesc.Count = 1;
    urd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &urd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&upload)))) {
        tex->Release();
        return false;
    }

    uint8_t* dst = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(upload->Map(0, &none, (void**)&dst)) || !dst) {
        upload->Release();
        tex->Release();
        return false;
    }
    for (UINT y = 0; y < copy_rows; y++)
        memcpy(dst + (size_t)y * row_pitch, data + (size_t)y * src_pitch, src_pitch);
    upload->Unmap(0, nullptr);

    // One-shot copy on a private allocator/list, fenced so we can release the
    // staging buffer before returning.
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&alloc))) ||
        FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                        nullptr, IID_PPV_ARGS(&cl)))) {
        if (alloc) alloc->Release();
        upload->Release();
        tex->Release();
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = format;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = row_pitch;

    D3D12_TEXTURE_COPY_LOCATION dstl{};
    dstl.pResource = tex;
    dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstl.SubresourceIndex = 0;

    cl->CopyTextureRegion(&dstl, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = {cl};
    g_queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    bool completed = false;
    if (SUCCEEDED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence))) &&
        (ev = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr) {
        g_queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, ev);
        completed = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
    }
    if (ev) CloseHandle(ev);
    if (fence) fence->Release();

    cl->Release();
    alloc->Release();
    if (!completed) {
        // The copy may still be in flight; releasing the staging buffer or
        // the texture now would be a use-after-free on the GPU. Leak both
        // deliberately and report failure.
        host_log("overlay: texture upload never completed - leaking staging");
        return false;
    }
    upload->Release();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    h0.ptr += (size_t)slot * g_srv_stride;
    g_dev->CreateShaderResourceView(tex, &sd, h0);
    g_textures[slot] = tex;
    return true;
}

bool create_pipeline(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC scd{};
    sc->GetDesc(&scd);
    const UINT nframes = scd.BufferCount ? scd.BufferCount : 2;

    // descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = nframes;
    if (FAILED(g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    g_rtv_stride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kMaxTextures;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_heap)))) return false;
    g_srv_stride = g_dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // root signature: inline constants + one SRV table + static sampler
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &sig, &err))) {
        if (err) err->Release();
        return false;
    }
    HRESULT hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(),
                                            sig->GetBufferSize(),
                                            IID_PPV_ARGS(&g_root));
    sig->Release();
    if (FAILED(hr)) return false;

    // shaders
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
                          "VSMain", "vs_5_0", 0, 0, &vs, &err))) {
        if (err) { host_log("overlay: VS compile failed: %s",
                            (const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
                          "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
        if (err) { host_log("overlay: PS compile failed: %s",
                            (const char*)err->GetBufferPointer()); err->Release(); }
        vs->Release();
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_root;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.InputLayout = {layout, 3};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = scd.BufferDesc.Format;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;

    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = FALSE;

    auto& rt0 = pd.BlendState.RenderTarget[0];
    rt0.BlendEnable = TRUE;
    rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOp = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
    vs->Release();
    ps->Release();
    if (FAILED(hr)) return false;

    // per-frame resources
    g_frames.resize(nframes);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < nframes; i++) {
        auto& f = g_frames[i];
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&f.alloc))))
            return false;
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&f.rt)))) return false;
        f.rtv = rtv;
        g_dev->CreateRenderTargetView(f.rt, nullptr, f.rtv);
        rtv.ptr += g_rtv_stride;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC vd{};
        vd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vd.Width = sizeof(Vertex) * kMaxVerts;
        vd.Height = 1;
        vd.DepthOrArraySize = 1;
        vd.MipLevels = 1;
        vd.Format = DXGI_FORMAT_UNKNOWN;
        vd.SampleDesc.Count = 1;
        vd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &vd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&f.vb))))
            return false;
        D3D12_RANGE none{0, 0};
        f.vb->Map(0, &none, (void**)&f.vb_cpu);
    }

    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        g_frames[0].alloc, g_pso,
                                        IID_PPV_ARGS(&g_cmd))))
        return false;
    g_cmd->Close();

    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&g_frame_fence))))
        return false;
    g_frame_fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_frame_fence_ev) return false;
    g_rt_format = scd.BufferDesc.Format;

    std::vector<uint8_t> atlas;
    if (!build_font_atlas(&atlas)) { host_log("overlay: font atlas failed"); return false; }
    if (!upload_texture(kAtlasW, kAtlasH, DXGI_FORMAT_R8G8B8A8_UNORM,
                        atlas.data(), kAtlasW * 4, kAtlasH, 0)) {
        host_log("overlay: font upload failed");
        return false;
    }

    host_log("overlay: pipeline ready (%u frames, %ux%u, fmt=%d)", nframes,
             scd.BufferDesc.Width, scd.BufferDesc.Height, (int)scd.BufferDesc.Format);
    return true;
}

// A failed create_pipeline must not keep back-buffer references: the game's
// own ResizeBuffers fails while anyone holds one, which turns a dormant
// overlay into a broken game. Called only from the failure path, before
// latching g_failed.
void release_pipeline() {
    for (auto& f : g_frames) {
        if (f.vb) { f.vb->Unmap(0, nullptr); f.vb->Release(); }
        if (f.rt) f.rt->Release();
        if (f.alloc) f.alloc->Release();
    }
    g_frames.clear();
    for (auto& t : g_textures) {
        if (t) { t->Release(); t = nullptr; }
    }
    if (g_cmd) { g_cmd->Release(); g_cmd = nullptr; }
    if (g_frame_fence) { g_frame_fence->Release(); g_frame_fence = nullptr; }
    if (g_frame_fence_ev) { CloseHandle(g_frame_fence_ev); g_frame_fence_ev = nullptr; }
    if (g_pso) { g_pso->Release(); g_pso = nullptr; }
    if (g_root) { g_root->Release(); g_root = nullptr; }
    if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
    if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
}

}  // namespace

// --- draw API ---------------------------------------------------------------

// tex == -1: solid quad, joins whatever run is open. tex >= 0: sampled quad,
// opens a new run when the current one is bound to a different texture.
static void push_quad(int tex, float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1, Color c) {
    if (g_verts.size() + 6 > kMaxVerts) return;

    if (g_segs.empty()) {
        g_segs.push_back({tex, (UINT)g_verts.size(), 0});
    } else {
        DrawSeg& s = g_segs.back();
        if (tex >= 0) {
            if (s.tex < 0) s.tex = tex;
            else if (s.tex != tex)
                g_segs.push_back({tex, (UINT)g_verts.size(), 0});
        }
    }

    const Vertex a{x,     y,     u0, v0, c.r, c.g, c.b, c.a};
    const Vertex b{x + w, y,     u1, v0, c.r, c.g, c.b, c.a};
    const Vertex d{x,     y + h, u0, v1, c.r, c.g, c.b, c.a};
    const Vertex e{x + w, y + h, u1, v1, c.r, c.g, c.b, c.a};
    g_verts.push_back(a); g_verts.push_back(b); g_verts.push_back(d);
    g_verts.push_back(b); g_verts.push_back(e); g_verts.push_back(d);
    g_segs.back().count += 6;
}

void draw_rect(float x, float y, float w, float h, Color c) {
    push_quad(-1, x, y, w, h, -1, -1, -1, -1, c);
}

void draw_rect_outline(float x, float y, float w, float h, float t, Color c) {
    draw_rect(x, y, w, t, c);
    draw_rect(x, y + h - t, w, t, c);
    draw_rect(x, y + t, t, h - 2 * t, c);
    draw_rect(x + w - t, y + t, t, h - 2 * t, c);
}

// A single solid triangle - the navigator's rotating arrow needs geometry
// the axis-aligned quads cannot express.
void draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                   Color c) {
    if (g_verts.size() + 3 > kMaxVerts) return;
    if (g_segs.empty()) g_segs.push_back({-1, (UINT)g_verts.size(), 0});
    g_verts.push_back({x0, y0, -1, -1, c.r, c.g, c.b, c.a});
    g_verts.push_back({x1, y1, -1, -1, c.r, c.g, c.b, c.a});
    g_verts.push_back({x2, y2, -1, -1, c.r, c.g, c.b, c.a});
    g_segs.back().count += 3;
}

void draw_text(float x, float y, float size, Color c, const char* text) {
    if (!text) return;
    const float scale = size / kFontPx;
    float pen = x;
    for (const char* p = text; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch < kFirstChar || ch > kLastChar) { pen += size * 0.4f; continue; }
        const Glyph& gl = g_glyphs[ch - kFirstChar];
        push_quad(0, pen, y, kCell * scale, kCell * scale, gl.u0, gl.v0, gl.u1,
                  gl.v1, c);
        pen += gl.w * scale;
    }
}

float measure_text(float size, const char* text) {
    if (!text) return 0;
    const float scale = size / kFontPx;
    float w = 0;
    for (const char* p = text; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch < kFirstChar || ch > kLastChar) { w += size * 0.4f; continue; }
        w += g_glyphs[ch - kFirstChar].w * scale;
    }
    return w;
}

void draw_image(int atlas, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, Color tint) {
    if (atlas < 1 || atlas >= g_texture_count || !g_textures[atlas]) return;
    push_quad(atlas, x, y, w, h, u0, v0, u1, v1, tint);
}

// Loads a BC7 DDS (the shape tools/gen-atlas.mjs writes) into the next
// descriptor slot. Must run after overlay_ready() - the device exists only
// once the first frame has been seen. Safe to call from the worker thread:
// the upload runs on its own allocator/list and fences before returning.
int overlay_load_atlas(const char* path) {
    if (!g_ready || g_failed) return -1;
    if (g_texture_count >= kMaxTextures) {
        host_log("overlay: atlas limit reached");
        return -1;
    }

    wchar_t wpath[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH)) return -1;
    HANDLE f = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        host_log("overlay: atlas not found: %s", path);
        return -1;
    }
    DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size < 148 || size > 64u * 1024 * 1024) {
        CloseHandle(f);
        host_log("overlay: atlas has unreasonable size: %s", path);
        return -1;
    }
    std::vector<uint8_t> dds(size);
    DWORD got = 0;
    BOOL ok = ReadFile(f, dds.data(), size, &got, nullptr);
    CloseHandle(f);
    if (!ok || got != size) {
        host_log("overlay: atlas read failed: %s", path);
        return -1;
    }

    // DDS with a DX10 header carrying BC7_UNORM, single mip.
    const uint32_t* u32 = (const uint32_t*)dds.data();
    if (memcmp(dds.data(), "DDS ", 4) != 0 ||
        memcmp(dds.data() + 84, "DX10", 4) != 0 || u32[128 / 4] != 98) {
        host_log("overlay: %s is not a BC7 DX10 DDS", path);
        return -1;
    }
    const UINT h = u32[12 / 4];
    const UINT w = u32[16 / 4];
    const UINT pitch = (w / 4) * 16;                    // BC7: 16B per 4x4 block
    const UINT rows = h / 4;
    if (w == 0 || h == 0 || (w | h) & 3 || 148 + (size_t)pitch * rows > size) {
        host_log("overlay: %s has inconsistent dimensions %ux%u", path, w, h);
        return -1;
    }

    const int slot = g_texture_count;
    if (!upload_texture(w, h, DXGI_FORMAT_BC7_UNORM, dds.data() + 148, pitch,
                        rows, slot)) {
        host_log("overlay: atlas upload failed: %s", path);
        return -1;
    }
    // Publish the slot only after the texture is fully resident.
    InterlockedIncrement(&g_texture_count);
    host_log("overlay: atlas %d = %s (%ux%u)", slot, path, w, h);
    return slot;
}

bool overlay_ready() { return g_ready; }
void overlay_set_draw(DrawFn fn) { g_draw = fn; }

// --- Present ----------------------------------------------------------------

namespace {

// The vtable hooks fire for every swap chain in the process (the patch lives
// in dxgi's shared vtable). Everything stateful below belongs to the one swap
// chain the pipeline was built against.
IDXGISwapChain* g_game_sc = nullptr;
HWND            g_game_hwnd = nullptr;
volatile LONG   g_init_claim = 0;

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT interval,
                                         UINT flags) {
    if (g_failed || !g_draw) return g_present_orig(sc, interval, flags);

    if (!g_ready) {
        // Only one presenter runs the one-time init at a time; anyone else
        // (a second swap chain, or a re-entrant present) passes through and
        // retries next frame. The claim is released on every exit.
        if (InterlockedCompareExchange(&g_init_claim, 1, 0) != 0)
            return g_present_orig(sc, interval, flags);
        const bool ok = [&]() -> bool {
            // Wait until the queue has been seen; without it nothing can be
            // submitted, and guessing is how overlays corrupt a device.
            if (!g_queue) return false;

            // The device comes from the GAME's swap chain, not our probe:
            // the probe existed only to reach the shared vtable.
            if (!g_dev) {
                if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_dev))) || !g_dev) {
                    g_failed = true;
                    host_log("overlay: swap chain is not D3D12 - disabled");
                    return false;
                }
                host_log("overlay: game device %p", (void*)g_dev);
            }
            IDXGISwapChain3* sc3 = nullptr;
            if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                g_failed = true;
                host_log("overlay: no IDXGISwapChain3");
                return false;
            }
            sc3->Release();
            if (!create_pipeline(sc)) {
                release_pipeline();
                g_failed = true;
                host_log("overlay: pipeline init failed - staying passive");
                return false;
            }
            g_game_sc = sc;
            DXGI_SWAP_CHAIN_DESC gd{};
            sc->GetDesc(&gd);
            g_game_hwnd = gd.OutputWindow;
            g_ready = true;
            return true;
        }();
        InterlockedExchange(&g_init_claim, 0);
        if (!ok) return g_present_orig(sc, interval, flags);
    }

    if (sc != g_game_sc) return g_present_orig(sc, interval, flags);

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        return g_present_orig(sc, interval, flags);
    }
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    if (idx >= g_frames.size()) return g_present_orig(sc, interval, flags);

    DXGI_SWAP_CHAIN_DESC scd{};
    sc->GetDesc(&scd);
    const float w = (float)scd.BufferDesc.Width;
    const float h = (float)scd.BufferDesc.Height;

    g_verts.clear();
    g_segs.clear();
    g_draw(w, h);
    if (g_verts.empty()) return g_present_orig(sc, interval, flags);

    auto& f = g_frames[idx];
    if (!f.rt || !f.vb_cpu) return g_present_orig(sc, interval, flags);

    // The GPU must have retired this context's previous submission before
    // its allocator resets or its vertex buffer is rewritten; back-buffer
    // index reuse alone does not guarantee that when the GPU runs behind.
    if (f.fence_val && g_frame_fence &&
        g_frame_fence->GetCompletedValue() < f.fence_val) {
        g_frame_fence->SetEventOnCompletion(f.fence_val, g_frame_fence_ev);
        if (WaitForSingleObject(g_frame_fence_ev, 1000) != WAIT_OBJECT_0)
            return g_present_orig(sc, interval, flags);
    }

    memcpy(f.vb_cpu, g_verts.data(), g_verts.size() * sizeof(Vertex));

    f.alloc->Reset();
    g_cmd->Reset(f.alloc, g_pso);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = f.rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmd->ResourceBarrier(1, &b);

    g_cmd->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    g_cmd->SetGraphicsRootSignature(g_root);
    ID3D12DescriptorHeap* heaps[] = {g_srv_heap};
    g_cmd->SetDescriptorHeaps(1, heaps);
    const float consts[4] = {1.0f / w, 1.0f / h, 0, 0};
    g_cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);

    D3D12_VIEWPORT vp{0, 0, w, h, 0, 1};
    D3D12_RECT sr{0, 0, (LONG)w, (LONG)h};
    g_cmd->RSSetViewports(1, &vp);
    g_cmd->RSSetScissorRects(1, &sr);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = f.vb->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)(g_verts.size() * sizeof(Vertex));
    vbv.StrideInBytes = sizeof(Vertex);
    g_cmd->IASetVertexBuffers(0, 1, &vbv);
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const D3D12_GPU_DESCRIPTOR_HANDLE srv0 =
        g_srv_heap->GetGPUDescriptorHandleForHeapStart();
    for (const DrawSeg& s : g_segs) {
        if (!s.count) continue;
        D3D12_GPU_DESCRIPTOR_HANDLE hgpu = srv0;
        hgpu.ptr += (UINT64)(s.tex < 0 ? 0 : s.tex) * g_srv_stride;
        g_cmd->SetGraphicsRootDescriptorTable(1, hgpu);
        g_cmd->DrawInstanced(s.count, 1, s.start, 0);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd->ResourceBarrier(1, &b);
    g_cmd->Close();

    ID3D12CommandList* lists[] = {g_cmd};
    g_queue->ExecuteCommandLists(1, lists);
    if (g_frame_fence) {
        f.fence_val = g_fence_next++;
        g_queue->Signal(g_frame_fence, f.fence_val);
    }

    return g_present_orig(sc, interval, flags);
}

// Without this, a window resize or alt-enter kills the game: ResizeBuffers
// fails while we hold references to the back buffers, or succeeds and leaves
// our RTVs pointing at freed resources. Release, forward, re-acquire.
HRESULT STDMETHODCALLTYPE hooked_resize(IDXGISwapChain* sc, UINT count, UINT w,
                                        UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (!g_ready || sc != g_game_sc)
        return g_resize_orig(sc, count, w, h, fmt, flags);

    for (auto& f : g_frames) {
        if (f.rt) { f.rt->Release(); f.rt = nullptr; }
    }
    HRESULT hr = g_resize_orig(sc, count, w, h, fmt, flags);
    if (FAILED(hr)) {
        g_failed = true;
        host_log("overlay: ResizeBuffers failed (0x%08x) - overlay disabled",
                 (unsigned)hr);
        return hr;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    sc->GetDesc(&scd);
    if (scd.BufferCount > g_frames.size()) {
        // The RTV heap was sized for the original count; growing it here is
        // not worth the complexity. Go dormant instead of guessing.
        g_failed = true;
        host_log("overlay: buffer count grew to %u - overlay disabled",
                 scd.BufferCount);
        return hr;
    }
    if (scd.BufferDesc.Format != g_rt_format) {
        // The PSO was compiled against the original RTV format; drawing into
        // a different one is undefined. Dormant beats artifacts.
        g_failed = true;
        host_log("overlay: back-buffer format changed (%d -> %d) - disabled",
                 (int)g_rt_format, (int)scd.BufferDesc.Format);
        return hr;
    }
    for (UINT i = 0; i < scd.BufferCount; i++) {
        auto& f = g_frames[i];   // f.rtv keeps its original heap slot
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&f.rt)))) {
            g_failed = true;
            host_log("overlay: buffer re-acquire failed - overlay disabled");
            return hr;
        }
        g_dev->CreateRenderTargetView(f.rt, nullptr, f.rtv);
    }
    host_log("overlay: swap chain resized to %ux%u", scd.BufferDesc.Width,
             scd.BufferDesc.Height);
    return hr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Install by patching the shared vtables, reached through throwaway objects.
//
// The first attempt tried to observe the game's swap chain by wrapping the
// DXGI factory. The proxy did see every CreateDXGIFactory* call - including
// dx12.hdll's - but never a swap chain, because D3D12 callers request
// IDXGIFactory4/6 and the wrapper could only stand in for Factory/1/2, so the
// real object passed straight through. Implementing the newer interfaces
// would be a lot of surface to keep correct for no benefit.
//
// Vtables are per-class and shared by every instance, so creating our own
// throwaway device, queue and swap chain lets us patch IDXGISwapChain::Present
// and ID3D12CommandQueue::ExecuteCommandLists once, for everyone - including
// the game's objects, however it chose to create them. The throwaway objects
// are released immediately; the patch lives in dxgi/d3d12's own vtable.
bool overlay_install() {
    if (g_ready || g_failed) return g_ready;

    // A message-only window is enough to create a swap chain against.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"fmk_probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        host_log("overlay: probe window failed");
        g_failed = true;
        return false;
    }

    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* sc = nullptr;
    bool ok = false;

    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&dev))) && dev) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue) {
            // Patch ExecuteCommandLists (slot 10) - every queue shares this.
            if (patch_vtable(queue, 10, (void*)&hooked_exec, (void**)&g_exec_orig))
                host_log("overlay: ExecuteCommandLists hooked");

            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = 8;
                sd.Height = 8;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd,
                                                              nullptr, nullptr, &sc)) &&
                    sc) {
                    // Patch Present (slot 8) and ResizeBuffers (slot 13) -
                    // shared by the game's swap chain.
                    if (patch_vtable(sc, 8, (void*)&hooked_present,
                                     (void**)&g_present_orig)) {
                        host_log("overlay: Present hooked via probe swap chain");
                        ok = true;
                    }
                    if (ok && patch_vtable(sc, 13, (void*)&hooked_resize,
                                           (void**)&g_resize_orig)) {
                        host_log("overlay: ResizeBuffers hooked");
                    }
                }
            }
        }
    }

    if (sc) sc->Release();
    if (factory) factory->Release();
    if (queue) queue->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!ok) {
        host_log("overlay: install failed - staying passive");
        g_failed = true;
    }
    return ok;
}

void* overlay_game_hwnd() {
    return g_ready ? (void*)g_game_hwnd : nullptr;
}

void overlay_shutdown() { g_ready = false; }

}  // namespace fmk
