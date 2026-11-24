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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define LIB_IO_CHUNK_MAX (1024u * 1024u)
#define LIB_IO_READ_STEP 4096u


static __thread int g_tls_epfd = -1;                     /* Thread-local epoll instance fd; -1 means not yet created on this thread. */
static __thread pid_t g_tls_epfd_pid = 0;                /* PID that created or validated g_tls_epfd; detects stale state after fork(). */
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT; /* Ensures pthread_atfork() is registered exactly once across all threads. */

/* Runs in the child process after fork() so inherited epoll fds are never shared. */
static void lib_io_on_fork_child(void) {
    /* Close any epoll fd inherited from the 
     * parent process address space/fd table. */
    if (g_tls_epfd >= 0) {
        close(g_tls_epfd);
        g_tls_epfd = -1;
    }
    /* Record the new child PID so subsequent 
     * PID checks match the active process. */
    g_tls_epfd_pid = getpid();
}

/* Initializes fork safety tracking once for the current process. */
static void lib_io_init_atfork(void) {
    /* Capture initial PID before registering the child fork handler. */
    g_tls_epfd_pid = getpid();

    /* Register only a child handler; 
     * pre-fork and parent handlers are not needed. */
    (void)pthread_atfork(NULL, NULL, lib_io_on_fork_child);
}

/* Returns a valid per-thread epoll fd for the current process, creating it lazily. */
static int lib_io_get_epfd(void) {
    /* Guarantee pthread_atfork() registration 
     * is completed once in a thread-safe way. */
    (void)pthread_once(&g_atfork_once, lib_io_init_atfork);

    /* Guard against cases where a forked 
     * child did not run the handler as expected. */
    pid_t cur_pid = getpid();
    if (lib_unlikely(g_tls_epfd_pid != cur_pid)) {
        g_tls_epfd = -1;
        g_tls_epfd_pid = cur_pid;
    }

    /* Lazily create the thread-local epoll instance on first use. */
    if (lib_unlikely(g_tls_epfd < 0)) {
        g_tls_epfd = epoll_create1(EPOLL_CLOEXEC);
    }
    return g_tls_epfd;
}

/* Computes remaining epoll wait time in milliseconds from a monotonic deadline. */
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

    int epfd = lib_io_get_epfd();
    if (lib_unlikely(epfd < 0)) {
        return LIB_IO_ERR;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        if (lib_likely(errno == ENOENT)) {
            if (lib_unlikely(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0)) {
                return LIB_IO_ERR;
            }
        } else {
            return LIB_IO_ERR;
        }
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

        /* Wakeup received; caller performs non-blocking read/write to observe exact errno/EOF. */
        status = LIB_IO_OK;
        break;
    }

    /* Always detach before returning so fd reuse cannot collide with stale registrations. */
    int saved_errno = errno;
    (void)epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    errno = saved_errno;

    return status;
}

/* Extracts a single line ending in '\n' (stripping optional trailing '\r'). */
static int lib_io_extract_line(lib_buf_t *buf, size_t *scan_off, char **out_line) {
    size_t start = (scan_off != NULL && *scan_off <= buf->len) ? *scan_off : 0u;
    if (start >= buf->len) {
        if (scan_off != NULL) {
            *scan_off = buf->len;
        }
        return 0;
    }

    const char *nl = memchr(buf->data + start, '\n', buf->len - start);
    if (nl == NULL) {
        if (scan_off != NULL) {
            *scan_off = buf->len;
        }
        return 0;
    }

    size_t line_len = (size_t)(nl - buf->data);
    size_t payload_len = line_len;
    if (payload_len > 0u && buf->data[payload_len - 1u] == '\r') {
        payload_len--;
    }

    if (lib_unlikely(payload_len >= SIZE_MAX)) {
        errno = EOVERFLOW;
        return -1;
    }

    char *line = malloc(payload_len + 1u);
    if (lib_unlikely(line == NULL)) {
        errno = ENOMEM;
        return -1;
    }

    if (lib_likely(payload_len > 0u)) {
        memcpy(line, buf->data, payload_len);
    }
    line[payload_len] = '\0';

    lib_buf_consume(buf, line_len + 1u);
    if (scan_off != NULL) {
        *scan_off = 0u;
    }
    *out_line = line;

    return 1;
}

/* Drains bytes into fd until completion, EAGAIN/EWOULDBLOCK, EOF, or fatal error. */
static lib_io_status_t lib_io_write_drain(int fd,
                                          const unsigned char *src,
                                          size_t len,
                                          size_t *off) {
    while (*off < len) {
        size_t rem = len - *off;
        size_t chunk = (rem > LIB_IO_CHUNK_MAX) ? LIB_IO_CHUNK_MAX : rem;

        ssize_t n = write(fd, src + *off, chunk);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return LIB_IO_OK;
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
        *off += (size_t)n;
    }
    return LIB_IO_OK;
}

lib_io_status_t lib_io_write_all(int fd, const void *data, size_t len, int64_t deadline_ms) {
    if (lib_unlikely(fd < 0)) {
        errno = EBADF;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(len > 0u && data == NULL)) {
        errno = EINVAL;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(len == 0u)) {
        return LIB_IO_OK;
    }

    const unsigned char *src = (const unsigned char *)data;
    size_t off = 0u;

    /* Fast path: drain immediately before waiting on epoll. */
    lib_io_status_t drc = lib_io_write_drain(fd, src, len, &off);
    if (drc != LIB_IO_OK || off == len) {
        return drc;
    }

    while (off < len) {
        lib_io_status_t wrc = lib_io_wait_epoll(fd, EPOLLOUT | EPOLLERR | EPOLLHUP, deadline_ms);
        if (lib_unlikely(wrc != LIB_IO_OK)) {
            return wrc;
        }

        drc = lib_io_write_drain(fd, src, len, &off);
        if (drc != LIB_IO_OK) {
            return drc;
        }
    }

    return LIB_IO_OK;
}

lib_io_status_t lib_io_read_line(int fd, lib_buf_t *buf, char **out_line, int64_t deadline_ms) {
    if (lib_unlikely(fd < 0)) {
        errno = EBADF;
        return LIB_IO_ERR;
    }
    if (lib_unlikely(buf == NULL || buf->data == NULL || out_line == NULL || buf->max_cap == 0u)) {
        errno = EINVAL;
        return LIB_IO_ERR;
    }

    *out_line = NULL;
    size_t scan_off = 0u;

    for (;;) {
        int ext = lib_io_extract_line(buf, &scan_off, out_line);
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
            if (buf->len >= buf->max_cap) {
                errno = EMSGSIZE;
                return LIB_IO_OVERFLOW;
            }

            size_t step = (buf->max_cap - buf->len < LIB_IO_READ_STEP)
                              ? (buf->max_cap - buf->len)
                              : LIB_IO_READ_STEP;
            size_t want = buf->len + step;

            lib_buf_status_t rrc = lib_buf_reserve(buf, want);
            if (lib_unlikely(rrc != LIB_BUF_OK)) {
                if (rrc == LIB_BUF_OVERFLOW) {
                    errno = EMSGSIZE;
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

            if (memchr(buf->data + scan_off, '\n', buf->len - scan_off) != NULL) {
                break;
            }
        }
    }
}