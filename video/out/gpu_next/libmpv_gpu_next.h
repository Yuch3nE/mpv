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

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include "video/out/libmpv.h"

struct ra_ctx;
struct gpu_next_render_target;

struct libmpv_gpu_next_context {
    struct mpv_global *global;
    struct mp_log *log;
    const struct libmpv_gpu_next_context_fns *fns;

    struct ra_ctx *ra_ctx;
    pl_log pllog;
    pl_gpu gpu;
    void *priv;
};

struct libmpv_gpu_next_context_fns {
    const char *api_name;
    int (*init)(struct libmpv_gpu_next_context *ctx, mpv_render_param *params);
    int (*wrap_target)(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params,
                       struct gpu_next_render_target *out);
    void (*done_frame)(struct libmpv_gpu_next_context *ctx, bool ds);
    void (*destroy)(struct libmpv_gpu_next_context *ctx);
};

extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl;
