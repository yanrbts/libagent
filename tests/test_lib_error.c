/**
 * Copyright (c) 2026 yanrbts
 *
 * Unity unit tests for lib_error module.
 */
#include "lib_error.h"
#include "unity.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct sink_capture {
    int       call_count;
    int       saw_reentrant_block;
    lib_err_t last_copy;
} sink_capture_t;

static sink_capture_t s_cap;

static void capture_log_sink(const lib_err_t *err, void *userdata)
{
    sink_capture_t *cap = (sink_capture_t *)userdata;

    if (cap == NULL || err == NULL) {
        return;
    }

    cap->call_count++;
    cap->last_copy = *err;

    /* Emitting inside the sink must be guarded against recursion. */
    lib_err_emit(LIB_ERR_DOM_AGENT, LIB_ERR_INTERNAL, 0, 0, "reentrant error");
    if (lib_err_last()->code != err->code || strcmp(lib_err_last()->msg, err->msg) != 0) {
        cap->saw_reentrant_block = 0;
    } else {
        cap->saw_reentrant_block = 1;
    }
}

static void invoke_vemit_helper(lib_err_domain_t domain,
                                lib_err_code_t code,
                                int sys_errno,
                                int status_code,
                                const char *fmt,
                                ...)
{
    va_list ap;

    va_start(ap, fmt);
    lib_err_vemit(domain, code, sys_errno, status_code, fmt, ap);
    va_end(ap);
}

void setUp(void)
{
    memset(&s_cap, 0, sizeof(s_cap));
    lib_err_set_log_sink(NULL, NULL);
    lib_err_clear();
}

void tearDown(void)
{
    lib_err_set_log_sink(NULL, NULL);
    lib_err_clear();
}

static void test_err_clear_and_null_fmt_safe(void)
{
    const lib_err_t *last;

    lib_err_clear();
    last = lib_err_last();
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_OK, last->code);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_NONE, last->domain);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_WARN, last->severity);
    TEST_ASSERT_EQUAL_INT(0, last->sys_errno);
    TEST_ASSERT_EQUAL_INT(0, last->status_code);
    TEST_ASSERT_EQUAL_INT(0, last->retryable);
    TEST_ASSERT_EQUAL_CHAR('\0', last->msg[0]);

    /* Must not crash when fmt is NULL. */
    lib_err_emit(LIB_ERR_DOM_AGENT, LIB_ERR_INTERNAL, 0, 0, NULL);
    last = lib_err_last();
    TEST_ASSERT_EQUAL_INT(LIB_ERR_INTERNAL, last->code);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_AGENT, last->domain);
    TEST_ASSERT_EQUAL_CHAR('\0', last->msg[0]);
}

static void test_enum_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("OK", lib_err_code_str(LIB_ERR_OK));
    TEST_ASSERT_EQUAL_STRING("INVALID_ARG", lib_err_code_str(LIB_ERR_INVALID_ARG));
    TEST_ASSERT_EQUAL_STRING("NOMEM", lib_err_code_str(LIB_ERR_NOMEM));
    TEST_ASSERT_EQUAL_STRING("NOT_FOUND", lib_err_code_str(LIB_ERR_NOT_FOUND));
    TEST_ASSERT_EQUAL_STRING("IO", lib_err_code_str(LIB_ERR_IO));
    TEST_ASSERT_EQUAL_STRING("TIMEOUT", lib_err_code_str(LIB_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("PROTOCOL", lib_err_code_str(LIB_ERR_PROTOCOL));
    TEST_ASSERT_EQUAL_STRING("TOOL_EXEC", lib_err_code_str(LIB_ERR_TOOL_EXEC));
    TEST_ASSERT_EQUAL_STRING("STATE", lib_err_code_str(LIB_ERR_STATE));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", lib_err_code_str(LIB_ERR_INTERNAL));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", lib_err_code_str((lib_err_code_t)999));

    TEST_ASSERT_EQUAL_STRING("NONE", lib_err_domain_str(LIB_ERR_DOM_NONE));
    TEST_ASSERT_EQUAL_STRING("HTTP", lib_err_domain_str(LIB_ERR_DOM_HTTP));
    TEST_ASSERT_EQUAL_STRING("LLM", lib_err_domain_str(LIB_ERR_DOM_LLM));
    TEST_ASSERT_EQUAL_STRING("TOOL", lib_err_domain_str(LIB_ERR_DOM_TOOL));
    TEST_ASSERT_EQUAL_STRING("AGENT", lib_err_domain_str(LIB_ERR_DOM_AGENT));
    TEST_ASSERT_EQUAL_STRING("MCP", lib_err_domain_str(LIB_ERR_DOM_MCP));
    TEST_ASSERT_EQUAL_STRING("SKILL", lib_err_domain_str(LIB_ERR_DOM_SKILL));
    TEST_ASSERT_EQUAL_STRING("NONE", lib_err_domain_str((lib_err_domain_t)999));

    TEST_ASSERT_EQUAL_STRING("WARN", lib_err_severity_str(LIB_ERR_SEV_WARN));
    TEST_ASSERT_EQUAL_STRING("ERROR", lib_err_severity_str(LIB_ERR_SEV_ERROR));
    TEST_ASSERT_EQUAL_STRING("FATAL", lib_err_severity_str(LIB_ERR_SEV_FATAL));
    TEST_ASSERT_EQUAL_STRING("ERROR", lib_err_severity_str((lib_err_severity_t)999));
}

static void test_code_from_errno_mapping(void)
{
    TEST_ASSERT_EQUAL_INT(LIB_ERR_OK, lib_err_code_from_errno(0));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_INVALID_ARG, lib_err_code_from_errno(EINVAL));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_INVALID_ARG, lib_err_code_from_errno(EBADF));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_NOMEM, lib_err_code_from_errno(ENOMEM));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_NOT_FOUND, lib_err_code_from_errno(ENOENT));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_TIMEOUT, lib_err_code_from_errno(ETIMEDOUT));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_IO, lib_err_code_from_errno(ECONNRESET));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_IO, lib_err_code_from_errno(EACCES));
}

static void test_infer_retryable_and_severity(void)
{
    TEST_ASSERT_EQUAL_INT(1, lib_err_infer_retryable(LIB_ERR_TIMEOUT, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, lib_err_infer_retryable(LIB_ERR_PROTOCOL, 0, 429));
    TEST_ASSERT_EQUAL_INT(1, lib_err_infer_retryable(LIB_ERR_PROTOCOL, 0, 503));
    TEST_ASSERT_EQUAL_INT(1, lib_err_infer_retryable(LIB_ERR_IO, ECONNREFUSED, 0));
    TEST_ASSERT_EQUAL_INT(1, lib_err_infer_retryable(LIB_ERR_IO, EAGAIN, 0));

    TEST_ASSERT_EQUAL_INT(0, lib_err_infer_retryable(LIB_ERR_INVALID_ARG, EINVAL, 400));
    TEST_ASSERT_EQUAL_INT(0, lib_err_infer_retryable(LIB_ERR_NOT_FOUND, ENOENT, 404));
    TEST_ASSERT_EQUAL_INT(0, lib_err_infer_retryable(LIB_ERR_PROTOCOL, 0, 401));

    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_FATAL, lib_err_infer_severity(LIB_ERR_NOMEM, 0));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_WARN, lib_err_infer_severity(LIB_ERR_TIMEOUT, 1));
    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_ERROR, lib_err_infer_severity(LIB_ERR_INVALID_ARG, 0));
}

static void test_emit_structured_and_msg_truncation(void)
{
    const lib_err_t *last;
    char long_msg[LIB_ERR_MSG_MAX + 64u];

    memset(long_msg, 'A', sizeof(long_msg) - 1u);
    long_msg[sizeof(long_msg) - 1u] = '\0';

    lib_err_emit(LIB_ERR_DOM_HTTP, LIB_ERR_PROTOCOL, 0, 502,
                 "upstream status %d", 502);

    last = lib_err_last();
    TEST_ASSERT_EQUAL_INT(LIB_ERR_PROTOCOL, last->code);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_HTTP, last->domain);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_WARN, last->severity);
    TEST_ASSERT_EQUAL_INT(0, last->sys_errno);
    TEST_ASSERT_EQUAL_INT(502, last->status_code);
    TEST_ASSERT_EQUAL_INT(1, last->retryable);
    TEST_ASSERT_EQUAL_STRING("upstream status 502", last->msg);

    lib_err_emit(LIB_ERR_DOM_LLM, LIB_ERR_INTERNAL, 0, 0, "%s", long_msg);
    last = lib_err_last();
    TEST_ASSERT_EQUAL_CHAR('\0', last->msg[LIB_ERR_MSG_MAX - 1u]);
    TEST_ASSERT_EQUAL_UINT(LIB_ERR_MSG_MAX - 1u, strlen(last->msg));

    invoke_vemit_helper(LIB_ERR_DOM_MCP, LIB_ERR_STATE, 0, 0,
                        "session=%s", "stream-1");
    last = lib_err_last();
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_MCP, last->domain);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_STATE, last->code);
    TEST_ASSERT_EQUAL_STRING("session=stream-1", last->msg);
}

static void test_emit_sys_appends_errno(void)
{
    const lib_err_t *last;
    char errno_tag[32];

    snprintf(errno_tag, sizeof(errno_tag), "(errno=%d)", ETIMEDOUT);
    lib_err_emit_sys(LIB_ERR_DOM_HTTP, ETIMEDOUT, 504,
                     "request to %s", "api.example.com");

    last = lib_err_last();
    TEST_ASSERT_EQUAL_INT(LIB_ERR_TIMEOUT, last->code);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_HTTP, last->domain);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_WARN, last->severity);
    TEST_ASSERT_EQUAL_INT(ETIMEDOUT, last->sys_errno);
    TEST_ASSERT_EQUAL_INT(504, last->status_code);
    TEST_ASSERT_EQUAL_INT(1, last->retryable);
    TEST_ASSERT_NOT_NULL(strstr(last->msg, "request to api.example.com"));
    TEST_ASSERT_NOT_NULL(strstr(last->msg, errno_tag));

    snprintf(errno_tag, sizeof(errno_tag), "(errno=%d)", ENOENT);
    lib_err_emit_sys(LIB_ERR_DOM_TOOL, ENOENT, 0,
                     "missing tool config %s", "math.json");

    last = lib_err_last();
    TEST_ASSERT_EQUAL_INT(LIB_ERR_NOT_FOUND, last->code);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_TOOL, last->domain);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_SEV_ERROR, last->severity);
    TEST_ASSERT_EQUAL_INT(ENOENT, last->sys_errno);
    TEST_ASSERT_EQUAL_INT(0, last->status_code);
    TEST_ASSERT_EQUAL_INT(0, last->retryable);
    TEST_ASSERT_NOT_NULL(strstr(last->msg, "missing tool config math.json"));
    TEST_ASSERT_NOT_NULL(strstr(last->msg, errno_tag));
}

static void test_log_sink_immediate_and_reentrant_safe(void)
{
    lib_err_set_log_sink(capture_log_sink, &s_cap);

    lib_err_emit(LIB_ERR_DOM_HTTP, LIB_ERR_PROTOCOL, 0, 503,
                 "service overloaded");

    TEST_ASSERT_EQUAL_INT(1, s_cap.call_count);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_PROTOCOL, s_cap.last_copy.code);
    TEST_ASSERT_EQUAL_INT(LIB_ERR_DOM_HTTP, s_cap.last_copy.domain);
    TEST_ASSERT_EQUAL_INT(503, s_cap.last_copy.status_code);
    TEST_ASSERT_EQUAL_STRING("service overloaded", s_cap.last_copy.msg);
    TEST_ASSERT_EQUAL_INT(1, s_cap.saw_reentrant_block);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_err_clear_and_null_fmt_safe);
    RUN_TEST(test_enum_strings);
    RUN_TEST(test_code_from_errno_mapping);
    RUN_TEST(test_infer_retryable_and_severity);
    RUN_TEST(test_emit_structured_and_msg_truncation);
    RUN_TEST(test_emit_sys_appends_errno);
    RUN_TEST(test_log_sink_immediate_and_reentrant_safe);
    return UNITY_END();
}