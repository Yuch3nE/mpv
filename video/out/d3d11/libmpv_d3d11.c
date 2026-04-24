#include "config.h"

#include "common/msg.h"
#include "osdep/windows_utils.h"
#include "video/out/d3d11/libmpv_d3d11_shared.h"
#include "osdep/windows_utils.h"
#include "video/out/d3d11/ra_d3d11.h"
#include "video/out/gpu/libmpv_gpu.h"

struct priv {
    struct libmpv_d3d11_shared shared;
    struct ra_tex *tex;
};

static int init(struct libmpv_gpu_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    int err = libmpv_d3d11_shared_init(&p->shared, p, ctx->log, ctx->global,
                                       params);
    if (err < 0)
        return err;

    ctx->ra_ctx = p->shared.ra_ctx;
    return 0;
}

static int wrap_fbo(struct libmpv_gpu_context *ctx, mpv_render_param *params,
                    struct ra_tex **out)
{
    struct priv *p = ctx->priv;
    ID3D11Resource *backbuffer = NULL;

    if (!p->tex) {
        HRESULT hr = libmpv_d3d11_shared_get_backbuffer(&p->shared,
                                                        (ID3D11Texture2D **)&backbuffer,
                                                        NULL);
        if (FAILED(hr)) {
            MP_ERR(ctx, "Couldn't get DXGI backbuffer: %s\n",
                   mp_HRESULT_to_str(hr));
            return MPV_ERROR_GENERIC;
        }

        p->tex = ra_d3d11_wrap_tex(p->shared.ra_ctx->ra, backbuffer);
        SAFE_RELEASE(backbuffer);
        if (!p->tex)
            return MPV_ERROR_GENERIC;
    }

    *out = p->tex;
    return 0;
}

static void done_frame(struct libmpv_gpu_context *ctx, bool ds)
{
    struct priv *p = ctx->priv;

    ra_d3d11_flush(p->shared.ra_ctx->ra);
    ra_tex_free(p->shared.ra_ctx->ra, &p->tex);
}

static void destroy(struct libmpv_gpu_context *ctx)
{
    struct priv *p = ctx->priv;

    if (p->shared.ra_ctx)
        ra_tex_free(p->shared.ra_ctx->ra, &p->tex);
    libmpv_d3d11_shared_uninit(&p->shared);
}

const struct libmpv_gpu_context_fns libmpv_gpu_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_DXGI,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};