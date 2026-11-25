/**
 * Copyright (c) 2026 yanrbts
 *
 * Offline Unity unit tests for lib_llm covering boundary inputs,
 * protocol encoding/parsing edge cases, and allocation failure paths.
 */

#include "unity.h"
#include "lib_llm.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#if defined(__GLIBC__)
#define HAVE_GLIBC_WRAP 1
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
#else
#define HAVE_GLIBC_WRAP 0
#endif

typedef struct mock_io {
    char         *req_json;
    size_t        req_len;
    char         *api_key;
    char         *url;
    int           timeout_ms;
    size_t        max_resp_sz;

    lib_llm_err_t rc;
    int           status;
    const char   *resp_body;
    const char   *io_err;
    int           calls;
} mock_io_t;

typedef struct oom_ctrl {
    int enabled;
    int fail_at;   /* 1-based allocation index to fail; <= 0 disables */
    int current;
} oom_ctrl_t;

static mock_io_t  g_io;
static oom_ctrl_t g_oom;

#if HAVE_GLIBC_WRAP
void *__wrap_malloc(size_t size)
{
    if (g_oom.enabled && g_oom.fail_at > 0) {
        g_oom.current++;
        if (g_oom.current == g_oom.fail_at) {
            return NULL;
        }
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (g_oom.enabled && g_oom.fail_at > 0) {
        g_oom.current++;
        if (g_oom.current == g_oom.fail_at) {
            return NULL;
        }
    }
    return __real_calloc(nmemb, size);
}
#endif

static char *xstrdup(const char *s)
{
    size_t n;
    char  *p;

    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1u);
    if (p == NULL) {
        if (!g_oom.enabled) {
            TEST_FAIL_MESSAGE("xstrdup out of memory outside OOM test");
        }
        return NULL;
    }
    memcpy(p, s, n + 1u);
    return p;
}

static void mock_io_reset(mock_io_t *io)
{
    free(io->req_json);
    free(io->api_key);
    free(io->url);
    memset(io, 0, sizeof(*io));
    io->rc = LIB_LLM_OK;
    io->status = 200;
}

static lib_llm_err_t mock_post(void *io_ctx,
                               const char *url,
                               const char *api_key,
                               const char *body_json,
                               size_t body_len,
                               int timeout_ms,
                               size_t max_resp_sz,
                               char **out_body,
                               size_t *out_len,
                               int *out_status,
                               char *errbuf,
                               size_t errlen)
{
    mock_io_t *io = (mock_io_t *)io_ctx;

    TEST_ASSERT_NOT_NULL(io);
    free(io->req_json);
    free(io->api_key);
    free(io->url);
    io->req_json = NULL;
    io->api_key = NULL;
    io->url = NULL;

    io->calls++;
    io->req_json = xstrdup(body_json);
    io->req_len = body_len;
    io->api_key = xstrdup(api_key);
    io->url = xstrdup(url);
    io->timeout_ms = timeout_ms;
    io->max_resp_sz = max_resp_sz;

    if (io->rc != LIB_LLM_OK) {
        if (io->io_err != NULL && errbuf != NULL && errlen > 0u) {
            (void)snprintf(errbuf, errlen, "%s", io->io_err);
        }
        return io->rc;
    }

    *out_status = io->status;
    if (io->resp_body != NULL) {
        *out_len = strlen(io->resp_body);
        *out_body = xstrdup(io->resp_body);
    } else {
        *out_len = 0u;
        *out_body = NULL;
    }
    return LIB_LLM_OK;
}

static lib_llm_ctx_t *open_default_ctx(mock_io_t *io)
{
    lib_llm_cfg_t  cfg;
    char           err[256] = {0};
    lib_llm_ctx_t *ctx;

    memset(&cfg, 0, sizeof(cfg));
    cfg.io.post     = mock_post;
    cfg.io.ctx      = io;
    cfg.endpoint    = "http://mock.local/v1/chat/completions";
    cfg.api_key     = "test-token";
    cfg.model       = "gemma4:e4b";
    cfg.timeout_ms  = 0;        /* trigger default */
    cfg.max_resp_sz = 0u;       /* trigger default */
    cfg.temperature = -1.0;     /* omit temperature */
    cfg.max_tokens  = 0;        /* omit max_tokens */

    ctx = lib_llm_open(&cfg, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(ctx, err);
    return ctx;
}

void setUp(void)
{
    memset(&g_oom, 0, sizeof(g_oom));
    mock_io_reset(&g_io);
}

void tearDown(void)
{
    memset(&g_oom, 0, sizeof(g_oom));
    mock_io_reset(&g_io);
}

void test_null_and_lifecycle_safety(void)
{
    lib_llm_res_t res;

    TEST_ASSERT_EQUAL_STRING("", lib_llm_errstr(NULL));
    TEST_ASSERT_EQUAL_STRING("ok", lib_llm_strerror(LIB_LLM_OK));
    TEST_ASSERT_EQUAL_STRING("invalid argument", lib_llm_strerror(LIB_LLM_ERR_ARG));
    TEST_ASSERT_EQUAL_STRING("out of memory", lib_llm_strerror(LIB_LLM_ERR_MEM));
    TEST_ASSERT_EQUAL_STRING("i/o error", lib_llm_strerror(LIB_LLM_ERR_IO));
    TEST_ASSERT_EQUAL_STRING("timed out", lib_llm_strerror(LIB_LLM_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("protocol error", lib_llm_strerror(LIB_LLM_ERR_PROTO));
    TEST_ASSERT_EQUAL_STRING("http error", lib_llm_strerror(LIB_LLM_ERR_HTTP));
    TEST_ASSERT_EQUAL_STRING("llm api error", lib_llm_strerror(LIB_LLM_ERR_API));
    TEST_ASSERT_EQUAL_STRING("unknown error", lib_llm_strerror((lib_llm_err_t)-999));

    lib_llm_close(NULL);
    lib_llm_free_res(NULL);
    lib_llm_free_calls(NULL, 3u);

    memset(&res, 0, sizeof(res));
    lib_llm_free_res(&res);
    lib_llm_free_res(&res);
}

void test_open_and_chat_arg_validation(void)
{
    char           err[256] = {0};
    lib_llm_cfg_t  cfg;
    lib_llm_ctx_t *ctx;
    lib_llm_msg_t  msg;
    lib_llm_res_t  res;

    TEST_ASSERT_NULL(lib_llm_open(NULL, err, sizeof(err)));
    TEST_ASSERT_TRUE(err[0] != '\0');

    memset(&cfg, 0, sizeof(cfg));
    TEST_ASSERT_NULL(lib_llm_open(&cfg, err, sizeof(err)));

    cfg.io.post = mock_post;
    TEST_ASSERT_NULL(lib_llm_open(&cfg, err, sizeof(err)));

    cfg.endpoint = "http://mock.local/v1/chat/completions";
    TEST_ASSERT_NULL(lib_llm_open(&cfg, err, sizeof(err)));

    cfg.model = "gemma4:e4b";
    ctx = lib_llm_open(&cfg, err, sizeof(err));
    TEST_ASSERT_NOT_NULL(ctx);

    memset(&msg, 0, sizeof(msg));
    msg.role = LIB_LLM_ROLE_USER;
    msg.content = "hello";

    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(NULL, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(ctx, NULL, 1u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(ctx, &msg, 0u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, NULL, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 1u, &res, err, sizeof(err)));

    lib_llm_close(ctx);
}

void test_request_encoding_optional_and_tool_fields(void)
{
    lib_llm_ctx_t     *ctx = open_default_ctx(&g_io);
    lib_llm_tool_call_t prev_calls[1];
    lib_llm_msg_t      msgs[3];
    lib_llm_tool_def_t tools[2];
    lib_llm_res_t      res;
    char               err[256] = {0};
    cJSON             *root;
    const cJSON       *marr;
    const cJSON       *tarr;

    /* Default timeout/max_resp_sz applied; negative temp & zero max_tokens omitted */
    TEST_ASSERT_TRUE(g_io.timeout_ms == 0); /* before first call */
    TEST_ASSERT_TRUE(ctx != NULL);

    memset(prev_calls, 0, sizeof(prev_calls));
    prev_calls[0].id = "call_1";
    prev_calls[0].name = "lookup";
    prev_calls[0].args_json = ""; /* defaults to {} */

    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = LIB_LLM_ROLE_USER;
    msgs[0].content = "run lookup";

    msgs[1].role = LIB_LLM_ROLE_ASSISTANT;
    msgs[1].content = NULL; /* becomes JSON null when tool_count > 0 */
    msgs[1].tool_calls = prev_calls;
    msgs[1].tool_count = 1u;

    msgs[2].role = LIB_LLM_ROLE_TOOL;
    msgs[2].content = NULL; /* defaults to "" for tool role */
    msgs[2].tool_call_id = "call_1";
    msgs[2].name = "lookup";

    memset(tools, 0, sizeof(tools));
    tools[0].name = "lookup";
    tools[0].description = "Look up value";
    tools[0].schema_json = "   \t\n  "; /* whitespace-only defaults to {} */

    tools[1].name = "ping";
    tools[1].description = NULL;
    tools[1].schema_json = NULL;

    g_io.status = 200;
    g_io.resp_body =
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"done\"}}]}";

    TEST_ASSERT_EQUAL_INT(LIB_LLM_OK,
                          lib_llm_chat(ctx, msgs, 3u, tools, 2u, &res, err, sizeof(err)));
    TEST_ASSERT_TRUE(g_io.timeout_ms > 0);
    TEST_ASSERT_TRUE(g_io.max_resp_sz > 0u);

    root = cJSON_Parse(g_io.req_json);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "temperature"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "max_tokens"));

    marr = cJSON_GetObjectItemCaseSensitive(root, "messages");
    TEST_ASSERT_TRUE(cJSON_IsArray(marr));
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(marr));

    tarr = cJSON_GetObjectItemCaseSensitive(root, "tools");
    TEST_ASSERT_TRUE(cJSON_IsArray(tarr));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(tarr));
    cJSON_Delete(root);

    TEST_ASSERT_EQUAL_STRING("done", res.text);
    TEST_ASSERT_EQUAL_INT64(0, res.usage.total_tokens);

    lib_llm_free_res(&res);
    lib_llm_close(ctx);
}

void test_invalid_msg_and_tool_schema_index_reporting(void)
{
    lib_llm_ctx_t     *ctx = open_default_ctx(&g_io);
    lib_llm_msg_t      msgs[2];
    lib_llm_tool_def_t bad_tool;
    lib_llm_res_t      res;
    char               err[256] = {0};

    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = LIB_LLM_ROLE_USER;
    msgs[0].content = "ok";

    /* Invalid message at index 1: tool role without tool_call_id */
    msgs[1].role = LIB_LLM_ROLE_TOOL;
    msgs[1].content = "data";
    msgs[1].tool_call_id = NULL;

    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(ctx, msgs, 2u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "invalid message at index 1"));
    TEST_ASSERT_EQUAL_INT(0, g_io.calls);

    /* Invalid tool definition at index 0: schema_json is not a JSON object */
    memset(&bad_tool, 0, sizeof(bad_tool));
    bad_tool.name = "bad_fn";
    bad_tool.schema_json = "[1, 2, 3]";

    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_ARG,
                          lib_llm_chat(ctx, msgs, 1u, &bad_tool, 1u, &res, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "invalid tool definition at index 0"));
    TEST_ASSERT_EQUAL_INT(0, g_io.calls);

    lib_llm_close(ctx);
}

void test_response_edge_cases_and_errors(void)
{
    lib_llm_ctx_t *ctx = open_default_ctx(&g_io);
    lib_llm_msg_t  msg;
    lib_llm_res_t  res;
    char           err[256] = {0};

    memset(&msg, 0, sizeof(msg));
    msg.role = LIB_LLM_ROLE_USER;
    msg.content = "test";

    /* arguments provided as JSON object instead of string */
    g_io.status = 200;
    g_io.resp_body =
        "{"
        "  \"choices\": [{"
        "    \"finish_reason\": \"tool_calls\","
        "    \"message\": {"
        "      \"tool_calls\": [{"
        "        \"function\": {\"name\": \"calc\", \"arguments\": {\"x\": 7}}"
        "      }]"
        "    }"
        "  }]"
        "}";
    TEST_ASSERT_EQUAL_INT(LIB_LLM_OK,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_NULL(res.text);
    TEST_ASSERT_EQUAL_UINT64(1u, (uint64_t)res.tool_count);
    TEST_ASSERT_EQUAL_STRING("", res.tool_calls[0].id);
    TEST_ASSERT_EQUAL_STRING("calc", res.tool_calls[0].name);
    TEST_ASSERT_EQUAL_STRING("{\"x\":7}", res.tool_calls[0].args_json);
    lib_llm_free_res(&res);

    /* HTTP 200 with error object returns LIB_LLM_ERR_API and zeroes out_res */
    mock_io_reset(&g_io);
    g_io.status = 200;
    g_io.resp_body = "{\"error\":{\"message\":\"context length exceeded\"}}";
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_API,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "context length exceeded"));
    TEST_ASSERT_NULL(res.raw_json);
    lib_llm_free_res(&res);

    /* HTTP 500 without JSON body */
    mock_io_reset(&g_io);
    g_io.status = 500;
    g_io.resp_body = "plain text failure";
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_HTTP,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "500"));

    /* Missing choices[0] */
    mock_io_reset(&g_io);
    g_io.status = 200;
    g_io.resp_body = "{\"choices\":[]}";
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_PROTO,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));

    /* tool_calls entry missing function.name */
    mock_io_reset(&g_io);
    g_io.status = 200;
    g_io.resp_body =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":{\"arguments\":\"{}\"}}]}}]}";
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_PROTO,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));

    /* Transport failure fallback to lib_llm_strerror when io_err empty */
    mock_io_reset(&g_io);
    g_io.rc = LIB_LLM_ERR_IO;
    g_io.io_err = NULL;
    TEST_ASSERT_EQUAL_INT(LIB_LLM_ERR_IO,
                          lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "i/o error"));

    lib_llm_close(ctx);
}

#if HAVE_GLIBC_WRAP
void test_oom_injection_paths(void)
{
    lib_llm_cfg_t  cfg;
    lib_llm_ctx_t *ctx;
    lib_llm_msg_t  msg;
    lib_llm_res_t  res;
    char           err[256] = {0};
    int            step;

    memset(&cfg, 0, sizeof(cfg));
    cfg.io.post  = mock_post;
    cfg.io.ctx   = &g_io;
    cfg.endpoint = "http://mock.local/v1/chat/completions";
    cfg.model    = "gemma4:e4b";

    /* Fail each allocation step during lib_llm_open */
    for (step = 1; step <= 8; step++) {
        g_oom.enabled = 1;
        g_oom.fail_at = step;
        g_oom.current = 0;
        ctx = lib_llm_open(&cfg, err, sizeof(err));
        g_oom.enabled = 0;
        if (ctx != NULL) {
            lib_llm_close(ctx);
        }
    }

    ctx = open_default_ctx(&g_io);
    memset(&msg, 0, sizeof(msg));
    msg.role = LIB_LLM_ROLE_USER;
    msg.content = "hello oom";

    g_io.status = 200;
    g_io.resp_body =
        "{"
        "  \"choices\": [{"
        "    \"finish_reason\": \"stop\","
        "    \"message\": {\"content\": \"ok\", \"tool_calls\": [{\"id\":\"c1\",\"function\":{\"name\":\"fn\",\"arguments\":\"{}\"}}]}"
        "  }]"
        "}";

    /* Fail each allocation step during lib_llm_chat */
    for (step = 1; step <= 40; step++) {
        mock_io_reset(&g_io);
        g_io.status = 200;
        g_io.resp_body =
            "{"
            "  \"choices\": [{"
            "    \"finish_reason\": \"stop\","
            "    \"message\": {\"content\": \"ok\", \"tool_calls\": [{\"id\":\"c1\",\"function\":{\"name\":\"fn\",\"arguments\":\"{}\"}}]}"
            "  }]"
            "}";
        memset(&res, 0, sizeof(res));

        g_oom.enabled = 1;
        g_oom.fail_at = step;
        g_oom.current = 0;
        (void)lib_llm_chat(ctx, &msg, 1u, NULL, 0u, &res, err, sizeof(err));
        g_oom.enabled = 0;

        lib_llm_free_res(&res);
    }

    lib_llm_close(ctx);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_and_lifecycle_safety);
    RUN_TEST(test_open_and_chat_arg_validation);
    RUN_TEST(test_request_encoding_optional_and_tool_fields);
    RUN_TEST(test_invalid_msg_and_tool_schema_index_reporting);
    RUN_TEST(test_response_edge_cases_and_errors);
#if HAVE_GLIBC_WRAP
    RUN_TEST(test_oom_injection_paths);
#endif
    return UNITY_END();
}