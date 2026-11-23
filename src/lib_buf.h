/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_BUF_H__
#define __LIB_BUF_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status codes returned by buffer initialization and capacity operations.
 */
typedef enum lib_buf_status {
    LIB_BUF_OK = 0,         /* Operation succeeded */
    LIB_BUF_ERR = -1,       /* Invalid argument or memory allocation failure */
    LIB_BUF_OVERFLOW = -2   /* Requested capacity exceeded configured max_cap */
} lib_buf_status_t;

/**
 * @brief Bounded, dynamically growable byte buffer for stream and frame assembly.
 *
 * Maintains unread bytes in `[0, len)` within a heap allocation up to `max_cap`,
 * supporting geometric growth, front consumption, and safe release.
 */
typedef struct lib_buf {
    char  *data;    /* Heap-allocated buffer storing unread bytes */
    size_t len;     /* Number of valid bytes currently stored in data */
    size_t cap;     /* Total allocated capacity of data in bytes */
    size_t max_cap; /* Hard upper bound on buffer capacity in bytes */
} lib_buf_t;

/**
 * @brief Initializes a bounded dynamic byte buffer.
 *
 * Allocates an initial heap buffer of `max(init_cap, default)` clamped to
 * `max_cap` and resets all state fields.
 *
 * @param[out] buf      Target buffer structure to initialize.
 * @param[in]  init_cap Optional initial capacity in bytes; 0 selects default.
 * @param[in]  max_cap  Maximum allowed capacity in bytes (must be greater than 0).
 *
 * @return `LIB_BUF_OK` on success, or `LIB_BUF_ERR` with `errno` set (`EINVAL`, `ENOMEM`).
 */
lib_buf_status_t lib_buf_init(lib_buf_t *buf, size_t init_cap, size_t max_cap);

/**
 * @brief Releases heap memory and zeroes all fields of a buffer.
 *
 * Safe to invoke on NULL or already-freed buffers.
 *
 * @param[in,out] buf Buffer structure to release.
 */
void lib_buf_free(lib_buf_t *buf);

/**
 * @brief Ensures the buffer has at least `need` total byte capacity.
 *
 * Grows capacity geometrically up to `max_cap` without exceeding `max_cap`.
 *
 * @param[in,out] buf  Initialized buffer structure.
 * @param[in]     need Required total capacity in bytes.
 *
 * @return `LIB_BUF_OK` on success, `LIB_BUF_OVERFLOW` if `need > max_cap`,
 *         or `LIB_BUF_ERR` on invalid input or allocation failure.
 */
lib_buf_status_t lib_buf_reserve(lib_buf_t *buf, size_t need);

/**
 * @brief Consumes `n` leading bytes from the buffer.
 *
 * Compacts remaining unread bytes when `n < buf->len`; sets `buf->len` to 0
 * when `n >= buf->len`. Safe to call with NULL or `n == 0`.
 *
 * @param[in,out] buf Buffer structure to advance.
 * @param[in]     n   Number of leading bytes to discard.
 */
void lib_buf_consume(lib_buf_t *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LIB_BUF_H */