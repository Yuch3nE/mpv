#include <limits.h>

#include "config.h"

#include <libplacebo/config.h>

#include "common/common.h"
#include "common/msg.h"
#include "mpv/render_vk.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/gpu_next/video.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "video/out/vulkan/common.h"

struct priv {
    pl_vulkan vulkan;
    struct ra_ctx *ra_ctx;
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

static int vk_format_depth(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return 10;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
        return 16;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return 8;
    default:
        return 0;
    }
}

static int get_target_size(struct libmpv_gpu_next_context *ctx,
                           mpv_render_param *params,
                           int *out_w,
                           int *out_h)
{
    mpv_vulkan_image *target =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VK_IMAGE, NULL);
    if (!target || !target->image || !target->width || !target->height)
        return MPV_ERROR_INVALID_PARAMETER;
    if (target->width > INT_MAX || target->height > INT_MAX)
        return MPV_ERROR_INVALID_PARAMETER;

    *out_w = (int)target->width;
    *out_h = (int)target->height;
    return 0;
}

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    mpv_vulkan_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!init_params || !init_params->instance || !init_params->phys_device ||
        !init_params->device || !init_params->get_proc_addr ||
        !init_params->enabled_features || !init_params->queue_count ||
        (!!init_params->lock_queue != !!init_params->unlock_queue) ||
        init_params->num_enabled_device_extensions > INT_MAX)
    {
        return MPV_ERROR_INVALID_PARAMETER;
    }

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_UNSUPPORTED;
    mppl_log_set_probing(ctx->pllog, false);

    p->vulkan = pl_vulkan_import(ctx->pllog, pl_vulkan_import_params(
        .instance = (VkInstance)init_params->instance,
        .get_proc_addr = (PFN_vkGetInstanceProcAddr)init_params->get_proc_addr,
        .phys_device = (VkPhysicalDevice)init_params->phys_device,
        .device = (VkDevice)init_params->device,
        .extensions = init_params->enabled_device_extensions,
        .num_extensions = (int)init_params->num_enabled_device_extensions,
        .queue_graphics = {
            .index = init_params->queue_family_index,
            .count = init_params->queue_count,
        },
        .queue_compute = {
            .index = init_params->queue_family_index,
            .count = init_params->queue_count,
        },
        .queue_transfer = {
            .index = init_params->queue_family_index,
            .count = init_params->queue_count,
        },
        .features = (const VkPhysicalDeviceFeatures2 *)init_params->enabled_features,
        .lock_queue = init_params->lock_queue,
        .unlock_queue = init_params->unlock_queue,
        .queue_ctx = init_params->queue_ctx,
    ));
    if (!p->vulkan)
        return MPV_ERROR_UNSUPPORTED;

    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->log = ctx->log;
    p->ra_ctx->opts = (struct ra_ctx_opts) {
        .allow_sw = true,
    };
    p->ra_ctx->ra = ra_create_pl(p->vulkan->gpu, ctx->log);
    if (!p->ra_ctx->ra)
        return MPV_ERROR_UNSUPPORTED;
    talloc_steal(p->ra_ctx, p->ra_ctx->ra);

    ctx->ra_ctx = p->ra_ctx;
    ctx->gpu = p->vulkan->gpu;
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

    int color_depth = vk_format_depth((VkFormat)target->format);
    if (!color_depth)
        color_depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH,
                                           int, 0);

    *out = (struct gpu_next_render_target) {
        .frame = {
            .color = {0},
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
        .surface_color = {0},
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
    if (p && p->vulkan)
        pl_vulkan_destroy(&p->vulkan);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);

    talloc_free(p);
    ctx->priv = NULL;
    ctx->ra_ctx = NULL;
    ctx->gpu = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk = {
    .api_name = MPV_RENDER_API_TYPE_VULKAN,
    .init = init,
    .get_target_size = get_target_size,
    .wrap_target = wrap_target,
    .done_frame = done_frame,
    .destroy = destroy,
};