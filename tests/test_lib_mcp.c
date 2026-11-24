/**
 * Copyright (c) 2026 yanrbts
 *
 * Unit and protocol tests for lib_mcp using Unity.
 */
#define _POSIX_C_SOURCE 200809L

#include "lib_mcp.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_self_bin[4096] = "./build/test/test_lib_mcp";

void setUp(void)
{
}

void tearDown(void)
{
}

static void mock_send_json(const char *json_line)
{
    fputs(json_line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void mock_extract_id(const char *line, char *out, size_t out_sz)
{
    const char *p = strstr(line, "\"id\"");
    if (!p) {
        snprintf(out, out_sz, "null");
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        snprintf(out, out_sz, "null");
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && i + 1 < out_sz) {
        if (*p != ' ' && *p != '\t') {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    if (i == 0) {
        snprintf(out, out_sz, "null");
    }
}

static int run_mock_server(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;

    /* stderr noise must not interfere with MCP stdout framing */
    fprintf(stderr, "[test_lib_mcp mock] started\n");
    fflush(stderr);

    while ((nread = getline(&line, &cap, stdin)) != -1) {
        while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            line[--nread] = '\0';
        }
        if (nread == 0) {
            continue;
        }

        if (strstr(line, "\"method\":\"notifications/initialized\"") != NULL ||
            strstr(line, "\"method\": \"notifications/initialized\"") != NULL) {
            continue;
        }

        char id_tok[64];
        mock_extract_id(line, id_tok, sizeof(id_tok));

        if (strstr(line, "\"method\":\"initialize\"") != NULL ||
            strstr(line, "\"method\": \"initialize\"") != NULL) {
            char resp[512];
            snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                     "\"protocolVersion\":\"2025-06-18\","
                     "\"capabilities\":{\"tools\":{}},"
                     "\"serverInfo\":{\"name\":\"test_mock\",\"version\":\"1.0.0\"}"
                     "}}",
                     id_tok);
            mock_send_json(resp);
        } else if (strstr(line, "\"method\":\"tools/list\"") != NULL ||
                   strstr(line, "\"method\": \"tools/list\"") != NULL) {
            /* Emit an unsolicited notification before the response */
            mock_send_json("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}");

            char resp[1024];
            snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":["
                     "{\"name\":\"echo\",\"description\":\"Echo message\","
                     "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}}}},"
                     "{\"name\":\"fail_tool\",\"description\":\"Returns tool error\","
                     "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                     "{\"name\":\"slow_tool\",\"description\":\"Delays past timeout\","
                     "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
                     "]}}",
                     id_tok);
            mock_send_json(resp);
        } else if (strstr(line, "\"method\":\"tools/call\"") != NULL ||
                   strstr(line, "\"method\": \"tools/call\"") != NULL) {
            if (strstr(line, "\"slow_tool\"") != NULL) {
                sleep(2);
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                         "\"isError\":false,\"content\":[{\"type\":\"text\",\"text\":\"late\"}]}}",
                         id_tok);
                mock_send_json(resp);
            } else if (strstr(line, "\"fail_tool\"") != NULL) {
                char resp[512];
                snprintf(resp, sizeof(resp),
                         "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                         "\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"denied\"}]}}",
                         id_tok);
                mock_send_json(resp);
            } else {
                char resp[512];
                snprintf(resp, sizeof(resp),
                         "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                         "\"isError\":false,\"content\":["
                         "{\"type\":\"text\",\"text\":\"part1-\"},"
                         "{\"type\":\"text\",\"text\":\"part2\"}"
                         "]}}",
                         id_tok);
                mock_send_json(resp);
            }
        } else {
            char err_resp[256];
            snprintf(err_resp, sizeof(err_resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}",
                     id_tok);
            mock_send_json(err_resp);
        }
    }

    free(line);
    return 0;
}

static void test_invalid_inputs_and_cleanup(void)
{
    char errbuf[128] = {0};
    lib_mcp_tool_t *tools = NULL;
    size_t count = 0;
    lib_mcp_res_t res;
    memset(&res, 0, sizeof(res));

    lib_mcp_close(NULL);
    lib_mcp_free_tools(NULL, 0);
    lib_mcp_free_res(NULL);

    TEST_ASSERT_NULL(lib_mcp_open(NULL, errbuf, sizeof(errbuf)));
    TEST_ASSERT_TRUE(errbuf[0] != '\0');

    lib_mcp_cfg_t bad_cfg;
    memset(&bad_cfg, 0, sizeof(bad_cfg));
    TEST_ASSERT_NULL(lib_mcp_open(&bad_cfg, errbuf, sizeof(errbuf)));

    TEST_ASSERT_EQUAL_INT(LIB_MCP_ERR_ARG,
                          lib_mcp_tools(NULL, &tools, &count, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_ERR_ARG,
                          lib_mcp_call(NULL, "echo", "{}", &res, errbuf, sizeof(errbuf)));

    TEST_ASSERT_EQUAL_STRING("ok", lib_mcp_strerror(LIB_MCP_OK));
    TEST_ASSERT_EQUAL_STRING("protocol error", lib_mcp_strerror(LIB_MCP_ERR_PROTO));
}

static void test_mcp_protocol_flow(void)
{
    char errbuf[256] = {0};
    const char *argv[] = { g_self_bin, "--mock-server", NULL };

    lib_mcp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmd = g_self_bin;
    cfg.argv = argv;
    cfg.timeout_ms = 1500;

    lib_mcp_ctx_t *ctx = lib_mcp_open(&cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    lib_mcp_tool_t *tools = NULL;
    size_t count = 0;
    lib_mcp_err_t rc = lib_mcp_tools(ctx, &tools, &count, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_OK, rc);
    TEST_ASSERT_EQUAL_UINT64(3u, (uint64_t)count);
    TEST_ASSERT_EQUAL_STRING("echo", tools[0].name);
    TEST_ASSERT_NOT_NULL(tools[0].schema_json);
    TEST_ASSERT_EQUAL_CHAR('{', tools[0].schema_json[0]);
    lib_mcp_free_tools(tools, count);

    lib_mcp_res_t res;
    rc = lib_mcp_call(ctx, "echo", "{\"msg\":\"hi\"}", &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, res.is_error);
    TEST_ASSERT_NOT_NULL(res.text);
    TEST_ASSERT_EQUAL_STRING("part1-\npart2", res.text);
    TEST_ASSERT_NOT_NULL(res.raw_json);
    lib_mcp_free_res(&res);

    rc = lib_mcp_call(ctx, "fail_tool", "{}", &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, res.is_error);
    TEST_ASSERT_NOT_NULL(res.text);
    TEST_ASSERT_EQUAL_STRING("denied", res.text);
    lib_mcp_free_res(&res);

    cfg.timeout_ms = 200;
    lib_mcp_close(ctx);
    ctx = lib_mcp_open(&cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    rc = lib_mcp_call(ctx, "slow_tool", "{}", &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_ERR_TIMEOUT, rc);
    lib_mcp_free_res(&res);

    lib_mcp_close(ctx);
}

static void test_real_mcp_server(void)
{
    const char *argv[] = {
        "npx",
        "-y",
        "@modelcontextprotocol/server-everything",
        NULL
    };

    lib_mcp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmd = "npx";
    cfg.argv = argv;
    cfg.timeout_ms = 20000;

    char errbuf[256] = {0};
    lib_mcp_ctx_t *ctx = lib_mcp_open(&cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    lib_mcp_tool_t *tools = NULL;
    size_t count = 0;
    lib_mcp_err_t rc = lib_mcp_tools(ctx, &tools, &count, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_OK, rc);
    TEST_ASSERT_TRUE(count > 0u);
    lib_mcp_free_tools(tools, count);

    lib_mcp_res_t res;
    rc = lib_mcp_call(ctx, "echo", "{\"message\":\"hello from lib_mcp\"}",
                      &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_MCP_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, res.is_error);
    TEST_ASSERT_NOT_NULL(res.raw_json);
    lib_mcp_free_res(&res);

    lib_mcp_close(ctx);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--mock-server") == 0) {
        return run_mock_server();
    }

    if (argc > 0 && argv[0] && argv[0][0] != '\0') {
        snprintf(g_self_bin, sizeof(g_self_bin), "%s", argv[0]);
    }

    UNITY_BEGIN();
    RUN_TEST(test_invalid_inputs_and_cleanup);
    RUN_TEST(test_mcp_protocol_flow);
    RUN_TEST(test_real_mcp_server);
    return UNITY_END();
}