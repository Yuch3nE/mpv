/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

#include <stddef.h>
#include <stdint.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vulkan backend
 * --------------
 *
 * This header contains definitions for using Vulkan with the render.h API.
 *
 * The Vulkan backend is intended to be used by hosts that already manage a
 * Vulkan instance and device and want mpv to render into Vulkan images they
 * own. The most common use case is bridging mpv into a host renderer that
 * cannot use OpenGL directly (for example a Metal compositor on macOS via
 * MoltenVK, or a Vulkan-only engine on Linux/Windows).
 *
 * Unlike MPV_RENDER_API_TYPE_OPENGL, the Vulkan backend does not own the
 * graphics device. The host MUST supply a fully created VkInstance,
 * VkPhysicalDevice, and VkDevice through MPV_RENDER_PARAM_VULKAN_INIT_PARAMS,
 * together with the queue family/count information and the exact enabled
 * Vulkan feature chain that the VkDevice was created with. The same VkDevice
 * must remain valid for the entire lifetime of the mpv_render_context.
 *
 * Reusing the host VkDevice (instead of creating a new one) is mandatory for
 * any zero-copy interop, e.g. importing MTLTexture / DXGI / dma-buf objects
 * into Vulkan, because Vulkan external memory handles are not portable across
 * devices.
 *
 * Use mpv_render_context_create() with MPV_RENDER_PARAM_API_TYPE set to
 * MPV_RENDER_API_TYPE_VULKAN, and MPV_RENDER_PARAM_VULKAN_INIT_PARAMS
 * provided. To render a frame, call mpv_render_context_render() with
 * MPV_RENDER_PARAM_VK_IMAGE describing the destination VkImage.
 *
 * Threading
 * ---------
 *
 * The Vulkan backend follows the general render.h threading rules. There is
 * no implicit per-thread Vulkan state, so mpv_render_* functions may be
 * called from any thread, as long as no two of them run concurrently for
 * the same context.
 *
 * Synchronization with the host renderer is done explicitly via
 * VkSemaphores carried by mpv_vulkan_image, see below.
 */

typedef void (*mpv_vulkan_queue_control_fn)(void *ctx,
                                            uint32_t queue_family_index,
                                            uint32_t queue_index);

/**
 * For initializing the mpv Vulkan backend via
 * MPV_RENDER_PARAM_VULKAN_INIT_PARAMS.
 *
 * All Vulkan handles are passed as opaque pointers / 64-bit integers so that
 * this header does not require <vulkan/vulkan.h>. The expected concrete
 * Vulkan types are documented per field.
 */
typedef struct mpv_vulkan_init_params {
    /**
     * VkInstance the host has created. Must be non-NULL.
     * Type: VkInstance.
     */
    void *instance;

    /**
     * VkPhysicalDevice that backs the device below. Must be non-NULL.
     * Type: VkPhysicalDevice.
     */
    void *phys_device;

    /**
     * VkDevice the host has created on top of phys_device. Must be non-NULL.
     * mpv will not destroy this device; the host owns its lifetime.
     * Type: VkDevice.
     */
    void *device;

    /**
     * Index of the queue family on phys_device that mpv/libplacebo is allowed
     * to submit graphics + compute work to. The queue family must support
     * both VK_QUEUE_GRAPHICS_BIT and VK_QUEUE_COMPUTE_BIT.
     */
    uint32_t queue_family_index;

    /**
     * Number of queues that were enabled from queue_family_index when `device`
     * was created. This is required because libplacebo imports queue families,
     * not a single VkQueue handle.
     *
     * If the host wants mpv to use a dedicated queue, it should create the
     * VkDevice with exactly one queue enabled in this family and pass 1 here.
     */
    uint32_t queue_count;

    /**
     * Required: function pointer to vkGetInstanceProcAddr for the loader the
     * host uses. mpv (libplacebo) loads all other Vulkan entry points
     * through this. Using the host loader here is what guarantees mpv talks
     * to the same ICD as the host application (on macOS this is typically
     * MoltenVK shipped inside the host bundle).
     *
     * Type: PFN_vkGetInstanceProcAddr.
     */
    void *get_proc_addr;

    /**
     * Required: exact VkPhysicalDeviceFeatures2 chain that was enabled when
     * creating `device`.
     *
     * Type: const VkPhysicalDeviceFeatures2* (or a pointer to a compatible
     * pNext chain rooted in VkPhysicalDeviceFeatures2).
     *
     * This is needed because Vulkan provides no API to query the enabled
     * feature set from an existing VkDevice, while libplacebo must know it
     * when importing the device.
     */
    const void *enabled_features;

    /**
     * Optional queue locking callbacks used when the host and mpv may submit
     * work to the same VkDevice concurrently.
     *
     * If set, both callbacks must be provided. mpv/libplacebo will call them
     * around queue submission work for the queue family/index it selected.
     * This lets the host serialize its own queue access with mpv without
     * forcing mpv to own the VkQueue directly.
     *
     * If omitted, the host must guarantee that it does not submit work
     * concurrently to the queues mpv is using, or dedicate the family to mpv
     * by creating the device with a single queue in that family.
     */
    mpv_vulkan_queue_control_fn lock_queue;
    mpv_vulkan_queue_control_fn unlock_queue;
    void *queue_ctx;

    /**
     * Optional: list of Vulkan device extensions the host enabled when
     * creating `device`. mpv passes this to libplacebo so it can opt into
     * extension-dependent code paths (external memory/semaphores, HDR
     * metadata, ...). Pointer + count semantics; pointers must remain valid
     * only during mpv_render_context_create().
     *
     * If NULL/0, mpv will assume no optional extensions are available.
     */
    const char *const *enabled_device_extensions;
    size_t num_enabled_device_extensions;

    /**
     * Optional: list of Vulkan instance extensions the host enabled when
     * creating `instance`. Same lifetime rules as above.
     */
    const char *const *enabled_instance_extensions;
    size_t num_enabled_instance_extensions;
} mpv_vulkan_init_params;

/**
 * Describes a VkImage render target for mpv_render_context_render().
 * Pass via MPV_RENDER_PARAM_VK_IMAGE.
 *
 * The host owns the VkImage. mpv will only read/write within the area
 * implied by `width`/`height` and will only ever transition the image to
 * the layouts described below. The host must also describe the image's
 * VkImageUsageFlags exactly, because libplacebo derives renderability and
 * sampling capabilities from them.
 *
 * Ownership transfer is controlled explicitly with `in_qf` / `out_qf`.
 * Use VK_QUEUE_FAMILY_EXTERNAL when handing the image over from or back to a
 * non-Vulkan API (e.g. Metal). Use VK_QUEUE_FAMILY_IGNORED if no queue family
 * ownership transfer is required, for example because the image was created
 * with VK_SHARING_MODE_CONCURRENT or because producer/consumer already agree
 * on the same queue family.
 *
 * Synchronization model
 * ---------------------
 *
 * mpv expects strict explicit synchronization:
 *
 *   1. Before mpv submits any rendering commands, it will issue a
 *      vkQueueSubmit that waits on `wait_semaphore` (if non-zero). The host
 *      MUST signal this semaphore from whatever queue last produced or
 *      released the image. If `wait_semaphore` is 0, mpv assumes the image
 *      is immediately available.
 *
 *   2. When mpv finishes rendering, it will signal `signal_semaphore`.
 *      The host MUST wait on this semaphore on whatever queue or API
 *      subsequently consumes the image. This semaphore is required, because
 *      libplacebo's external-image hold/release protocol needs an explicit
 *      handoff point to return ownership to the host.
 *
 *   3. Both wait_value and signal_value are interpreted as timeline
 *      semaphore payloads. Set them to 0 for binary semaphores (default).
 *      If timeline semaphores are used, the host MUST have enabled
 *      VK_KHR_timeline_semaphore on the device.
 */
typedef struct mpv_vulkan_image {
    /**
     * VkImage to render into. Must be non-NULL.
     * Type: VkImage (non-dispatchable handle).
     */
    uint64_t image;

    /**
     * Pixel dimensions of `image`. Must match the actual image extent.
     */
    uint32_t width;
    uint32_t height;

    /**
     * VkFormat the image was created with. Must match exactly.
     * Type: VkFormat (uint32_t per Vulkan spec).
     */
    uint32_t format;

    /**
     * Exact VkImageUsageFlags used when creating `image`.
     * Type: VkImageUsageFlags (uint32_t per Vulkan spec).
     */
    uint32_t usage;

    /**
     * Which aspect of `image` to wrap. Leave as 0 for normal color images,
     * which defaults to the full color aspect.
     * Type: VkImageAspectFlags (uint32_t per Vulkan spec).
     */
    uint32_t aspect;

    /**
     * Layout the host hands the image over to mpv in. mpv assumes the image
     * is currently in this layout when its first command sees it.
     * Typical value: VK_IMAGE_LAYOUT_UNDEFINED if the host does not care
     * about preserving previous contents, otherwise an explicit layout
     * such as VK_IMAGE_LAYOUT_GENERAL or VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
     * Type: VkImageLayout (uint32_t per Vulkan spec).
     */
    uint32_t in_layout;

    /**
     * Queue family that currently owns `image` when mpv starts using it.
     * Type: uint32_t queue family index, VK_QUEUE_FAMILY_EXTERNAL, or
     * VK_QUEUE_FAMILY_IGNORED.
     */
    uint32_t in_qf;

    /**
     * Layout mpv must transition the image into before signalling
     * signal_semaphore / returning. The host should pick whatever layout
     * its consumer expects (e.g. VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
     * for sampling, or VK_IMAGE_LAYOUT_PRESENT_SRC_KHR for direct
     * presentation through a swapchain the host owns).
     */
    uint32_t out_layout;

    /**
     * Queue family ownership that mpv must release `image` to before
     * signalling signal_semaphore / returning.
     * Type: uint32_t queue family index, VK_QUEUE_FAMILY_EXTERNAL, or
     * VK_QUEUE_FAMILY_IGNORED.
     */
    uint32_t out_qf;

    /**
     * VkSemaphore mpv must wait on before reading/writing `image`. May be 0.
     * Type: VkSemaphore (non-dispatchable handle).
     */
    uint64_t wait_semaphore;

    /**
     * Timeline value for wait_semaphore. Ignored unless wait_semaphore is a
     * timeline semaphore. Set to 0 for binary semaphores.
     */
    uint64_t wait_value;

    /**
    * VkSemaphore mpv must signal once rendering and the layout transition
    * to out_layout are complete. This field is required and must be non-0.
     */
    uint64_t signal_semaphore;

    /**
     * Timeline value for signal_semaphore. Ignored for binary semaphores.
     */
    uint64_t signal_value;
} mpv_vulkan_image;

#ifdef __cplusplus
}
#endif

#endif
