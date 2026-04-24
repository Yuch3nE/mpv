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
    int (*query_target)(struct libmpv_gpu_next_context *ctx,
                        struct gpu_next_render_target *out);
    int (*wrap_target)(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params,
                       struct gpu_next_render_target *out);
    void (*done_frame)(struct libmpv_gpu_next_context *ctx, bool ds);
    void (*destroy)(struct libmpv_gpu_next_context *ctx);
};

extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl;
extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11;
