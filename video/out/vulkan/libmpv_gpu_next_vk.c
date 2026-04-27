#include "config.h"

#include "common/common.h"
#include "common/msg.h"
#include "mpv/render_vk.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"
#include "video/out/vulkan/libmpv_vulkan_shared.h"

struct priv {
    struct libmpv_vulkan_shared shared;
    pl_tex wrapped_target;

    uint64_t wrapped_image;
    uint32_t wrapped_width;
    uint32_t wrapped_height;
    uint32_t wrapped_format;
    uint32_t wrapped_usage;
    uint32_t wrapped_aspect;

    mpv_vulkan_image current_target;
};

#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES
#define MP_VK_HANDLE(type, value) ((type)(uintptr_t)(value))
#else
#define MP_VK_HANDLE(type, value) ((type)(value))
#endif

static void clear_wrapped_target_state(struct priv *p)
{
    p->wrapped_image = 0;
    p->wrapped_width = 0;
    p->wrapped_height = 0;
    p->wrapped_format = 0;
    p->wrapped_usage = 0;
    p->wrapped_aspect = 0;
    p->current_target = (mpv_vulkan_image){0};
}

static void destroy_wrapped_target(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->wrapped_target)
        pl_tex_destroy(ctx->gpu, &p->wrapped_target);

    clear_wrapped_target_state(p);
}

static bool same_wrapped_target(struct priv *p, const mpv_vulkan_image *target)
{
    return p->wrapped_target &&
           p->wrapped_image == target->image &&
           p->wrapped_width == target->width &&
           p->wrapped_height == target->height &&
           p->wrapped_format == target->format &&
           p->wrapped_usage == target->usage &&
           p->wrapped_aspect == target->aspect;
}

static int get_target_size(struct libmpv_gpu_next_context *ctx,
                           mpv_render_param *params,
                           int *out_w,
                           int *out_h)
{
    return libmpv_vulkan_shared_get_target_size(
        get_mpv_render_param(params, MPV_RENDER_PARAM_VK_IMAGE, NULL),
        out_w, out_h);
}

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    int err = libmpv_vulkan_shared_init(&p->shared, p, ctx->log,
                                        ctx->global, params);
    if (err < 0)
        return err;

    ctx->ra_ctx = p->shared.ra_ctx;
    ctx->pllog = p->shared.pllog;
    ctx->gpu = p->shared.vulkan->gpu;
    return 0;
}

static int set_parameter(struct libmpv_gpu_next_context *ctx,
                         mpv_render_param param)
{
    struct priv *p = ctx->priv;

    if (param.type == MPV_RENDER_PARAM_VK_TARGET_STATE) {
        mpv_vulkan_image *target = param.data;
        if (!target)
            return MPV_ERROR_INVALID_PARAMETER;
        return libmpv_vulkan_shared_set_target_state(&p->shared, ctx->log,
                                                     target, 0);
    }

    return MPV_ERROR_NOT_IMPLEMENTED;
}

static int query_target(struct libmpv_gpu_next_context *ctx,
                        struct gpu_next_render_target *out)
{
    struct priv *p = ctx->priv;
    *out = (struct gpu_next_render_target) {
        .surface_color = p->shared.surface_color,
        .color_depth = p->shared.surface_color_depth,
        .flip_y = false,
    };
    return 0;
}

static int wrap_target(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params,
                       struct gpu_next_render_target *out)
{
    struct priv *p = ctx->priv;
    *out = (struct gpu_next_render_target){0};

    mpv_vulkan_image *target =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VK_IMAGE, NULL);
    if (!target || !target->image || !target->width || !target->height ||
        !target->format || !target->usage || !target->signal_semaphore)
    {
        return MPV_ERROR_INVALID_PARAMETER;
    }

    int err = libmpv_vulkan_shared_set_target_state(
        &p->shared, ctx->log, target,
        GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH, int, 0));
    if (err < 0)
        return err;

    if (!same_wrapped_target(p, target))
        destroy_wrapped_target(ctx);

    if (!p->wrapped_target) {
        p->wrapped_target = pl_vulkan_wrap(ctx->gpu, pl_vulkan_wrap_params(
            .image = MP_VK_HANDLE(VkImage, target->image),
            .aspect = target->aspect,
            .width = (int)target->width,
            .height = (int)target->height,
            .depth = 0,
            .format = (VkFormat)target->format,
            .usage = (VkImageUsageFlags)target->usage
        ));
        if (!p->wrapped_target)
            return MPV_ERROR_GENERIC;

        p->wrapped_image = target->image;
        p->wrapped_width = target->width;
        p->wrapped_height = target->height;
        p->wrapped_format = target->format;
        p->wrapped_usage = target->usage;
        p->wrapped_aspect = target->aspect;
    }

    pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
        .tex = p->wrapped_target,
        .layout = (VkImageLayout)target->in_layout,
        .qf = target->in_qf,
        .semaphore = {
            .sem = target->wait_semaphore
                ? MP_VK_HANDLE(VkSemaphore, target->wait_semaphore)
                : VK_NULL_HANDLE,
            .value = target->wait_value,
        }
    ));

    p->current_target = *target;

    int components = 4;
    if (p->wrapped_target->params.format)
        components = p->wrapped_target->params.format->num_components;

    int color_depth = p->shared.surface_color_depth;

    struct pl_color_space frame_color = p->shared.surface_color;
    if (frame_color.transfer == PL_COLOR_TRC_UNKNOWN &&
        frame_color.primaries == PL_COLOR_PRIM_UNKNOWN)
    {
        frame_color = pl_color_space_srgb;
    }

    *out = (struct gpu_next_render_target) {
        .frame = {
            .color = frame_color,
            .repr = pl_color_repr_rgb,
            .num_planes = 1,
            .planes = {
                {
                    .texture = p->wrapped_target,
                    .components = components,
                    .component_mapping = {0, 1, 2, 3},
                },
            },
        },
        .surface_color = p->shared.surface_color,
        .color_depth = color_depth,
        .flip_y = false,
    };
    return 0;
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool ds)
{
    struct priv *p = ctx->priv;
    if (!p || !p->wrapped_target)
        return;

    if (!pl_vulkan_hold_ex(ctx->gpu, pl_vulkan_hold_params(
            .tex = p->wrapped_target,
            .layout = (VkImageLayout)p->current_target.out_layout,
            .qf = p->current_target.out_qf,
            .semaphore = {
                .sem = MP_VK_HANDLE(VkSemaphore, p->current_target.signal_semaphore),
                .value = p->current_target.signal_value,
            }
        )))
    {
        MP_ERR(ctx, "Failed to return the rendered VkImage to the host.\n");
        destroy_wrapped_target(ctx);
    }
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;

    destroy_wrapped_target(ctx);
    if (p)
        libmpv_vulkan_shared_uninit(&p->shared);

    talloc_free(p);
    ctx->priv = NULL;
    ctx->ra_ctx = NULL;
    ctx->pllog = NULL;
    ctx->gpu = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk = {
    .api_name = MPV_RENDER_API_TYPE_VULKAN,
    .init = init,
    .set_parameter = set_parameter,
    .query_target = query_target,
    .get_target_size = get_target_size,
    .wrap_target = wrap_target,
    .done_frame = done_frame,
    .destroy = destroy,
};