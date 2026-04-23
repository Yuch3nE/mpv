#pragma once

#include "private.h"

bool gpu_next_update_auto_profile(struct priv *p, int *events);
void gpu_next_update_icc_opts(struct priv *p, const struct mp_icc_opts *opts);
void gpu_next_update_lut(struct priv *p, struct user_lut *lut);