#include "config.h"

#include <libplacebo/config.h>

#if PL_HAVE_D3D11
#include <libplacebo/d3d11.h>
#endif

#include "common/msg.h"
#include "mpv/render_dxgi.h"
#include "osdep/windows_utils.h"
#include "video/out/d3d11/ra_d3d11.h"
#include "video/out/gpu/d3d11_helpers.h"
#include "video/out/gpu/spirv.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"
#include "video/out/placebo/utils.h"

struct priv {
    struct ra_ctx *ra_ctx;
    ID3D11Device *device;
    IDXGISwapChain *swapchain;
    pl_tex wrapped_target;
    struct mp_dxgi_factory_ctx dxgi;
#if PL_HAVE_D3D11
    pl_d3d11 d3d11;
#endif
};

static void destroy_wrapped_target(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p || !p->wrapped_target)
        return;

    pl_tex_destroy(ctx->gpu, &p->wrapped_target);
}

static void destroy_ra_ctx(struct ra_ctx **ra_ctxp)
{
    struct ra_ctx *ra_ctx = *ra_ctxp;
    if (!ra_ctx)
        return;

    if (ra_ctx->ra)
        ra_ctx->ra->fns->destroy(ra_ctx->ra);
    if (ra_ctx->spirv && ra_ctx->spirv->fns->uninit)
        ra_ctx->spirv->fns->uninit(ra_ctx);

    talloc_free(ra_ctx);
    *ra_ctxp = NULL;
}

static int dxgi_format_depth(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return 10;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return 16;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 8;
    default:
        return 0;
    }
}

static struct pl_color_space get_swapchain_color_space(struct priv *p)
{
    DXGI_OUTPUT_DESC1 desc;
    if (mp_dxgi_output_desc_from_swapchain(&p->dxgi, p->swapchain, &desc))
        return mp_dxgi_desc_to_color_space(&desc);
    return (struct pl_color_space){0};
}

static int get_swapchain_color_depth(struct priv *p, DXGI_FORMAT format)
{
    DXGI_OUTPUT_DESC1 desc;
    if (!mp_dxgi_output_desc_from_swapchain(&p->dxgi, p->swapchain, &desc))
        desc.BitsPerColor = 0;

    const struct ra_format *ra_format =
        ra_d3d11_get_ra_format(p->ra_ctx->ra, format);
    int format_depth = ra_format ? ra_format->component_depth[0] : 0;
    if (!format_depth)
        format_depth = dxgi_format_depth(format);

    if (!desc.BitsPerColor)
        return format_depth;
    if (!format_depth)
        return desc.BitsPerColor;

    return MPMIN(format_depth, desc.BitsPerColor);
}

static int query_target(struct libmpv_gpu_next_context *ctx,
                        struct gpu_next_render_target *out)
{
    struct priv *p = ctx->priv;
    DXGI_SWAP_CHAIN_DESC desc;
    HRESULT hr = IDXGISwapChain_GetDesc(p->swapchain, &desc);
    if (FAILED(hr)) {
        MP_ERR(ctx, "Could not query DXGI swapchain: %s\n", mp_HRESULT_to_str(hr));
        return MPV_ERROR_GENERIC;
    }

    *out = (struct gpu_next_render_target) {
        .surface_color = get_swapchain_color_space(p),
        .color_depth = get_swapchain_color_depth(p, desc.BufferDesc.Format),
        .allow_color_hint = false,
        .flip_y = false,
    };
    return 0;
}

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    MP_VERBOSE(ctx, "Creating libmpv gpu-next d3d11 context\n");
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_dxgi_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_DXGI_INIT_PARAMS, NULL);
    if (!init_params || !init_params->device || !init_params->swapchain)
        return MPV_ERROR_INVALID_PARAMETER;

    p->device = init_params->device;
    p->swapchain = init_params->swapchain;
    ID3D11Device_AddRef(p->device);
    IDXGISwapChain_AddRef(p->swapchain);

    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->log = ctx->log;
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->opts = (struct ra_ctx_opts) {
        .allow_sw = true,
    };

    if (!spirv_compiler_init(p->ra_ctx))
        return MPV_ERROR_UNSUPPORTED;

    p->ra_ctx->ra = ra_d3d11_create(p->device, ctx->log, p->ra_ctx->spirv);
    if (!p->ra_ctx->ra)
        return MPV_ERROR_UNSUPPORTED;

#if !PL_HAVE_D3D11
    MP_ERR(ctx, "libplacebo was built without D3D11 support.\n");
    return MPV_ERROR_UNSUPPORTED;
#else
    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_UNSUPPORTED;
    mppl_log_set_probing(ctx->pllog, false);

    p->d3d11 = pl_d3d11_create(ctx->pllog, pl_d3d11_params(
        .device = p->device,
    ));
    if (!p->d3d11)
        return MPV_ERROR_UNSUPPORTED;

    ctx->ra_ctx = p->ra_ctx;
    ctx->gpu = p->d3d11->gpu;
    return 0;
#endif
}

static int wrap_target(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params,
                       struct gpu_next_render_target *out)
{
    struct priv *p = ctx->priv;
    *out = (struct gpu_next_render_target){0};

#if !PL_HAVE_D3D11
    return MPV_ERROR_UNSUPPORTED;
#else
    ID3D11Texture2D *backbuffer = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                          (void **)&backbuffer);
    if (FAILED(hr)) {
        MP_ERR(ctx, "Could not get DXGI backbuffer: %s\n", mp_HRESULT_to_str(hr));
        return MPV_ERROR_GENERIC;
    }

    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(backbuffer, &desc);

    destroy_wrapped_target(ctx);
    p->wrapped_target = pl_d3d11_wrap(ctx->gpu, pl_d3d11_wrap_params(
        .tex = (ID3D11Resource *)backbuffer,
        .fmt = desc.Format,
        .w = desc.Width,
        .h = desc.Height,
    ));
    SAFE_RELEASE(backbuffer);
    if (!p->wrapped_target)
        return MPV_ERROR_GENERIC;

    struct pl_color_space surface_color = get_swapchain_color_space(p);
    struct pl_color_space frame_color = surface_color;
    if (frame_color.transfer == PL_COLOR_TRC_UNKNOWN &&
        frame_color.primaries == PL_COLOR_PRIM_UNKNOWN)
    {
        frame_color = pl_color_space_srgb;
    }

    int color_depth = get_swapchain_color_depth(p, desc.Format);
    if (!color_depth)
        color_depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH,
                                           int, 0);

    *out = (struct gpu_next_render_target) {
        .frame = {
            .color = frame_color,
            .repr = pl_color_repr_rgb,
            .num_planes = 1,
            .planes = {
                {
                    .texture = p->wrapped_target,
                    .components = 4,
                    .component_mapping = {0, 1, 2, 3},
                },
            },
        },
        .surface_color = surface_color,
        .color_depth = color_depth,
        .allow_color_hint = false,
        .flip_y = false,
    };
    return 0;
#endif
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool ds)
{
    if (ctx->gpu)
        pl_gpu_flush(ctx->gpu);
    destroy_wrapped_target(ctx);
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;

    destroy_wrapped_target(ctx);
    if (p) {
#if PL_HAVE_D3D11
        if (p->d3d11)
            pl_d3d11_destroy(&p->d3d11);
#endif
        destroy_ra_ctx(&p->ra_ctx);
        mp_dxgi_factory_uninit(&p->dxgi);
        SAFE_RELEASE(p->swapchain);
        SAFE_RELEASE(p->device);
        talloc_free(p);
    }
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);

    ctx->priv = NULL;
    ctx->ra_ctx = NULL;
    ctx->gpu = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_DXGI,
    .init = init,
    .query_target = query_target,
    .wrap_target = wrap_target,
    .done_frame = done_frame,
    .destroy = destroy,
};