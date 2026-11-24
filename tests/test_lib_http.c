/**
 * Copyright (c) 2026 yanrbts
 *
 * Unit and loopback integration tests for lib_http using Unity.
 */
#include "lib_http.h"
#include "mongoose.h"
#include "unity.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct mock_srv {
    pthread_t       tid;
    pthread_mutex_t mtx;
    int             ready;
    int             stop;
    int             port;
    char            url_base[128];
} mock_srv_t;

static mock_srv_t g_srv;
static int g_srv_started = 0;

static void mock_http_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) {
        return;
    }

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (mg_match(hm->uri, mg_str("/status/404"), NULL)) {
        mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                      "{\"error\":\"not_found\"}");
        return;
    }

    if (mg_match(hm->uri, mg_str("/status/500"), NULL)) {
        mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                      "{\"error\":\"internal_error\"}");
        return;
    }

    if (mg_match(hm->uri, mg_str("/slow"), NULL)) {
        usleep(400 * 1000);
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "late");
        return;
    }

    if (mg_match(hm->uri, mg_str("/big"), NULL)) {
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n",
                      "0123456789abcdef0123456789abcdef0123456789abcdef");
        return;
    }

    if (mg_match(hm->uri, mg_str("/echo"), NULL)) {
        struct mg_str *auth = mg_http_get_header(hm, "Authorization");
        struct mg_str *x_hdr = mg_http_get_header(hm, "X-Custom-Tag");
        char resp[2048];
        snprintf(resp, sizeof(resp),
                 "{\"method\":\"%.*s\",\"auth\":\"%.*s\",\"tag\":\"%.*s\",\"body\":\"%.*s\"}",
                 (int)hm->method.len,
                 hm->method.buf ? hm->method.buf : "",
                 auth ? (int)auth->len : 0,
                 auth && auth->buf ? auth->buf : "",
                 x_hdr ? (int)x_hdr->len : 0,
                 x_hdr && x_hdr->buf ? x_hdr->buf : "",
                 (int)hm->body.len,
                 hm->body.buf ? hm->body.buf : "");
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", resp);
        return;
    }

    mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "ok");
}

static void *mock_srv_thread(void *arg)
{
    mock_srv_t *srv = (mock_srv_t *)arg;
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_connection *lc = NULL;
    for (int p = 18080; p < 18180; p++) {
        char listen_url[64];
        snprintf(listen_url, sizeof(listen_url), "http://127.0.0.1:%d", p);
        lc = mg_http_listen(&mgr, listen_url, mock_http_handler, srv);
        if (lc != NULL) {
            pthread_mutex_lock(&srv->mtx);
            srv->port = p;
            snprintf(srv->url_base, sizeof(srv->url_base), "http://127.0.0.1:%d", p);
            srv->ready = 1;
            pthread_mutex_unlock(&srv->mtx);
            break;
        }
    }

    if (lc == NULL) {
        pthread_mutex_lock(&srv->mtx);
        srv->ready = -1;
        pthread_mutex_unlock(&srv->mtx);
        mg_mgr_free(&mgr);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&srv->mtx);
        int should_stop = srv->stop;
        pthread_mutex_unlock(&srv->mtx);
        if (should_stop) {
            break;
        }
        mg_mgr_poll(&mgr, 20);
    }

    mg_mgr_free(&mgr);
    return NULL;
}

static void ensure_mock_srv(void)
{
    if (g_srv_started) {
        return;
    }
    memset(&g_srv, 0, sizeof(g_srv));
    pthread_mutex_init(&g_srv.mtx, NULL);
    if (pthread_create(&g_srv.tid, NULL, mock_srv_thread, &g_srv) != 0) {
        pthread_mutex_destroy(&g_srv.mtx);
        return;
    }

    int ready = 0;
    for (;;) {
        pthread_mutex_lock(&g_srv.mtx);
        ready = g_srv.ready;
        pthread_mutex_unlock(&g_srv.mtx);
        if (ready != 0) {
            break;
        }
        usleep(1000);
    }

    if (ready > 0) {
        g_srv_started = 1;
    } else {
        pthread_join(g_srv.tid, NULL);
        pthread_mutex_destroy(&g_srv.mtx);
    }
}

void setUp(void)
{
    ensure_mock_srv();
    TEST_ASSERT_EQUAL_INT(1, g_srv_started);
}

void tearDown(void)
{
    /* Keep loopback server alive across test cases in this binary */
}

static void test_http_invalid_args(void)
{
    char errbuf[128] = {0};
    lib_http_res_t res;
    memset(&res, 0, sizeof(res));

    lib_http_err_t rc = lib_http_exec(NULL, NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_ERR_ARG, rc);
    TEST_ASSERT_TRUE(errbuf[0] != '\0');

    lib_http_ctx_t *ctx = lib_http_open(NULL, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    lib_http_req_t bad_req;
    memset(&bad_req, 0, sizeof(bad_req));
    bad_req.method = "GET\r\nEvil: 1";
    bad_req.url = g_srv.url_base;
    rc = lib_http_exec(ctx, &bad_req, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_ERR_ARG, rc);

    lib_http_close(ctx);
    lib_http_close(NULL);
    lib_http_free_res(&res);
    lib_http_free_res(NULL);
}

static void test_http_post_json_and_headers(void)
{
    char errbuf[128] = {0};
    lib_http_ctx_t *ctx = lib_http_open(NULL, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    char url[256];
    snprintf(url, sizeof(url), "%s/echo", g_srv.url_base);

    lib_http_res_t res;
    lib_http_err_t rc = lib_http_post_json(ctx,
                                           url,
                                           "secret-tok-123",
                                           "{\"hello\":\"world\"}",
                                           2000,
                                           &res,
                                           errbuf,
                                           sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_OK, rc);
    TEST_ASSERT_EQUAL_INT(200, res.status_code);
    TEST_ASSERT_NOT_NULL(res.body);
    TEST_ASSERT_NOT_NULL(strstr(res.body, "\"method\":\"POST\""));
    TEST_ASSERT_NOT_NULL(strstr(res.body, "\"auth\":\"Bearer secret-tok-123\""));
    lib_http_free_res(&res);

    lib_http_header_t hdrs[1] = {
        { .name = "X-Custom-Tag", .value = "agent-v1" }
    };
    lib_http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.headers = hdrs;
    req.header_num = 1u;
    req.timeout_ms = 2000;

    rc = lib_http_exec(ctx, &req, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_OK, rc);
    TEST_ASSERT_EQUAL_INT(200, res.status_code);
    TEST_ASSERT_NOT_NULL(res.body);
    TEST_ASSERT_NOT_NULL(strstr(res.body, "\"method\":\"GET\""));
    TEST_ASSERT_NOT_NULL(strstr(res.body, "\"tag\":\"agent-v1\""));
    lib_http_free_res(&res);

    lib_http_close(ctx);
}

static void test_http_status_codes(void)
{
    char errbuf[128] = {0};
    lib_http_ctx_t *ctx = lib_http_open(NULL, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    char url404[256];
    snprintf(url404, sizeof(url404), "%s/status/404", g_srv.url_base);

    lib_http_req_t req404;
    memset(&req404, 0, sizeof(req404));
    req404.method = "GET";
    req404.url = url404;

    lib_http_res_t res;
    lib_http_err_t rc = lib_http_exec(ctx, &req404, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_OK, rc);
    TEST_ASSERT_EQUAL_INT(404, res.status_code);
    TEST_ASSERT_NOT_NULL(res.body);
    TEST_ASSERT_NOT_NULL(strstr(res.body, "not_found"));
    lib_http_free_res(&res);

    char url500[256];
    snprintf(url500, sizeof(url500), "%s/status/500", g_srv.url_base);
    rc = lib_http_post_json(ctx, url500, NULL, "{}", 2000, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_OK, rc);
    TEST_ASSERT_EQUAL_INT(500, res.status_code);
    TEST_ASSERT_NOT_NULL(res.body);
    TEST_ASSERT_NOT_NULL(strstr(res.body, "internal_error"));
    lib_http_free_res(&res);

    lib_http_close(ctx);
}

static void test_http_max_resp_size(void)
{
    char errbuf[128] = {0};
    lib_http_cfg_t cfg = {
        .timeout_ms  = 2000,
        .max_resp_sz = 16u
    };
    lib_http_ctx_t *ctx = lib_http_open(&cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    char url[256];
    snprintf(url, sizeof(url), "%s/big", g_srv.url_base);

    lib_http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;

    lib_http_res_t res;
    lib_http_err_t rc = lib_http_exec(ctx, &req, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_ERR_SIZE, rc);
    TEST_ASSERT_NULL(res.body);

    lib_http_close(ctx);
}

static void test_http_timeout(void)
{
    char errbuf[128] = {0};
    lib_http_ctx_t *ctx = lib_http_open(NULL, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    char url[256];
    snprintf(url, sizeof(url), "%s/slow", g_srv.url_base);

    lib_http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.timeout_ms = 80;

    lib_http_res_t res;
    lib_http_err_t rc = lib_http_exec(ctx, &req, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_ERR_TIMEOUT, rc);
    TEST_ASSERT_NULL(res.body);

    lib_http_close(ctx);
}

static void test_http_real_https_get(void)
{
    char errbuf[256] = {0};
    lib_http_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = 8000;
#ifdef LIBAGENT_TEST_CA_PATH
    cfg.ca_path = LIBAGENT_TEST_CA_PATH;
#endif

    lib_http_ctx_t *ctx = lib_http_open(&cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ctx);

    lib_http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = "https://httpbin.org/get";
    req.timeout_ms = 8000;

    lib_http_res_t res;
    lib_http_err_t rc = lib_http_exec(ctx, &req, &res, errbuf, sizeof(errbuf));

#if defined(MG_TLS) && (MG_TLS != MG_TLS_NONE)
    if (rc != LIB_HTTP_OK) {
        printf("  [warn] real HTTPS request failed (%s): %s\n",
               lib_http_strerror(rc), errbuf);
    }
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_OK, rc);
    TEST_ASSERT_TRUE(res.status_code >= 200 && res.status_code < 400);
    TEST_ASSERT_NOT_NULL(res.body);
    TEST_ASSERT_TRUE(res.body_len > 0u);
    lib_http_free_res(&res);
#else
    TEST_ASSERT_EQUAL_INT(LIB_HTTP_ERR_INIT, rc);
    TEST_ASSERT_NULL(res.body);
#endif

    lib_http_close(ctx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_http_invalid_args);
    RUN_TEST(test_http_post_json_and_headers);
    RUN_TEST(test_http_status_codes);
    RUN_TEST(test_http_max_resp_size);
    RUN_TEST(test_http_timeout);
    RUN_TEST(test_http_real_https_get);
    return UNITY_END();
}