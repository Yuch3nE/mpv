/*
 * Copyright (c) 2013 Stefano Pigozzi <stefano.pigozzi@gmail.com>
 *               2017 Aman Gupta <ffmpeg@tmm1.net>
 *               2023 rcombs <rcombs@rcombs.me>
 *
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
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <libavutil/hwcontext.h>

#include <libplacebo/renderer.h>

#include "config.h"

#include "video/out/gpu/hwdec.h"
#include "video/out/placebo/ra_pl.h"
#include "video/mp_image_pool.h"

#if HAVE_VULKAN
#include "video/out/vulkan/common.h"
#endif

#include "hwdec_vt.h"

static bool check_hwdec(const struct ra_hwdec *hw)
{
    pl_gpu gpu = ra_pl_get(hw->ra_ctx->ra);
    if (!gpu) {
        // This is not a libplacebo RA;
        return false;
    }

    if (!(gpu->import_caps.tex & PL_HANDLE_IOSURFACE)) {
        MP_VERBOSE(hw, "VideoToolbox libplacebo interop requires support for "
                       "PL_HANDLE_IOSURFACE import.\n");
        return false;
    }

    return true;
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    if (!mapper->dst_params.imgfmt) {
        MP_ERR(mapper, "Unsupported CVPixelBuffer format.\n");
        return -1;
    }

    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &p->desc)) {
        MP_ERR(mapper, "Unsupported texture format.\n");
        return -1;
    }

    for (int n = 0; n < p->desc.num_planes; n++) {
        if (!p->desc.planes[n] || p->desc.planes[n]->ctype != RA_CTYPE_UNORM) {
            MP_ERR(mapper, "Format unsupported.\n");
            return -1;
        }
    }

    // The IOSurface import path does not need an MTLDevice or
    // CVMetalTextureCache. Leave p->mtl_texture_cache as NULL.
    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    pl_gpu gpu = ra_pl_get(mapper->owner->ra_ctx->ra);

    // The per-plane ratex entries reference sub-planes of p->iosurf_pltex.
    // Clear their priv to prevent ra_tex_free -> pl_tex_destroy from running
    // on a sub-plane (which is undefined behavior per libplacebo's API).
    for (int i = 0; i < p->desc.num_planes; i++) {
        if (mapper->tex[i])
            mapper->tex[i]->priv = NULL;
        ra_tex_free(mapper->ra, &mapper->tex[i]);
    }

    if (p->iosurf_pltex) {
        pl_tex parent = (pl_tex) p->iosurf_pltex;
        pl_tex_destroy(gpu, &parent);
        p->iosurf_pltex = NULL;
    }
}

// Map kCVPixelFormatType_* to a libplacebo planar pl_fmt name. Only the
// formats the VideoToolbox decoder actually emits are listed here.
static const struct {
    OSType cv_fmt;
    const char *pl_fmt_name;
} cv_to_pl_fmt[] = {
    // 4:2:0 8-bit bi-planar (NV12)
    {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,  "g8_br8_420"},
    {kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,   "g8_br8_420"},
    // 4:2:0 10-bit bi-planar (P010)
    {kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, "gx10_bxrx10_420"},
    {kCVPixelFormatType_420YpCbCr10BiPlanarFullRange,  "gx10_bxrx10_420"},
    // 4:2:2 10-bit bi-planar
    {kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, "gx10_bxrx10_422"},
    {kCVPixelFormatType_422YpCbCr10BiPlanarFullRange,  "gx10_bxrx10_422"},
    // 4:4:4 10-bit bi-planar
    {kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, "gx10_bxrx10_444"},
    {kCVPixelFormatType_444YpCbCr10BiPlanarFullRange,  "gx10_bxrx10_444"},
    {0, NULL},
};

static const char *cv_fmt_to_pl_name(OSType cv_fmt)
{
    for (int i = 0; cv_to_pl_fmt[i].pl_fmt_name; i++) {
        if (cv_to_pl_fmt[i].cv_fmt == cv_fmt)
            return cv_to_pl_fmt[i].pl_fmt_name;
    }
    return NULL;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    pl_gpu gpu = ra_pl_get(mapper->owner->ra_ctx->ra);

    CVPixelBufferRelease(p->pbuf);
    p->pbuf = (CVPixelBufferRef)mapper->src->planes[3];
    CVPixelBufferRetain(p->pbuf);

    const bool planar = CVPixelBufferIsPlanar(p->pbuf);
    const int planes  = CVPixelBufferGetPlaneCount(p->pbuf);
    mp_assert((planar && planes == p->desc.num_planes) || p->desc.num_planes == 1);

    IOSurfaceRef iosurf = CVPixelBufferGetIOSurface(p->pbuf);
    if (!iosurf) {
        MP_ERR(mapper, "CVPixelBuffer is not IOSurface backed.\n");
        return -1;
    }

    OSType cv_fmt = CVPixelBufferGetPixelFormatType(p->pbuf);
    const char *pl_fmt_name = cv_fmt_to_pl_name(cv_fmt);
    if (!pl_fmt_name) {
        MP_ERR(mapper, "Unsupported CVPixelFormat 0x%08x for IOSurface "
                       "import.\n", (unsigned) cv_fmt);
        return -1;
    }

    pl_fmt plfmt = pl_find_named_fmt(gpu, pl_fmt_name);
    if (!plfmt) {
        MP_ERR(mapper, "libplacebo lacks pl_fmt '%s'.\n", pl_fmt_name);
        return -1;
    }

    const size_t width  = CVPixelBufferGetWidth(p->pbuf);
    const size_t height = CVPixelBufferGetHeight(p->pbuf);

    struct pl_tex_params tex_params = {
        .w = width,
        .h = height,
        .d = 0,
        .format = plfmt,
        .sampleable = true,
        .import_handle = PL_HANDLE_IOSURFACE,
        .shared_mem = (struct pl_shared_mem) {
            .handle = { .handle = iosurf },
        },
    };

    pl_tex pltex = pl_tex_create(gpu, &tex_params);
    if (!pltex) {
        MP_ERR(mapper, "pl_tex_create with PL_HANDLE_IOSURFACE failed.\n");
        return -1;
    }

    p->iosurf_pltex = (void *) pltex;

    const int num_planes = plfmt->num_planes ? plfmt->num_planes : 1;
    if (num_planes != p->desc.num_planes) {
        MP_ERR(mapper, "pl_fmt '%s' plane count %d does not match imgfmt "
                       "plane count %d.\n",
               pl_fmt_name, num_planes, p->desc.num_planes);
        return -1;
    }

    for (int i = 0; i < num_planes; i++) {
        pl_tex sub = plfmt->num_planes ? pltex->planes[i] : pltex;
        if (!sub) {
            MP_ERR(mapper, "pl_tex sub-plane %d missing.\n", i);
            return -1;
        }
        struct ra_tex *ratex = talloc_ptrtype(NULL, ratex);
        if (!mppl_wrap_tex(mapper->ra, sub, ratex)) {
            talloc_free(ratex);
            return -1;
        }
        mapper->tex[i] = ratex;
    }

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    CVPixelBufferRelease(p->pbuf);
}

bool vt_pl_init(const struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    if (!check_hwdec(hw))
        return false;

    p->interop_init   = mapper_init;
    p->interop_uninit = mapper_uninit;
    p->interop_map    = mapper_map;
    p->interop_unmap  = mapper_unmap;

    return true;
}
