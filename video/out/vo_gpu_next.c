/*
 * Copyright (C) 2021 Niklas Haas
 *
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
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include "ta/ta_talloc.h"

#include "gpu_next/video.h"

struct gpu_next_priv {
    struct ra_ctx *ra_ctx;
    struct gpu_ctx *context;
    gpu_next *renderer;
    struct mp_image_params target_params;
    bool frame_pending;
};

static void load_hwdec_api(void *ctx, struct hwdec_imgfmt_request *params)
{
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, params);
}

static void update_ra_ctx_options(struct vo *vo, struct ra_ctx_opts *ctx_opts)
{
    struct gl_video_opts *gl_opts = mp_get_config_group(vo, vo->global,
                                                        &gl_video_conf);
    struct gl_next_opts *next_opts = mp_get_config_group(vo, vo->global,
                                                         &gl_next_conf);
    bool border_alpha = (next_opts->border_background == BACKGROUND_COLOR &&
                         gl_opts->background_color.a != 255) ||
                        next_opts->border_background == BACKGROUND_NONE;
    ctx_opts->want_alpha = (gl_opts->background == BACKGROUND_COLOR &&
                            gl_opts->background_color.a != 255) ||
                           gl_opts->background == BACKGROUND_NONE ||
                           border_alpha;
    talloc_free(gl_opts);
    talloc_free(next_opts);
}

static void resize(struct vo *vo)
{
    struct gpu_next_priv *p = vo->priv;
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    if (vo->dwidth && vo->dheight) {
        gpu_ctx_resize(p->context, vo->dwidth, vo->dheight);
        vo->want_redraw = true;
    }

    gpu_next_set_viewport(p->renderer, &src, &dst, &osd);
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct gpu_next_priv *p = vo->priv;
    if (!p->ra_ctx->fns->reconfig(p->ra_ctx))
        return -1;

    resize(vo);
    gpu_next_set_video_params(p->renderer, params);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = NULL;
    mp_mutex_unlock(&vo->params_mutex);
    return 0;
}

static int preinit(struct vo *vo)
{
    struct gpu_next_priv *p = vo->priv;

    struct ra_ctx_opts *ctx_opts = mp_get_config_group(vo, vo->global,
                                                       &ra_ctx_conf);
    update_ra_ctx_options(vo, ctx_opts);
    p->context = gpu_ctx_create(vo, ctx_opts);
    talloc_free(ctx_opts);
    if (!p->context)
        goto err_out;

    p->ra_ctx = p->context->ra_ctx;

    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_set_loader(vo->hwdec_devs, load_hwdec_api, vo);
    p->renderer = gpu_next_init_renderer(vo->global, vo->log, p->ra_ctx,
                                         p->context->pllog, p->context->gpu,
                                         p->context->swapchain,
                                         vo->hwdec_devs, false, "vo/gpu-next");
    if (!p->renderer)
        goto err_out;

    gpu_next_set_osd_source(p->renderer, vo->osd);
    gpu_next_configure_queue(p->renderer, vo);
    return 0;

err_out:
    if (vo->hwdec_devs) {
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    if (p->renderer) {
        gpu_next_uninit_renderer(p->renderer);
        p->renderer = NULL;
    }
    gpu_ctx_destroy(&p->context);
    p->ra_ctx = NULL;
    return -1;
}

static void uninit(struct vo *vo)
{
    struct gpu_next_priv *p = vo->priv;
    if (vo->hwdec_devs) {
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }

    if (p->renderer) {
        gpu_next_uninit_renderer(p->renderer);
        p->renderer = NULL;
    }

    gpu_ctx_destroy(&p->context);
    p->ra_ctx = NULL;
    p->frame_pending = false;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct gpu_next_priv *p = vo->priv;
    if (gpu_next_refresh_options(p->renderer))
        gpu_next_configure_queue(p->renderer, vo);

    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    struct pl_swapchain_frame swframe;
    bool should_draw = sw->fns->start_frame(sw, NULL);
    if (!should_draw || !gpu_next_start_frame(p->renderer, &swframe)) {
        gpu_next_skip_render(p->renderer, frame);
        return VO_FALSE;
    }

    struct gpu_next_render_target target = {
        .frame = (struct pl_frame){0},
        .ra_swapchain = sw,
        .swapchain = NULL,
        .surface_color = sw->fns->target_csp ? sw->fns->target_csp(sw)
                                             : (struct pl_color_space){0},
        .color_depth = sw->fns->color_depth ? sw->fns->color_depth(sw) : 0,
        .flip_y = false,
    };
    pl_frame_from_swapchain(&target.frame, &swframe);

    struct gpu_next_render_result result = {0};
    gpu_next_render_to_target(p->renderer, frame, &target, &result);

    mp_mutex_lock(&vo->params_mutex);
    p->target_params = result.target_params;
    vo->target_params = &p->target_params;
    if (vo->params) {
        vo->params->color.hdr = result.video_hdr;
        vo->has_peak_detect_values = result.has_peak_detect_values;
    }
    mp_mutex_unlock(&vo->params_mutex);

    p->frame_pending = true;
    return VO_TRUE;
}

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align, int flags)
{
    struct gpu_next_priv *p = vo->priv;
    return gpu_next_get_image(p->renderer, imgfmt, w, h, stride_align, flags);
}

static void flip_page(struct vo *vo)
{
    struct gpu_next_priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;

    if (p->frame_pending) {
        if (!gpu_next_submit_frame(p->renderer))
            MP_ERR(vo, "Failed presenting frame!\n");
        p->frame_pending = false;
    }

    sw->fns->swap_buffers(sw);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct gpu_next_priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    if (sw->fns->get_vsync)
        sw->fns->get_vsync(sw, info);
}

static int query_format(struct vo *vo, int format)
{
    struct gpu_next_priv *p = vo->priv;
    return gpu_next_check_format(p->renderer, format);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct gpu_next_priv *p = vo->priv;
    gpu_next *renderer = p->renderer;

    switch (request) {
    case VOCTRL_SET_PANSCAN:
        resize(vo);
        return VO_TRUE;
    case VOCTRL_PAUSE:
        if (gpu_next_showing_interpolated_frame(renderer))
            vo->want_redraw = true;
        return VO_TRUE;

    case VOCTRL_UPDATE_RENDER_OPTS: {
        update_ra_ctx_options(vo, &p->ra_ctx->opts);
        if (p->ra_ctx->fns->update_render_opts)
            p->ra_ctx->fns->update_render_opts(p->ra_ctx);
        vo->want_redraw = true;

        if (gpu_next_refresh_options(renderer))
            gpu_next_configure_queue(renderer, vo);

        // Also re-query the auto profile, in case `update_render_options`
        // unloaded a manually specified icc profile in favor of
        // icc-profile-auto
        int events = 0;
        gpu_next_update_auto_profile(renderer, &events);
        vo_event(vo, events);
        return VO_TRUE;
    }

    case VOCTRL_RESET:
        gpu_next_request_reset(renderer);
        return VO_TRUE;

    case VOCTRL_PERFORMANCE_DATA: {
        gpu_next_perfdata(renderer, data);
        return true;
    }

    case VOCTRL_SCREENSHOT:
        gpu_next_take_screenshot(renderer, data);
        return true;

    case VOCTRL_EXTERNAL_RESIZE:
        reconfig(vo, NULL);
        return true;

    case VOCTRL_LOAD_HWDEC_API:
        gpu_next_load_hwdec_api(renderer, vo->hwdec_devs, data);
        return true;
    }

    int events = 0;
    int r = p->ra_ctx->fns->control(p->ra_ctx, &events, request, data);
    if (events & VO_EVENT_ICC_PROFILE_CHANGED) {
        if (gpu_next_update_auto_profile(renderer, &events))
            vo->want_redraw = true;
    }
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return r;
}

static void wakeup(struct vo *vo)
{
    struct gpu_next_priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wakeup)
        p->ra_ctx->fns->wakeup(p->ra_ctx);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct gpu_next_priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wait_events) {
        p->ra_ctx->fns->wait_events(p->ra_ctx, until_time_ns);
    } else {
        vo_wait_default(vo, until_time_ns);
    }
}

const struct vo_driver video_out_gpu_next = {
    .description = "Video output based on libplacebo",
    .name = "gpu-next",
    .caps = VO_CAP_ROTATE90 |
            VO_CAP_FILM_GRAIN |
            VO_CAP_VFLIP |
            0x0,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct gpu_next_priv),
};
