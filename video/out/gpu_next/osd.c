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

#include "private.h"

void gpu_next_update_overlays(gpu_next *p, struct mp_osd_res res,
                              int flags, enum pl_overlay_coords coords,
                              struct gpu_next_osd_state *state,
                              struct pl_frame *frame, struct mp_image *src,
                              int stereo_mode)
{
    double pts = src ? src->pts : 0;
    int div[2];
    mp_get_3d_side_by_side(stereo_mode, div);
    res.w /= div[0];
    res.h /= div[1];
    frame->overlays = state->overlays;
    frame->num_overlays = 0;

    if (!p->osd)
        return;

    struct sub_bitmap_list *subs = osd_render(p->osd, res, pts, flags,
                                              mp_draw_sub_formats);

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;
        struct gpu_next_osd_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);
        bool ok = pl_tex_recreate(p->gpu, &entry->tex, &(struct pl_tex_params) {
            .format = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable = true,
        });
        if (!ok) {
            MP_ERR(p, "Failed recreating OSD texture!\n");
            break;
        }
        ok = pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
            .tex        = entry->tex,
            .rc         = { .x1 = item->packed_w, .y1 = item->packed_h },
            .row_pitch  = item->packed->stride[0],
            .ptr        = item->packed->planes[0],
        });
        if (!ok) {
            MP_ERR(p, "Failed uploading OSD texture!\n");
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8) & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                },
            };
            MP_TARRAY_APPEND(p, entry->parts, entry->num_parts, part);
        }

        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = pl_color_space_srgb,
            .coords = coords,
        };

        switch (item->format) {
        case SUBBITMAP_BGRA:
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            if (src) {
                ol->color = src->params.color;
                if (pl_color_transfer_is_hdr(ol->color.transfer)) {
                    bool use_static = p->next_opts->image_subs_hdr_peak == -2;
                    if (use_static || p->next_opts->image_subs_hdr_peak == -3) {
                        float max;
                        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                            .color      = &ol->color,
                            .metadata   = use_static ? PL_HDR_METADATA_HDR10
                                                     : PL_HDR_METADATA_ANY,
                            .scaling    = PL_HDR_NITS,
                            .out_max    = &max,
                        ));
                        ol->color.hdr = (struct pl_hdr_metadata) {
                            .max_luma = max,
                        };
                    } else if (p->next_opts->image_subs_hdr_peak != -1) {
                        ol->color.hdr = (struct pl_hdr_metadata) {
                            .max_luma = p->next_opts->image_subs_hdr_peak,
                        };
                    }
                }
            }
            break;
        case SUBBITMAP_LIBASS:
            if (src && item->video_color_space && !pl_color_space_is_hdr(&src->params.color))
                ol->color = src->params.color;
            if (src && pl_color_transfer_is_hdr(frame->color.transfer)) {
                ol->color.hdr = (struct pl_hdr_metadata) {
                    .max_luma = p->next_opts->sub_hdr_peak,
                };
            }
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            break;
        }

        if (div[0] > 1 || div[1] > 1) {
            int orig_num = entry->num_parts;
            for (int x = 0; x < div[0]; x++) {
                for (int y = 0; y < div[1]; y++) {
                    if (x == 0 && y == 0)
                        continue;
                    float off_x = res.w * x;
                    float off_y = res.h * y;
                    for (int i = 0; i < orig_num; i++) {
                        struct pl_overlay_part duped = entry->parts[i];
                        duped.dst.x0 += off_x;
                        duped.dst.x1 += off_x;
                        duped.dst.y0 += off_y;
                        duped.dst.y1 += off_y;
                        MP_TARRAY_APPEND(p, entry->parts, entry->num_parts,
                                         duped);
                    }
                }
            }
            ol->parts = entry->parts;
            ol->num_parts = entry->num_parts;
        }
    }

    talloc_free(subs);
}