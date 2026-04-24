#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "video/out/libmpv.h"

struct mp_log;
struct mpv_global;
struct ra_ctx;

struct libmpv_d3d11_shared {
    struct ra_ctx *ra_ctx;
    ID3D11Device *device;
    IDXGISwapChain *swapchain;
};

int libmpv_d3d11_shared_init(struct libmpv_d3d11_shared *shared,
                             void *ta_parent,
                             struct mp_log *log,
                             struct mpv_global *global,
                             mpv_render_param *params);
void libmpv_d3d11_shared_uninit(struct libmpv_d3d11_shared *shared);
HRESULT libmpv_d3d11_shared_get_backbuffer(struct libmpv_d3d11_shared *shared,
                                           ID3D11Texture2D **out,
                                           D3D11_TEXTURE2D_DESC *out_desc);