#include "config.h"

#include <libplacebo/config.h>

#if PL_HAVE_D3D11
#include <libplacebo/d3d11.h>
#endif

#include "common/msg.h"
#include "osdep/windows_utils.h"
#include "video/out/d3d11/libmpv_d3d11_shared.h"
#include "video/out/d3d11/ra_d3d11.h"
#include "video/out/gpu/d3d11_helpers.h"
#include "video/out/gpu/spirv.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"
#include "video/out/placebo/utils.h"

struct priv {
    struct libmpv_d3d11_shared shared;
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
    if (mp_dxgi_output_desc_from_swapchain(&p->dxgi, p->shared.swapchain, &desc))
        return mp_dxgi_desc_to_color_space(&desc);
    return (struct pl_color_space){0};
}

static int get_swapchain_color_depth(struct priv *p, DXGI_FORMAT format)
{
    DXGI_OUTPUT_DESC1 desc;
    if (!mp_dxgi_output_desc_from_swapchain(&p->dxgi, p->shared.swapchain, &desc))
        desc.BitsPerColor = 0;

    const struct ra_format *ra_format =
        ra_d3d11_get_ra_format(p->shared.ra_ctx->ra, format);
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
    HRESULT hr = IDXGISwapChain_GetDesc(p->shared.swapchain, &desc);
    if (FAILED(hr)) {
        MP_ERR(ctx, "Could not query DXGI swapchain: %s\n", mp_HRESULT_to_str(hr));
        return MPV_ERROR_GENERIC;
    }

    *out = (struct gpu_next_render_target) {
        .surface_color = get_swapchain_color_space(p),
        .color_depth = get_swapchain_color_depth(p, desc.BufferDesc.Format),
        .flip_y = false,
    };
    return 0;
}

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    MP_VERBOSE(ctx, "Creating libmpv gpu-next d3d11 context\n");
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    int err = libmpv_d3d11_shared_init(&p->shared, p, ctx->log, ctx->global,
                                       params);
    if (err < 0)
        return err;

#if !PL_HAVE_D3D11
    MP_ERR(ctx, "libplacebo was built without D3D11 support.\n");
    return MPV_ERROR_UNSUPPORTED;
#else
    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_UNSUPPORTED;
    mppl_log_set_probing(ctx->pllog, false);

    p->d3d11 = pl_d3d11_create(ctx->pllog, pl_d3d11_params(
        .device = p->shared.device,
    ));
    if (!p->d3d11)
        return MPV_ERROR_UNSUPPORTED;

    ctx->ra_ctx = p->shared.ra_ctx;
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
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr = libmpv_d3d11_shared_get_backbuffer(&p->shared, &backbuffer,
                                                    &desc);
    if (FAILED(hr)) {
        MP_ERR(ctx, "Could not get DXGI backbuffer: %s\n", mp_HRESULT_to_str(hr));
        return MPV_ERROR_GENERIC;
    }

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
        libmpv_d3d11_shared_uninit(&p->shared);
        mp_dxgi_factory_uninit(&p->dxgi);
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