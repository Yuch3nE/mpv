#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/frame_queue.h>

#include "common/common.h"
#include "common/stats.h"
#include "misc/bstr.h"
#include "options/m_config.h"
#include "options/options.h"
#include "osdep/threads.h"
#include "sub/draw_bmp.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "video/out/vo.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#include "video/out/gpu_next/context.h"

struct user_lut {
    char *opt;
    char *path;
    int type;
    struct pl_custom_lut *lut;
};

struct gl_next_opts;
struct priv;
typedef struct priv gpu_next;

struct gl_next_opts {
    bool delayed_peak;
    int sub_hdr_peak;
    int image_subs_hdr_peak;
    int border_background;
    float background_blur_radius;
    float corner_rounding;
    bool inter_preserve;
    struct user_lut lut;
    struct user_lut image_lut;
    struct user_lut target_lut;
    int target_hint;
    int target_hint_mode;
    bool target_hint_strict;
    char **raw_opts;
};

struct gpu_next_render_target {
    struct pl_frame frame;
    struct ra_swapchain *ra_swapchain;
    pl_swapchain swapchain;
    struct pl_color_space surface_color;
    int color_depth;
    bool allow_color_hint;
    bool flip_y;
};

struct gpu_next_render_result {
    struct mp_image_params target_params;
    struct pl_hdr_metadata video_hdr;
    bool has_peak_detect_values;
};

struct gpu_next_colorspace_hint {
    bool enabled;
    bool valid;
    struct pl_color_space color;
};

extern const struct m_sub_options gl_next_conf;

gpu_next *gpu_next_init_renderer(struct mpv_global *global,
                                 struct mp_log *log, struct ra_ctx *ra_ctx,
                                 pl_log pllog, pl_gpu gpu, pl_swapchain sw,
                                 struct mp_hwdec_devices *hwdec_devs,
                                 const char *stats_name);
void gpu_next_uninit_renderer(gpu_next *p);
void gpu_next_set_osd_source(gpu_next *p, struct osd_state *osd);
void gpu_next_set_video_params(gpu_next *p,
                               const struct mp_image_params *params);
void gpu_next_set_viewport(gpu_next *p, const struct mp_rect *src,
                           const struct mp_rect *dst,
                           const struct mp_osd_res *osd);
bool gpu_next_refresh_options(gpu_next *p);
struct mp_image *gpu_next_get_image(gpu_next *p, int imgfmt, int w, int h,
                                    int stride_align, int flags);
bool gpu_next_check_format(gpu_next *p, int format);
bool gpu_next_start_frame(gpu_next *p, struct pl_swapchain_frame *frame);
bool gpu_next_submit_frame(gpu_next *p);
void gpu_next_load_hwdec_api(gpu_next *p, struct mp_hwdec_devices *hwdec_devs,
                             void *data);
void gpu_next_skip_render(gpu_next *p, struct vo_frame *frame);
bool gpu_next_render_to_target(gpu_next *p, struct vo_frame *frame,
                               struct gpu_next_render_target *target,
                               struct gpu_next_render_result *result);
struct gpu_next_colorspace_hint gpu_next_get_colorspace_hint(
    gpu_next *p, struct vo_frame *frame,
    const struct gpu_next_render_target *target);
void gpu_next_take_screenshot(gpu_next *p, struct voctrl_screenshot *args);
void gpu_next_configure_queue(gpu_next *p, struct vo *vo);
bool gpu_next_update_auto_profile(gpu_next *p, int *events);
void gpu_next_perfdata(gpu_next *p, struct voctrl_performance_data *perf);
void gpu_next_request_reset(gpu_next *p);
bool gpu_next_showing_interpolated_frame(gpu_next *p);