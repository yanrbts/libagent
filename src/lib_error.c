/**
 * Copyright (c) 2026 yanrbts
 */
#include "lib_config.h"
#include "lib_error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/** Per-thread latest structured error descriptor. */
static LIB_ERR_TLS lib_err_t      tls_err;

/** Per-thread real-time error log sink callback. */
static LIB_ERR_TLS lib_err_log_fn tls_log_fn = NULL;

/** Per-thread opaque user pointer passed to @c tls_log_fn. */
static LIB_ERR_TLS void          *tls_log_ud = NULL;

/** Per-thread re-entrancy guard preventing recursive sink invocations. */
static LIB_ERR_TLS int            tls_in_sink = 0;

/**
 * Format @p body together with human-readable description and numeric value of @p sys_errno.
 *
 * @param[out] dst       Destination buffer for the combined error string.
 * @param[in]  dst_sz    Size of @p dst in bytes.
 * @param[in]  body      Pre-formatted caller message body (may be empty).
 * @param[in]  sys_errno Non-zero POSIX errno value to describe.
 */
static void err_format_errno_suffix(char *dst,
                                    size_t dst_sz,
                                    const char *body,
                                    int sys_errno)
{
    char sys_buf[96];
    const char *sys_txt;

    if (dst == NULL || dst_sz == 0u) {
        return;
    }

    sys_buf[0] = '\0';
    sys_txt = NULL;

#if defined(_GNU_SOURCE) && !defined(__APPLE__)
    sys_txt = strerror_r(sys_errno, sys_buf, sizeof(sys_buf));
#else
    if (strerror_r(sys_errno, sys_buf, sizeof(sys_buf)) == 0) {
        sys_txt = sys_buf;
    }
#endif

    if (sys_txt != NULL && sys_txt[0] != '\0') {
        if (body != NULL && body[0] != '\0') {
            (void)snprintf(dst, dst_sz, "%s: %s (errno=%d)", body, sys_txt, sys_errno);
        } else {
            (void)snprintf(dst, dst_sz, "%s (errno=%d)", sys_txt, sys_errno);
        }
    } else if (body != NULL && body[0] != '\0') {
        (void)snprintf(dst, dst_sz, "%s (errno=%d)", body, sys_errno);
    } else {
        (void)snprintf(dst, dst_sz, "errno=%d", sys_errno);
    }
}

void lib_err_set_log_sink(lib_err_log_fn fn, void *userdata)
{
    tls_log_fn = fn;
    tls_log_ud = userdata;
}

const lib_err_t *lib_err_last(void)
{
    return &tls_err;
}

void lib_err_clear(void)
{
    memset(&tls_err, 0, sizeof(tls_err));
    tls_err.code = LIB_ERR_OK;
    tls_err.domain = LIB_ERR_DOM_NONE;
    tls_err.severity = LIB_ERR_SEV_WARN;
}

const char *lib_err_code_str(lib_err_code_t code)
{
    switch (code) {
    case LIB_ERR_OK:          return "OK";
    case LIB_ERR_INVALID_ARG: return "INVALID_ARG";
    case LIB_ERR_NOMEM:       return "NOMEM";
    case LIB_ERR_NOT_FOUND:   return "NOT_FOUND";
    case LIB_ERR_IO:          return "IO";
    case LIB_ERR_TIMEOUT:     return "TIMEOUT";
    case LIB_ERR_PROTOCOL:    return "PROTOCOL";
    case LIB_ERR_TOOL_EXEC:   return "TOOL_EXEC";
    case LIB_ERR_STATE:       return "STATE";
    case LIB_ERR_INTERNAL:    return "INTERNAL";
    default:                  return "INTERNAL";
    }
}

const char *lib_err_domain_str(lib_err_domain_t domain)
{
    switch (domain) {
    case LIB_ERR_DOM_NONE:  return "NONE";
    case LIB_ERR_DOM_HTTP:  return "HTTP";
    case LIB_ERR_DOM_LLM:   return "LLM";
    case LIB_ERR_DOM_TOOL:  return "TOOL";
    case LIB_ERR_DOM_AGENT: return "AGENT";
    case LIB_ERR_DOM_MCP:   return "MCP";
    case LIB_ERR_DOM_SKILL: return "SKILL";
    case LIB_ERR_DOM_LOOP:  return "LOOP";
    default:                return "NONE";
    }
}

const char *lib_err_severity_str(lib_err_severity_t sev)
{
    switch (sev) {
    case LIB_ERR_SEV_WARN:  return "WARN";
    case LIB_ERR_SEV_ERROR: return "ERROR";
    case LIB_ERR_SEV_FATAL: return "FATAL";
    default:                return "ERROR";
    }
}

lib_err_code_t lib_err_code_from_errno(int sys_errno)
{
    if (sys_errno == 0) {
        return LIB_ERR_OK;
    }
    if (sys_errno == EINVAL || sys_errno == EBADF || sys_errno == EDOM || sys_errno == ERANGE) {
        return LIB_ERR_INVALID_ARG;
    }
    if (sys_errno == ENOMEM) {
        return LIB_ERR_NOMEM;
    }
    if (sys_errno == ENOENT) {
        return LIB_ERR_NOT_FOUND;
    }
    if (sys_errno == ETIMEDOUT) {
        return LIB_ERR_TIMEOUT;
    }
    return LIB_ERR_IO;
}

int lib_err_infer_retryable(lib_err_code_t code, int sys_errno, int status_code)
{
    if (code == LIB_ERR_TIMEOUT) {
        return 1;
    }
    if (status_code == 408 || status_code == 429 || status_code >= 500) {
        return 1;
    }
    if (code == LIB_ERR_IO) {
        if (sys_errno == EAGAIN || sys_errno == ECONNREFUSED ||
            sys_errno == ECONNRESET || sys_errno == ENETUNREACH ||
            sys_errno == EHOSTUNREACH || sys_errno == EPIPE) {
            return 1;
        }
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
        if (sys_errno == EWOULDBLOCK) {
            return 1;
        }
#endif
    }
    return 0;
}

lib_err_severity_t lib_err_infer_severity(lib_err_code_t code, int retryable)
{
    if (code == LIB_ERR_NOMEM) {
        return LIB_ERR_SEV_FATAL;
    }
    if (retryable != 0) {
        return LIB_ERR_SEV_WARN;
    }
    return LIB_ERR_SEV_ERROR;
}

void lib_err_vemit(lib_err_domain_t domain,
                   lib_err_code_t code,
                   int sys_errno,
                   int status_code,
                   const char *fmt,
                   va_list ap)
{
    lib_err_t rec;
    char body[LIB_ERR_MSG_MAX];
    va_list ap_copy;

    /* Assemble the full record on stack first so tls_err never exposes a half-built state. */
    memset(&rec, 0, sizeof(rec));
    rec.code = code;
    rec.domain = domain;
    rec.sys_errno = sys_errno;
    rec.status_code = status_code;
    rec.retryable = lib_err_infer_retryable(code, sys_errno, status_code);
    rec.severity = lib_err_infer_severity(code, rec.retryable);

    body[0] = '\0';
    if (fmt != NULL) {
        va_copy(ap_copy, ap);
        (void)vsnprintf(body, sizeof(body), fmt, ap_copy);
        va_end(ap_copy);
    }

    if (sys_errno != 0) {
        err_format_errno_suffix(rec.msg, sizeof(rec.msg), body, sys_errno);
    } else if (body[0] != '\0') {
        (void)snprintf(rec.msg, sizeof(rec.msg), "%s", body);
    }

    tls_err = rec;

    if (tls_log_fn != NULL && tls_in_sink == 0) {
        tls_in_sink = 1;
        tls_log_fn(&tls_err, tls_log_ud);
        tls_in_sink = 0;
    }
}

void lib_err_emit(lib_err_domain_t domain,
                  lib_err_code_t code,
                  int sys_errno,
                  int status_code,
                  const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    lib_err_vemit(domain, code, sys_errno, status_code, fmt, ap);
    va_end(ap);
}

void lib_err_emit_sys(lib_err_domain_t domain,
                      int sys_errno,
                      int status_code,
                      const char *fmt, ...)
{
    lib_err_code_t code;
    va_list ap;

    code = lib_err_code_from_errno(sys_errno);
    va_start(ap, fmt);
    lib_err_vemit(domain, code, sys_errno, status_code, fmt, ap);
    va_end(ap);
}

void lib_err_emit_msg(lib_err_domain_t domain,
                      lib_err_code_t code,
                      const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    lib_err_vemit(domain, code, 0, 0, fmt, ap);
    va_end(ap);
}