/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_UTIL_H__
#define __LIB_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define lib_likely(x)   __builtin_expect(!!(x), 1)
#define lib_unlikely(x) __builtin_expect(!!(x), 0)
#else
#define lib_likely(x)   (!!(x))
#define lib_unlikely(x) (!!(x))
#endif

int64_t lib_mono_ms(void);
int lib_set_nonblock(int fd);
void lib_close_fd(int *fd);

static inline char *lib_strndup_ex(const char *src, size_t len, int *oom)
{
    char *p;

    if (src == NULL) {
        return NULL;
    }
    if (len > SIZE_MAX - 1u) {
        if (oom != NULL) {
            *oom = 1;
        }
        return NULL;
    }

    p = (char *)malloc(len + 1u);
    if (p == NULL) {
        if (oom != NULL) {
            *oom = 1;
        }
        return NULL;
    }

    if (len > 0u) {
        memcpy(p, src, len);
    }
    p[len] = '\0';
    return p;
}

static inline char *lib_strdup_ex(const char *src, int *oom)
{
    if (src == NULL) {
        return NULL;
    }
    return lib_strndup_ex(src, strlen(src), oom);
}

#ifdef __cplusplus
}
#endif

#endif /* LIB_UTIL_H */