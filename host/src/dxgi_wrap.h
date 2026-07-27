#pragma once

#include <windows.h>

struct IDXGISwapChain;

namespace fmk {

using SwapChainCb = void (*)(IDXGISwapChain*);

// Wraps a freshly created DXGI factory so swap-chain creation is observable.
// Returns the object to hand back to the caller: the wrapper when we can fully
// stand in for the requested interface, otherwise the original untouched.
void* dxgi_wrap_factory(void* real, REFIID riid);

void dxgi_note_swapchain(IDXGISwapChain* sc);
void dxgi_set_swapchain_cb(SwapChainCb cb);
IDXGISwapChain* dxgi_swapchain();

}  // namespace fmk
