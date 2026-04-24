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

#include "private.h"

const struct pl_hook *gpu_next_load_user_hook(struct priv *p,
                                              const char *path);
void gpu_next_update_hook_opts_dynamic(struct priv *p,
                                       const struct pl_hook *hook,
                                       const struct mp_image *mpi);
void gpu_next_update_hook_opts(struct priv *p, char **opts,
                               const char *shaderpath,
                               const struct pl_hook *hook);