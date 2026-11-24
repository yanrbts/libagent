/**
 * Copyright (c) 2026 yanrbts
 *
 * Production-grade synchronous HTTP client built on Mongoose.
 */
#include "lib_http.h"
#include "mongoose.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HTTP_DEFAULT_TIMEOUT_MS   10000
#define HTTP_DEFAULT_MAX_RESP_SZ  (16u * 1024u * 1024u)
#define HTTP_DEFAULT_MAX_HDR_CNT  64u
#define HTTP_DEFAULT_POLL_STEP_MS 20

struct lib_http_ctx {
    int    timeout_ms;
    size_t max_resp_sz;
    size_t max_hdr_cnt;
    int    poll_step_ms;
    const char *ca_path;    /**< Optional CA bundle path for TLS verification; NULL uses system/default */
    int    tls_skip_verify; /**< Non-zero to skip peer certificate verification (testing only) */
    char   last_err[256];
};

typedef struct http_run_state {
    const lib_http_ctx_t *ctx;
    const lib_http_req_t *req;
    lib_http_res_t       *out;

    struct mg_connection *conn;
    int64_t               deadline_ms;
    size_t                max_resp_sz;
    size_t                max_hdr_cnt;

    int                   req_sent;
    int                   msg_done;
    int                   failed;
    lib_http_err_t        err_code;
    char                  err_msg[256];
} http_run_state_t;

static void http_set_err_buf(char *errbuf, size_t errlen, const char *fmt, ...)
{
    if (errbuf == NULL || errlen == 0u) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static void http_fail(http_run_state_t *st, lib_http_err_t code, const char *fmt, ...)
{
    if (st == NULL) {
        return;
    }
    if (!st->failed) {
        st->failed = 1;
        st->err_code = code;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(st->err_msg, sizeof(st->err_msg), fmt, ap);
        va_end(ap);
    }
    st->msg_done = 1;
    if (st->conn != NULL) {
        st->conn->is_closing = 1;
    }
}

static void http_record_err(lib_http_ctx_t *ctx,
                            char *errbuf,
                            size_t errlen,
                            const char *msg)
{
    const char *s = (msg != NULL && msg[0] != '\0') ? msg : "unknown http error";
    if (ctx != NULL) {
        snprintf(ctx->last_err, sizeof(ctx->last_err), "%s", s);
    }
    http_set_err_buf(errbuf, errlen, "%s", s);
}

/* Reject CR/LF/NUL/control chars to prevent header/request-line injection. */
static int http_has_unsafe_ctl(const char *s, int allow_space_tab)
{
    if (s == NULL) {
        return 1;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            return 1;
        }
        if (*p < 0x20u || *p == 0x7fu) {
            if (allow_space_tab && (*p == ' ' || *p == '\t')) {
                continue;
            }
            return 1;
        }
    }
    return 0;
}

static int http_valid_method(const char *m)
{
    if (m == NULL || m[0] == '\0') {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)m; *p != '\0'; p++) {
        /* RFC 9110 token characters */
        if (isalnum(*p) || strchr("!#$%&'*+-.^_`|~", (int)*p) != NULL) {
            continue;
        }
        return 0;
    }
    return 1;
}

static int http_valid_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    if (http_has_unsafe_ctl(url, 0)) {
        return 0;
    }
    /* Mongoose client HTTP supports http:// and ws://; reject empty host */
    const char *scheme_end = strstr(url, "://");
    if (scheme_end == NULL) {
        return 0;
    }
    const char *host_start = scheme_end + 3u;
    if (*host_start == '\0' || *host_start == '/' || *host_start == '?' || *host_start == '#') {
        return 0;
    }
    return 1;
}

static char *http_strndup_safe(const char *src, size_t len)
{
    char *p = (char *)malloc(len + 1u);
    if (p == NULL) {
        return NULL;
    }
    if (len > 0u && src != NULL) {
        memcpy(p, src, len);
    }
    p[len] = '\0';
    return p;
}

static int http_store_body(http_run_state_t *st, const char *data, size_t len)
{
    if (len > st->max_resp_sz) {
        http_fail(st, LIB_HTTP_ERR_SIZE,
                  "response body (%zu bytes) exceeds max_resp_sz (%zu)",
                  len, st->max_resp_sz);
        return -1;
    }

    char *buf = (char *)malloc(len + 1u);
    if (buf == NULL) {
        http_fail(st, LIB_HTTP_ERR_NOMEM, "failed to allocate %zu bytes for response body",
                  len + 1u);
        return -1;
    }

    if (len > 0u && data != NULL) {
        memcpy(buf, data, len);
    }
    buf[len] = '\0';

    free(st->out->body);
    st->out->body = buf;
    st->out->body_len = len;
    return 0;
}

static int http_store_headers(http_run_state_t *st, const struct mg_http_message *hm)
{
    size_t cap_cnt = 0u;
    for (int i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
        if (hm->headers[i].name.len == 0u) {
            break;
        }
        cap_cnt++;
    }

    if (cap_cnt > st->max_hdr_cnt) {
        cap_cnt = st->max_hdr_cnt;
    }

    if (cap_cnt == 0u) {
        st->out->headers = NULL;
        st->out->header_num = 0u;
        return 0;
    }

    lib_http_header_t *arr =
        (lib_http_header_t *)calloc(cap_cnt, sizeof(lib_http_header_t));
    if (arr == NULL) {
        http_fail(st, LIB_HTTP_ERR_NOMEM, "failed to allocate response headers");
        return -1;
    }

    size_t stored = 0u;
    for (size_t i = 0; i < cap_cnt; i++) {
        const struct mg_str *hn = &hm->headers[i].name;
        const struct mg_str *hv = &hm->headers[i].value;
        char *k = http_strndup_safe(hn->buf, hn->len);
        char *v = http_strndup_safe(hv->buf, hv->len);
        if (k == NULL || v == NULL) {
            free(k);
            free(v);
            for (size_t j = 0; j < stored; j++) {
                free((void *)arr[j].name);
                free((void *)arr[j].value);
            }
            free(arr);
            http_fail(st, LIB_HTTP_ERR_NOMEM, "failed to duplicate response header");
            return -1;
        }
        arr[stored].name = k;
        arr[stored].value = v;
        stored++;
    }

    st->out->headers = arr;
    st->out->header_num = stored;
    return 0;
}

static void http_send_request(struct mg_connection *c, http_run_state_t *st)
{
    const lib_http_req_t *req = st->req;
    struct mg_str host = mg_url_host(req->url);
    const char *uri = mg_url_uri(req->url);

    if (uri == NULL || uri[0] == '\0') {
        uri = "/";
    }

    int has_host = 0;
    int has_conn = 0;
    int has_clen = 0;

    for (size_t i = 0; i < req->header_num; i++) {
        const char *hn = req->headers[i].name;
        if (strcasecmp(hn, "Host") == 0) {
            has_host = 1;
        } else if (strcasecmp(hn, "Connection") == 0) {
            has_conn = 1;
        } else if (strcasecmp(hn, "Content-Length") == 0) {
            has_clen = 1;
        }
    }

    mg_printf(c, "%s %s HTTP/1.1\r\n", req->method, uri);

    if (!has_host && host.len > 0u) {
        mg_printf(c, "Host: %.*s\r\n", (int)host.len, host.buf);
    }

    if (!has_conn) {
        mg_printf(c, "Connection: close\r\n");
    }

    for (size_t i = 0; i < req->header_num; i++) {
        mg_printf(c, "%s: %s\r\n", req->headers[i].name, req->headers[i].value);
    }

    if (!has_clen && (req->body_len > 0u || strcasecmp(req->method, "POST") == 0 ||
                      strcasecmp(req->method, "PUT") == 0 ||
                      strcasecmp(req->method, "PATCH") == 0)) {
        mg_printf(c, "Content-Length: %zu\r\n", req->body_len);
    }

    mg_printf(c, "\r\n");

    if (req->body_len > 0u && req->body != NULL) {
        mg_send(c, req->body, req->body_len);
    }

    st->req_sent = 1;
}

static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    http_run_state_t *st = (http_run_state_t *)c->fn_data;
    if (st == NULL) {
        return;
    }

    if (st->msg_done || st->failed) {
        c->is_closing = 1;
        return;
    }

    switch (ev) {
    case MG_EV_CONNECT: {
        /* In Mongoose, ev_data on MG_EV_CONNECT is non-NULL error string if connect failed */
        if (ev_data != NULL) {
            const char *err_s = (const char *)ev_data;
            http_fail(st, LIB_HTTP_ERR_CONNECT, "connect failed: %s",
                      err_s[0] != '\0' ? err_s : "socket error");
            return;
        }

        if (mg_url_is_ssl(st->req->url)) {
#if defined(MG_TLS) && (MG_TLS != MG_TLS_NONE)
            struct mg_str host = mg_url_host(st->req->url);
            char sni[256];
            if (host.len == 0u || host.len >= sizeof(sni)) {
                http_fail(st, LIB_HTTP_ERR_ARG, "invalid TLS host length");
                return;
            }
            memcpy(sni, host.buf, host.len);
            sni[host.len] = '\0';

            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.name = mg_str(sni);
            if (st->ctx->ca_path != NULL && st->ctx->ca_path[0] != '\0') {
                opts.ca = mg_str(st->ctx->ca_path);
            }
            /* Note: Mongoose TLS behavior depends on compiled TLS backend (OpenSSL/mbedTLS). */
            mg_tls_init(c, &opts);
#else
            http_fail(st, LIB_HTTP_ERR_INIT,
                      "HTTPS requested but lib_http was built without TLS backend");
            return;
#endif
        }

        http_send_request(c, st);
        break;
    }

    case MG_EV_READ: {
        if (c->recv.len > st->max_resp_sz + 8192u) {
            http_fail(st, LIB_HTTP_ERR_SIZE,
                      "incoming stream (%zu bytes) exceeds limit",
                      c->recv.len);
        }
        break;
    }

    case MG_EV_HTTP_MSG: {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (hm == NULL) {
            http_fail(st, LIB_HTTP_ERR_PROTO, "empty HTTP message");
            return;
        }

        if (hm->body.len > st->max_resp_sz) {
            http_fail(st, LIB_HTTP_ERR_SIZE,
                      "response body (%zu bytes) exceeds max_resp_sz (%zu)",
                      hm->body.len, st->max_resp_sz);
            return;
        }

        st->out->status_code = mg_http_status(hm);
        if (http_store_headers(st, hm) != 0) {
            return;
        }
        if (http_store_body(st, hm->body.buf, hm->body.len) != 0) {
            return;
        }

        st->msg_done = 1;
        c->is_closing = 1;
        break;
    }

    case MG_EV_ERROR: {
        const char *msg = (const char *)ev_data;
        if (!st->req_sent) {
            http_fail(st, LIB_HTTP_ERR_CONNECT, "network error before request sent: %s",
                      (msg && msg[0]) ? msg : "unknown");
        } else {
            http_fail(st, LIB_HTTP_ERR_RECV, "network error during HTTP exchange: %s",
                      (msg && msg[0]) ? msg : "unknown");
        }
        break;
    }

    case MG_EV_CLOSE: {
        st->conn = NULL;
        if (!st->msg_done && !st->failed) {
            if (!st->req_sent) {
                http_fail(st, LIB_HTTP_ERR_CONNECT, "connection closed before request sent");
            } else {
                http_fail(st, LIB_HTTP_ERR_RECV, "connection closed before full HTTP response");
            }
        }
        break;
    }

    default:
        break;
    }
}

lib_http_ctx_t *lib_http_open(const lib_http_cfg_t *cfg,
                              char *errbuf,
                              size_t errlen)
{
    lib_http_ctx_t *ctx = (lib_http_ctx_t *)calloc(1u, sizeof(*ctx));
    if (ctx == NULL) {
        http_set_err_buf(errbuf, errlen, "out of memory allocating lib_http_ctx");
        return NULL;
    }

    ctx->timeout_ms   = HTTP_DEFAULT_TIMEOUT_MS;
    ctx->max_resp_sz  = HTTP_DEFAULT_MAX_RESP_SZ;
    ctx->max_hdr_cnt  = HTTP_DEFAULT_MAX_HDR_CNT;
    ctx->poll_step_ms = HTTP_DEFAULT_POLL_STEP_MS;

    if (cfg != NULL) {
        if (cfg->timeout_ms > 0) {
            ctx->timeout_ms = cfg->timeout_ms;
        }
        if (cfg->max_resp_sz > 0u) {
            ctx->max_resp_sz = cfg->max_resp_sz;
        }
        if (cfg->max_hdr_cnt > 0u) {
            ctx->max_hdr_cnt = cfg->max_hdr_cnt;
        }
        if (cfg->poll_step_ms > 0) {
            ctx->poll_step_ms = cfg->poll_step_ms;
        }
        if (cfg->ca_path != NULL) {
            ctx->ca_path = strdup(cfg->ca_path);
            if (ctx->ca_path == NULL) {
                free(ctx);
                http_set_err_buf(errbuf, errlen, "out of memory duplicating ca_path");
                return NULL;
            }
        }
        ctx->tls_skip_verify = cfg->tls_skip_verify;
    }

    return ctx;
}

void lib_http_close(lib_http_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    free((void *)ctx->ca_path);
    free(ctx);
}

void lib_http_free_res(lib_http_res_t *res)
{
    if (res == NULL) {
        return;
    }
    free(res->body);
    res->body = NULL;
    res->body_len = 0u;

    if (res->headers != NULL) {
        for (size_t i = 0; i < res->header_num; i++) {
            free((void *)res->headers[i].name);
            free((void *)res->headers[i].value);
        }
        free(res->headers);
        res->headers = NULL;
    }
    res->header_num = 0u;
    res->status_code = 0;
}

lib_http_err_t lib_http_exec(lib_http_ctx_t *ctx,
                             const lib_http_req_t *req,
                             lib_http_res_t *out_res,
                             char *errbuf,
                             size_t errlen)
{
    if (lib_unlikely(ctx == NULL || req == NULL || out_res == NULL)) {
        http_record_err(ctx, errbuf, errlen, "ctx, req, and out_res must not be NULL");
        return LIB_HTTP_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    if (lib_unlikely(!http_valid_method(req->method))) {
        http_record_err(ctx, errbuf, errlen, "invalid or unsafe HTTP method");
        return LIB_HTTP_ERR_ARG;
    }

    if (lib_unlikely(!http_valid_url(req->url))) {
        http_record_err(ctx, errbuf, errlen, "invalid or unsafe URL");
        return LIB_HTTP_ERR_ARG;
    }

    if (lib_unlikely(req->header_num > 0u && req->headers == NULL)) {
        http_record_err(ctx, errbuf, errlen, "headers is NULL while header_num > 0");
        return LIB_HTTP_ERR_ARG;
    }

    if (lib_unlikely(req->body_len > 0u && req->body == NULL)) {
        http_record_err(ctx, errbuf, errlen, "body is NULL while body_len > 0");
        return LIB_HTTP_ERR_ARG;
    }

    for (size_t i = 0; i < req->header_num; i++) {
        const char *hn = req->headers[i].name;
        const char *hv = req->headers[i].value;
        if (lib_unlikely(hn == NULL || hn[0] == '\0' || hv == NULL)) {
            http_record_err(ctx, errbuf, errlen, "header name/value must not be NULL or empty");
            return LIB_HTTP_ERR_ARG;
        }
        if (lib_unlikely(http_has_unsafe_ctl(hn, 0) || http_has_unsafe_ctl(hv, 1))) {
            http_record_err(ctx, errbuf, errlen, "header name or value contains illegal control chars");
            return LIB_HTTP_ERR_ARG;
        }
    }

    int eff_timeout = (req->timeout_ms > 0) ? req->timeout_ms : ctx->timeout_ms;
    if (eff_timeout <= 0) {
        eff_timeout = HTTP_DEFAULT_TIMEOUT_MS;
    }

    http_run_state_t st;
    memset(&st, 0, sizeof(st));
    st.ctx         = ctx;
    st.req         = req;
    st.out         = out_res;
    st.max_resp_sz = ctx->max_resp_sz;
    st.max_hdr_cnt = ctx->max_hdr_cnt;
    st.deadline_ms = lib_mono_ms() + (int64_t)eff_timeout;
    st.err_code    = LIB_HTTP_OK;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    st.conn = mg_http_connect(&mgr, req->url, http_ev_handler, &st);
    if (st.conn == NULL) {
        mg_mgr_free(&mgr);
        lib_http_free_res(out_res);
        http_record_err(ctx, errbuf, errlen, "mg_http_connect failed");
        return LIB_HTTP_ERR_CONNECT;
    }

    while (!st.msg_done && !st.failed) {
        int64_t now_ms = lib_mono_ms();
        int64_t rem_ms = st.deadline_ms - now_ms;
        if (rem_ms <= 0) {
            http_fail(&st, LIB_HTTP_ERR_TIMEOUT, "HTTP request timed out after %d ms",
                      eff_timeout);
            /* Poll once briefly so Mongoose can close socket cleanly */
            mg_mgr_poll(&mgr, 1);
            break;
        }

        int step_ms = ctx->poll_step_ms;
        if (rem_ms < (int64_t)step_ms) {
            step_ms = (int)rem_ms;
        }
        if (step_ms < 1) {
            step_ms = 1;
        }
        mg_mgr_poll(&mgr, step_ms);
    }

    mg_mgr_free(&mgr);

    if (st.failed) {
        lib_http_free_res(out_res);
        http_record_err(ctx, errbuf, errlen, st.err_msg);
        return st.err_code;
    }

    if (!st.msg_done) {
        lib_http_free_res(out_res);
        http_record_err(ctx, errbuf, errlen, "HTTP request ended without complete response");
        return LIB_HTTP_ERR_RECV;
    }

    return LIB_HTTP_OK;
}

lib_http_err_t lib_http_request_json(lib_http_ctx_t *ctx,
                                     const char *method,
                                     const char *url,
                                     const char *bearer_tok,
                                     const char *json_body,
                                     int timeout_ms,
                                     lib_http_res_t *out_res,
                                     char *errbuf,
                                     size_t errlen)
{
    if (lib_unlikely(json_body == NULL)) {
        http_record_err(ctx, errbuf, errlen, "json_body must not be NULL");
        return LIB_HTTP_ERR_ARG;
    }

    lib_http_header_t hdrs[3];
    size_t hcnt = 0u;

    hdrs[hcnt].name = "Content-Type";
    hdrs[hcnt].value = "application/json";
    hcnt++;

    hdrs[hcnt].name = "Accept";
    hdrs[hcnt].value = "application/json";
    hcnt++;

    char auth_buf[1024];
    if (bearer_tok != NULL && bearer_tok[0] != '\0') {
        if (http_has_unsafe_ctl(bearer_tok, 0)) {
            http_record_err(ctx, errbuf, errlen, "bearer token contains illegal control chars");
            return LIB_HTTP_ERR_ARG;
        }
        int n = 0;
        if (strncasecmp(bearer_tok, "Bearer ", 7) == 0) {
            n = snprintf(auth_buf, sizeof(auth_buf), "%s", bearer_tok);
        } else {
            n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", bearer_tok);
        }
        if (lib_unlikely(n < 0 || (size_t)n >= sizeof(auth_buf))) {
            http_record_err(ctx, errbuf, errlen, "bearer token exceeds header buffer");
            return LIB_HTTP_ERR_ARG;
        }
        hdrs[hcnt].name = "Authorization";
        hdrs[hcnt].value = auth_buf;
        hcnt++;
    }

    const char *eff_method = (method != NULL && method[0] != '\0') ? method : "POST";

    lib_http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method     = eff_method;
    req.url        = url;
    req.headers    = hdrs;
    req.header_num = hcnt;
    req.body       = json_body;
    req.body_len   = strlen(json_body);
    req.timeout_ms = timeout_ms;

    return lib_http_exec(ctx, &req, out_res, errbuf, errlen);
}

lib_http_err_t lib_http_post_json(lib_http_ctx_t *ctx,
                                    const char *url,
                                    const char *bearer_tok,
                                    const char *json_body,
                                    int timeout_ms,
                                    lib_http_res_t *out_res,
                                    char *errbuf,
                                    size_t errlen)
{
    return lib_http_request_json(ctx,
                                 "POST",
                                 url,
                                 bearer_tok,
                                 json_body,
                                 timeout_ms,
                                 out_res,
                                 errbuf,
                                 errlen);
}

const char *lib_http_strerror(lib_http_err_t err)
{
    switch (err) {
    case LIB_HTTP_OK:          return "ok";
    case LIB_HTTP_ERR_ARG:     return "invalid argument";
    case LIB_HTTP_ERR_INIT:    return "initialization failed";
    case LIB_HTTP_ERR_CONNECT: return "connection failed";
    case LIB_HTTP_ERR_SEND:    return "send failed";
    case LIB_HTTP_ERR_RECV:    return "receive failed";
    case LIB_HTTP_ERR_TIMEOUT: return "request timed out";
    case LIB_HTTP_ERR_SIZE:    return "response size exceeded limit";
    case LIB_HTTP_ERR_PROTO:   return "HTTP protocol error";
    case LIB_HTTP_ERR_NOMEM:   return "out of memory";
    default:                   return "unknown error";
    }
}