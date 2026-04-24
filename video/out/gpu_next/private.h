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

#pragma once

#include "video.h"
#include "osd.h"

struct scaler_params {
    struct pl_filter_config config;
};

struct user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct frame_info {
    int count;
    struct pl_dispatch_info info[VO_PASS_PERF_MAX];
};

struct cache {
    struct mp_log *log;
    struct mpv_global *global;
    char *dir;
    const char *name;
    size_t size_limit;
    pl_cache cache;
};

struct frame_priv {
    struct priv *p;
    struct gpu_next_osd_state subs;
    uint64_t osd_sync;
    struct ra_hwdec *hwdec;
};

struct priv {
    struct mp_log *log;
    struct mpv_global *global;
    struct stats_ctx *stats;
    struct osd_state *osd;
    struct ra_ctx *ra_ctx;
    struct ra_hwdec_ctx hwdec_ctx;
    struct ra_hwdec_mapper *hwdec_mapper;
    struct timer_pool *hwdec_timer;
    struct mp_pass_perf hwdec_perf;
    struct timer_pool *sw_upload_timer;
    struct mp_pass_perf sw_upload_perf;

    mp_mutex dr_lock;
    pl_buf *dr_buffers;
    int num_dr_buffers;

    pl_log pllog;
    pl_gpu gpu;
    pl_renderer rr;
    pl_queue queue;
    pl_swapchain sw;
    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    pl_tex *sub_tex;
    int num_sub_tex;

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    struct gpu_next_osd_state osd_state;

    uint64_t last_id;
    uint64_t osd_sync;
    double last_pts;
    bool is_interpolated;
    bool want_reset;

    pl_options pars;
    struct m_config_cache *opts_cache;
    struct m_config_cache *next_opts_cache;
    struct gl_next_opts *next_opts;
    struct cache shader_cache, icc_cache;
    struct mp_csp_equalizer_state *video_eq;
    struct scaler_params scalers[SCALER_COUNT];
    const struct pl_hook **hooks;
    enum pl_color_levels output_levels;

    struct pl_icc_params icc_params;
    char *icc_path;
    pl_icc_object icc_profile;

    struct user_hook *user_hooks;
    int num_user_hooks;

    struct frame_info perf_fresh;
    struct frame_info perf_redraw;

    struct mp_image_params target_params;
    struct mp_image_params video_params;

    double ambient_lux;
    bool warned_no_color_hint;
};