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

#include "misc/bstr.h"
#include "options/m_option.h"
#include "options/path.h"
#include "stream/stream.h"

#include "user_shaders.h"

const struct pl_hook *gpu_next_load_user_hook(struct priv *p,
                                              const char *path)
{
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < p->num_user_hooks; i++) {
        if (strcmp(p->user_hooks[i].path, path) == 0)
            return p->user_hooks[i].hook;
    }

    char *fname = mp_get_user_path(NULL, p->global, path);
    bstr shader = stream_read_file(fname, p, p->global, 1000000000);
    talloc_free(fname);

    const struct pl_hook *hook = NULL;
    if (shader.len)
        hook = pl_mpv_user_shader_parse(p->gpu, shader.start, shader.len);

    MP_TARRAY_APPEND(p, p->user_hooks, p->num_user_hooks, (struct user_hook) {
        .path = talloc_strdup(p, path),
        .hook = hook,
    });

    return hook;
}

void gpu_next_update_hook_opts_dynamic(struct priv *p,
                                       const struct pl_hook *hook,
                                       const struct mp_image *mpi)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        double val;
        const struct pl_hook_par *hp = &hook->parameters[i];
        if (!gpu_get_auto_param(mpi, bstr0(hp->name), &val))
            continue;

        switch (hp->type) {
        case PL_VAR_FLOAT: hp->data->f = val; break;
        case PL_VAR_SINT:  hp->data->i = lrint(val); break;
        case PL_VAR_UINT:  hp->data->u = lrint(val); break;
        }
    }
}

void gpu_next_update_hook_opts(struct priv *p, char **opts,
                               const char *shaderpath,
                               const struct pl_hook *hook)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *hp = &hook->parameters[i];
        memcpy(hp->data, &hp->initial, sizeof(*hp->data));
    }

    if (!opts)
        return;

    const char *basename = mp_basename(shaderpath);
    struct bstr shadername;
    if (!mp_splitext(basename, &shadername))
        shadername = bstr0(basename);

    for (int n = 0; opts[n * 2]; n++) {
        struct bstr k = bstr0(opts[n * 2 + 0]);
        struct bstr v = bstr0(opts[n * 2 + 1]);
        int pos;
        if ((pos = bstrchr(k, '/')) >= 0) {
            if (!bstr_equals(bstr_splice(k, 0, pos), shadername))
                continue;
            k = bstr_cut(k, pos + 1);
        }

        for (int i = 0; i < hook->num_parameters; i++) {
            const struct pl_hook_par *hp = &hook->parameters[i];
            if (!bstr_equals0(k, hp->name) != 0)
                continue;

            m_option_t opt = {
                .name = hp->name,
            };

            if (hp->names) {
                for (int j = hp->minimum.i; j <= hp->maximum.i; j++) {
                    if (bstr_equals0(v, hp->names[j])) {
                        hp->data->i = j;
                        goto next_hook;
                    }
                }
            }

            switch (hp->type) {
            case PL_VAR_FLOAT:
                opt.type = &m_option_type_float;
                opt.min = hp->minimum.f;
                opt.max = hp->maximum.f;
                break;
            case PL_VAR_SINT:
                opt.type = &m_option_type_int;
                opt.min = hp->minimum.i;
                opt.max = hp->maximum.i;
                break;
            case PL_VAR_UINT:
                opt.type = &m_option_type_int;
                opt.min = MPMIN(hp->minimum.u, INT_MAX);
                opt.max = MPMIN(hp->maximum.u, INT_MAX);
                break;
            }

            if (!opt.type)
                goto next_hook;

            opt.type->parse(p->log, &opt, k, v, hp->data);
            goto next_hook;
        }

    next_hook:;
    }
}