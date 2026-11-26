/**
 * Copyright (c) 2026 yanrbts
 *
 * Unity integration test suite for lib_llm + lib_http against Ollama (gemma4:e4b).
 */

#include "unity.h"
#include "lib_llm.h"
#include "lib_http.h"
#include "lib_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OLLAMA_DEFAULT_BASE "http://192.168.110.16:11434"
#define OLLAMA_CHAT_PATH    "/v1/chat/completions"
#define OLLAMA_MODEL        "gemma4:e4b"
#define OLLAMA_TIMEOUT_MS   60000
#define OLLAMA_MAX_RESP_SZ  (16u * 1024u * 1024u)

static lib_http_ctx_t *g_http = NULL;
static lib_llm_ctx_t  *g_llm  = NULL;
static char            g_endpoint[256];

static lib_llm_err_t map_http_err(lib_http_err_t rc)
{
    switch (rc) {
    case LIB_HTTP_OK:          return LIB_LLM_OK;
    case LIB_HTTP_ERR_ARG:     return LIB_LLM_ERR_ARG;
    case LIB_HTTP_ERR_NOMEM:   return LIB_LLM_ERR_MEM;
    case LIB_HTTP_ERR_TIMEOUT: return LIB_LLM_ERR_TIMEOUT;
    case LIB_HTTP_ERR_PROTO:   return LIB_LLM_ERR_PROTO;
    default:                   return LIB_LLM_ERR_IO;
    }
}

static lib_llm_err_t http_io_post_adapter(void *io_ctx,
                                          const char *url,
                                          const char *api_key,
                                          const char *body_json,
                                          size_t body_len,
                                          int timeout_ms,
                                          size_t max_resp_sz,
                                          char **out_body,
                                          size_t *out_len,
                                          int *out_status)
{
    lib_http_ctx_t *http = (lib_http_ctx_t *)io_ctx;
    lib_http_res_t  res;
    lib_http_err_t  rc;

    (void)body_len;
    (void)max_resp_sz;

    if (http == NULL || url == NULL || out_body == NULL ||
        out_len == NULL || out_status == NULL) {
        return LIB_LLM_ERR_ARG;
    }

    *out_body = NULL;
    *out_len = 0u;
    *out_status = 0;

    memset(&res, 0, sizeof(res));
    rc = lib_http_post_json(http, url, api_key, body_json,
                            timeout_ms, &res);
    if (rc != LIB_HTTP_OK) {
        lib_http_free_res(&res);
        return map_http_err(rc);
    }

    *out_status = res.status_code;
    *out_len = res.body_len;
    *out_body = res.body;
    res.body = NULL;
    res.body_len = 0u;
    lib_http_free_res(&res);
    return LIB_LLM_OK;
}

static void build_ollama_endpoint(char *buf, size_t len)
{
    const char *host = getenv("OLLAMA_HOST");
    size_t      n;

    if (host == NULL || host[0] == '\0') {
        (void)snprintf(buf, len, "%s%s", OLLAMA_DEFAULT_BASE, OLLAMA_CHAT_PATH);
        return;
    }

    if (strncmp(host, "http://", 7) == 0 || strncmp(host, "https://", 8) == 0) {
        (void)snprintf(buf, len, "%s%s", host, OLLAMA_CHAT_PATH);
    } else {
        (void)snprintf(buf, len, "http://%s%s", host, OLLAMA_CHAT_PATH);
    }

    n = strlen(buf);
    if (n > 0u && buf[n - 1u] == '/') {
        buf[n - 1u] = '\0';
    }
}

void setUp(void)
{
    lib_http_cfg_t hcfg;
    lib_llm_cfg_t  lcfg;

    build_ollama_endpoint(g_endpoint, sizeof(g_endpoint));

    memset(&hcfg, 0, sizeof(hcfg));
    hcfg.timeout_ms  = OLLAMA_TIMEOUT_MS;
    hcfg.max_resp_sz = OLLAMA_MAX_RESP_SZ;

    g_http = lib_http_open(&hcfg);

    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.io.post     = http_io_post_adapter;
    lcfg.io.ctx      = g_http;
    lcfg.endpoint    = g_endpoint;
    lcfg.api_key     = NULL;
    lcfg.model       = OLLAMA_MODEL;
    lcfg.timeout_ms  = OLLAMA_TIMEOUT_MS;
    lcfg.max_resp_sz = OLLAMA_MAX_RESP_SZ;
    lcfg.temperature = 0.1;
    lcfg.max_tokens  = 256;

    g_llm = lib_llm_open(&lcfg);
    TEST_ASSERT_NOT_NULL_MESSAGE(g_llm, lib_err_last()->msg);
}

void tearDown(void)
{
    lib_llm_close(g_llm);
    g_llm = NULL;

    lib_http_close(g_http);
    g_http = NULL;
}

void test_gemma4_e4b_basic_chat(void)
{
    lib_llm_msg_t msgs[2];
    lib_llm_res_t res;
    lib_llm_err_t rc;

    memset(msgs, 0, sizeof(msgs));
    msgs[0].role    = LIB_LLM_ROLE_SYSTEM;
    msgs[0].content = "Reply concisely in one short sentence.";
    msgs[1].role    = LIB_LLM_ROLE_USER;
    msgs[1].content = "Reply OK and compute 2 + 2.";

    rc = lib_llm_chat(g_llm, msgs, 2u, NULL, 0u, &res);
    TEST_ASSERT_EQUAL_INT(LIB_LLM_OK, rc);
    TEST_ASSERT_NOT_NULL(res.text);
    TEST_ASSERT_TRUE(res.text[0] != '\0');
    TEST_ASSERT_NOT_NULL(res.raw_json);

    printf("\n[endpoint=%s model=%s] reply: %s\n", g_endpoint, OLLAMA_MODEL, res.text);
    printf("[usage] prompt=%lld completion=%lld total=%lld\n",
           (long long)res.usage.prompt_tokens,
           (long long)res.usage.completion_tokens,
           (long long)res.usage.total_tokens);

    lib_llm_free_res(&res);
}

void test_gemma4_e4b_tool_call_flow(void)
{
    lib_llm_tool_def_t tool;
    lib_llm_msg_t      turn1[1];
    lib_llm_res_t      res1;
    lib_llm_err_t      rc;

    memset(&tool, 0, sizeof(tool));
    tool.name        = "get_uptime";
    tool.description = "Get system uptime in seconds";
    tool.schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";

    memset(turn1, 0, sizeof(turn1));
    turn1[0].role    = LIB_LLM_ROLE_USER;
    turn1[0].content = "Call tool get_uptime now.";

    rc = lib_llm_chat(g_llm, turn1, 1u, &tool, 1u, &res1);
    TEST_ASSERT_EQUAL_INT(LIB_LLM_OK, rc);

    if (res1.tool_count > 0u) {
        lib_llm_msg_t turn2[3];
        lib_llm_res_t res2;

        TEST_ASSERT_NOT_NULL(res1.tool_calls);
        TEST_ASSERT_EQUAL_STRING("get_uptime", res1.tool_calls[0].name);
        TEST_ASSERT_NOT_NULL(res1.tool_calls[0].args_json);
        printf("\n[tool_call] id=%s name=%s args=%s\n",
               res1.tool_calls[0].id ? res1.tool_calls[0].id : "",
               res1.tool_calls[0].name,
               res1.tool_calls[0].args_json);

        memset(turn2, 0, sizeof(turn2));
        turn2[0] = turn1[0];
        turn2[1].role = LIB_LLM_ROLE_ASSISTANT;
        turn2[1].content = NULL;
        turn2[1].tool_calls = res1.tool_calls;
        turn2[1].tool_count = res1.tool_count;

        turn2[2].role = LIB_LLM_ROLE_TOOL;
        turn2[2].content = "{\"uptime_sec\": 3600}";
        turn2[2].tool_call_id = (res1.tool_calls[0].id && res1.tool_calls[0].id[0] != '\0')
                                    ? res1.tool_calls[0].id
                                    : "call_0";
        turn2[2].name = "get_uptime";

        rc = lib_llm_chat(g_llm, turn2, 3u, &tool, 1u, &res2);
        TEST_ASSERT_EQUAL_INT(LIB_LLM_OK, rc);
        TEST_ASSERT_NOT_NULL(res2.text);
        printf("[final_reply] %s\n", res2.text);
        lib_llm_free_res(&res2);
    } else {
        TEST_ASSERT_NOT_NULL(res1.text);
        TEST_ASSERT_TRUE(res1.text[0] != '\0');
        printf("\n[direct_reply] %s\n", res1.text);
    }

    lib_llm_free_res(&res1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gemma4_e4b_basic_chat);
    RUN_TEST(test_gemma4_e4b_tool_call_flow);
    return UNITY_END();
}