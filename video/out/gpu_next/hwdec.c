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

#include "config.h"

#include "common/stats.h"
#include "misc/mp_assert.h"
#include "video/mp_image.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"

#include "hwdec.h"
#include "private.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
#include <libplacebo/opengl.h>
#include "video/out/opengl/ra_gl.h"
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
#include <libplacebo/d3d11.h>
#include "video/out/d3d11/ra_d3d11.h"
#include "osdep/windows_utils.h"
#endif

void gpu_next_load_hwdec_api(struct priv *p, struct mp_hwdec_devices *hwdec_devs,
                             void *data)
{
    ra_hwdec_ctx_load_fmt(&p->hwdec_ctx, hwdec_devs, data);
}

bool gpu_next_hwdec_reconfig(struct priv *p, struct ra_hwdec *hwdec,
                             const struct mp_image_params *par)
{
    if (p->hwdec_mapper) {
        if (mp_image_params_static_equal(par, &p->hwdec_mapper->src_params)) {
            p->hwdec_mapper->src_params.repr.dovi = par->repr.dovi;
            p->hwdec_mapper->dst_params.repr.dovi = par->repr.dovi;
            p->hwdec_mapper->src_params.color.hdr = par->color.hdr;
            p->hwdec_mapper->dst_params.color.hdr = par->color.hdr;
            return p->hwdec_mapper;
        }
        ra_hwdec_mapper_free(&p->hwdec_mapper);
        timer_pool_destroy(p->hwdec_timer);
        p->hwdec_timer = NULL;
    }

    p->hwdec_mapper = ra_hwdec_mapper_create(hwdec, par);
    if (!p->hwdec_mapper) {
        MP_ERR(p, "Initializing texture for hardware decoding failed.\n");
        return NULL;
    }
    p->hwdec_timer = timer_pool_create(p->ra_ctx->ra);

    return p->hwdec_mapper;
}

static pl_tex hwdec_get_tex(struct priv *p, int n)
{
    struct ra_tex *ratex = p->hwdec_mapper->tex[n];
    struct ra *ra = p->hwdec_mapper->ra;
    if (ra_pl_get(ra))
        return (pl_tex)ratex->priv;

#if HAVE_GL && defined(PL_HAVE_OPENGL)
    if (ra_is_gl(ra) && pl_opengl_get(p->gpu)) {
        struct pl_opengl_wrap_params par = {
            .width = ratex->params.w,
            .height = ratex->params.h,
        };

        ra_gl_get_format(ratex->params.format, &par.iformat,
                         &(GLenum){0}, &(GLenum){0});
        ra_gl_get_raw_tex(ra, ratex, &par.texture, &par.target);
        return pl_opengl_wrap(p->gpu, &par);
    }
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (ra_is_d3d11(ra)) {
        int array_slice = 0;
        ID3D11Resource *res = ra_d3d11_get_raw_tex(ra, ratex, &array_slice);
        pl_tex tex = pl_d3d11_wrap(p->gpu, pl_d3d11_wrap_params(
            .tex = res,
            .array_slice = array_slice,
            .fmt = ra_d3d11_get_format(ratex->params.format),
            .w = ratex->params.w,
            .h = ratex->params.h,
        ));
        SAFE_RELEASE(res);
        return tex;
    }
#endif

    MP_ERR(p, "Failed mapping hwdec frame? Open a bug!\n");
    return false;
}

bool gpu_next_hwdec_acquire(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->p;
    if (!gpu_next_hwdec_reconfig(p, fp->hwdec, &mpi->params))
        return false;

    stats_time_start(p->stats, "hwdec-map");
    timer_pool_start(p->hwdec_timer);
    if (ra_hwdec_mapper_map(p->hwdec_mapper, mpi) < 0) {
        MP_ERR(p, "Mapping hardware decoded surface failed.\n");
        timer_pool_stop(p->hwdec_timer);
        stats_time_end(p->stats, "hwdec-map");
        return false;
    }

    for (int n = 0; n < frame->num_planes; n++) {
        if (!(frame->planes[n].texture = hwdec_get_tex(p, n))) {
            timer_pool_stop(p->hwdec_timer);
            stats_time_end(p->stats, "hwdec-map");
            return false;
        }
    }

    timer_pool_stop(p->hwdec_timer);
    p->hwdec_perf = timer_pool_measure(p->hwdec_timer);
    stats_time_end(p->stats, "hwdec-map");

    return true;
}

void gpu_next_hwdec_release(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->p;
    if (!ra_pl_get(p->hwdec_mapper->ra)) {
        for (int n = 0; n < frame->num_planes; n++)
            pl_tex_destroy(p->gpu, &frame->planes[n].texture);
    }

    ra_hwdec_mapper_unmap(p->hwdec_mapper);
}
