#include <string.h>

#include "config.h"
#include "mpv/client.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"

static const struct libmpv_gpu_next_context_fns *context_backends[] = {
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
        talloc_free(p->renderer);
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

    ctx->hwdec_devs = hwdec_devices_create();
    p->renderer = gpu_next_init_renderer(ctx->global, ctx->log,
                                         p->context->ra_ctx,
                                         p->context->pllog,
                                         p->context->gpu, NULL,
                                         ctx->hwdec_devs,
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
    gpu_next_render_to_target(p->renderer, frame, &target, &result);
    p->context->fns->done_frame(p->context, frame->display_synced);
    return 0;
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
