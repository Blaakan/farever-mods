// ---------------------------------------------------------------------------
// dxgi_wrap.cpp
//
// Gets us the swap chain without pattern scanning or vtable hunting.
//
// Because the proxy owns CreateDXGIFactory*, every factory the game makes
// passes through our hands. We return a wrapper whose CreateSwapChain* methods
// forward to the real factory and then record the resulting swap chain. From
// there, hooking Present is a single vtable entry on an object we legitimately
// hold a pointer to - no signature scanning, nothing to re-find after a patch.
//
// This is the reason dxgi.dll was the right proxy choice: it is not just an
// injection vector, it is a supported seam onto the exact object we need.
//
// The wrapper is a pass-through: every method forwards. It exists only to
// observe.
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <stdint.h>

#include "dxgi_wrap.h"

namespace fmk {

namespace {

IDXGISwapChain* g_swapchain = nullptr;
SwapChainCb     g_on_swapchain = nullptr;

}  // namespace

void dxgi_set_swapchain_cb(SwapChainCb cb) { g_on_swapchain = cb; }
IDXGISwapChain* dxgi_swapchain() { return g_swapchain; }

void dxgi_note_swapchain(IDXGISwapChain* sc) {
    if (!sc || g_swapchain == sc) return;
    g_swapchain = sc;
    if (g_on_swapchain) g_on_swapchain(sc);
}

// ---------------------------------------------------------------------------
// Minimal IDXGIFactory wrapper.
//
// Only the swap-chain creators are interesting; everything else forwards
// verbatim. QueryInterface hands back this wrapper for factory interfaces so
// the game cannot accidentally unwrap itself by asking for a newer version.
// ---------------------------------------------------------------------------

class FactoryWrap : public IDXGIFactory2 {
public:
    explicit FactoryWrap(IUnknown* inner) : inner_(inner) { inner_->AddRef(); }

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        // Hand back ourselves for any factory interface we can stand in for.
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
            riid == __uuidof(IDXGIFactory2)) {
            AddRef();
            *ppv = static_cast<IDXGIFactory2*>(this);
            return S_OK;
        }
        // Anything newer (Factory3..7) we do not model: forward it. We lose
        // observation on that path, which is why we also fall back to a
        // Present hook installed from the first frame we do see.
        return inner_->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (r == 0) {
            inner_->Release();
            delete this;
        }
        return r;
    }

    // --- IDXGIObject ---
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override {
        return f()->SetPrivateData(n, s, d);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* p) override {
        return f()->SetPrivateDataInterface(n, p);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override {
        return f()->GetPrivateData(n, s, d);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override {
        return f()->GetParent(riid, pp);
    }

    // --- IDXGIFactory ---
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override {
        return f()->EnumAdapters(i, a);
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT fl) override {
        return f()->MakeWindowAssociation(h, fl);
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override {
        return f()->GetWindowAssociation(h);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                              IDXGISwapChain** out) override {
        HRESULT hr = f()->CreateSwapChain(dev, d, out);
        if (SUCCEEDED(hr) && out) dxgi_note_swapchain(*out);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override {
        return f()->CreateSoftwareAdapter(m, a);
    }

    // --- IDXGIFactory1 ---
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override {
        return f1()->EnumAdapters1(i, a);
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return f1()->IsCurrent(); }

    // --- IDXGIFactory2 ---
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override {
        return f2()->IsWindowedStereoEnabled();
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
        IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* ro,
        IDXGISwapChain1** out) override {
        HRESULT hr = f2()->CreateSwapChainForHwnd(dev, hwnd, d, fd, ro, out);
        if (SUCCEEDED(hr) && out) dxgi_note_swapchain(*out);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
        IUnknown* dev, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* ro, IDXGISwapChain1** out) override {
        HRESULT hr = f2()->CreateSwapChainForCoreWindow(dev, w, d, ro, out);
        if (SUCCEEDED(hr) && out) dxgi_note_swapchain(*out);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override {
        return f2()->GetSharedResourceAdapterLuid(h, l);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND h, UINT m, DWORD* c) override {
        return f2()->RegisterStereoStatusWindow(h, m, c);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE h, DWORD* c) override {
        return f2()->RegisterStereoStatusEvent(h, c);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override {
        f2()->UnregisterStereoStatus(c);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND h, UINT m, DWORD* c) override {
        return f2()->RegisterOcclusionStatusWindow(h, m, c);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE h, DWORD* c) override {
        return f2()->RegisterOcclusionStatusEvent(h, c);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override {
        f2()->UnregisterOcclusionStatus(c);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
        IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* ro,
        IDXGISwapChain1** out) override {
        HRESULT hr = f2()->CreateSwapChainForComposition(dev, d, ro, out);
        if (SUCCEEDED(hr) && out) dxgi_note_swapchain(*out);
        return hr;
    }

private:
    IDXGIFactory*  f()  { return (IDXGIFactory*)inner_; }
    IDXGIFactory1* f1() { return (IDXGIFactory1*)inner_; }
    IDXGIFactory2* f2() { return (IDXGIFactory2*)inner_; }

    IUnknown* inner_;
    ULONG     refs_ = 1;
};

void* dxgi_wrap_factory(void* real, REFIID riid) {
    // Only wrap what we can fully stand in for.
    if (riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2)) {
        auto* w = new FactoryWrap((IUnknown*)real);
        ((IUnknown*)real)->Release();  // wrapper holds its own reference
        return w;
    }
    return real;
}

}  // namespace fmk
