/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

int64_t lib_mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
}

int lib_set_nonblock(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }

    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void lib_close_fd(int *fd) {
    if (!fd || *fd < 0) {
        return;
    }

    int saved_errno = errno;
    int target = *fd;
    *fd = -1;
    while (close(target) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
    errno = saved_errno;
}