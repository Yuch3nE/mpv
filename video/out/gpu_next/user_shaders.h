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