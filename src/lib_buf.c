/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#include "lib_buf.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LIB_BUF_DEFAULT_CAP 4096u

lib_buf_status_t lib_buf_init(lib_buf_t *buf, size_t init_cap, size_t max_cap) {
    if (lib_unlikely(!buf || max_cap == 0)) {
        errno = EINVAL;
        return LIB_BUF_ERR;
    }

    memset(buf, 0, sizeof(*buf));
    buf->max_cap = max_cap;

    size_t cap = (init_cap > 0) ? init_cap : LIB_BUF_DEFAULT_CAP;
    if (lib_unlikely(cap > max_cap)) {
        cap = max_cap;
    }

    buf->data = malloc(cap);
    if (lib_unlikely(!buf->data)) {
        errno = ENOMEM;
        return LIB_BUF_ERR;
    }
    buf->cap = cap;
    return LIB_BUF_OK;
}

void lib_buf_free(lib_buf_t *buf) {
    if (lib_unlikely(!buf)) {
        return;
    }
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

lib_buf_status_t lib_buf_reserve(lib_buf_t *buf, size_t need) {
    if (lib_unlikely(!buf || !buf->data || buf->max_cap == 0)) {
        errno = EINVAL;
        return LIB_BUF_ERR;
    }
    if (lib_unlikely(need > buf->max_cap)) {
        errno = EMSGSIZE;
        return LIB_BUF_OVERFLOW;
    }
    if (lib_likely(need <= buf->cap)) {
        return LIB_BUF_OK;
    }

    /**
     * buf->cap < need <= buf->max_cap, so grow geometrically up to max_cap.
     * Start with either the current capacity or a default, then double until
     * we reach the requested need or max_cap. If doubling would exceed max_cap,
     * clamp to max_cap and break.
     */
    size_t ncap = (buf->cap > 0) ? buf->cap : LIB_BUF_DEFAULT_CAP;
    if (lib_unlikely(ncap > buf->max_cap)) {
        ncap = buf->max_cap;
    }
    while (ncap < need) {
        if (lib_unlikely(ncap > buf->max_cap / 2u)) {
            ncap = buf->max_cap;
            break;
        }
        ncap *= 2u;
    }

    char *ndata = realloc(buf->data, ncap);
    if (lib_unlikely(!ndata)) {
        errno = ENOMEM;
        return LIB_BUF_ERR;
    }
    buf->data = ndata;
    buf->cap = ncap;
    return LIB_BUF_OK;
}

void lib_buf_consume(lib_buf_t *buf, size_t n) {
    if (lib_unlikely(!buf || n == 0 || buf->len == 0)) {
        return;
    }
    if (lib_likely(n >= buf->len)) {
        buf->len = 0;
        return;
    }
    memmove(buf->data, buf->data + n, buf->len - n);
    buf->len -= n;
}