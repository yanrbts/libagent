/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_IO_H__
#define __LIB_IO_H__

#include "lib_buf.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status codes returned by epoll-driven non-blocking I/O operations.
 *
 * Negative values distinguish timeout, peer termination, frame overflow, and
 * descriptor/system errors (`errno` preserved where applicable).
 */
typedef enum lib_io_status {
    LIB_IO_OK = 0,          /* Operation completed successfully */
    LIB_IO_ERR = -1,        /* Invalid argument, allocation failure, or syscall error */
    LIB_IO_TIMEOUT = -2,    /* Operation exceeded deadline_ms before completion */
    LIB_IO_EOF = -3,        /* Peer closed stream or write hit broken pipe/reset */
    LIB_IO_OVERFLOW = -4    /* Line or frame exceeded buffer max_cap limit */
} lib_io_status_t;

/**
 * @brief Writes all `len` bytes to `fd` using `epoll` and `CLOCK_MONOTONIC`.
 *
 * Waits for descriptor writability via `epoll`, retries on `EINTR` and partial
 * writes, and enforces `deadline_ms` (in monotonic milliseconds; `<= 0` means
 * no timeout).
 *
 * @param[in] fd          Non-blocking writable file descriptor.
 * @param[in] data        Pointer to payload bytes (may be NULL when `len == 0`).
 * @param[in] len         Number of bytes to write.
 * @param[in] deadline_ms Monotonic deadline in milliseconds, or `<= 0` for none.
 *
 * @return `LIB_IO_OK` when all bytes are written, `LIB_IO_TIMEOUT` on expiry,
 *         `LIB_IO_EOF` on `EPIPE`/`ECONNRESET`, or `LIB_IO_ERR` on failure.
 */
lib_io_status_t lib_io_write_all(int fd, const void *data, size_t len, int64_t deadline_ms);

/**
 * @brief Reads a single newline-delimited line from `fd` into a heap string.
 *
 * Inspects buffered bytes in `buf` first, then polls `fd` via `epoll` and
 * reads into `buf` until `\n` is found. Strips trailing `\n` and optional `\r`,
 * allocates a NUL-terminated string in `*out_line`, and leaves remaining bytes
 * in `buf` for subsequent calls.
 *
 * @param[in]     fd          Non-blocking readable file descriptor.
 * @param[in,out] buf         Line-framing receive buffer associated with `fd`.
 * @param[out]    out_line    Receives newly allocated NUL-terminated line on success.
 * @param[in]     deadline_ms Monotonic deadline in milliseconds, or `<= 0` for none.
 *
 * @return `LIB_IO_OK` when a line is extracted, `LIB_IO_EOF` on clean EOF or
 *         peer reset, `LIB_IO_TIMEOUT` on expiry, `LIB_IO_OVERFLOW` if a line
 *         exceeds `buf->max_cap`, or `LIB_IO_ERR` on failure.
 */
lib_io_status_t lib_io_read_line(int fd, lib_buf_t *buf, char **out_line, int64_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* LIB_IO_H */