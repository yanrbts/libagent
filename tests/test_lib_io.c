#include "lib_buf.h"
#include "lib_io.h"
#include "util.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IO_STRESS_WORKERS     64
#define IO_STRESS_MSGS_PER_WR 200
#define IO_STRESS_PAYLOAD_MAX 4096u
#define IO_STRESS_TIMEOUT_MS  15000

typedef struct io_worker_arg {
    int      worker_id;  /* Worker index used to seed deterministic traffic */
    int      msgs_count; /* Number of echo round-trips per worker */
    uint64_t bytes_sent; /* Total bytes written by worker */
    uint64_t bytes_recv; /* Total bytes verified from echo peer */
    int      failed;     /* Non-zero if any assertion or I/O check failed */
} io_worker_arg_t;

typedef struct io_echo_peer_arg {
    int fd;         /* Non-blocking peer stream descriptor */
    int max_cap;    /* Maximum receive frame capacity in bytes */
    int timeout_ms; /* Per-operation timeout in milliseconds */
    int failed;     /* Non-zero if peer encountered an error */
} io_echo_peer_arg_t;

/* Fills dst with deterministic printable payload bytes terminated by '\n'. */
static void fill_deterministic_line(char *dst, size_t total_len, uint64_t seed)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ULL;

    for (size_t i = 0; i + 1u < total_len; i++) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        uint64_t r = (x * 0x2545F4914F6CDD1DULL) >> 32;
        dst[i] = charset[(size_t)(r % (sizeof(charset) - 1u))];
    }
    dst[total_len - 1u] = '\n';
}

static void test_buf_and_io_args(void)
{
    lib_buf_t buf;
    memset(&buf, 0, sizeof(buf));

    assert(lib_buf_init(NULL, 64, 1024) == LIB_BUF_ERR);
    assert(errno == EINVAL);

    assert(lib_buf_init(&buf, 64, 0) == LIB_BUF_ERR);
    assert(errno == EINVAL);

    assert(lib_buf_reserve(NULL, 64) == LIB_BUF_ERR);
    lib_buf_free(NULL);
    lib_buf_consume(NULL, 16);

    assert(lib_buf_init(&buf, 16, 128) == LIB_BUF_OK);
    assert(buf.cap == 16);
    assert(buf.max_cap == 128);

    assert(lib_buf_reserve(&buf, 64) == LIB_BUF_OK);
    assert(buf.cap >= 64 && buf.cap <= 128);

    assert(lib_buf_reserve(&buf, 129) == LIB_BUF_OVERFLOW);
    assert(errno == EMSGSIZE);

    memcpy(buf.data, "abcdef", 6);
    buf.len = 6;
    lib_buf_consume(&buf, 2);
    assert(buf.len == 4);
    assert(memcmp(buf.data, "cdef", 4) == 0);

    lib_buf_consume(&buf, 100);
    assert(buf.len == 0);

    lib_buf_free(&buf);
    assert(buf.data == NULL && buf.cap == 0 && buf.len == 0);

    char *line = NULL;
    assert(lib_io_write_all(-1, "x", 1, 0) == LIB_IO_ERR);
    assert(errno == EBADF);
    assert(lib_io_write_all(0, NULL, 4, 0) == LIB_IO_ERR);
    assert(errno == EINVAL);
    assert(lib_io_read_line(-1, &buf, &line, 0) == LIB_IO_ERR);
    assert(errno == EBADF);

    printf("[PASS] test_buf_and_io_args\n");
}

static void test_framing_crlf_multiframe_and_overflow(void)
{
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(lib_set_nonblock(sv[0]) == 0);
    assert(lib_set_nonblock(sv[1]) == 0);

    lib_buf_t rbuf;
    assert(lib_buf_init(&rbuf, 32, 256) == LIB_BUF_OK);

    const char *batch = "first_line\nsecond_crlf\r\nthird_line\n";
    int64_t deadline = lib_mono_ms() + 2000;
    assert(lib_io_write_all(sv[1], batch, strlen(batch), deadline) == LIB_IO_OK);

    char *line = NULL;
    assert(lib_io_read_line(sv[0], &rbuf, &line, deadline) == LIB_IO_OK);
    assert(line != NULL && strcmp(line, "first_line") == 0);
    free(line);
    line = NULL;

    assert(lib_io_read_line(sv[0], &rbuf, &line, deadline) == LIB_IO_OK);
    assert(line != NULL && strcmp(line, "second_crlf") == 0);
    free(line);
    line = NULL;

    assert(lib_io_read_line(sv[0], &rbuf, &line, deadline) == LIB_IO_OK);
    assert(line != NULL && strcmp(line, "third_line") == 0);
    free(line);
    line = NULL;

    char oversized[400];
    memset(oversized, 'A', sizeof(oversized));
    oversized[sizeof(oversized) - 1u] = '\n';
    assert(lib_io_write_all(sv[1], oversized, sizeof(oversized), deadline) == LIB_IO_OK);

    lib_io_status_t st = lib_io_read_line(sv[0], &rbuf, &line, deadline);
    assert(st == LIB_IO_OVERFLOW);
    assert(line == NULL);

    lib_close_fd(&sv[1]);
    lib_buf_t eof_buf;
    assert(lib_buf_init(&eof_buf, 64, 1024) == LIB_BUF_OK);
    assert(lib_io_read_line(sv[0], &eof_buf, &line, deadline) == LIB_IO_EOF);
    assert(line == NULL);

    lib_buf_free(&eof_buf);
    lib_buf_free(&rbuf);
    lib_close_fd(&sv[0]);

    printf("[PASS] test_framing_crlf_multiframe_and_overflow\n");
}

static void test_timeout_and_epipe(void)
{
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(lib_set_nonblock(sv[0]) == 0);
    assert(lib_set_nonblock(sv[1]) == 0);

    lib_buf_t rbuf;
    assert(lib_buf_init(&rbuf, 64, 1024) == LIB_BUF_OK);

    char *line = NULL;
    int64_t short_deadline = lib_mono_ms() + 120;
    lib_io_status_t st = lib_io_read_line(sv[0], &rbuf, &line, short_deadline);
    assert(st == LIB_IO_TIMEOUT);
    assert(line == NULL);

    lib_close_fd(&sv[0]);
    char *blob = malloc(256u * 1024u);
    assert(blob != NULL);
    memset(blob, 'Z', 256u * 1024u);

    int eof_or_err = 0;
    for (int i = 0; i < 64; i++) {
        lib_io_status_t wrc = lib_io_write_all(sv[1], blob, 256u * 1024u, lib_mono_ms() + 1000);
        if (wrc == LIB_IO_EOF || wrc == LIB_IO_ERR) {
            eof_or_err = 1;
            break;
        }
    }
    assert(eof_or_err == 1);

    free(blob);
    lib_buf_free(&rbuf);
    lib_close_fd(&sv[1]);

    printf("[PASS] test_timeout_and_epipe\n");
}

/* Peer thread reads newline-delimited frames from arg->fd and echoes each back. */
static void *echo_peer_thread(void *opaque)
{
    io_echo_peer_arg_t *arg = (io_echo_peer_arg_t *)opaque;
    lib_buf_t buf;

    if (lib_buf_init(&buf, 512u, (size_t)arg->max_cap) != LIB_BUF_OK) {
        arg->failed = 1;
        return NULL;
    }

    for (;;) {
        char *line = NULL;
        int64_t dl = lib_mono_ms() + arg->timeout_ms;
        lib_io_status_t rrc = lib_io_read_line(arg->fd, &buf, &line, dl);
        if (rrc == LIB_IO_EOF) {
            break;
        }
        if (lib_unlikely(rrc != LIB_IO_OK || !line)) {
            free(line);
            arg->failed = 1;
            break;
        }

        size_t len = strlen(line);
        char *out = malloc(len + 2u);
        if (lib_unlikely(!out)) {
            free(line);
            arg->failed = 1;
            break;
        }
        memcpy(out, line, len);
        out[len] = '\n';
        out[len + 1u] = '\0';
        free(line);

        lib_io_status_t wrc = lib_io_write_all(arg->fd, out, len + 1u, dl);
        free(out);
        if (lib_unlikely(wrc != LIB_IO_OK)) {
            arg->failed = 1;
            break;
        }
    }

    lib_buf_free(&buf);
    return NULL;
}

/* Client worker sends variable-length frames over socketpair and checks echo. */
static void *stress_client_worker(void *opaque)
{
    io_worker_arg_t *w = (io_worker_arg_t *)opaque;
    int sv[2] = {-1, -1};

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        w->failed = 1;
        return NULL;
    }
    if (lib_set_nonblock(sv[0]) != 0 || lib_set_nonblock(sv[1]) != 0) {
        lib_close_fd(&sv[0]);
        lib_close_fd(&sv[1]);
        w->failed = 1;
        return NULL;
    }

    io_echo_peer_arg_t peer_arg;
    memset(&peer_arg, 0, sizeof(peer_arg));
    peer_arg.fd = sv[1];
    peer_arg.max_cap = (int)(IO_STRESS_PAYLOAD_MAX * 2u);
    peer_arg.timeout_ms = IO_STRESS_TIMEOUT_MS;

    pthread_t peer_tid;
    if (pthread_create(&peer_tid, NULL, echo_peer_thread, &peer_arg) != 0) {
        lib_close_fd(&sv[0]);
        lib_close_fd(&sv[1]);
        w->failed = 1;
        return NULL;
    }

    lib_buf_t rbuf;
    if (lib_buf_init(&rbuf, 512u, IO_STRESS_PAYLOAD_MAX * 2u) != LIB_BUF_OK) {
        lib_close_fd(&sv[0]);
        pthread_join(peer_tid, NULL);
        lib_close_fd(&sv[1]);
        w->failed = 1;
        return NULL;
    }

    char *send_buf = malloc(IO_STRESS_PAYLOAD_MAX);
    if (!send_buf) {
        lib_buf_free(&rbuf);
        lib_close_fd(&sv[0]);
        pthread_join(peer_tid, NULL);
        lib_close_fd(&sv[1]);
        w->failed = 1;
        return NULL;
    }

    for (int i = 0; i < w->msgs_count; i++) {
        uint64_t seed = ((uint64_t)(w->worker_id + 1) << 32) | (uint64_t)(i + 1);
        size_t line_len = 8u + (size_t)((seed * 1103515245ULL + 12345ULL) %
                                        (IO_STRESS_PAYLOAD_MAX - 8u));
        fill_deterministic_line(send_buf, line_len, seed);

        int64_t dl = lib_mono_ms() + IO_STRESS_TIMEOUT_MS;
        lib_io_status_t wrc = lib_io_write_all(sv[0], send_buf, line_len, dl);
        if (lib_unlikely(wrc != LIB_IO_OK)) {
            w->failed = 1;
            break;
        }
        w->bytes_sent += (uint64_t)line_len;

        char *echo_line = NULL;
        lib_io_status_t rrc = lib_io_read_line(sv[0], &rbuf, &echo_line, dl);
        if (lib_unlikely(rrc != LIB_IO_OK || !echo_line)) {
            free(echo_line);
            w->failed = 1;
            break;
        }

        size_t expected_len = line_len - 1u;
        if (lib_unlikely(strlen(echo_line) != expected_len ||
                         memcmp(echo_line, send_buf, expected_len) != 0)) {
            free(echo_line);
            w->failed = 1;
            break;
        }
        w->bytes_recv += (uint64_t)strlen(echo_line) + 1u;
        free(echo_line);
    }

    free(send_buf);
    lib_buf_free(&rbuf);
    lib_close_fd(&sv[0]);

    pthread_join(peer_tid, NULL);
    lib_close_fd(&sv[1]);

    if (peer_arg.failed) {
        w->failed = 1;
    }
    return NULL;
}

static void test_high_concurrency_stress(void)
{
    pthread_t tids[IO_STRESS_WORKERS];
    io_worker_arg_t args[IO_STRESS_WORKERS];
    memset(args, 0, sizeof(args));

    int64_t start_ms = lib_mono_ms();

    for (int i = 0; i < IO_STRESS_WORKERS; i++) {
        args[i].worker_id = i;
        args[i].msgs_count = IO_STRESS_MSGS_PER_WR;
        assert(pthread_create(&tids[i], NULL, stress_client_worker, &args[i]) == 0);
    }

    uint64_t total_sent = 0;
    uint64_t total_recv = 0;
    for (int i = 0; i < IO_STRESS_WORKERS; i++) {
        assert(pthread_join(tids[i], NULL) == 0);
        assert(args[i].failed == 0);
        assert(args[i].bytes_sent == args[i].bytes_recv);
        total_sent += args[i].bytes_sent;
        total_recv += args[i].bytes_recv;
    }

    int64_t elapsed_ms = lib_mono_ms() - start_ms;
    if (elapsed_ms <= 0) {
        elapsed_ms = 1;
    }

    printf("[PASS] test_high_concurrency_stress (workers=%d, msgs_per_worker=%d, "
           "total_bytes=%llu, elapsed=%lld ms)\n",
           IO_STRESS_WORKERS,
           IO_STRESS_MSGS_PER_WR,
           (unsigned long long)(total_sent + total_recv),
           (long long)elapsed_ms);
}

int main(void)
{
    test_buf_and_io_args();
    test_framing_crlf_multiframe_and_overflow();
    test_timeout_and_epipe();
    test_high_concurrency_stress();
    printf("All lib_io industrial tests passed.\n");
    return 0;
}