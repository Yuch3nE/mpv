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

#pragma once

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "private.h"

// Forward delayed hwdec format requests to the shared ra_hwdec_ctx.
void gpu_next_load_hwdec_api(struct priv *p, struct mp_hwdec_devices *devs,
                             void *data);

// Reconfigure the hwdec mapper for the given image params; returns true on
// success and stores the mapper in `p->hwdec_mapper`.
bool gpu_next_hwdec_reconfig(struct priv *p, struct ra_hwdec *hwdec,
                             const struct mp_image_params *par);

// pl_source_frame.acquire / release callbacks used by the renderer to map
// hardware-decoded surfaces into pl_frame planes.
bool gpu_next_hwdec_acquire(pl_gpu gpu, struct pl_frame *frame);
void gpu_next_hwdec_release(pl_gpu gpu, struct pl_frame *frame);
