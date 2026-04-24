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

#include "video.h"

struct gpu_next_osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct gpu_next_osd_state {
    struct gpu_next_osd_entry entries[MAX_OSD_PARTS];
    struct pl_overlay overlays[MAX_OSD_PARTS];
};

void gpu_next_update_overlays(gpu_next *p, struct mp_osd_res res,
                              int flags, enum pl_overlay_coords coords,
                              struct gpu_next_osd_state *state,
                              struct pl_frame *frame, struct mp_image *src,
                              int stereo_mode);