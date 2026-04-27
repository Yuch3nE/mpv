#include <limits.h>

#include "common/common.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "video/csputils.h"
#include "video/out/gpu/context.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "video/out/vulkan/common.h"
#include "video/out/vulkan/libmpv_vulkan_shared.h"

static int parse_choice_value(const struct m_opt_choice_alternatives *choices,
                              const char *name,
                              int *out)
{
    if (!name || !name[0]) {
        *out = 0;
        return 0;
    }

    for (int n = 0; choices[n].name; n++) {
        if (strcmp(choices[n].name, name) == 0) {
            *out = choices[n].value;
            return 0;
        }
    }

    return -1;
}

static int parse_surface_color(struct mp_log *log,
                               const mpv_vulkan_image *target,
                               struct pl_color_space *out)
{
    *out = (struct pl_color_space){0};

    int primaries = 0;
    int transfer = 0;
    if (parse_choice_value(pl_csp_prim_names, target->surface_primaries,
                           &primaries) < 0 ||
        parse_choice_value(pl_csp_trc_names, target->surface_transfer,
                           &transfer) < 0)
    {
        mp_err(log, "Invalid Vulkan surface colorspace name(s): primaries='%s', transfer='%s'\n",
               target->surface_primaries ? target->surface_primaries : "",
               target->surface_transfer ? target->surface_transfer : "");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    out->primaries = primaries;
    out->transfer = transfer;
    out->hdr.min_luma = target->surface_min_luma;
    out->hdr.max_luma = target->surface_max_luma;
    out->hdr.max_cll = target->surface_max_cll;
    out->hdr.max_fall = target->surface_max_fall;
    return 0;
}

int libmpv_vulkan_shared_vk_format_depth(uint32_t format)
{
    switch ((VkFormat)format) {
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

int libmpv_vulkan_shared_get_target_size(const mpv_vulkan_image *target,
                                         int *out_w,
                                         int *out_h)
{
    if (!target || !target->image || !target->width || !target->height)
        return MPV_ERROR_INVALID_PARAMETER;
    if (target->width > INT_MAX || target->height > INT_MAX)
        return MPV_ERROR_INVALID_PARAMETER;

    *out_w = (int)target->width;
    *out_h = (int)target->height;
    return 0;
}

int libmpv_vulkan_shared_init(struct libmpv_vulkan_shared *shared,
                              void *ta_parent,
                              struct mp_log *log,
                              struct mpv_global *global,
                              mpv_render_param *params)
{
    *shared = (struct libmpv_vulkan_shared){0};

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

    shared->pllog = mppl_log_create(ta_parent, log);
    if (!shared->pllog)
        return MPV_ERROR_UNSUPPORTED;
    mppl_log_set_probing(shared->pllog, false);

    shared->vulkan = pl_vulkan_import(shared->pllog, pl_vulkan_import_params(
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
    if (!shared->vulkan) {
        pl_log_destroy(&shared->pllog);
        return MPV_ERROR_UNSUPPORTED;
    }

    shared->ra_ctx = talloc_zero(ta_parent, struct ra_ctx);
    shared->ra_ctx->global = global;
    shared->ra_ctx->log = log;
    shared->ra_ctx->opts = (struct ra_ctx_opts) {
        .allow_sw = true,
    };
    shared->ra_ctx->ra = ra_create_pl(shared->vulkan->gpu, log);
    if (!shared->ra_ctx->ra) {
        libmpv_vulkan_shared_uninit(shared);
        return MPV_ERROR_UNSUPPORTED;
    }
    talloc_steal(shared->ra_ctx, shared->ra_ctx->ra);
    return 0;
}

void libmpv_vulkan_shared_uninit(struct libmpv_vulkan_shared *shared)
{
    if (shared->vulkan)
        pl_vulkan_destroy(&shared->vulkan);
    if (shared->pllog)
        pl_log_destroy(&shared->pllog);
    talloc_free(shared->ra_ctx);
    *shared = (struct libmpv_vulkan_shared){0};
}

int libmpv_vulkan_shared_set_target_state(struct libmpv_vulkan_shared *shared,
                                          struct mp_log *log,
                                          const mpv_vulkan_image *target,
                                          int depth_fallback)
{
    // Reject obviously invalid HDR luminance values (negative or NaN). Zero
    // means "unset" and is allowed.
    float lumas[] = {
        target->surface_min_luma, target->surface_max_luma,
        target->surface_max_cll, target->surface_max_fall,
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(lumas); i++) {
        if (lumas[i] < 0 || lumas[i] != lumas[i]) {
            mp_err(log, "Invalid Vulkan surface luminance value: %f\n",
                   lumas[i]);
            return MPV_ERROR_INVALID_PARAMETER;
        }
    }

    struct pl_color_space surface_color;
    int err = parse_surface_color(log, target, &surface_color);
    if (err < 0)
        return err;

    int color_depth = libmpv_vulkan_shared_vk_format_depth(target->format);
    if (!color_depth)
        color_depth = depth_fallback;

    shared->surface_color = surface_color;
    shared->surface_color_depth = color_depth;
    shared->has_target_state = true;
    return 0;
}