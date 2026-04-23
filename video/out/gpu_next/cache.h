#pragma once

#include "private.h"

void gpu_next_cache_init(struct priv *p, struct cache *cache, size_t max_size,
                         const char *dir_opt);
void gpu_next_cache_uninit(struct priv *p, struct cache *cache);