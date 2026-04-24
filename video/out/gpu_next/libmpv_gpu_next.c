/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "config.h"
#include "misc/bstr.h"
#include "mpv/client.h"
#if HAVE_D3D11
#include "mpv/render_dxgi.h"
#include "video/out/gpu/d3d11_helpers.h"
#endif
#include "mpv/render_gl.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/ra.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"
#include "video/out/libmpv.h"

struct native_resource_entry {
    const char *name;   // ra_add_native_resource() internal name
    size_t size;        // size of struct pointed to (0 for no copy)
};

// Mirrors gpu/libmpv_gpu.c:native_resource_map so that hwdec backends which
// rely on host-provided native resources (X11 display, Wayland display, DRM
// params, ...) work the same way under the gpu-next libmpv backend.
static const struct native_resource_entry native_resource_map[] = {
    [MPV_RENDER_PARAM_X11_DISPLAY] = {
        .name = "x11",
        .size = 0,
    },
    [MPV_RENDER_PARAM_WL_DISPLAY] = {
        .name = "wl",
        .size = 0,
    },
    [MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE] = {
        .name = "drm_draw_surface_size",
        .size = sizeof(mpv_opengl_drm_draw_surface_size),
    },
    [MPV_RENDER_PARAM_DRM_DISPLAY_V2] = {
        .name = "drm_params_v2",
        .size = sizeof(mpv_opengl_drm_params_v2),
    },
};

static const struct libmpv_gpu_next_context_fns *context_backends[] = {
#if HAVE_D3D11
    &libmpv_gpu_next_context_d3d11,
#endif
#if HAVE_GL
    &libmpv_gpu_next_context_gl,
#endif
    NULL,
};

struct backend_priv {
    struct libmpv_gpu_next_context *context;
    gpu_next *renderer;
};

static void destroy_context(struct libmpv_gpu_next_context **contextp)
{
    struct libmpv_gpu_next_context *context = *contextp;
    if (!context)
        return;

    context->fns->destroy(context);
    talloc_free(context);
    *contextp = NULL;
}

static void cleanup_backend(struct render_backend *ctx)
{
    struct backend_priv *p = ctx->priv;
    if (!p)
        return;

    if (p->renderer) {
        gpu_next_uninit_renderer(p->renderer);
        p->renderer = NULL;
    }

    hwdec_devices_destroy(ctx->hwdec_devs);
    ctx->hwdec_devs = NULL;

    destroy_context(&p->context);
    talloc_free(p);
    ctx->priv = NULL;
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct backend_priv);
    struct backend_priv *p = ctx->priv;

    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api) {
        cleanup_backend(ctx);
        return MPV_ERROR_INVALID_PARAMETER;
    }

    for (int n = 0; context_backends[n]; n++) {
        const struct libmpv_gpu_next_context_fns *backend = context_backends[n];
        if (strcmp(backend->api_name, api) == 0) {
            p->context = talloc_zero(p, struct libmpv_gpu_next_context);
            *p->context = (struct libmpv_gpu_next_context) {
                .global = ctx->global,
                .log = ctx->log,
                .fns = backend,
            };
            break;
        }
    }

    if (!p->context) {
        cleanup_backend(ctx);
        return MPV_ERROR_NOT_IMPLEMENTED;
    }

    int err = p->context->fns->init(p->context, params);
    if (err < 0) {
        cleanup_backend(ctx);
        return err;
    }

    // Forward host-provided native resources (X11/Wayland/DRM) to the ra used
    // by the shared hwdec context, matching the gpu backend behavior.
    for (int n = 0; params && params[n].type; n++) {
        if (params[n].type > 0 &&
            params[n].type < MP_ARRAY_SIZE(native_resource_map) &&
            native_resource_map[params[n].type].name)
        {
            const struct native_resource_entry *entry =
                &native_resource_map[params[n].type];
            void *data = params[n].data;
            if (entry->size)
                data = talloc_memdup(p, data, entry->size);
            ra_add_native_resource(p->context->ra_ctx->ra, entry->name, data);
        }
    }

    ctx->hwdec_devs = hwdec_devices_create();
    p->renderer = gpu_next_init_renderer(ctx->global, ctx->log,
                                         p->context->ra_ctx,
                                         p->context->pllog,
                                         p->context->gpu, NULL,
                                         ctx->hwdec_devs, true,
                                         "libmpv/gpu-next");
    if (!p->renderer) {
        cleanup_backend(ctx);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct backend_priv *p = ctx->priv;
    return gpu_next_check_format(p->renderer, imgfmt);
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct backend_priv *p = ctx->priv;

    switch (param.type) {
    case MPV_RENDER_PARAM_ICC_PROFILE: {
        mpv_byte_array *data = param.data;
        gpu_next_set_icc_profile(p->renderer,
                                 bstrdup(NULL, (bstr){data->data, data->size}));
        return 0;
    }
    case MPV_RENDER_PARAM_AMBIENT_LIGHT: {
        MP_WARN(ctx, "MPV_RENDER_PARAM_AMBIENT_LIGHT is deprecated and might be "
                     "removed in the future (replacement: gamma-auto.lua)\n");
        int lux = *(int *)param.data;
        gpu_next_set_ambient_lux(p->renderer, (double)lux);
        return 0;
    }
    default:
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
}

static int get_info(struct render_backend *ctx, mpv_render_param param,
                    struct vo_frame *frame)
{
    struct backend_priv *p = ctx->priv;

#if HAVE_D3D11
    if (param.type == MPV_RENDER_PARAM_DXGI_COLORSPACE_HINT &&
        p->context->fns->query_target)
    {
        mpv_dxgi_colorspace_hint *hint = param.data;
        *hint = (mpv_dxgi_colorspace_hint){0};

        struct gpu_next_render_target target;
        int err = p->context->fns->query_target(p->context, &target);
        if (err < 0)
            return err;

        struct gpu_next_colorspace_hint res =
            gpu_next_get_colorspace_hint(p->renderer, frame, &target);
        if (res.valid) {
            struct mp_image_params params = {
                .color = res.color,
                .repr = pl_color_repr_rgb,
            };
            pl_color_space_infer(&params.color);
            const struct pl_raw_primaries *prim =
                pl_raw_primaries_get(params.color.primaries);
            hint->state = MPV_DXGI_COLORSPACE_HINT_SET;
            hint->color_space = mp_params_to_dxgi_colorspace(ctx->log, &params);
            hint->bits_per_color = target.color_depth;
            if (prim) {
                hint->primaries[0][0] = prim->red.x;
                hint->primaries[0][1] = prim->red.y;
                hint->primaries[1][0] = prim->green.x;
                hint->primaries[1][1] = prim->green.y;
                hint->primaries[2][0] = prim->blue.x;
                hint->primaries[2][1] = prim->blue.y;
                hint->primaries[3][0] = prim->white.x;
                hint->primaries[3][1] = prim->white.y;
            }
            if (pl_primaries_valid(&params.color.hdr.prim)) {
                hint->primaries[0][0] = params.color.hdr.prim.red.x;
                hint->primaries[0][1] = params.color.hdr.prim.red.y;
                hint->primaries[1][0] = params.color.hdr.prim.green.x;
                hint->primaries[1][1] = params.color.hdr.prim.green.y;
                hint->primaries[2][0] = params.color.hdr.prim.blue.x;
                hint->primaries[2][1] = params.color.hdr.prim.blue.y;
                hint->primaries[3][0] = params.color.hdr.prim.white.x;
                hint->primaries[3][1] = params.color.hdr.prim.white.y;
            }
            hint->min_luma = params.color.hdr.min_luma;
            hint->max_luma = params.color.hdr.max_luma;
            hint->max_cll = params.color.hdr.max_cll;
            hint->max_fall = params.color.hdr.max_fall;
        } else if (!res.enabled) {
            hint->state = MPV_DXGI_COLORSPACE_HINT_CLEAR;
        }

        return 0;
    }
#endif

    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_set_video_params(p->renderer, params);
}

static void reset(struct render_backend *ctx)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_request_reset(p->renderer);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_set_osd_source(p->renderer, vo ? vo->osd : NULL);
    if (vo)
        gpu_next_configure_queue(p->renderer, vo);
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_set_viewport(p->renderer, src, dst, osd);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    struct backend_priv *p = ctx->priv;
    struct gpu_next_render_target target;
    int err = p->context->fns->wrap_target(p->context, params, &target);
    if (err < 0)
        return err;

    *out_w = target.frame.planes[0].texture->params.w;
    *out_h = target.frame.planes[0].texture->params.h;
    p->context->fns->done_frame(p->context, false);
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct backend_priv *p = ctx->priv;
    struct gpu_next_render_target target;
    int err = p->context->fns->wrap_target(p->context, params, &target);
    if (err < 0)
        return err;

    struct gpu_next_render_result result;
    bool ok = gpu_next_render_to_target(p->renderer, frame, &target, &result);
    p->context->fns->done_frame(p->context, frame->display_synced);
    return ok ? 0 : MPV_ERROR_GENERIC;
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    struct backend_priv *p = ctx->priv;
    return gpu_next_get_image(p->renderer, imgfmt, w, h, stride_align, flags);
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_take_screenshot(p->renderer, args);
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    struct backend_priv *p = ctx->priv;
    gpu_next_perfdata(p->renderer, out);
}

static void destroy(struct render_backend *ctx)
{
    cleanup_backend(ctx);
}

const struct render_backend_fns render_backend_gpu_next = {
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .get_info = get_info,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = get_image,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .destroy = destroy,
};
