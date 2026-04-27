#pragma once

#include <stdbool.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>

#include "mpv/render_vk.h"

struct mp_log;
struct mpv_global;
struct ra_ctx;

struct libmpv_vulkan_shared {
    struct ra_ctx *ra_ctx;
    pl_log pllog;
    pl_vulkan vulkan;
    struct pl_color_space surface_color;
    int surface_color_depth;
    bool has_target_state;
};

int libmpv_vulkan_shared_init(struct libmpv_vulkan_shared *shared,
                              void *ta_parent,
                              struct mp_log *log,
                              struct mpv_global *global,
                              mpv_render_param *params);
void libmpv_vulkan_shared_uninit(struct libmpv_vulkan_shared *shared);
int libmpv_vulkan_shared_set_target_state(struct libmpv_vulkan_shared *shared,
                                          struct mp_log *log,
                                          const mpv_vulkan_image *target,
                                          int depth_fallback);
int libmpv_vulkan_shared_get_target_size(const mpv_vulkan_image *target,
                                         int *out_w,
                                         int *out_h);
int libmpv_vulkan_shared_vk_format_depth(uint32_t format);