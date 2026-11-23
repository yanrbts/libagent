/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_UTIL_H__
#define __LIB_UTIL_H__

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* LIB_UTIL_H */