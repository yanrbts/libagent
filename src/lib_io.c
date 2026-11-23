/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#include "lib_io.h"
#include "lib_buf.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define LIB_IO_CHUNK_MAX (1024u * 1024u)
#define LIB_IO_READ_STEP 4096u

/* Computes remaining poll wait time in milliseconds from a monotonic deadline. */
static lib_io_status_t lib_io_calc_timeout_ms(int64_t deadline_ms, int *out_timeout_ms) {
    if (deadline_ms <= 0) {
        *out_timeout_ms = -1;
        return LIB_IO_OK;
    }

    int64_t now = lib_mono_ms();
    if (lib_unlikely(now <= 0)) {
        errno = EIO;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(now >= deadline_ms)) {
        errno = ETIMEDOUT;
        return LIB_IO_TIMEOUT;
    }

    int64_t rem = deadline_ms - now;
    if (lib_unlikely(rem > (int64_t)INT_MAX)) {
        *out_timeout_ms = INT_MAX;
    } else {
        *out_timeout_ms = (int)rem;
    }
    return LIB_IO_OK;
}

/* Waits until fd becomes ready for the requested epoll events or deadline expires. */
static lib_io_status_t lib_io_wait_epoll(int fd, uint32_t events, int64_t deadline_ms) {
    if (lib_unlikely(fd < 0)) {
        errno = EBADF;
        return LIB_IO_ERR;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (lib_unlikely(epfd < 0)) {
        return LIB_IO_ERR;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (lib_unlikely(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0)) {
        lib_close_fd(&epfd);
        return LIB_IO_ERR;
    }

    lib_io_status_t status = LIB_IO_ERR;

    for (;;) {
        int timeout_ms = -1;
        lib_io_status_t trc = lib_io_calc_timeout_ms(deadline_ms, &timeout_ms);
        if (lib_unlikely(trc != LIB_IO_OK)) {
            status = trc;
            break;
        }

        struct epoll_event out_ev;
        memset(&out_ev, 0, sizeof(out_ev));

        int nready = epoll_wait(epfd, &out_ev, 1, timeout_ms);
        if (lib_unlikely(nready < 0)) {
            if (errno == EINTR) {
                continue;
            }
            status = LIB_IO_ERR;
            break;
        }
        if (lib_unlikely(nready == 0)) {
            errno = ETIMEDOUT;
            status = LIB_IO_TIMEOUT;
            break;
        }
        if (lib_unlikely((out_ev.events & EPOLLERR) != 0)) {
            int so_err = 0;
            socklen_t slen = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err != 0) {
                errno = so_err;
                status = (so_err == EPIPE || so_err == ECONNRESET) ? LIB_IO_EOF : LIB_IO_ERR;
                break;
            }
        }

        status = LIB_IO_OK;
        break;
    }

    lib_close_fd(&epfd);
    return status;
}

/* Extracts a single line ending in '\n' (stripping optional '\r') if present. */
static int lib_io_extract_line(lib_buf_t *buf, char **out_line) {
    const char *nl = memchr(buf->data, '\n', buf->len);
    if (!nl) {
        return 0;
    }

    size_t line_len = (size_t)(nl - buf->data);
    size_t payload_len = line_len;
    if (payload_len > 0 && buf->data[payload_len - 1u] == '\r') {
        payload_len--;
    }

    char *line = malloc(payload_len + 1u);
    if (lib_unlikely(!line)) {
        errno = ENOMEM;
        return -1;
    }
    if (lib_likely(payload_len > 0)) {
        memcpy(line, buf->data, payload_len);
    }
    line[payload_len] = '\0';

    lib_buf_consume(buf, line_len + 1u);
    *out_line = line;

    return 1;
}

lib_io_status_t lib_io_write_all(int fd, const void *data, size_t len, int64_t deadline_ms) {
    if (lib_unlikely(fd < 0)) {
        errno = EBADF;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(len > 0 && !data)) {
        errno = EINVAL;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(len == 0)) {
        return LIB_IO_OK;
    }

    const unsigned char *src = (const unsigned char *)data;
    size_t off = 0;

    while (off < len) {
        lib_io_status_t wrc = lib_io_wait_epoll(fd, EPOLLOUT | EPOLLERR | EPOLLHUP, deadline_ms);
        if (lib_unlikely(wrc != LIB_IO_OK)) {
            return wrc;
        }

        while (off < len) {
            size_t rem = len - off;
            size_t chunk = (rem > LIB_IO_CHUNK_MAX) ? LIB_IO_CHUNK_MAX : rem;

            ssize_t n = write(fd, src + off, chunk);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EPIPE || errno == ECONNRESET) {
                    return LIB_IO_EOF;
                }
                return LIB_IO_ERR;
            }
            if (lib_unlikely(n == 0)) {
                errno = EPIPE;
                return LIB_IO_EOF;
            }
            off += (size_t)n;
        }
    }

    return LIB_IO_OK;
}

lib_io_status_t lib_io_read_line(int fd, lib_buf_t *buf, char **out_line, int64_t deadline_ms) {
    if (lib_unlikely(fd < 0)) {
        errno = EBADF;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(!buf || !buf->data || !out_line || buf->max_cap == 0)) {
        errno = EINVAL;
        return LIB_IO_ERR;
    }

    *out_line = NULL;

    for (;;) {
        int ext = lib_io_extract_line(buf, out_line);
        if (lib_likely(ext > 0)) {
            return LIB_IO_OK;
        }
        if (lib_unlikely(ext < 0)) {
            return LIB_IO_ERR;
        }

        if (lib_unlikely(buf->len >= buf->max_cap)) {
            errno = EMSGSIZE;
            return LIB_IO_OVERFLOW;
        }

        lib_io_status_t wrc = lib_io_wait_epoll(fd,
                                                EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR,
                                                deadline_ms);
        if (lib_unlikely(wrc != LIB_IO_OK)) {
            return wrc;
        }

        for (;;) {
            if (memchr(buf->data, '\n', buf->len) != NULL) {
                break;
            }
            if (buf->len >= buf->max_cap) {
                errno = EMSGSIZE;
                return LIB_IO_OVERFLOW;
            }

            size_t want = buf->len + LIB_IO_READ_STEP;
            if (want > buf->max_cap) {
                want = buf->max_cap;
            }
            lib_buf_status_t rrc = lib_buf_reserve(buf, want);
            if (lib_unlikely(rrc != LIB_BUF_OK)) {
                if (rrc == LIB_BUF_OVERFLOW) {
                    return LIB_IO_OVERFLOW;
                }
                return LIB_IO_ERR;
            }

            size_t space = buf->cap - buf->len;
            if (space > LIB_IO_CHUNK_MAX) {
                space = LIB_IO_CHUNK_MAX;
            }

            ssize_t n = read(fd, buf->data + buf->len, space);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == ECONNRESET) {
                    return LIB_IO_EOF;
                }
                return LIB_IO_ERR;
            }
            if (n == 0) {
                return LIB_IO_EOF;
            }
            buf->len += (size_t)n;
        }
    }
}