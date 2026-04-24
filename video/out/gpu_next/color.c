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

#include <string.h>

#include "options/path.h"
#include "stream/stream.h"

#include "color.h"

static bool update_icc(struct priv *p, struct bstr icc)
{
    struct pl_icc_profile profile = {
        .data = icc.start,
        .len  = icc.len,
    };

    pl_icc_profile_compute_signature(&profile);

    bool ok = pl_icc_update(p->pllog, &p->icc_profile, &profile, &p->icc_params);
    talloc_free(icc.start);
    return ok;
}

void gpu_next_set_icc_profile_data(struct priv *p, struct bstr profile)
{
    // Take ownership of profile.start, replacing any auto-loaded ICC data
    // and inhibiting future auto-profile updates from overriding it.
    update_icc(p, profile);
    TA_FREEP(&p->icc_path);
}

bool gpu_next_update_auto_profile(struct priv *p, int *events)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (!opts->icc_opts || !opts->icc_opts->profile_auto || p->icc_path)
        return false;

    MP_VERBOSE(p, "Querying ICC profile...\n");
    bstr icc = {0};
    int r = p->ra_ctx->fns->control(p->ra_ctx, events, VOCTRL_GET_ICC_PROFILE, &icc);

    if (r != VO_NOTAVAIL) {
        if (r == VO_FALSE) {
            MP_WARN(p, "Could not retrieve an ICC profile.\n");
        } else if (r == VO_NOTIMPL) {
            MP_ERR(p, "icc-profile-auto not implemented on this platform.\n");
        }

        update_icc(p, icc);
        return true;
    }

    return false;
}

void gpu_next_update_icc_opts(struct priv *p, const struct mp_icc_opts *opts)
{
    if (!opts)
        return;

    if (!opts->profile_auto && !p->icc_path)
        update_icc(p, (bstr){0});

    int s_r = 0, s_g = 0, s_b = 0;
    gl_parse_3dlut_size(opts->size_str, &s_r, &s_g, &s_b);
    p->icc_params = pl_icc_default_params;
    p->icc_params.intent = opts->intent;
    p->icc_params.size_r = s_r;
    p->icc_params.size_g = s_g;
    p->icc_params.size_b = s_b;
    p->icc_params.cache = p->icc_cache.cache;

    if (!opts->profile || !opts->profile[0]) {
        update_icc(p, (bstr){0});
        TA_FREEP(&p->icc_path);
        return;
    }

    if (p->icc_path && strcmp(opts->profile, p->icc_path) == 0)
        return;

    char *fname = mp_get_user_path(NULL, p->global, opts->profile);
    MP_VERBOSE(p, "Opening ICC profile '%s'\n", fname);
    struct bstr icc = stream_read_file(fname, p, p->global, 100000000);
    talloc_free(fname);
    update_icc(p, icc);
    talloc_replace(p, p->icc_path, opts->profile);
}

void gpu_next_update_lut(struct priv *p, struct user_lut *lut)
{
    if (!lut->opt || !lut->opt[0]) {
        pl_lut_free(&lut->lut);
        TA_FREEP(&lut->path);
        return;
    }

    if (lut->path && strcmp(lut->path, lut->opt) == 0)
        return;

    pl_lut_free(&lut->lut);
    talloc_replace(p, lut->path, lut->opt);

    char *fname = mp_get_user_path(NULL, p->global, lut->path);
    MP_VERBOSE(p, "Loading custom LUT '%s'\n", fname);
    const int lut_max_size = 1536 << 20;
    struct bstr lutdata = stream_read_file(fname, NULL, p->global, lut_max_size);
    if (!lutdata.len) {
        MP_ERR(p, "Failed to read LUT data from %s, make sure it's a valid file and smaller or equal to %d bytes\n",
               fname, lut_max_size);
    } else {
        lut->lut = pl_lut_parse_cube(p->pllog, lutdata.start, lutdata.len);
    }
    talloc_free(fname);
    talloc_free(lutdata.start);
}