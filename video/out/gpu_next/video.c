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

#include <limits.h>
#include <math.h>
#include <stdbool.h>

#include "config.h"

#include <libplacebo/utils/libav.h>

#include "options/m_option.h"
#include "video/out/gpu/video_shaders.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "ta/ta_talloc.h"

#include "video/fmt-conversion.h"

#include "cache.h"
#include "color.h"
#include "hwdec.h"
#include "private.h"
#include "user_shaders.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
#include <libplacebo/opengl.h>
#include "video/out/opengl/ra_gl.h"
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
#include <libplacebo/d3d11.h>
#include "video/out/d3d11/ra_d3d11.h"
#include "osdep/windows_utils.h"
#endif

static const struct m_opt_choice_alternatives lut_types[] = {
    {"auto",        PL_LUT_UNKNOWN},
    {"native",      PL_LUT_NATIVE},
    {"normalized",  PL_LUT_NORMALIZED},
    {"conversion",  PL_LUT_CONVERSION},
    {0}
};

#define OPT_BASE_STRUCT struct gl_next_opts
const struct m_sub_options gl_next_conf = {
    .opts = (const struct m_option[]) {
        {"sub-hdr-peak", OPT_CHOICE(sub_hdr_peak, {"sdr", PL_COLOR_SDR_WHITE}),
            M_RANGE(10, 10000)},
        {"image-subs-hdr-peak", OPT_CHOICE(image_subs_hdr_peak, {"sdr", PL_COLOR_SDR_WHITE},
            {"video", -1}, {"video-static", -2}, {"video-dynamic", -3}),  M_RANGE(10, 10000)},
        {"allow-delayed-peak-detect", OPT_BOOL(delayed_peak)},
        {"border-background", OPT_CHOICE(border_background,
            {"none",  BACKGROUND_NONE},
            {"color", BACKGROUND_COLOR},
            {"tiles", BACKGROUND_TILES},
            {"blur", BACKGROUND_BLUR})},
        {"background-blur-radius", OPT_FLOAT(background_blur_radius)},
        {"corner-rounding", OPT_FLOAT(corner_rounding), M_RANGE(0, 1)},
        {"interpolation-preserve", OPT_BOOL(inter_preserve)},
        {"lut", OPT_STRING(lut.opt), .flags = M_OPT_FILE},
        {"lut-type", OPT_CHOICE_C(lut.type, lut_types)},
        {"image-lut", OPT_STRING(image_lut.opt), .flags = M_OPT_FILE},
        {"image-lut-type", OPT_CHOICE_C(image_lut.type, lut_types)},
        {"target-lut", OPT_STRING(target_lut.opt), .flags = M_OPT_FILE},
        {"target-colorspace-hint", OPT_CHOICE(target_hint, {"auto", -1}, {"no", 0}, {"yes", 1})},
        {"target-colorspace-hint-mode", OPT_CHOICE(target_hint_mode, {"target", 0}, {"source", 1}, {"source-dynamic", 2})},
        {"target-colorspace-hint-strict", OPT_BOOL(target_hint_strict)},
        {"libplacebo-opts", OPT_KEYVALUELIST(raw_opts)},
        {0},
    },
    .defaults = &(struct gl_next_opts) {
        .border_background = BACKGROUND_COLOR,
        .background_blur_radius = 16.0f,
        .inter_preserve = true,
        .sub_hdr_peak = PL_COLOR_SDR_WHITE,
        .image_subs_hdr_peak = 1000,
        .target_hint = -1,
        .target_hint_strict = true,
    },
    .size = sizeof(struct gl_next_opts),
    .change_flags = UPDATE_VIDEO,
};

static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt, bool use_uint);
static void gpu_next_update_render_options(struct priv *p);

static void free_dr_buf(void *opaque, uint8_t *data)
{
    struct priv *p = opaque;
    mp_mutex_lock(&p->dr_lock);

    for (int i = 0; i < p->num_dr_buffers; i++) {
        if (p->dr_buffers[i]->data == data) {
            pl_buf_destroy(p->gpu, &p->dr_buffers[i]);
            MP_TARRAY_REMOVE_AT(p->dr_buffers, p->num_dr_buffers, i);
            mp_mutex_unlock(&p->dr_lock);
            return;
        }
    }

    MP_ASSERT_UNREACHABLE();
}

void gpu_next_set_osd_source(struct priv *p, struct osd_state *osd)
{
    p->osd = osd;
}

void gpu_next_set_video_params(struct priv *p,
                               const struct mp_image_params *params)
{
    p->video_params = params ? *params : (struct mp_image_params){0};
}

void gpu_next_set_viewport(struct priv *p, const struct mp_rect *src,
                           const struct mp_rect *dst,
                           const struct mp_osd_res *osd)
{
    if (!src || !dst || !osd)
        return;

    if (mp_rect_equals(&p->src, src) &&
        mp_rect_equals(&p->dst, dst) &&
        osd_res_equals(p->osd_res, *osd))
        return;

    p->osd_sync++;
    p->src = *src;
    p->dst = *dst;
    p->osd_res = *osd;
}

struct mp_image *gpu_next_get_image(struct priv *p, int imgfmt, int w, int h,
                                    int stride_align, int flags)
{
    pl_gpu gpu = p->gpu;
    if (!gpu->limits.thread_safe || !gpu->limits.max_mapped_size)
        return NULL;

    if ((flags & VO_DR_FLAG_HOST_CACHED) && !gpu->limits.host_cached)
        return NULL;

    stride_align = mp_lcm(stride_align, gpu->limits.align_tex_xfer_pitch);
    stride_align = mp_lcm(stride_align, gpu->limits.align_tex_xfer_offset);
    int size = mp_image_get_alloc_size(imgfmt, w, h, stride_align);
    if (size < 0)
        return NULL;

    pl_buf buf = pl_buf_create(gpu, &(struct pl_buf_params) {
        .memory_type = PL_BUF_MEM_HOST,
        .host_mapped = true,
        .size = size + stride_align,
    });

    if (!buf)
        return NULL;

    struct mp_image *mpi = mp_image_from_buffer(imgfmt, w, h, stride_align,
                                                buf->data, buf->params.size,
                                                p, free_dr_buf);
    if (!mpi) {
        pl_buf_destroy(gpu, &buf);
        return NULL;
    }

    mp_mutex_lock(&p->dr_lock);
    MP_TARRAY_APPEND(p, p->dr_buffers, p->num_dr_buffers, buf);
    mp_mutex_unlock(&p->dr_lock);

    return mpi;
}

static bool gpu_next_format_supported(struct priv *p, int format, bool use_uint)
{
    struct pl_bit_encoding bits;
    struct pl_plane_data data[4] = {0};
    int planes = plane_data_from_imgfmt(data, &bits, format, use_uint);
    if (!planes)
        return false;

    for (int i = 0; i < planes; i++) {
        if (!pl_plane_find_fmt(p->gpu, NULL, &data[i]))
            return false;
    }

    return true;
}

bool gpu_next_check_format(struct priv *p, int format)
{
    if (ra_hwdec_get(&p->hwdec_ctx, format))
        return true;

    if (gpu_next_format_supported(p, format, false))
        return true;

    return gpu_next_format_supported(p, format, true);
}

bool gpu_next_start_frame(struct priv *p, struct pl_swapchain_frame *frame)
{
    return pl_swapchain_start_frame(p->sw, frame);
}

bool gpu_next_submit_frame(struct priv *p)
{
    return pl_swapchain_submit_frame(p->sw);
}

static inline void copy_frame_info_to_mp(struct frame_info *pl,
                                         struct mp_frame_perf *mp,
                                         struct mp_pass_perf *hwdec_perf,
                                         struct mp_pass_perf *sw_upload_perf)
{
    static_assert(MP_ARRAY_SIZE(pl->info) == MP_ARRAY_SIZE(mp->perf), "");
    mp_assert(pl->count <= VO_PASS_PERF_MAX);

    struct mp_pass_perf *perf = mp->perf;
    char (*desc)[VO_PASS_DESC_MAX_LEN] = mp->desc;
    struct mp_pass_perf *perf_end = perf + VO_PASS_PERF_MAX;

    if (hwdec_perf && hwdec_perf->count > 0) {
        *perf++ = *hwdec_perf;
        snprintf(*desc, sizeof(*desc), "map frame (hwdec)");
        desc++;
    }

    if (sw_upload_perf && sw_upload_perf->count > 0) {
        *perf++ = *sw_upload_perf;
        snprintf(*desc, sizeof(*desc), "upload frame");
        desc++;
    }

    for (int i = 0; i < pl->count && perf < perf_end; ++i) {
        const struct pl_dispatch_info *pass = &pl->info[i];

        static_assert(VO_PERF_SAMPLE_COUNT >= MP_ARRAY_SIZE(pass->samples), "");
        mp_assert(pass->num_samples <= MP_ARRAY_SIZE(pass->samples));

        perf->count = MPMIN(pass->num_samples, VO_PERF_SAMPLE_COUNT);
        memcpy(perf->samples, pass->samples, perf->count * sizeof(pass->samples[0]));
        perf->last = pass->last;
        perf->peak = pass->peak;
        perf->avg = pass->average;

        strncpy(*desc, pass->shader->description, sizeof(*desc) - 1);
        (*desc)[sizeof(*desc) - 1] = '\0';
        perf++;
        desc++;
    }

    mp->count = perf - mp->perf;
}

void gpu_next_perfdata(struct priv *p, struct voctrl_performance_data *perf)
{
    copy_frame_info_to_mp(&p->perf_fresh, &perf->fresh, &p->hwdec_perf,
                          &p->sw_upload_perf);
    copy_frame_info_to_mp(&p->perf_redraw, &perf->redraw, NULL, NULL);
}

gpu_next *gpu_next_init_renderer(struct mpv_global *global,
                                 struct mp_log *log, struct ra_ctx *ra_ctx,
                                 pl_log pllog, pl_gpu gpu, pl_swapchain sw,
                                 struct mp_hwdec_devices *hwdec_devs,
                                 bool load_all_hwdecs,
                                 const char *stats_name)
{
    gpu_next *p = talloc_zero(NULL, struct priv);
    p->opts_cache = m_config_cache_alloc(p, global, &gl_video_conf);
    p->next_opts_cache = m_config_cache_alloc(p, global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;
    p->video_eq = mp_csp_equalizer_create(p, global);
    p->global = global;
    p->log = log;
    p->stats = stats_ctx_create(p, global,
                                stats_name ? stats_name : "gpu-next");
    p->ra_ctx = ra_ctx;
    p->pllog = pllog;
    p->gpu = gpu;
    p->sw = sw;
    p->hwdec_ctx = (struct ra_hwdec_ctx){
        .log = p->log,
        .global = p->global,
        .ra_ctx = p->ra_ctx,
    };

    const struct gl_video_opts *gl_opts = p->opts_cache->opts;
    if (hwdec_devs) {
        ra_hwdec_ctx_init(&p->hwdec_ctx, hwdec_devs, gl_opts->hwdec_interop,
                          load_all_hwdecs);
    }
    mp_mutex_init(&p->dr_lock);

    if (gl_opts->shader_cache)
        gpu_next_cache_init(p, &p->shader_cache, 10 << 20, gl_opts->shader_cache_dir);
    if (gl_opts->icc_opts->cache)
        gpu_next_cache_init(p, &p->icc_cache, 20 << 20, gl_opts->icc_opts->cache_dir);

    pl_gpu_set_cache(p->gpu, p->shader_cache.cache);
    p->rr = pl_renderer_create(p->pllog, p->gpu);
    p->queue = pl_queue_create(p->gpu);
    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(p->gpu, "r8");
    p->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(p->gpu, "bgra8");
    p->osd_sync = 1;

    p->pars = pl_options_alloc(p->pllog);
    if (!p->rr || !p->queue || !p->pars) {
        gpu_next_uninit_renderer(p);
        return NULL;
    }

    gpu_next_update_render_options(p);
    return p;
}

void gpu_next_uninit_renderer(struct priv *p)
{
    if (!p)
        return;

    pl_queue_destroy(&p->queue);
    for (int i = 0; i < MP_ARRAY_SIZE(p->osd_state.entries); i++)
        pl_tex_destroy(p->gpu, &p->osd_state.entries[i].tex);
    for (int i = 0; i < p->num_sub_tex; i++)
        pl_tex_destroy(p->gpu, &p->sub_tex[i]);
    for (int i = 0; i < p->num_user_hooks; i++)
        pl_mpv_user_shader_destroy(&p->user_hooks[i].hook);

    timer_pool_destroy(p->sw_upload_timer);
    ra_hwdec_mapper_free(&p->hwdec_mapper);
    timer_pool_destroy(p->hwdec_timer);
    ra_hwdec_ctx_uninit(&p->hwdec_ctx);

    mp_assert(p->num_dr_buffers == 0);
    mp_mutex_destroy(&p->dr_lock);

    gpu_next_cache_uninit(p, &p->shader_cache);
    gpu_next_cache_uninit(p, &p->icc_cache);

    pl_lut_free(&p->next_opts->image_lut.lut);
    pl_lut_free(&p->next_opts->lut.lut);
    pl_lut_free(&p->next_opts->target_lut.lut);

    pl_icc_close(&p->icc_profile);
    pl_renderer_destroy(&p->rr);

    for (int i = 0; i < VO_PASS_PERF_MAX; ++i) {
        pl_shader_info_deref(&p->perf_fresh.info[i].shader);
        pl_shader_info_deref(&p->perf_redraw.info[i].shader);
    }

    pl_options_free(&p->pars);

    p->ra_ctx = NULL;
    p->pllog = NULL;
    p->gpu = NULL;
    p->sw = NULL;

    talloc_free(p);
}

void gpu_next_set_ambient_lux(struct priv *p, double lux)
{
    if (!p)
        return;
    // gpu-next does not currently consume ambient light data, but accept it
    // for API compatibility with the gpu backend (and potential future use).
    p->ambient_lux = lux;
}

void gpu_next_set_icc_profile(struct priv *p, struct bstr profile)
{
    if (!p) {
        talloc_free(profile.start);
        return;
    }
    gpu_next_set_icc_profile_data(p, profile);
}

void gpu_next_request_reset(struct priv *p)
{
    p->want_reset = true;
}

bool gpu_next_showing_interpolated_frame(struct priv *p)
{
    return p->is_interpolated;
}

static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt, bool use_uint)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;

    if (desc.flags & MP_IMGFLAG_HWACCEL)
        return 0;
    if (!(desc.flags & MP_IMGFLAG_NE))
        return 0;
    if (desc.flags & MP_IMGFLAG_PAL)
        return 0;
    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return 0;

    bool has_bits = false;
    bool any_padded = false;

    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        if (desc.bpp[p] % 8)
            return 0;

        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;

            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;

            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        uint64_t total_bits = 0;
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];
            any_padded |= sorted[c].pad;

            if (!out_bits || data->component_map[c] == PL_CHANNEL_A)
                continue;

            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };

            if (!has_bits) {
                *out_bits = bits;
                has_bits = true;
            } else if (!pl_bit_encoding_equal(out_bits, &bits)) {
                *out_bits = (struct pl_bit_encoding){0};
                out_bits = NULL;
            }
        }

        data->pixel_stride = desc.bpp[p] / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT) ? PL_FMT_FLOAT
                    : (use_uint ? PL_FMT_UINT : PL_FMT_UNORM);
    }

    if (any_padded && !out_bits)
        return 0;

    return desc.num_planes;
}

static pl_buf get_dr_buf(struct priv *p, const uint8_t *ptr)
{
    mp_mutex_lock(&p->dr_lock);

    for (int i = 0; i < p->num_dr_buffers; i++) {
        pl_buf buf = p->dr_buffers[i];
        if (ptr >= buf->data && ptr < buf->data + buf->params.size) {
            mp_mutex_unlock(&p->dr_lock);
            return buf;
        }
    }

    mp_mutex_unlock(&p->dr_lock);
    return NULL;
}

static bool map_frame(pl_gpu gpu, pl_tex *tex, const struct pl_source_frame *src,
                      struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct mp_image_params par = mpi->params;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->p;

    fp->hwdec = ra_hwdec_get(&p->hwdec_ctx, mpi->imgfmt);
    if (fp->hwdec) {
        if (!gpu_next_hwdec_reconfig(p, fp->hwdec, &mpi->params)) {
            talloc_free(mpi);
            return false;
        }

        par = p->hwdec_mapper->dst_params;
    }

    mp_image_params_guess_csp(&par);

    *frame = (struct pl_frame) {
        .color = par.color,
        .repr = par.repr,
        .profile = {
            .data = mpi->icc_profile ? mpi->icc_profile->data : NULL,
            .len = mpi->icc_profile ? mpi->icc_profile->size : 0,
        },
        .rotation = par.rotate / 90,
        .user_data = mpi,
    };

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(frame->color.transfer))
        frame->color.hdr.max_luma = opts->hdr_reference_white;

    if (opts->treat_srgb_as_power22 & 1 && frame->color.transfer == PL_COLOR_TRC_SRGB)
        frame->color.transfer = PL_COLOR_TRC_GAMMA22;

    if (fp->hwdec) {
        p->sw_upload_perf.count = 0;

        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(par.imgfmt);
        frame->acquire = gpu_next_hwdec_acquire;
        frame->release = gpu_next_hwdec_release;
        frame->num_planes = desc.num_planes;
        for (int n = 0; n < frame->num_planes; n++) {
            struct pl_plane *plane = &frame->planes[n];
            int *map = plane->component_mapping;
            for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
                if (desc.comps[c].plane != n)
                    continue;

                uint8_t offset = desc.comps[c].offset;
                int index = plane->components++;
                while (index > 0 && desc.comps[map[index - 1]].offset > offset) {
                    map[index] = map[index - 1];
                    index--;
                }
                map[index] = c;
            }
        }
    } else {
        p->hwdec_perf.count = 0;

        if (!p->sw_upload_timer)
            p->sw_upload_timer = timer_pool_create(p->ra_ctx->ra);

        struct pl_plane_data data[4] = {0};
        bool use_uint = false;

        if (!gpu_next_format_supported(p, mpi->imgfmt, false))
            use_uint = true;

        frame->num_planes = plane_data_from_imgfmt(data, &frame->repr.bits,
                                                   mpi->imgfmt, use_uint);
        stats_time_start(p->stats, "swdec-upload");
        timer_pool_start(p->sw_upload_timer);
        for (int n = 0; n < frame->num_planes; n++) {
            struct pl_plane *plane = &frame->planes[n];
            data[n].width = mp_image_plane_w(mpi, n);
            data[n].height = mp_image_plane_h(mpi, n);
            if (mpi->stride[n] < 0) {
                data[n].pixels = mpi->planes[n] + (data[n].height - 1) * mpi->stride[n];
                data[n].row_stride = -mpi->stride[n];
                plane->flipped = true;
            } else {
                data[n].pixels = mpi->planes[n];
                data[n].row_stride = mpi->stride[n];
            }

            pl_buf buf = get_dr_buf(p, data[n].pixels);
            if (buf) {
                data[n].buf = buf;
                data[n].buf_offset = (uint8_t *)data[n].pixels - buf->data;
                data[n].pixels = NULL;
            } else if (gpu->limits.callbacks) {
                data[n].callback = talloc_free;
                data[n].priv = mp_image_new_ref(mpi);
            }

            if (!pl_upload_plane(gpu, plane, &tex[n], &data[n])) {
                MP_ERR(p, "Failed uploading frame!\n");
                timer_pool_stop(p->sw_upload_timer);
                stats_time_end(p->stats, "swdec-upload");
                talloc_free(data[n].priv);
                talloc_free(mpi);
                return false;
            }
        }
        timer_pool_stop(p->sw_upload_timer);
        p->sw_upload_perf = timer_pool_measure(p->sw_upload_timer);
        stats_time_end(p->stats, "swdec-upload");
    }

    pl_frame_set_chroma_location(frame, par.chroma_location);

    if (mpi->film_grain) {
        pl_film_grain_from_av(&frame->film_grain,
                              (AVFilmGrainParams *)mpi->film_grain->data);
    }

    pl_icc_profile_compute_signature(&frame->profile);

    gpu_next_update_lut(p, &p->next_opts->image_lut);
    frame->lut = p->next_opts->image_lut.lut;
    frame->lut_type = p->next_opts->image_lut.type;
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->p;
    for (int i = 0; i < MP_ARRAY_SIZE(fp->subs.entries); i++) {
        pl_tex tex = fp->subs.entries[i].tex;
        if (tex)
            MP_TARRAY_APPEND(p, p->sub_tex, p->num_sub_tex, tex);
    }
    talloc_free(mpi);
}

static void discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

static void info_callback(void *priv, const struct pl_render_info *info)
{
    struct priv *p = priv;
    if (info->index >= VO_PASS_PERF_MAX)
        return;

    struct frame_info *frame;
    switch (info->stage) {
    case PL_RENDER_STAGE_FRAME: frame = &p->perf_fresh; break;
    case PL_RENDER_STAGE_BLEND: frame = &p->perf_redraw; break;
    default: abort();
    }

    frame->count = info->index + 1;
    pl_dispatch_info_move(&frame->info[info->index], info->pass);
}

static void apply_target_contrast(struct priv *p, struct pl_color_space *color,
                                  float min_luma)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;

    if (!opts->target_contrast) {
        color->hdr.min_luma = min_luma;
        return;
    }

    if (opts->target_contrast == -1) {
        color->hdr.min_luma = 1e-7;
        mp_assert(color->hdr.min_luma > 0);
        return;
    }

    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color = color,
        .metadata = PL_HDR_METADATA_HDR10,
        .scaling = PL_HDR_NITS,
        .out_max = &color->hdr.max_luma,
    ));

    color->hdr.min_luma = color->hdr.max_luma / opts->target_contrast;
}

static void apply_target_options(struct priv *p, struct pl_frame *target,
                                 float min_luma, bool hint, int color_depth)
{
    gpu_next_update_lut(p, &p->next_opts->target_lut);
    target->lut = p->next_opts->target_lut.lut;
    target->lut_type = p->next_opts->target_lut.type;

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (p->output_levels)
        target->repr.levels = p->output_levels;
    if (opts->target_prim && (!target->color.primaries || !hint))
        target->color.primaries = opts->target_prim;
    if (opts->target_trc && (!target->color.transfer || !hint))
        target->color.transfer = opts->target_trc;
    if (opts->target_peak && (!target->color.hdr.max_luma || !hint))
        target->color.hdr.max_luma = opts->target_peak;
    if (opts->hdr_reference_white && (!target->color.hdr.max_luma || !hint) &&
        !pl_color_transfer_is_hdr(target->color.transfer)) {
        target->color.hdr.max_luma = opts->hdr_reference_white;
    }
    if ((!target->color.hdr.min_luma || !hint))
        apply_target_contrast(p, &target->color, min_luma);
    if (opts->target_gamut)
        mp_parse_raw_primaries(mp_null_log, opts->target_gamut,
                               &target->color.hdr.prim);
    int dither_depth = opts->dither_depth;
    if (dither_depth == 0)
        dither_depth = color_depth;
#if PL_API_VER >= 362
    if (target->color.transfer == PL_COLOR_TRC_SCRGB)
        dither_depth = -1;
#endif
    if (dither_depth > 0) {
        struct pl_bit_encoding *tbits = &target->repr.bits;
        tbits->color_depth += dither_depth - tbits->sample_depth;
        tbits->sample_depth = dither_depth;
    }

    if (opts->icc_opts->icc_use_luma) {
        p->icc_params.max_luma = 0.0f;
    } else {
        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
            .color    = &target->color,
            .metadata = PL_HDR_METADATA_HDR10,
            .scaling  = PL_HDR_NITS,
            .out_max  = &p->icc_params.max_luma,
        ));
    }

    pl_icc_update(p->pllog, &p->icc_profile, NULL, &p->icc_params);
    target->icc = p->icc_profile;
}

static void apply_crop(struct pl_frame *frame, struct mp_rect crop,
                       int width, int height)
{
    frame->crop = (struct pl_rect2df) {
        .x0 = crop.x0,
        .y0 = crop.y0,
        .x1 = crop.x1,
        .y1 = crop.y1,
    };

    pl_rect2df_rotate(&frame->crop, -frame->rotation);
    if (frame->crop.x1 < frame->crop.x0) {
        frame->crop.x0 = width - frame->crop.x0;
        frame->crop.x1 = width - frame->crop.x1;
    }

    if (frame->crop.y1 < frame->crop.y0) {
        frame->crop.y0 = height - frame->crop.y0;
        frame->crop.y1 = height - frame->crop.y1;
    }
}

static bool set_colorspace_hint(struct priv *p, struct ra_swapchain *sw,
                                pl_swapchain pl_sw,
                                struct pl_color_space *hint)
{
    if (!sw || !pl_sw)
        return false;

    struct mp_image_params params = {
        .color = hint ? *hint : pl_color_space_srgb,
        .repr = {
            .sys = PL_COLOR_SYSTEM_RGB,
            .levels = p->output_levels ? p->output_levels : PL_COLOR_LEVELS_FULL,
            .alpha = p->ra_ctx->opts.want_alpha ? PL_ALPHA_INDEPENDENT : PL_ALPHA_NONE,
        },
    };

    if (sw->fns->set_color && sw->fns->set_color(sw, hint ? &params : NULL)) {
        if (hint) {
            *hint = params.color;
            return true;
        }
    }
    pl_swapchain_colorspace_hint(pl_sw, hint);
    return false;
}

static void update_tm_viz(struct pl_color_map_params *params,
                          const struct pl_frame *target)
{
    if (!params->visualize_lut)
        return;

    const float out_w = fabsf(pl_rect_w(target->crop));
    const float out_h = fabsf(pl_rect_h(target->crop));
    const float size = MPMIN(out_w / 2.0f, out_h);
    params->visualize_rect = (pl_rect2df) {
        .x0 = 1.0f - size / out_w,
        .x1 = 1.0f,
        .y0 = 0.0f,
        .y1 = size / out_h,
    };
    params->visualize_hue = M_PI / 4.0;
}

void gpu_next_skip_render(struct priv *p, struct vo_frame *frame)
{
    struct pl_render_params params = p->pars->params;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    bool can_interpolate = opts->interpolation && frame->display_synced &&
                           !frame->still && frame->num_frames > 1;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;

    if (!frame->current)
        return;

    struct pl_queue_params qparams = *pl_queue_params(
        .pts = frame->current->pts + pts_offset,
        .radius = pl_frame_mix_radius(&params),
        .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
        .drift_compensation = 0,
    );
    pl_queue_update(p->queue, NULL, &qparams);
}

bool gpu_next_render_to_target(struct priv *p, struct vo_frame *frame,
                               struct gpu_next_render_target *target,
                               struct gpu_next_render_result *result)
{
    pl_options pars = p->pars;
    pl_gpu gpu = p->gpu;
    gpu_next_refresh_options(p);

    struct pl_render_params params = pars->params;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    bool will_redraw = frame->display_synced && frame->num_vsyncs > 1;
    bool cache_frame = will_redraw || frame->still;
    bool can_interpolate = opts->interpolation && frame->display_synced &&
                           !frame->still && frame->num_frames > 1;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;
    params.info_callback = info_callback;
    params.info_priv = p;
    params.skip_caching_single_frame = !cache_frame;
    params.preserve_mixing_cache = p->next_opts->inter_preserve && !frame->still;
    if (frame->still)
        params.frame_mixer = NULL;

    if (frame->current && frame->current->params.vflip) {
        pl_matrix2x2 m = { .m = {{1, 0}, {0, -1}} };
        pars->distort_params.transform.mat = m;
        params.distort_params = &pars->distort_params;
    } else {
        params.distort_params = NULL;
    }

    struct pl_source_frame vpts;
    if (frame->current && !p->want_reset) {
        if (pl_queue_peek(p->queue, 0, &vpts) &&
            frame->current->pts + MPMAX(0, pts_offset) < vpts.pts)
        {
            MP_VERBOSE(p, "Forcing queue refill, PTS(%f + %f | %f) < VPTS(%f)\n",
                       frame->current->pts, pts_offset,
                       frame->ideal_frame_vsync_duration, vpts.pts);
            p->want_reset = true;
        }
    }

    for (int n = 0; n < frame->num_frames; n++) {
        int id = frame->frame_id + n;

        if (p->want_reset) {
            pl_renderer_flush_cache(p->rr);
            pl_queue_reset(p->queue);
            p->last_pts = 0.0;
            p->last_id = 0;
            p->want_reset = false;
        }

        if (id <= p->last_id)
            continue;

        struct mp_image *mpi = mp_image_new_ref(frame->frames[n]);
        struct frame_priv *fp = talloc_zero(mpi, struct frame_priv);
        mpi->priv = fp;
        fp->p = p;

        pl_queue_push(p->queue, &(struct pl_source_frame) {
            .pts = mpi->pts,
            .duration = can_interpolate ? frame->approx_duration : 0,
            .frame_data = mpi,
            .map = map_frame,
            .unmap = unmap_frame,
            .discard = discard_frame,
        });

        p->last_id = id;
    }

    struct pl_color_space target_csp = target->surface_color;
    if (target_csp.primaries == PL_COLOR_PRIM_UNKNOWN)
        target_csp.primaries = mp_get_best_prim_container(&target_csp.hdr.prim);
    if (!pl_color_transfer_is_hdr(target_csp.transfer) &&
        target_csp.hdr.min_luma > PL_COLOR_SDR_WHITE / PL_COLOR_SDR_CONTRAST)
        target_csp.hdr.min_luma = 0;
    target_csp.hdr.max_fall = 0;

    struct pl_color_space hint = {0};
    bool target_hint = p->next_opts->target_hint == 1 ||
                       (p->next_opts->target_hint == -1 &&
                        target_csp.transfer != PL_COLOR_TRC_UNKNOWN);
    bool can_hint = target->ra_swapchain || target->swapchain;
    if (target_hint && !can_hint && !p->warned_no_color_hint) {
        MP_VERBOSE(p, "target-colorspace-hint requested but the current render "
                      "backend cannot push a color space hint to the surface; "
                      "the host application must configure the surface itself.\n");
        p->warned_no_color_hint = true;
    }
    bool target_unknown = target_csp.transfer == PL_COLOR_TRC_UNKNOWN;
    if (target_unknown) {
        target_csp = (struct pl_color_space) {
            .transfer = opts->target_trc ? opts->target_trc
                                         : pl_color_space_hdr10.transfer,
        };
    }
    bool external_params = false;
    if (target_hint && frame->current) {
        const struct pl_color_space *source = &frame->current->params.color;
        const struct pl_color_space *target_csp_ref = &target_csp;
        hint = *source;
        if (!hint.hdr.min_luma)
            hint.hdr.min_luma = target_csp_ref->hdr.min_luma;
        if (p->next_opts->target_hint_mode == 0) {
            hint = *target_csp_ref;
            if (pl_color_transfer_is_hdr(hint.transfer) &&
                !pl_primaries_valid(&hint.hdr.prim))
                pl_color_space_merge(&hint, source);
            if (target_unknown && !opts->target_trc &&
                !pl_color_transfer_is_hdr(source->transfer))
                hint = *source;
            if (target_csp_ref->hdr.max_luma) {
                hint.hdr.max_luma = target_csp_ref->hdr.max_luma;
                hint.hdr.min_luma = target_csp_ref->hdr.min_luma;
                hint.hdr.max_cll  = target_csp_ref->hdr.max_cll;
                hint.hdr.max_fall = target_csp_ref->hdr.max_fall;
            }
        }
        if (p->next_opts->target_hint_mode == 2) {
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color      = &hint,
                .metadata   = PL_HDR_METADATA_ANY,
                .scaling    = PL_HDR_NITS,
                .out_min    = !hint.hdr.min_luma ? &hint.hdr.min_luma : NULL,
                .out_max    = &hint.hdr.max_luma,
            ));
            hint.hdr.max_cll = hint.hdr.max_luma;
            if (hint.hdr.max_fall > hint.hdr.max_cll)
                hint.hdr.max_fall = 0;
        }
        struct pl_color_space source_csp = *source;
        pl_color_space_infer_map(&source_csp, &hint);
        if (pl_color_transfer_is_hdr(target_csp_ref->transfer) && opts->tone_map.inverse) {
            hint.transfer     = target_csp_ref->transfer;
            hint.hdr.max_luma = target_csp_ref->hdr.max_luma;
            hint.hdr.min_luma = target_csp_ref->hdr.min_luma;
            hint.hdr.max_cll  = target_csp_ref->hdr.max_cll;
            hint.hdr.max_fall = target_csp_ref->hdr.max_fall;
        }
        if (opts->target_prim)
            hint.primaries = opts->target_prim;
        if (opts->target_gamut)
            mp_parse_raw_primaries(mp_null_log, opts->target_gamut, &hint.hdr.prim);
        if (opts->target_trc)
            hint.transfer = opts->target_trc;
        if (opts->target_peak)
            hint.hdr.max_luma = opts->target_peak;
        if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(hint.transfer))
            hint.hdr.max_luma = opts->hdr_reference_white;
        if (!hint.hdr.max_cll)
            hint.hdr.max_cll = hint.hdr.max_luma;
        if (source->hdr.max_luma > hint.hdr.max_luma || opts->tone_map.inverse) {
            if (!hint.hdr.max_cll || hint.hdr.max_luma < hint.hdr.max_cll ||
                opts->tone_map.inverse)
                hint.hdr.max_cll = hint.hdr.max_luma;
            hint.hdr.max_fall = 0;
        }
        if (hint.hdr.max_cll && hint.hdr.max_fall > hint.hdr.max_cll)
            hint.hdr.max_fall = 0;
        apply_target_contrast(p, &hint, hint.hdr.min_luma);
        if (p->icc_profile)
            hint = p->icc_profile->csp;
        if (opts->icc_opts->icc_use_luma) {
            p->icc_params.max_luma = 0.0f;
        } else {
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color    = &hint,
                .metadata = PL_HDR_METADATA_HDR10,
                .scaling  = PL_HDR_NITS,
                .out_max  = &p->icc_params.max_luma,
            ));
        }
        pl_icc_update(p->pllog, &p->icc_profile, NULL, &p->icc_params);
        if (p->icc_profile)
            hint = p->icc_profile->csp;
        if (can_hint) {
            external_params = set_colorspace_hint(p, target->ra_swapchain,
                                                  target->swapchain, &hint);
        }
    } else if (!target_hint) {
        if (!hint.hdr.min_luma)
            hint.hdr.min_luma = target_csp.hdr.min_luma;
        if (can_hint) {
            external_params = set_colorspace_hint(p, target->ra_swapchain,
                                                  target->swapchain, NULL);
        }
    }

    bool valid = false;
    p->is_interpolated = false;

    struct pl_frame target_frame = target->frame;
    if (external_params)
        target_frame.color = hint;
    bool strict_sw_params = target_hint && p->next_opts->target_hint_strict;
    apply_target_options(p, &target_frame, hint.hdr.min_luma, strict_sw_params,
                         target->color_depth);
    bool clip_gamut = pl_primaries_valid(&target_frame.color.hdr.prim);
#if PL_API_VER >= 362
    clip_gamut = clip_gamut && target_frame.color.transfer != PL_COLOR_TRC_SCRGB;
#endif
    if (clip_gamut) {
        target_frame.color.hdr.prim =
            pl_primaries_clip(&target_frame.color.hdr.prim,
                              pl_raw_primaries_get(target_frame.color.primaries));
    }
    if (target_frame.color.transfer == PL_COLOR_TRC_SRGB && frame->current &&
        ((opts->sdr_adjust_gamma == 0 && opts->target_trc == PL_COLOR_TRC_UNKNOWN) ||
         opts->sdr_adjust_gamma == -1))
    {
        switch (frame->current->params.color.transfer) {
        case PL_COLOR_TRC_BT_1886:
        case PL_COLOR_TRC_GAMMA22:
        case PL_COLOR_TRC_SRGB:
            target_frame.color.transfer = frame->current->params.color.transfer;
        }
    }
    if (target_frame.color.transfer == PL_COLOR_TRC_SRGB) {
        if (opts->treat_srgb_as_power22 & 2)
            target_frame.color.transfer = PL_COLOR_TRC_GAMMA22;
#ifdef _WIN32
        bool target_pq = !target_unknown && target_csp.transfer == PL_COLOR_TRC_PQ;
        if (opts->treat_srgb_as_power22 & 4 && target_pq)
            target_frame.color.transfer = PL_COLOR_TRC_SRGB;
#endif
    }
    stats_time_start(p->stats, "osd-update");
    gpu_next_update_overlays(p, p->osd_res,
                             (frame->current && opts->blend_subs) ? OSD_DRAW_OSD_ONLY : 0,
                             PL_OVERLAY_COORDS_DST_FRAME, &p->osd_state,
                             &target_frame, frame->current,
                             frame->current ? frame->current->params.stereo3d : 0);
    stats_time_end(p->stats, "osd-update");
    apply_crop(&target_frame, p->dst,
               target_frame.planes[0].texture->params.w,
               target_frame.planes[0].texture->params.h);
    if (target->flip_y)
        MPSWAP(float, target_frame.crop.y0, target_frame.crop.y1);
    update_tm_viz(&pars->color_map_params, &target_frame);

    struct pl_frame_mix mix = {0};
    if (frame->current) {
        struct pl_queue_params qparams = *pl_queue_params(
            .pts = frame->current->pts + pts_offset,
            .radius = pl_frame_mix_radius(&params),
            .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
            .interpolation_threshold = opts->interpolation_threshold,
            .drift_compensation = 0,
        );

        struct pl_source_frame first;
        if (pl_queue_peek(p->queue, 0, &first) && qparams.pts < first.pts) {
            if (first.pts != frame->current->pts)
                MP_VERBOSE(p, "Current PTS(%f) != VPTS(%f)\n",
                           frame->current->pts, first.pts);
            MP_VERBOSE(p, "Clamping first frame PTS from %f to %f\n",
                       qparams.pts, first.pts);
            qparams.pts = first.pts;
        }
        p->last_pts = qparams.pts;

        switch (pl_queue_update(p->queue, &mix, &qparams)) {
        case PL_QUEUE_ERR:
            MP_ERR(p, "Failed updating frames!\n");
            goto done;
        case PL_QUEUE_EOF:
            abort();
        case PL_QUEUE_MORE:
            MP_DBG(p, "Render queue underrun.\n");
            break;
        case PL_QUEUE_OK:
            break;
        }

        for (int i = 0; i < mix.num_frames; i++) {
            struct pl_frame *image = (struct pl_frame *)mix.frames[i];
            struct mp_image *mpi = image->user_data;
            struct frame_priv *fp = mpi->priv;
            apply_crop(image, p->src, p->video_params.w, p->video_params.h);
            if (opts->blend_subs) {
                if (frame->redraw)
                    p->osd_sync++;
                if (fp->osd_sync < p->osd_sync) {
                    float w = pl_rect_w(opts->blend_subs == BLEND_SUBS_VIDEO
                                        ? image->crop : target_frame.crop);
                    float h = pl_rect_h(opts->blend_subs == BLEND_SUBS_VIDEO
                                        ? image->crop : target_frame.crop);
                    float rx = w / pl_rect_w(image->crop);
                    float ry = h / pl_rect_h(image->crop);
                    struct mp_osd_res res = {
                        .w = w,
                        .h = h,
                        .ml = -image->crop.x0 * rx,
                        .mr = (image->crop.x1 - p->video_params.w) * rx,
                        .mt = -image->crop.y0 * ry,
                        .mb = (image->crop.y1 - p->video_params.h) * ry,
                        .display_par = 1.0,
                    };
                    enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
                        ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
                    stats_time_start(p->stats, "osd-blend-update");
                    gpu_next_update_overlays(p, res, OSD_DRAW_SUB_ONLY,
                                             rel, &fp->subs, image, mpi,
                                             mpi->params.stereo3d);
                    stats_time_end(p->stats, "osd-blend-update");
                    fp->osd_sync = p->osd_sync;
                }
            } else {
                image->num_overlays = 0;
                fp->osd_sync = 0;
            }

            ((uint64_t *)mix.signatures)[i] ^= fp->osd_sync << 48;
        }

        for (int i = 0; i < pars->params.num_hooks; i++)
            gpu_next_update_hook_opts_dynamic(p, p->hooks[i], frame->current);
    }

    stats_time_start(p->stats, "render");
    bool render_ok = pl_render_image_mix(p->rr, &mix, &target_frame, &params);
    stats_time_end(p->stats, "render");
    if (!render_ok) {
        MP_ERR(p, "Failed rendering frame!\n");
        goto done;
    }

    struct pl_frame ref_frame;
    pl_frames_infer_mix(p->rr, &mix, &target_frame, &ref_frame);

    p->target_params = (struct mp_image_params) {
        .imgfmt_name = target_frame.planes[0].texture->params.format
                        ? target_frame.planes[0].texture->params.format->name : NULL,
        .w = mp_rect_w(p->dst),
        .h = mp_rect_h(p->dst),
        .color = target_frame.color,
        .repr = target_frame.repr,
        .rotate = target_frame.rotation,
    };

    if (result) {
        result->target_params = p->target_params;
        result->has_peak_detect_values =
            pl_renderer_get_hdr_metadata(p->rr, &p->video_params.color.hdr);
        result->video_hdr = p->video_params.color.hdr;
    }

    p->is_interpolated = pts_offset != 0 && mix.num_frames > 1;
    valid = true;

done:
    if (!valid)
        pl_tex_clear(gpu, target_frame.planes[0].texture,
                     (float[4]){0.5, 0.0, 1.0, 1.0});

    pl_gpu_flush(gpu);
    return valid;
}

void gpu_next_take_screenshot(struct priv *p, struct voctrl_screenshot *args)
{
    pl_options pars = p->pars;
    pl_gpu gpu = p->gpu;
    pl_tex fbo = NULL;
    args->res = NULL;

    gpu_next_refresh_options(p);
    struct pl_render_params params = pars->params;
    params.info_callback = NULL;
    params.skip_caching_single_frame = true;
    params.preserve_mixing_cache = false;
    params.frame_mixer = NULL;

    struct pl_peak_detect_params peak_params;
    if (params.peak_detect_params) {
        peak_params = *params.peak_detect_params;
        params.peak_detect_params = &peak_params;
        peak_params.allow_delayed = false;
    }

    struct pl_frame_mix mix;
    enum pl_queue_status status;
    struct pl_queue_params qparams = *pl_queue_params(
        .pts = p->last_pts,
        .drift_compensation = 0,
    );
    status = pl_queue_update(p->queue, &mix, &qparams);
    mp_assert(status != PL_QUEUE_EOF);
    if (status == PL_QUEUE_ERR) {
        MP_ERR(p, "Unknown error occurred while trying to take screenshot!\n");
        return;
    }
    if (!mix.num_frames) {
        MP_ERR(p, "No frames available to take screenshot of, is a file loaded?\n");
        return;
    }

    struct pl_frame image = *(struct pl_frame *)mix.frames[0];
    struct mp_image *mpi = image.user_data;
    struct mp_rect src = p->src, dst = p->dst;
    struct mp_osd_res osd = p->osd_res;
    if (!args->scaled) {
        int w, h;
        mp_image_params_get_dsize(&mpi->params, &w, &h);
        if (w < 1 || h < 1)
            return;

        int src_w = mpi->params.w;
        int src_h = mpi->params.h;
        src = (struct mp_rect){0, 0, src_w, src_h};
        dst = (struct mp_rect){0, 0, w, h};

        if (mp_image_crop_valid(&mpi->params))
            src = mpi->params.crop;

        if (mpi->params.rotate % 180 == 90) {
            MPSWAP(int, w, h);
            MPSWAP(int, src_w, src_h);
        }
        mp_rect_rotate(&src, src_w, src_h, mpi->params.rotate);
        mp_rect_rotate(&dst, w, h, mpi->params.rotate);

        osd = (struct mp_osd_res) {
            .display_par = 1.0,
            .w = mp_rect_w(dst),
            .h = mp_rect_h(dst),
        };
    }

    int mpfmt;
    for (int depth = args->high_bit_depth ? 16 : 8; depth; depth -= 8) {
        if (depth == 16) {
            mpfmt = IMGFMT_RGBA64;
        } else {
            mpfmt = p->ra_ctx->opts.want_alpha ? IMGFMT_RGBA : IMGFMT_RGB0;
        }
        pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, depth, depth,
                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
        if (!fmt)
            continue;

        fbo = pl_tex_create(gpu, pl_tex_params(
            .w = osd.w,
            .h = osd.h,
            .format = fmt,
            .blit_dst = true,
            .renderable = true,
            .host_readable = true,
            .storable = fmt->caps & PL_FMT_CAP_STORABLE,
        ));
        if (fbo)
            break;
    }

    if (!fbo) {
        MP_ERR(p, "Failed creating target FBO for screenshot!\n");
        return;
    }

    struct pl_frame target = {
        .repr = pl_color_repr_rgb,
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
    };

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (args->scaled) {
        apply_target_options(p, &target, 0, false, 0);
    } else if (args->native_csp) {
        target.color = image.color;
    } else {
        target.color = pl_color_space_srgb;
    }

    if (opts->treat_srgb_as_power22 & 1 &&
        target.color.transfer == PL_COLOR_TRC_SRGB &&
        mpi->params.color.transfer == PL_COLOR_TRC_SRGB)
    {
        target.color.transfer = PL_COLOR_TRC_GAMMA22;
    }

    apply_crop(&image, src, mpi->params.w, mpi->params.h);
    apply_crop(&target, dst, fbo->params.w, fbo->params.h);
    update_tm_viz(&pars->color_map_params, &target);

    int osd_flags = 0;
    if (!args->subs)
        osd_flags |= OSD_DRAW_OSD_ONLY;
    if (!args->osd)
        osd_flags |= OSD_DRAW_SUB_ONLY;

    struct frame_priv *fp = mpi->priv;
    if (opts->blend_subs) {
        float w = pl_rect_w(opts->blend_subs == BLEND_SUBS_VIDEO ? image.crop : target.crop);
        float h = pl_rect_h(opts->blend_subs == BLEND_SUBS_VIDEO ? image.crop : target.crop);
        float rx = w / pl_rect_w(image.crop);
        float ry = h / pl_rect_h(image.crop);
        struct mp_osd_res res = {
            .w = w,
            .h = h,
            .ml = -image.crop.x0 * rx,
            .mr = (image.crop.x1 - p->video_params.w) * rx,
            .mt = -image.crop.y0 * ry,
            .mb = (image.crop.y1 - p->video_params.h) * ry,
            .display_par = 1.0,
        };
        enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
            ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
        gpu_next_update_overlays(p, res, osd_flags,
                                 rel, &fp->subs, &image, mpi,
                                 mpi->params.stereo3d);
    } else {
        gpu_next_update_overlays(p, osd, osd_flags,
                                 PL_OVERLAY_COORDS_DST_FRAME,
                                 &p->osd_state, &target, mpi,
                                 mpi->params.stereo3d);
        image.num_overlays = 0;
    }

    if (!pl_render_image(p->rr, &image, &target, &params)) {
        MP_ERR(p, "Failed rendering frame!\n");
        goto done;
    }

    args->res = mp_image_alloc(mpfmt, fbo->params.w, fbo->params.h);
    if (!args->res)
        goto done;

    args->res->params.color.primaries = target.color.primaries;
    args->res->params.color.transfer = target.color.transfer;
    args->res->params.repr.levels = target.repr.levels;
    args->res->params.color.hdr = target.color.hdr;
    if (args->scaled)
        args->res->params.p_w = args->res->params.p_h = 1;

    bool ok = pl_tex_download(gpu, pl_tex_transfer_params(
        .tex = fbo,
        .ptr = args->res->planes[0],
        .row_pitch = args->res->stride[0],
    ));

    if (!ok)
        TA_FREEP(&args->res);

done:
    pl_tex_destroy(gpu, &fbo);
}

static const struct pl_filter_config *map_scaler(struct priv *p,
                                                 enum scaler_unit unit)
{
    const struct pl_filter_preset fixed_scalers[] = {
        { "bilinear",       &pl_filter_bilinear },
        { "bicubic_fast",   &pl_filter_bicubic },
        { "nearest",        &pl_filter_nearest },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset fixed_frame_mixers[] = {
        { "linear",         &pl_filter_bilinear },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset *fixed_presets =
        unit == SCALER_TSCALE ? fixed_frame_mixers : fixed_scalers;

    const struct gl_video_opts *opts = p->opts_cache->opts;
    const struct scaler_config *cfg = &opts->scaler[unit];
    struct scaler_config tmp;
    if (cfg->kernel.function == SCALER_INHERIT) {
        tmp = *cfg;
        scaler_conf_merge(&tmp, &opts->scaler[SCALER_SCALE], unit);
        cfg = &tmp;
    }

    const char *kernel_name = m_opt_choice_str(cfg->kernel.functions,
                                               cfg->kernel.function);

    for (int i = 0; fixed_presets[i].name; i++) {
        if (strcmp(kernel_name, fixed_presets[i].name) == 0)
            return fixed_presets[i].filter;
    }

    struct scaler_params *par = &p->scalers[unit];
    const struct pl_filter_preset *preset;
    const struct pl_filter_function_preset *fpreset;
    if ((preset = pl_find_filter_preset(kernel_name))) {
        par->config = *preset->filter;
    } else if ((fpreset = pl_find_filter_function_preset(kernel_name))) {
        par->config = (struct pl_filter_config) {
            .kernel = fpreset->function,
            .params[0] = fpreset->function->params[0],
            .params[1] = fpreset->function->params[1],
        };
    } else {
        MP_ERR(p, "Failed mapping filter function '%s', no libplacebo analog?\n",
               kernel_name);
        return &pl_filter_bilinear;
    }

    const struct pl_filter_function_preset *wpreset;
    if ((wpreset = pl_find_filter_function_preset(
             m_opt_choice_str(cfg->window.functions, cfg->window.function)))) {
        par->config.window = wpreset->function;
        par->config.wparams[0] = wpreset->function->params[0];
        par->config.wparams[1] = wpreset->function->params[1];
    }

    for (int i = 0; i < 2; i++) {
        if (!isnan(cfg->kernel.params[i]))
            par->config.params[i] = cfg->kernel.params[i];
        if (!isnan(cfg->window.params[i]))
            par->config.wparams[i] = cfg->window.params[i];
    }

    par->config.clamp = cfg->clamp;
    if (cfg->antiring > 0.0)
        par->config.antiring = cfg->antiring;
    if (cfg->kernel.blur > 0.0)
        par->config.blur = cfg->kernel.blur;
    if (cfg->kernel.taper > 0.0)
        par->config.taper = cfg->kernel.taper;
    if (cfg->radius > 0.0) {
        if (par->config.kernel->resizable) {
            par->config.radius = cfg->radius;
        } else {
            MP_WARN(p, "Filter radius specified but filter '%s' is not resizable, ignoring\n",
                    kernel_name);
        }
    }

    return &par->config;
}

void gpu_next_configure_queue(struct priv *p, struct vo *vo)
{
    if (!vo)
        return;

    int req_frames = 2;
    if (p->pars->params.frame_mixer) {
        req_frames += ceilf(p->pars->params.frame_mixer->kernel->radius) *
                      (p->pars->params.skip_anti_aliasing ? 1 : 2);
    }
    vo_set_queue_params(vo, 0, MPMIN(VO_MAX_REQ_FRAMES, req_frames));
}

static void gpu_next_update_render_options(struct priv *p)
{
    pl_options pars = p->pars;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    pars->params.background_color[0] = opts->background_color.r / 255.0;
    pars->params.background_color[1] = opts->background_color.g / 255.0;
    pars->params.background_color[2] = opts->background_color.b / 255.0;
    pars->params.background_transparency = 1 - opts->background_color.a / 255.0;
    pars->params.skip_anti_aliasing = !opts->correct_downscaling;
    pars->params.disable_linear_scaling = !opts->linear_downscaling && !opts->linear_upscaling;
    pars->params.disable_fbos = opts->dumb_mode == 1;

    static const int map_background_types[] = {
        [BACKGROUND_NONE]  = PL_CLEAR_SKIP,
        [BACKGROUND_COLOR] = PL_CLEAR_COLOR,
        [BACKGROUND_TILES] = PL_CLEAR_TILES,
        [BACKGROUND_BLUR]  = PL_CLEAR_BLUR,
    };
    pars->params.background = map_background_types[opts->background];
    pars->params.border = map_background_types[p->next_opts->border_background];
    pars->params.blur_radius = p->next_opts->background_blur_radius;
    pars->params.tile_size = opts->background_tile_size * 2;
    for (int i = 0; i < 2; ++i) {
        pars->params.tile_colors[i][0] = opts->background_tile_color[i].r / 255.0f;
        pars->params.tile_colors[i][1] = opts->background_tile_color[i].g / 255.0f;
        pars->params.tile_colors[i][2] = opts->background_tile_color[i].b / 255.0f;
    }

    pars->params.corner_rounding = p->next_opts->corner_rounding;
    pars->params.correct_subpixel_offsets = !opts->scaler_resizes_only;

    pars->params.upscaler = map_scaler(p, SCALER_SCALE);
    pars->params.downscaler = map_scaler(p, SCALER_DSCALE);
    pars->params.plane_upscaler = map_scaler(p, SCALER_CSCALE);
    pars->params.frame_mixer = opts->interpolation ? map_scaler(p, SCALER_TSCALE) : NULL;

    pars->params.deband_params = opts->deband ? &pars->deband_params : NULL;
    pars->deband_params.iterations = opts->deband_opts->iterations;
    pars->deband_params.radius = opts->deband_opts->range;
    pars->deband_params.threshold = opts->deband_opts->threshold / 16.384;
    pars->deband_params.grain = opts->deband_opts->grain / 8.192;

    pars->params.sigmoid_params = opts->sigmoid_upscaling ? &pars->sigmoid_params : NULL;
    pars->sigmoid_params.center = opts->sigmoid_center;
    pars->sigmoid_params.slope = opts->sigmoid_slope;

    pars->params.peak_detect_params = opts->tone_map.compute_peak >= 0 ? &pars->peak_detect_params : NULL;
    pars->peak_detect_params.smoothing_period = opts->tone_map.decay_rate;
    pars->peak_detect_params.scene_threshold_low = opts->tone_map.scene_threshold_low;
    pars->peak_detect_params.scene_threshold_high = opts->tone_map.scene_threshold_high;
    pars->peak_detect_params.percentile = opts->tone_map.peak_percentile;
    pars->peak_detect_params.allow_delayed = p->next_opts->delayed_peak;

    const struct pl_tone_map_function * const tone_map_funs[] = {
        [TONE_MAPPING_AUTO]     = &pl_tone_map_auto,
        [TONE_MAPPING_CLIP]     = &pl_tone_map_clip,
        [TONE_MAPPING_MOBIUS]   = &pl_tone_map_mobius,
        [TONE_MAPPING_REINHARD] = &pl_tone_map_reinhard,
        [TONE_MAPPING_HABLE]    = &pl_tone_map_hable,
        [TONE_MAPPING_GAMMA]    = &pl_tone_map_gamma,
        [TONE_MAPPING_LINEAR]   = &pl_tone_map_linear,
        [TONE_MAPPING_SPLINE]   = &pl_tone_map_spline,
        [TONE_MAPPING_BT_2390]  = &pl_tone_map_bt2390,
        [TONE_MAPPING_BT_2446A] = &pl_tone_map_bt2446a,
        [TONE_MAPPING_ST2094_40] = &pl_tone_map_st2094_40,
        [TONE_MAPPING_ST2094_10] = &pl_tone_map_st2094_10,
    };

    const struct pl_gamut_map_function * const gamut_modes[] = {
        [GAMUT_AUTO]            = pl_color_map_default_params.gamut_mapping,
        [GAMUT_CLIP]            = &pl_gamut_map_clip,
        [GAMUT_PERCEPTUAL]      = &pl_gamut_map_perceptual,
        [GAMUT_RELATIVE]        = &pl_gamut_map_relative,
        [GAMUT_SATURATION]      = &pl_gamut_map_saturation,
        [GAMUT_ABSOLUTE]        = &pl_gamut_map_absolute,
        [GAMUT_DESATURATE]      = &pl_gamut_map_desaturate,
        [GAMUT_DARKEN]          = &pl_gamut_map_darken,
        [GAMUT_WARN]            = &pl_gamut_map_highlight,
        [GAMUT_LINEAR]          = &pl_gamut_map_linear,
    };

    pars->color_map_params.tone_mapping_function = tone_map_funs[opts->tone_map.curve];
AV_NOWARN_DEPRECATED(
    pars->color_map_params.tone_mapping_param = opts->tone_map.curve_param;
    if (isnan(pars->color_map_params.tone_mapping_param))
        pars->color_map_params.tone_mapping_param = 0.0;
)
    pars->color_map_params.inverse_tone_mapping = opts->tone_map.inverse;
    pars->color_map_params.contrast_recovery = opts->tone_map.contrast_recovery;
    pars->color_map_params.visualize_lut = opts->tone_map.visualize;
    pars->color_map_params.contrast_smoothness = opts->tone_map.contrast_smoothness;
    pars->color_map_params.gamut_mapping = gamut_modes[opts->tone_map.gamut_mode];

    pars->params.dither_params = NULL;
    pars->params.error_diffusion = NULL;

    switch (opts->dither_algo) {
    case DITHER_ERROR_DIFFUSION:
        pars->params.error_diffusion = pl_find_error_diffusion_kernel(opts->error_diffusion);
        if (!pars->params.error_diffusion) {
            MP_WARN(p, "Could not find error diffusion kernel '%s', falling back to fruit.\n",
                    opts->error_diffusion);
        }
        MP_FALLTHROUGH;
    case DITHER_ORDERED:
    case DITHER_FRUIT:
        pars->params.dither_params = &pars->dither_params;
        pars->dither_params.method = opts->dither_algo == DITHER_ORDERED
                                ? PL_DITHER_ORDERED_FIXED
                                : PL_DITHER_BLUE_NOISE;
        pars->dither_params.lut_size = opts->dither_size;
        pars->dither_params.temporal = opts->temporal_dither;
        break;
    }

    if (opts->dither_depth < 0) {
        pars->params.dither_params = NULL;
        pars->params.error_diffusion = NULL;
    }

    gpu_next_update_icc_opts(p, opts->icc_opts);

    pars->params.num_hooks = 0;
    const struct pl_hook *hook;
    for (int i = 0; opts->user_shaders && opts->user_shaders[i]; i++) {
        if ((hook = gpu_next_load_user_hook(p, opts->user_shaders[i]))) {
            MP_TARRAY_APPEND(p, p->hooks, pars->params.num_hooks, hook);
            gpu_next_update_hook_opts(p, opts->user_shader_opts,
                                      opts->user_shaders[i], hook);
        }
    }

    pars->params.hooks = p->hooks;

    MP_DBG(p, "Render options updated, resetting render state.\n");
    p->want_reset = true;
}

bool gpu_next_refresh_options(struct priv *p)
{
    pl_options pars = p->pars;
    int old_image_lut_type = p->next_opts->image_lut.type;
    bool changed = m_config_cache_update(p->opts_cache);
    changed = m_config_cache_update(p->next_opts_cache) || changed;
    if (changed) {
        struct user_lut image_lut = p->next_opts->image_lut;
        p->want_reset |= image_lut.opt &&
            ((!image_lut.path && image_lut.opt) ||
             (image_lut.path && strcmp(image_lut.path, image_lut.opt)) ||
             (old_image_lut_type != image_lut.type));
        gpu_next_update_render_options(p);
    }

    gpu_next_update_lut(p, &p->next_opts->lut);
    pars->params.lut = p->next_opts->lut.lut;
    pars->params.lut_type = p->next_opts->lut.type;

    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    mp_csp_equalizer_state_get(p->video_eq, &cparams);
    pars->color_adjustment.brightness = cparams.brightness;
    pars->color_adjustment.contrast = cparams.contrast;
    pars->color_adjustment.hue = cparams.hue;
    pars->color_adjustment.saturation = cparams.saturation;
    pars->color_adjustment.gamma = cparams.gamma * opts->gamma;
    p->output_levels = cparams.levels_out;

    for (char **kv = p->next_opts->raw_opts; kv && kv[0]; kv += 2)
        pl_options_set_str(pars, kv[0], kv[1]);

    return changed;
}

