#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>

#include "misc/io_utils.h"
#include "options/path.h"
#include "osdep/io.h"
#include "stream/stream.h"

#include "cache.h"

static char *cache_filepath(void *ta_ctx, char *dir, const char *prefix,
                            uint64_t key)
{
    bstr filename = {0};
    bstr_xappend_asprintf(ta_ctx, &filename, "%s_%016" PRIx64, prefix, key);
    return mp_path_join_bstr(ta_ctx, bstr0(dir), filename);
}

static pl_cache_obj cache_load_obj(void *p, uint64_t key)
{
    struct cache *c = p;
    void *ta_ctx = talloc_new(NULL);
    pl_cache_obj obj = {0};

    if (!c->dir)
        goto done;

    char *filepath = cache_filepath(ta_ctx, c->dir, c->name, key);
    if (!filepath)
        goto done;

    if (stat(filepath, &(struct stat){0}))
        goto done;

    int64_t load_start = mp_time_ns();
    struct bstr data = stream_read_file(filepath, ta_ctx, c->global,
                                        STREAM_MAX_READ_SIZE);
    int64_t load_end = mp_time_ns();
    MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu), load time(%.3f ms)\n",
           __func__, key, data.len, MP_TIME_NS_TO_MS(load_end - load_start));

    obj = (pl_cache_obj){
        .key = key,
        .data = talloc_steal(NULL, data.start),
        .size = data.len,
        .free = talloc_free,
    };

done:
    talloc_free(ta_ctx);
    return obj;
}

static void cache_save_obj(void *p, pl_cache_obj obj)
{
    const struct cache *c = p;
    void *ta_ctx = talloc_new(NULL);

    if (!c->dir)
        goto done;

    char *filepath = cache_filepath(ta_ctx, c->dir, c->name, obj.key);
    if (!filepath)
        goto done;

    if (!obj.data || !obj.size) {
        unlink(filepath);
        goto done;
    }

    struct stat st;
    if (!stat(filepath, &st) && st.st_size == obj.size) {
        MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu)\n", __func__, obj.key,
               obj.size);
        goto done;
    }

    int64_t save_start = mp_time_ns();
    mp_save_to_file(filepath, obj.data, obj.size);
    int64_t save_end = mp_time_ns();
    MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu), save time(%.3f ms)\n",
           __func__, obj.key, obj.size, MP_TIME_NS_TO_MS(save_end - save_start));

done:
    talloc_free(ta_ctx);
}

void gpu_next_cache_init(struct priv *p, struct cache *cache, size_t max_size,
                         const char *dir_opt)
{
    const char *name = cache == &p->shader_cache ? "shader" : "icc";
    const size_t limit = max_size ? max_size :
                         (cache == &p->shader_cache ? 128 << 20 : 1536 << 20);

    char *dir;
    if (dir_opt && dir_opt[0]) {
        dir = mp_get_user_path(p, p->global, dir_opt);
    } else {
        dir = mp_find_user_file(p, p->global, "cache", "");
    }
    if (!dir || !dir[0])
        return;

    mp_mkdirp(dir);
    *cache = (struct cache){
        .log        = p->log,
        .global     = p->global,
        .dir        = dir,
        .name       = name,
        .size_limit = limit,
        .cache = pl_cache_create(pl_cache_params(
            .log = p->pllog,
            .get = cache_load_obj,
            .set = cache_save_obj,
            .priv = cache
        )),
    };
}

struct file_entry {
    char *filepath;
    size_t size;
    time_t atime;
};

static int compare_atime(const void *a, const void *b)
{
    return ((struct file_entry *)b)->atime - ((struct file_entry *)a)->atime;
}

void gpu_next_cache_uninit(struct priv *p, struct cache *cache)
{
    if (!cache->cache)
        return;

    void *ta_ctx = talloc_new(NULL);
    struct file_entry *files = NULL;
    size_t num_files = 0;
    mp_assert(cache->dir);
    mp_assert(cache->name);

    DIR *d = opendir(cache->dir);
    if (!d)
        goto done;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        char *filepath = mp_path_join(ta_ctx, cache->dir, dir->d_name);
        if (!filepath)
            continue;
        struct stat filestat;
        if (stat(filepath, &filestat))
            continue;
        if (!S_ISREG(filestat.st_mode))
            continue;
        bstr fname = bstr0(dir->d_name);
        if (!bstr_eatstart0(&fname, cache->name))
            continue;
        if (!bstr_eatstart0(&fname, "_"))
            continue;
        if (fname.len != 16)
            continue;
        MP_TARRAY_APPEND(ta_ctx, files, num_files,
                         (struct file_entry){
                             .filepath = filepath,
                             .size     = filestat.st_size,
                             .atime    = filestat.st_atime,
                         });
    }
    closedir(d);

    if (!num_files)
        goto done;

    qsort(files, num_files, sizeof(struct file_entry), compare_atime);

    time_t t = time(NULL);
    size_t cache_size = 0;
    size_t cache_limit = cache->size_limit ? cache->size_limit : SIZE_MAX;
    for (int i = 0; i < num_files; i++) {
        cache_size += files[i].size;
        double rel_use = difftime(t, files[i].atime);
        if (cache_size > cache_limit && rel_use > 60 * 60 * 24) {
            MP_VERBOSE(p, "Removing %s | size: %9zu bytes | last used: %9d seconds ago\n",
                       files[i].filepath, files[i].size, (int)rel_use);
            unlink(files[i].filepath);
        }
    }

done:
    talloc_free(ta_ctx);
    pl_cache_destroy(&cache->cache);
}