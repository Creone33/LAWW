/*
 * lwan - simple web server
 * Copyright (c) 2012 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include "lwan.h"
#include "lwan-cache.h"
#include "lwan-openat.h"
#include "lwan-serve-files.h"
#include "lwan-sendfile.h"
#include "hash.h"
#include "realpathat.h"

#define SET_NTH_HEADER(number_, key_, value_) \
    do { \
        headers[number_].key = (key_); \
        headers[number_].value = (value_); \
    } while(0)

typedef struct serve_files_priv_t_	serve_files_priv_t;
typedef struct file_cache_entry_t_	file_cache_entry_t;
typedef struct cache_funcs_t_		cache_funcs_t;
typedef struct mmap_cache_data_t_	mmap_cache_data_t;
typedef struct sendfile_cache_data_t_	sendfile_cache_data_t;
typedef struct redir_cache_data_t_	redir_cache_data_t;

struct serve_files_priv_t_ {
    struct {
        char *path;
        size_t path_len;
        int fd;
    } root;

    int extra_modes;
    char *index_html;

    struct cache_t *cache;
};

struct cache_funcs_t_ {
    bool (*init)(file_cache_entry_t *ce,
                 serve_files_priv_t *priv,
                 const char *full_path,
                 struct stat *st);
    void (*free)(void *data);

    lwan_http_status_t (*serve)(file_cache_entry_t *ce,
                                serve_files_priv_t *priv,
                                lwan_request_t *request);
};

struct mmap_cache_data_t_ {
    struct {
        void *contents;
        /* zlib expects unsigned longs instead of size_t */
        unsigned long size;
    } compressed, uncompressed;
};

struct sendfile_cache_data_t_ {
    /*
     * FIXME Investigate if keeping files open and dup()ing them
     *       is faster than openat()ing. This won't scale as well,
     *       but might be a good alternative for popular files.
     */

    char *filename;
    size_t size;
};

struct redir_cache_data_t_ {
    char *redir_to;
};

struct file_cache_entry_t_ {
    struct cache_entry_t base;

    struct {
        char string[31];
        time_t integer;
    } last_modified;

    const char *mime_type;
    const cache_funcs_t *funcs;
};

static bool _mmap_init(file_cache_entry_t *ce, serve_files_priv_t *priv,
                       const char *full_path, struct stat *st);
static void _mmap_free(void *data);
static lwan_http_status_t _mmap_serve(file_cache_entry_t *ce,
                                      serve_files_priv_t *priv,
                                      lwan_request_t *request);
static bool _sendfile_init(file_cache_entry_t *ce, serve_files_priv_t *priv,
                           const char *full_path, struct stat *st);
static void _sendfile_free(void *data);
static lwan_http_status_t _sendfile_serve(file_cache_entry_t *ce,
                                          serve_files_priv_t *priv,
                                          lwan_request_t *request);

static const cache_funcs_t mmap_funcs = {
    .init = _mmap_init,
    .free = _mmap_free,
    .serve = _mmap_serve
};

static const cache_funcs_t sendfile_funcs = {
    .init = _sendfile_init,
    .free = _sendfile_free,
    .serve = _sendfile_serve
};

static char *index_html = "index.html";

static void
_compress_cached_entry(mmap_cache_data_t *md)
{
    static const size_t deflated_header_size = sizeof("Content-Encoding: deflate");

    md->compressed.size = compressBound(md->uncompressed.size);

    if (UNLIKELY(!(md->compressed.contents = malloc(md->compressed.size))))
        goto error_zero_out;

    if (UNLIKELY(compress(md->compressed.contents, &md->compressed.size,
                          md->uncompressed.contents, md->uncompressed.size) != Z_OK))
        goto error_free_compressed;

    if ((md->compressed.size + deflated_header_size) < md->uncompressed.size)
        return;

error_free_compressed:
    free(md->compressed.contents);
    md->compressed.contents = NULL;
error_zero_out:
    md->compressed.size = 0;
}

static bool
_mmap_init(file_cache_entry_t *ce,
           serve_files_priv_t *priv,
           const char *full_path,
           struct stat *st)
{
    mmap_cache_data_t *md = (mmap_cache_data_t *)(ce + 1);
    int file_fd;
    bool success;

    file_fd = open(full_path, O_RDONLY | priv->extra_modes);
    if (UNLIKELY(file_fd < 0))
        return false;

    md->uncompressed.contents = mmap(NULL, st->st_size, PROT_READ,
                                     MAP_SHARED, file_fd, 0);
    if (UNLIKELY(md->uncompressed.contents == MAP_FAILED)) {
        success = false;
        goto close_file;
    }

    if (UNLIKELY(madvise(md->uncompressed.contents, st->st_size,
                         MADV_WILLNEED) < 0))
        lwan_status_perror("madvise");

    md->uncompressed.size = st->st_size;
    _compress_cached_entry(md);

    success = true;

close_file:
    close(file_fd);

    return success;
}

static bool
_sendfile_init(file_cache_entry_t *ce,
               serve_files_priv_t *priv,
               const char *full_path,
               struct stat *st)
{
    sendfile_cache_data_t *sd = (sendfile_cache_data_t *)(ce + 1);

    sd->size = st->st_size;
    sd->filename = strdup(full_path + priv->root.path_len + 1);

    return !!sd->filename;
}

static struct cache_entry_t *
_create_cache_entry(const char *key, void *context)
{
    serve_files_priv_t *priv = context;
    file_cache_entry_t *fce;
    struct stat st;
    size_t data_size;
    const cache_funcs_t *funcs;
    char *full_path;

    full_path = realpathat(priv->root.fd, priv->root.path, key, NULL);
    if (UNLIKELY(!full_path))
        return NULL;
    if (!strcmp(full_path + priv->root.path_len, priv->root.path + priv->root.path_len))
        goto error;

    if (UNLIKELY(fstatat(priv->root.fd, key, &st, 0) < 0))
        goto error;

    if (st.st_size <= 16384) {
        data_size = sizeof(mmap_cache_data_t);
        funcs = &mmap_funcs;
    } else {
        data_size = sizeof(sendfile_cache_data_t);
        funcs = &sendfile_funcs;
    }

    fce = malloc(sizeof(*fce) + data_size);
    if (UNLIKELY(!fce))
        goto error;

    if (UNLIKELY(!funcs->init(fce, priv, full_path, &st)))
        goto error_init;

    lwan_format_rfc_time(st.st_mtime, fce->last_modified.string);

    fce->mime_type = lwan_determine_mime_type_for_file_name(full_path + priv->root.path_len);
    fce->last_modified.integer = st.st_mtime;
    fce->funcs = funcs;

    return (struct cache_entry_t *)fce;

error_init:
    free(fce);
error:
    free(full_path);
    return NULL;
}

static void
_mmap_free(void *data)
{
    mmap_cache_data_t *md = data;

    munmap(md->uncompressed.contents, md->uncompressed.size);
    free(md->compressed.contents);
}

static void
_sendfile_free(void *data)
{
    sendfile_cache_data_t *sd = data;

    free(sd->filename);
}

static void
_destroy_cache_entry(struct cache_entry_t *entry, void *context __attribute__((unused)))
{
    file_cache_entry_t *fce = (file_cache_entry_t *)entry;

    fce->funcs->free(fce + 1);
    free(fce);
}

static void *
serve_files_init(void *args)
{
    struct lwan_serve_files_settings_t *settings = args;
    char *canonical_root;
    int root_fd;
    serve_files_priv_t *priv;
    int extra_modes = O_NOATIME;

    canonical_root = realpath(settings->root_path, NULL);
    if (!canonical_root) {
        lwan_status_perror("Could not obtain real path of \"%s\"",
                            settings->root_path);
        goto out_realpath;
    }

    root_fd = open(canonical_root, O_RDONLY | O_DIRECTORY | extra_modes);
    if (root_fd < 0) {
        root_fd = open(canonical_root, O_RDONLY | O_DIRECTORY);
        extra_modes &= ~O_NOATIME;
    }
    if (root_fd < 0) {
        lwan_status_perror("Could not open directory \"%s\"",
                            canonical_root);
        goto out_open;
    }

    priv = malloc(sizeof(*priv));
    if (!priv) {
        lwan_status_perror("malloc");
        goto out_malloc;
    }

    priv->cache = cache_create(_create_cache_entry, _destroy_cache_entry,
                priv, 5);
    if (!priv->cache) {
        lwan_status_error("Couldn't create cache");
        goto out_cache_create;
    }

    priv->root.path = canonical_root;
    priv->root.path_len = strlen(canonical_root);
    priv->root.fd = root_fd;
    priv->extra_modes = extra_modes;
    priv->index_html = settings->index_html ? settings->index_html : index_html;

    return priv;

out_cache_create:
    free(priv);
out_malloc:
    close(root_fd);
out_open:
    free(canonical_root);
out_realpath:
    return NULL;
}

static void
serve_files_shutdown(void *data)
{
    serve_files_priv_t *priv = data;

    if (!priv) {
        lwan_status_warning("Nothing to shutdown");
        return;
    }

#ifndef NDEBUG
    unsigned hits, misses, evictions;
    cache_get_stats(priv->cache, &hits, &misses, &evictions);
    lwan_status_debug("Cache stats: %d hits, %d misses, %d evictions",
            hits, misses, evictions);
#endif

    cache_destroy(priv->cache);
    close(priv->root.fd);
    free(priv->root.path);
    free(priv);
}

static ALWAYS_INLINE bool
_client_has_fresh_content(lwan_request_t *request, time_t mtime)
{
    return request->header.if_modified_since && mtime <= request->header.if_modified_since;
}

static size_t
_prepare_headers(lwan_request_t *request,
                 lwan_http_status_t return_status,
                 file_cache_entry_t *fce,
                 size_t size,
                 bool deflated,
                 char *header_buf,
                 size_t header_buf_size)
{
    lwan_key_value_t headers[5];

    request->response.headers = headers;
    request->response.content_length = size;

    SET_NTH_HEADER(0, "Last-Modified", fce->last_modified.string);
    SET_NTH_HEADER(1, "Date", request->thread->date.date);
    SET_NTH_HEADER(2, "Expires", request->thread->date.expires);

    if (deflated) {
        SET_NTH_HEADER(3, "Content-Encoding", "deflate");
        SET_NTH_HEADER(4, NULL, NULL);
    } else {
        SET_NTH_HEADER(3, NULL, NULL);
    }

    return lwan_prepare_response_header(request, return_status,
                                    header_buf, header_buf_size);
}

static ALWAYS_INLINE bool
_compute_range(lwan_request_t *request, off_t *from, off_t *to, off_t size)
{
    off_t f, t;

    f = request->header.range.from;
    t = request->header.range.to;

    /*
     * No Range: header present: both t and f are -1
     */
    if (LIKELY(t <= 0 && f <= 0)) {
        *from = 0;
        *to = size;
        return true;
    }

    /*
     * To goes beyond from or To and From are the same: this is unsatisfiable.
     */
    if (UNLIKELY(t >= f))
        return false;

    /*
     * Range goes beyond the size of the file
     */
    if (UNLIKELY(f >= size || t >= size))
        return false;

    /*
     * t < 0 means ranges from f to the file size
     */
    if (t < 0)
        t = size - f;
    else
        t -= f;

    /*
     * If for some reason the previous calculations yields something
     * less than zero, the range is unsatisfiable.
     */
    if (UNLIKELY(t <= 0))
        return false;

    *from = f;
    *to = t;

    return true;
}

static lwan_http_status_t
_sendfile_serve(file_cache_entry_t *fce,
                serve_files_priv_t *priv,
                lwan_request_t *request)
{
    sendfile_cache_data_t *sd = (sendfile_cache_data_t *)(fce + 1);
    char *headers = request->buffer;
    size_t header_len;
    lwan_http_status_t return_status = HTTP_OK;
    off_t from, to;

    if (UNLIKELY(!_compute_range(request, &from, &to, sd->size)))
        return HTTP_RANGE_UNSATISFIABLE;

    if (_client_has_fresh_content(request, fce->last_modified.integer))
        return_status = HTTP_NOT_MODIFIED;

    header_len = _prepare_headers(request, return_status,
                                  fce, sd->size, false,
                                  headers, DEFAULT_HEADERS_SIZE);
    if (UNLIKELY(!header_len))
        return HTTP_INTERNAL_ERROR;

    if (request->method == HTTP_HEAD || return_status == HTTP_NOT_MODIFIED) {
        if (UNLIKELY(write(request->fd, headers, header_len) < 0))
            return HTTP_INTERNAL_ERROR;
    } else {
        /*
         * lwan_openat() will yield from the coroutine if openat()
         * can't open the file due to not having free file descriptors
         * around. This will happen just a handful of times.
         * The file will be automatically closed whenever this
         * coroutine is freed.
         */
        int file_fd = lwan_openat(request, priv->root.fd, sd->filename,
                                  O_RDONLY | priv->extra_modes);
        if (UNLIKELY(file_fd < 0)) {
            switch (file_fd) {
            case -EACCES:
                return HTTP_FORBIDDEN;
            case -ENFILE:
                return HTTP_UNAVAILABLE;
            default:
                return HTTP_NOT_FOUND;
            }
        }

        if (UNLIKELY(send(request->fd, headers, header_len, MSG_MORE) < 0))
            return HTTP_INTERNAL_ERROR;

        if (UNLIKELY(lwan_sendfile(request, file_fd, from, to) < 0))
            return HTTP_INTERNAL_ERROR;
    }

    return return_status;
}

static lwan_http_status_t
_mmap_serve(file_cache_entry_t *fce,
            serve_files_priv_t *priv __attribute__((unused)),
            lwan_request_t *request)
{
    mmap_cache_data_t *md = (mmap_cache_data_t *)(fce + 1);
    char *headers = request->buffer;
    size_t header_len;
    size_t size;
    void *contents;
    lwan_http_status_t return_status = HTTP_OK;
    bool deflated;

    if (_client_has_fresh_content(request, fce->last_modified.integer))
        return_status = HTTP_NOT_MODIFIED;

    deflated = request->header.accept_encoding.deflate && md->compressed.size;
    if (LIKELY(deflated)) {
        contents = md->compressed.contents;
        size = md->compressed.size;
    } else {
        contents = md->uncompressed.contents;
        size = md->uncompressed.size;
    }

    header_len = _prepare_headers(request, return_status,
                                  fce, size, deflated,
                                  headers, DEFAULT_HEADERS_SIZE);
    if (UNLIKELY(!header_len))
        return HTTP_INTERNAL_ERROR;

    if (request->method == HTTP_HEAD || return_status == HTTP_NOT_MODIFIED) {
        if (UNLIKELY(write(request->fd, headers, header_len) < 0))
            return_status = HTTP_INTERNAL_ERROR;
    } else {
        struct iovec response_vec[] = {
            { .iov_base = headers, .iov_len = header_len },
            { .iov_base = contents, .iov_len = size }
        };

        if (UNLIKELY(writev(request->fd, response_vec, N_ELEMENTS(response_vec)) < 0))
            return_status = HTTP_INTERNAL_ERROR;
    }

    return return_status;
}

static struct cache_entry_t *
_create_temporary_cache_entry(serve_files_priv_t *priv, char *path)
{
    file_cache_entry_t *fce;
    sendfile_cache_data_t *sd;
    struct stat st;
    char *real;

    if (UNLIKELY(fstatat(priv->root.fd, path, &st, 0) < 0))
        return NULL;

    if (S_ISDIR(st.st_mode)) {
        char *tmp;

        if (UNLIKELY(asprintf(&tmp, "%s/%s", path, priv->index_html) < 0))
            return NULL;

        fce = (file_cache_entry_t *)_create_temporary_cache_entry(priv, tmp);
        free(tmp);

        return (struct cache_entry_t *)fce;
    }

    fce = malloc(sizeof(*fce) + sizeof(*sd));
    if (UNLIKELY(!fce))
        return NULL;

    sd = (sendfile_cache_data_t *)(fce + 1);
    sd->size = st.st_size;

    real = realpathat(priv->root.fd, priv->root.path, path, NULL);
    if (UNLIKELY(!real))
        goto error_realpath_failed;
    if (UNLIKELY(strncmp(real, priv->root.path, priv->root.path_len)))
        goto error_not_in_canonical_path;

    sd->filename = real;

    lwan_format_rfc_time(st.st_mtime, fce->last_modified.string);
    fce->last_modified.integer = st.st_mtime;
    fce->mime_type = lwan_determine_mime_type_for_file_name(path);
    fce->funcs = &sendfile_funcs;

    cache_entry_set_floating((struct cache_entry_t *)fce, true);

    return (struct cache_entry_t *)fce;

error_not_in_canonical_path:
    free(real);
error_realpath_failed:
    free(fce);
    return NULL;
}

static struct cache_entry_t *
_fetch_from_cache_and_ref(serve_files_priv_t *priv, char *path)
{
    struct cache_entry_t *ce;
    int error;

    /*
     * If the cache is locked, don't block waiting for it to be unlocked:
     * just serve the file using sendfile().
     */
    ce = cache_get_and_ref_entry(priv->cache, path, &error);
    if (LIKELY(ce))
        return ce;
    if (UNLIKELY(error == EWOULDBLOCK))
        return _create_temporary_cache_entry(priv, path);

    return NULL;
}

static lwan_http_status_t
_serve_cached_file_stream(lwan_request_t *request, void *data)
{
    file_cache_entry_t *fce = data;
    serve_files_priv_t *priv = request->response.stream.priv;
    lwan_http_status_t return_status;

    return_status = fce->funcs->serve(fce, priv, request);
    cache_entry_unref(priv->cache, (struct cache_entry_t *)fce);

    return return_status;
}

static lwan_http_status_t
serve_files_handle_cb(lwan_request_t *request, lwan_response_t *response, void *data)
{
    lwan_http_status_t return_status = HTTP_OK;
    char *path;
    serve_files_priv_t *priv = data;
    struct cache_entry_t *ce;

    if (UNLIKELY(!priv)) {
        return_status = HTTP_INTERNAL_ERROR;
        goto fail;
    }

    while (*request->url.value == '/' && request->url.len > 0) {
        ++request->url.value;
        --request->url.len;
    }

    if (!request->url.len)
        path = priv->index_html;
    else
        path = request->url.value;

    ce = _fetch_from_cache_and_ref(priv, path);
    if (!ce) {
        char *tmp;

        if (!strstr(path, "/../")) {
            return_status = HTTP_NOT_FOUND;
            goto fail;
        }

        tmp = realpathat(priv->root.fd, priv->root.path, path, NULL);
        if (UNLIKELY(!tmp)) {
            return_status = HTTP_NOT_FOUND;
            goto fail;
        }
        if (LIKELY(!strncmp(tmp, priv->root.path, priv->root.path_len)))
            ce = _fetch_from_cache_and_ref(priv, tmp + priv->root.path_len + 1);

        free(tmp);

        if (UNLIKELY(!ce)) {
            return_status = HTTP_NOT_FOUND;
            goto fail;
        }
    }

    response->mime_type = (char *)((file_cache_entry_t *)ce)->mime_type;
    response->stream.callback = _serve_cached_file_stream;
    response->stream.data = ce;
    response->stream.priv = priv;

    return HTTP_OK;

fail:
    response->stream.callback = NULL;
    return return_status;
}

lwan_handler_t serve_files = {
    .init = serve_files_init,
    .shutdown = serve_files_shutdown,
    .handle = serve_files_handle_cb,
    .flags = HANDLER_PARSE_IF_MODIFIED_SINCE | HANDLER_PARSE_RANGE | HANDLER_PARSE_ACCEPT_ENCODING
};
