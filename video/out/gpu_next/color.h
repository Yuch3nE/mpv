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

bool gpu_next_update_auto_profile(struct priv *p, int *events);
void gpu_next_update_icc_opts(struct priv *p, const struct mp_icc_opts *opts);
void gpu_next_update_lut(struct priv *p, struct user_lut *lut);
void gpu_next_set_icc_profile_data(struct priv *p, struct bstr profile);