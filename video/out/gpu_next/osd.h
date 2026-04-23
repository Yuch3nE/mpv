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