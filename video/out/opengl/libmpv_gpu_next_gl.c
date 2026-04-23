#include <stdlib.h>

#include <libplacebo/opengl.h>

#include "common.h"
#include "context.h"
#include "ra_gl.h"
#include "options/m_config.h"
#include "mpv/render_gl.h"
#include "ta/ta_talloc.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"
#include "video/out/placebo/utils.h"

#if HAVE_EGL
#include <EGL/egl.h>
#endif

struct priv {
    GL *gl;
    struct ra_ctx *ra_ctx;
    pl_opengl opengl;
    pl_tex wrapped_target;
};

static void destroy_wrapped_target(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p || !p->wrapped_target)
        return;

    pl_tex_destroy(ctx->gpu, &p->wrapped_target);
}

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_opengl_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, NULL);
    if (!init_params)
        return MPV_ERROR_INVALID_PARAMETER;

    p->gl = talloc_zero(p, GL);
    mpgl_load_functions2(p->gl, init_params->get_proc_address,
                         init_params->get_proc_address_ctx,
                         NULL, ctx->log);
    if (!p->gl->version && !p->gl->es) {
        MP_FATAL(ctx, "OpenGL not initialized.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->log = ctx->log;
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->opts = (struct ra_ctx_opts) {
        .allow_sw = true,
    };

    struct ra_ctx_params gl_params = {0};
    p->gl->SwapInterval = NULL;
    if (!ra_gl_ctx_init(p->ra_ctx, p->gl, gl_params))
        return MPV_ERROR_UNSUPPORTED;

    struct ra_ctx_opts *ctx_opts = mp_get_config_group(ctx, ctx->global,
                                                       &ra_ctx_conf);
    p->ra_ctx->opts.debug = ctx_opts->debug;
    p->gl->debug_context = ctx_opts->debug;
    ra_gl_set_debug(p->ra_ctx->ra, ctx_opts->debug);
    talloc_free(ctx_opts);

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_UNSUPPORTED;
    mppl_log_set_probing(ctx->pllog, false);

    struct pl_opengl_params pl_params = *pl_opengl_params(
        .debug = p->ra_ctx->opts.debug,
        .allow_software = p->ra_ctx->opts.allow_sw,
        .get_proc_addr_ex = (void *)p->gl->get_fn,
        .proc_ctx = p->gl->fn_ctx,
    );
#if HAVE_EGL
    pl_params.egl_display = eglGetCurrentDisplay();
    pl_params.egl_context = eglGetCurrentContext();
#endif

    p->opengl = pl_opengl_create(ctx->pllog, &pl_params);
    if (!p->opengl)
        return MPV_ERROR_UNSUPPORTED;

    ctx->ra_ctx = p->ra_ctx;
    ctx->gpu = p->opengl->gpu;
    return 0;
}

static int wrap_target(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params,
                       struct gpu_next_render_target *out)
{
    struct priv *p = ctx->priv;
    *out = (struct gpu_next_render_target){0};

    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    if (fbo->fbo && !(p->gl->mpgl_caps & MPGL_CAP_FB)) {
        MP_FATAL(ctx, "Rendering to FBO requested, but no FBO extension found!\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    destroy_wrapped_target(ctx);
    p->wrapped_target = pl_opengl_wrap(ctx->gpu, pl_opengl_wrap_params(
        .framebuffer = fbo->fbo,
        .width = fbo->w,
        .height = abs(fbo->h),
        .iformat = fbo->internal_format
    ));
    if (!p->wrapped_target)
        return MPV_ERROR_GENERIC;

    *out = (struct gpu_next_render_target) {
        .frame = {
            .color = pl_color_space_srgb,
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
        .surface_color = pl_color_space_srgb,
        .color_depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH,
                                            int, 0),
        .allow_color_hint = false,
        .flip_y = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_FLIP_Y,
                                       int, 0),
    };
    return 0;
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool ds)
{
    destroy_wrapped_target(ctx);
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;

    destroy_wrapped_target(ctx);
    if (p && p->opengl)
        pl_opengl_destroy(&p->opengl);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
    if (p && p->ra_ctx)
        ra_gl_ctx_uninit(p->ra_ctx);

    talloc_free(p);
    ctx->priv = NULL;
    ctx->ra_ctx = NULL;
    ctx->gpu = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl = {
    .api_name = MPV_RENDER_API_TYPE_OPENGL,
    .init = init,
    .wrap_target = wrap_target,
    .done_frame = done_frame,
    .destroy = destroy,
};
