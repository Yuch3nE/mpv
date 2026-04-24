#include "config.h"

#include "mpv/render_dxgi.h"
#include "osdep/windows_utils.h"
#include "video/out/d3d11/libmpv_d3d11_shared.h"
#include "video/out/d3d11/ra_d3d11.h"

int libmpv_d3d11_shared_init(struct libmpv_d3d11_shared *shared,
                             void *ta_parent,
                             struct mp_log *log,
                             struct mpv_global *global,
                             mpv_render_param *params)
{
    *shared = (struct libmpv_d3d11_shared){0};

    mpv_dxgi_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_DXGI_INIT_PARAMS, NULL);
    if (!init_params || !init_params->device || !init_params->swapchain)
        return MPV_ERROR_INVALID_PARAMETER;

    shared->device = init_params->device;
    shared->swapchain = init_params->swapchain;
    ID3D11Device_AddRef(shared->device);
    IDXGISwapChain_AddRef(shared->swapchain);

    shared->ra_ctx = talloc_zero(ta_parent, struct ra_ctx);
    shared->ra_ctx->log = log;
    shared->ra_ctx->global = global;
    shared->ra_ctx->opts = (struct ra_ctx_opts) {
        .allow_sw = true,
    };

    if (!spirv_compiler_init(shared->ra_ctx)) {
        libmpv_d3d11_shared_uninit(shared);
        return MPV_ERROR_UNSUPPORTED;
    }

    shared->ra_ctx->ra = ra_d3d11_create(shared->device, log,
                                         shared->ra_ctx->spirv);
    if (!shared->ra_ctx->ra) {
        libmpv_d3d11_shared_uninit(shared);
        return MPV_ERROR_UNSUPPORTED;
    }

    return 0;
}

void libmpv_d3d11_shared_uninit(struct libmpv_d3d11_shared *shared)
{
    if (shared->ra_ctx) {
        if (shared->ra_ctx->ra)
            shared->ra_ctx->ra->fns->destroy(shared->ra_ctx->ra);
        if (shared->ra_ctx->spirv && shared->ra_ctx->spirv->fns->uninit)
            shared->ra_ctx->spirv->fns->uninit(shared->ra_ctx);

        talloc_free(shared->ra_ctx);
        shared->ra_ctx = NULL;
    }

    SAFE_RELEASE(shared->swapchain);
    SAFE_RELEASE(shared->device);
}

HRESULT libmpv_d3d11_shared_get_backbuffer(struct libmpv_d3d11_shared *shared,
                                           ID3D11Texture2D **out,
                                           D3D11_TEXTURE2D_DESC *out_desc)
{
    *out = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(shared->swapchain, 0,
                                          &IID_ID3D11Texture2D,
                                          (void **)out);
    if (SUCCEEDED(hr) && out_desc)
        ID3D11Texture2D_GetDesc(*out, out_desc);
    return hr;
}