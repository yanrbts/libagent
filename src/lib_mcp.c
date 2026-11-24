/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */

#include "lib_mcp.h"
#include "lib_buf.h"
#include "lib_io.h"
#include "util.h"

#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LIB_MCP_DEF_TIMEOUT_MS 10000
#define LIB_MCP_DEF_MAX_LINE   (16u * 1024u * 1024u)
#define LIB_MCP_PROTO_VER      "2025-06-18"
#define LIB_MCP_CLIENT_NAME    "lib_mcp_c_client"
#define LIB_MCP_CLIENT_VER     "1.0.0"

#define MCP_JSONRPC_VERSION    "2.0"
#define MCP_KEY_JSONRPC        "jsonrpc"
#define MCP_KEY_ID             "id"
#define MCP_KEY_METHOD         "method"
#define MCP_KEY_PARAMS         "params"
#define MCP_KEY_RESULT         "result"
#define MCP_KEY_ERROR          "error"
#define MCP_KEY_CODE           "code"
#define MCP_KEY_MESSAGE        "message"
#define MCP_KEY_TOOLS          "tools"
#define MCP_KEY_NAME           "name"
#define MCP_KEY_DESCRIPTION    "description"
#define MCP_KEY_INPUT_SCHEMA   "inputSchema"
#define MCP_KEY_ARGUMENTS      "arguments"
#define MCP_KEY_IS_ERROR       "isError"
#define MCP_KEY_CONTENT        "content"
#define MCP_KEY_CONTENTS       "contents"
#define MCP_KEY_TYPE           "type"
#define MCP_KEY_TEXT           "text"
#define MCP_KEY_BLOB           "blob"
#define MCP_KEY_URI            "uri"
#define MCP_KEY_MIME_TYPE      "mimeType"
#define MCP_KEY_RESOURCES      "resources"
#define MCP_KEY_PROMPTS        "prompts"
#define MCP_KEY_REQUIRED       "required"
#define MCP_KEY_MESSAGES       "messages"
#define MCP_KEY_ROLE           "role"
#define MCP_VAL_TYPE_TEXT      "text"

typedef enum lib_mcp_method_id {
    LIB_MCP_METH_INITIALIZE = 0,
    LIB_MCP_METH_NOTIF_INITIALIZED,
    LIB_MCP_METH_TOOLS_LIST,
    LIB_MCP_METH_TOOLS_CALL,
    LIB_MCP_METH_RESOURCES_LIST,
    LIB_MCP_METH_RESOURCES_READ,
    LIB_MCP_METH_PROMPTS_LIST,
    LIB_MCP_METH_PROMPTS_GET,
    LIB_MCP_METH__COUNT
} lib_mcp_method_id_t;

static const char *const g_mcp_methods[LIB_MCP_METH__COUNT] = {
    [LIB_MCP_METH_INITIALIZE]          = "initialize",
    [LIB_MCP_METH_NOTIF_INITIALIZED]   = "notifications/initialized",
    [LIB_MCP_METH_TOOLS_LIST]          = "tools/list",
    [LIB_MCP_METH_TOOLS_CALL]          = "tools/call",
    [LIB_MCP_METH_RESOURCES_LIST]      = "resources/list",
    [LIB_MCP_METH_RESOURCES_READ]      = "resources/read",
    [LIB_MCP_METH_PROMPTS_LIST]        = "prompts/list",
    [LIB_MCP_METH_PROMPTS_GET]         = "prompts/get",
};

_Static_assert(sizeof(g_mcp_methods) / sizeof(g_mcp_methods[0]) == LIB_MCP_METH__COUNT,
               "g_mcp_methods count mismatch");

struct lib_mcp_ctx {
    pid_t     pid;           /* Spawned MCP server child process ID */
    int       in_fd;         /* Write end connected to child stdin */
    int       out_fd;        /* Read end connected to child stdout */
    int       timeout_ms;    /* Default per-request timeout in milliseconds */
    size_t    max_line_sz;   /* Maximum single JSON-RPC frame size in bytes */
    int64_t   next_id;       /* Monotonically increasing JSON-RPC request ID */
    lib_buf_t rbuf;          /* Receive frame buffer for child stdout */
    char      last_err[256]; /* Last diagnostic error message */
};

/* Resolves an internal method identifier to its wire-level JSON-RPC method string. */
static const char *lib_mcp_method_name(lib_mcp_method_id_t method_id)
{
    if (method_id < 0 || method_id >= LIB_MCP_METH__COUNT) {
        return "";
    }
    return g_mcp_methods[method_id];
}

/* Formats and stores a diagnostic message into ctx and optional errbuf. */
static void lib_mcp_set_err(lib_mcp_ctx_t *ctx, char *errbuf, size_t errlen,
                            const char *fmt, ...)
{
    char tmp[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (ctx != NULL) {
        snprintf(ctx->last_err, sizeof(ctx->last_err), "%s", tmp);
    }
    if (errbuf != NULL && errlen > 0u) {
        snprintf(errbuf, errlen, "%s", tmp);
    }
}

/* Maps lib_io_status_t and errno into lib_mcp_err_t with diagnostics. */
static lib_mcp_err_t lib_mcp_map_io_status(lib_mcp_ctx_t *ctx,
                                           lib_io_status_t io_rc,
                                           const char *op,
                                           char *errbuf,
                                           size_t errlen)
{
    int saved_errno = errno;

    if (lib_likely(io_rc == LIB_IO_OK)) {
        return LIB_MCP_OK;
    }
    if (io_rc == LIB_IO_TIMEOUT) {
        lib_mcp_set_err(ctx, errbuf, errlen, "%s timed out", op);
        return LIB_MCP_ERR_TIMEOUT;
    }
    if (io_rc == LIB_IO_EOF) {
        lib_mcp_set_err(ctx, errbuf, errlen, "%s: peer closed connection", op);
        return LIB_MCP_ERR_CLOSED;
    }
    if (io_rc == LIB_IO_OVERFLOW) {
        lib_mcp_set_err(ctx, errbuf, errlen, "%s: frame exceeds max_line_sz", op);
        return LIB_MCP_ERR_PROTO;
    }
    if (saved_errno == ENOMEM) {
        lib_mcp_set_err(ctx, errbuf, errlen, "%s: out of memory", op);
        return LIB_MCP_ERR_MEM;
    }

    lib_mcp_set_err(ctx, errbuf, errlen, "%s failed: %s", op, strerror(saved_errno));
    return LIB_MCP_ERR_IO;
}

/* Closes both ends of a pipe pair safely. */
static void lib_mcp_close_pipe(int pfd[2])
{
    if (pfd == NULL) {
        return;
    }
    lib_close_fd(&pfd[0]);
    lib_close_fd(&pfd[1]);
}

/* Computes a monotonic deadline in milliseconds from a timeout_ms value. */
static int64_t lib_mcp_make_deadline(int timeout_ms)
{
    if (timeout_ms <= 0) {
        return 0;
    }
    int64_t now = lib_mono_ms();
    if (lib_unlikely(now <= 0)) {
        return 0;
    }
    if (now > INT64_MAX - (int64_t)timeout_ms) {
        return INT64_MAX;
    }
    return now + (int64_t)timeout_ms;
}

/* Serializes and writes a single newline-delimited JSON-RPC frame. */
static lib_mcp_err_t lib_mcp_send_msg(lib_mcp_ctx_t *ctx,
                                      const cJSON *msg,
                                      int timeout_ms,
                                      char *errbuf,
                                      size_t errlen)
{
    if (lib_unlikely(ctx == NULL || msg == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to send_msg");
        return LIB_MCP_ERR_ARG;
    }

    char *raw = cJSON_PrintUnformatted(msg);
    if (lib_unlikely(raw == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "failed to serialize JSON-RPC message");
        return LIB_MCP_ERR_MEM;
    }

    size_t raw_len = strlen(raw);
    if (lib_unlikely(raw_len >= SIZE_MAX || raw_len + 1u > ctx->max_line_sz)) {
        free(raw);
        lib_mcp_set_err(ctx, errbuf, errlen, "outgoing frame exceeds max_line_sz");
        return LIB_MCP_ERR_PROTO;
    }

    lib_buf_t wbuf;
    if (lib_unlikely(lib_buf_init(&wbuf, raw_len + 2u, ctx->max_line_sz) != LIB_BUF_OK)) {
        free(raw);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory initializing write buffer");
        return LIB_MCP_ERR_MEM;
    }

    memcpy(wbuf.data, raw, raw_len);
    wbuf.data[raw_len] = '\n';
    wbuf.len = raw_len + 1u;
    free(raw);

    int64_t deadline = lib_mcp_make_deadline(timeout_ms);
    lib_io_status_t wrc = lib_io_write_all(ctx->in_fd, wbuf.data, wbuf.len, deadline);
    lib_buf_free(&wbuf);

    return lib_mcp_map_io_status(ctx, wrc, "write", errbuf, errlen);
}

/* Sends a JSON-RPC request and waits for the matching response object. */
static lib_mcp_err_t lib_mcp_rpc_req(lib_mcp_ctx_t *ctx,
                                     lib_mcp_method_id_t method_id,
                                     cJSON *params,
                                     int timeout_ms,
                                     cJSON **out_result,
                                     char *errbuf,
                                     size_t errlen)
{
    *out_result = NULL;

    const char *method = lib_mcp_method_name(method_id);
    if (lib_unlikely(method[0] == '\0')) {
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid MCP method identifier");
        return LIB_MCP_ERR_ARG;
    }

    int eff_timeout = (timeout_ms > 0) ? timeout_ms : ctx->timeout_ms;
    int64_t deadline = lib_mcp_make_deadline(eff_timeout);
    int64_t req_id = ctx->next_id++;

    cJSON *req = cJSON_CreateObject();
    if (lib_unlikely(req == NULL)) {
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating JSON-RPC request");
        return LIB_MCP_ERR_MEM;
    }

    if (!cJSON_AddStringToObject(req, MCP_KEY_JSONRPC, MCP_JSONRPC_VERSION) ||
        !cJSON_AddNumberToObject(req, MCP_KEY_ID, (double)req_id) ||
        !cJSON_AddStringToObject(req, MCP_KEY_METHOD, method)) {
        cJSON_Delete(params);
        cJSON_Delete(req);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory populating JSON-RPC request");
        return LIB_MCP_ERR_MEM;
    }

    if (params != NULL) {
        cJSON_AddItemToObject(req, MCP_KEY_PARAMS, params);
    }

    lib_mcp_err_t src = lib_mcp_send_msg(ctx, req, eff_timeout, errbuf, errlen);
    cJSON_Delete(req);
    if (lib_unlikely(src != LIB_MCP_OK)) {
        return src;
    }

    for (;;) {
        char *line = NULL;
        lib_io_status_t rrc = lib_io_read_line(ctx->out_fd, &ctx->rbuf, &line, deadline);
        if (lib_unlikely(rrc != LIB_IO_OK)) {
            free(line);
            return lib_mcp_map_io_status(ctx, rrc, "read", errbuf, errlen);
        }

        if (line[0] == '\0') {
            free(line);
            continue;
        }

        cJSON *doc = cJSON_Parse(line);
        free(line);
        if (lib_unlikely(doc == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "invalid JSON frame from MCP server");
            return LIB_MCP_ERR_PROTO;
        }

        if (lib_unlikely(!cJSON_IsObject(doc))) {
            cJSON_Delete(doc);
            lib_mcp_set_err(ctx, errbuf, errlen, "JSON-RPC message is not an object");
            return LIB_MCP_ERR_PROTO;
        }

        const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(doc, MCP_KEY_ID);
        if (!cJSON_IsNumber(id_item) || (int64_t)id_item->valuedouble != req_id) {
            cJSON_Delete(doc);
            continue;
        }

        const cJSON *err_item = cJSON_GetObjectItemCaseSensitive(doc, MCP_KEY_ERROR);
        if (cJSON_IsObject(err_item)) {
            const cJSON *emsg = cJSON_GetObjectItemCaseSensitive(err_item, MCP_KEY_MESSAGE);
            const cJSON *ecode = cJSON_GetObjectItemCaseSensitive(err_item, MCP_KEY_CODE);
            lib_mcp_set_err(ctx, errbuf, errlen, "RPC error (%d): %s",
                            cJSON_IsNumber(ecode) ? ecode->valueint : -1,
                            (cJSON_IsString(emsg) && emsg->valuestring != NULL)
                                ? emsg->valuestring
                                : "unknown");
            cJSON_Delete(doc);
            return LIB_MCP_ERR_RPC;
        }

        const cJSON *res_item = cJSON_GetObjectItemCaseSensitive(doc, MCP_KEY_RESULT);
        if (lib_unlikely(res_item == NULL)) {
            cJSON_Delete(doc);
            lib_mcp_set_err(ctx, errbuf, errlen, "JSON-RPC response missing result");
            return LIB_MCP_ERR_PROTO;
        }

        *out_result = cJSON_Duplicate(res_item, 1);
        cJSON_Delete(doc);
        if (lib_unlikely(*out_result == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory cloning RPC result");
            return LIB_MCP_ERR_MEM;
        }
        return LIB_MCP_OK;
    }
}

/* Executes the MCP initialize handshake and emits notifications/initialized. */
static lib_mcp_err_t lib_mcp_handshake(lib_mcp_ctx_t *ctx, char *errbuf, size_t errlen)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *caps = cJSON_CreateObject();
    cJSON *info = cJSON_CreateObject();
    if (lib_unlikely(params == NULL || caps == NULL || info == NULL)) {
        cJSON_Delete(params);
        cJSON_Delete(caps);
        cJSON_Delete(info);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory building initialize params");
        return LIB_MCP_ERR_MEM;
    }

    cJSON_AddStringToObject(info, MCP_KEY_NAME, LIB_MCP_CLIENT_NAME);
    cJSON_AddStringToObject(info, "version", LIB_MCP_CLIENT_VER);
    cJSON_AddStringToObject(params, "protocolVersion", LIB_MCP_PROTO_VER);
    cJSON_AddItemToObject(params, "capabilities", caps);
    cJSON_AddItemToObject(params, "clientInfo", info);

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_INITIALIZE, params, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    cJSON_Delete(res);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    cJSON *notif = cJSON_CreateObject();
    if (lib_unlikely(notif == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory building initialized notification");
        return LIB_MCP_ERR_MEM;
    }
    cJSON_AddStringToObject(notif, MCP_KEY_JSONRPC, MCP_JSONRPC_VERSION);
    cJSON_AddStringToObject(notif, MCP_KEY_METHOD,
                            lib_mcp_method_name(LIB_MCP_METH_NOTIF_INITIALIZED));

    rc = lib_mcp_send_msg(ctx, notif, ctx->timeout_ms, errbuf, errlen);
    cJSON_Delete(notif);
    return rc;
}

/* Parses optional args_json string into a cJSON object. */
static lib_mcp_err_t lib_mcp_parse_args_obj(lib_mcp_ctx_t *ctx,
                                            const char *args_json,
                                            cJSON **out_obj,
                                            char *errbuf,
                                            size_t errlen)
{
    *out_obj = NULL;

    if (args_json != NULL && args_json[0] != '\0') {
        cJSON *obj = cJSON_Parse(args_json);
        if (lib_unlikely(obj == NULL || !cJSON_IsObject(obj))) {
            cJSON_Delete(obj);
            lib_mcp_set_err(ctx, errbuf, errlen, "args_json must parse to a JSON object");
            return LIB_MCP_ERR_ARG;
        }
        *out_obj = obj;
        return LIB_MCP_OK;
    }

    cJSON *obj = cJSON_CreateObject();
    if (lib_unlikely(obj == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating arguments object");
        return LIB_MCP_ERR_MEM;
    }
    *out_obj = obj;
    return LIB_MCP_OK;
}

/* Appends a text slice into tbuf, separating multiple slices with '\n'. */
static lib_buf_status_t lib_mcp_append_text(lib_buf_t *tbuf,
                                            const char *src,
                                            size_t src_len,
                                            int need_nl)
{
    size_t nl_len = (need_nl != 0) ? 1u : 0u;
    if (src_len > SIZE_MAX - nl_len - 1u || tbuf->len > SIZE_MAX - src_len - nl_len - 1u) {
        errno = EOVERFLOW;
        return LIB_BUF_ERR;
    }

    size_t want_cap = tbuf->len + nl_len + src_len + 1u;
    lib_buf_status_t rrc = lib_buf_reserve(tbuf, want_cap);
    if (lib_unlikely(rrc != LIB_BUF_OK)) {
        return rrc;
    }

    if (need_nl != 0) {
        tbuf->data[tbuf->len++] = '\n';
    }
    if (src_len > 0u) {
        memcpy(tbuf->data + tbuf->len, src, src_len);
        tbuf->len += src_len;
    }
    tbuf->data[tbuf->len] = '\0';
    return LIB_BUF_OK;
}

/* Finalizes tbuf into a heap-allocated NUL-terminated string. */
static lib_mcp_err_t lib_mcp_finalize_text(lib_mcp_ctx_t *ctx,
                                           lib_buf_t *tbuf,
                                           int has_tbuf,
                                           char **out_text,
                                           char *errbuf,
                                           size_t errlen)
{
    if (!has_tbuf) {
        return LIB_MCP_OK;
    }
    if (tbuf->data == NULL) {
        char *empty = strdup("");
        if (lib_unlikely(empty == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating empty text");
            return LIB_MCP_ERR_MEM;
        }
        *out_text = empty;
        return LIB_MCP_OK;
    }
    *out_text = tbuf->data;
    tbuf->data = NULL;
    lib_buf_free(tbuf);
    return LIB_MCP_OK;
}

/* Aggregates all text blocks from tools/call result->content into a single string. */
static lib_mcp_err_t lib_mcp_collect_tool_text(lib_mcp_ctx_t *ctx,
                                               const cJSON *res,
                                               char **out_text,
                                               char *errbuf,
                                               size_t errlen)
{
    *out_text = NULL;

    const cJSON *content = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_CONTENT);
    if (!cJSON_IsArray(content)) {
        return LIB_MCP_OK;
    }

    lib_buf_t tbuf;
    memset(&tbuf, 0, sizeof(tbuf));
    int has_tbuf = 0;

    const cJSON *blk = NULL;
    cJSON_ArrayForEach(blk, content) {
        const cJSON *btype = cJSON_GetObjectItemCaseSensitive(blk, MCP_KEY_TYPE);
        const cJSON *btext = cJSON_GetObjectItemCaseSensitive(blk, MCP_KEY_TEXT);
        if (!cJSON_IsString(btype) || btype->valuestring == NULL ||
            strcmp(btype->valuestring, MCP_VAL_TYPE_TEXT) != 0 ||
            !cJSON_IsString(btext) || btext->valuestring == NULL) {
            continue;
        }

        if (!has_tbuf) {
            if (lib_unlikely(lib_buf_init(&tbuf, 256u, ctx->max_line_sz) != LIB_BUF_OK)) {
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating text buffer");
                return LIB_MCP_ERR_MEM;
            }
            has_tbuf = 1;
        }

        lib_buf_status_t arc = lib_mcp_append_text(&tbuf,
                                                   btext->valuestring,
                                                   strlen(btext->valuestring),
                                                   (tbuf.len > 0u) ? 1 : 0);
        if (lib_unlikely(arc != LIB_BUF_OK)) {
            lib_buf_free(&tbuf);
            lib_mcp_set_err(ctx, errbuf, errlen,
                            (arc == LIB_BUF_OVERFLOW) ? "tool text exceeds max_line_sz"
                                                       : "out of memory building tool text");
            return (arc == LIB_BUF_OVERFLOW) ? LIB_MCP_ERR_PROTO : LIB_MCP_ERR_MEM;
        }
    }

    return lib_mcp_finalize_text(ctx, &tbuf, has_tbuf, out_text, errbuf, errlen);
}

/* Aggregates text (or blob fallback) entries from resources/read result->contents. */
static lib_mcp_err_t lib_mcp_collect_resource_text(lib_mcp_ctx_t *ctx,
                                                   const cJSON *res,
                                                   char **out_text,
                                                   char *errbuf,
                                                   size_t errlen)
{
    *out_text = NULL;

    const cJSON *contents = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_CONTENTS);
    if (!cJSON_IsArray(contents)) {
        return LIB_MCP_OK;
    }

    lib_buf_t tbuf;
    memset(&tbuf, 0, sizeof(tbuf));
    int has_tbuf = 0;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, contents) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_TEXT);
        const cJSON *blob = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_BLOB);
        const char *val = NULL;

        if (cJSON_IsString(text) && text->valuestring != NULL) {
            val = text->valuestring;
        } else if (cJSON_IsString(blob) && blob->valuestring != NULL) {
            val = blob->valuestring;
        } else {
            continue;
        }

        if (!has_tbuf) {
            if (lib_unlikely(lib_buf_init(&tbuf, 256u, ctx->max_line_sz) != LIB_BUF_OK)) {
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating resource buffer");
                return LIB_MCP_ERR_MEM;
            }
            has_tbuf = 1;
        }

        lib_buf_status_t arc = lib_mcp_append_text(&tbuf, val, strlen(val),
                                                   (tbuf.len > 0u) ? 1 : 0);
        if (lib_unlikely(arc != LIB_BUF_OK)) {
            lib_buf_free(&tbuf);
            lib_mcp_set_err(ctx, errbuf, errlen,
                            (arc == LIB_BUF_OVERFLOW) ? "resource content exceeds max_line_sz"
                                                       : "out of memory building resource text");
            return (arc == LIB_BUF_OVERFLOW) ? LIB_MCP_ERR_PROTO : LIB_MCP_ERR_MEM;
        }
    }

    return lib_mcp_finalize_text(ctx, &tbuf, has_tbuf, out_text, errbuf, errlen);
}

/* Extracts text blocks from a single prompt message object into tbuf. */
static lib_mcp_err_t lib_mcp_append_prompt_msg(lib_mcp_ctx_t *ctx,
                                               lib_buf_t *tbuf,
                                               int *has_tbuf,
                                               const cJSON *msg,
                                               char *errbuf,
                                               size_t errlen)
{
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, MCP_KEY_CONTENT);
    if (!cJSON_IsObject(content) && !cJSON_IsArray(content)) {
        return LIB_MCP_OK;
    }

    const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, MCP_KEY_ROLE);
    const char *role_str = (cJSON_IsString(role) && role->valuestring != NULL)
                               ? role->valuestring
                               : NULL;

    if (cJSON_IsObject(content)) {
        const cJSON *ctype = cJSON_GetObjectItemCaseSensitive(content, MCP_KEY_TYPE);
        const cJSON *ctext = cJSON_GetObjectItemCaseSensitive(content, MCP_KEY_TEXT);
        if (cJSON_IsString(ctype) && ctype->valuestring != NULL &&
            strcmp(ctype->valuestring, MCP_VAL_TYPE_TEXT) == 0 &&
            cJSON_IsString(ctext) && ctext->valuestring != NULL) {
            if (!(*has_tbuf)) {
                if (lib_unlikely(lib_buf_init(tbuf, 256u, ctx->max_line_sz) != LIB_BUF_OK)) {
                    lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating prompt buffer");
                    return LIB_MCP_ERR_MEM;
                }
                *has_tbuf = 1;
            }
            if (role_str != NULL) {
                size_t rlen = strlen(role_str);
                if (lib_unlikely(lib_mcp_append_text(tbuf, "[", 1u, (tbuf->len > 0u) ? 1 : 0) != LIB_BUF_OK ||
                                 lib_mcp_append_text(tbuf, role_str, rlen, 0) != LIB_BUF_OK ||
                                 lib_mcp_append_text(tbuf, "]: ", 3u, 0) != LIB_BUF_OK)) {
                    lib_buf_free(tbuf);
                    lib_mcp_set_err(ctx, errbuf, errlen, "failed formatting prompt role");
                    return LIB_MCP_ERR_MEM;
                }
            }
            lib_buf_status_t arc = lib_mcp_append_text(tbuf, ctext->valuestring,
                                                       strlen(ctext->valuestring),
                                                       (role_str == NULL && tbuf->len > 0u) ? 1 : 0);
            if (lib_unlikely(arc != LIB_BUF_OK)) {
                lib_buf_free(tbuf);
                lib_mcp_set_err(ctx, errbuf, errlen,
                                (arc == LIB_BUF_OVERFLOW) ? "prompt text exceeds max_line_sz"
                                                           : "out of memory building prompt text");
                return (arc == LIB_BUF_OVERFLOW) ? LIB_MCP_ERR_PROTO : LIB_MCP_ERR_MEM;
            }
        }
        return LIB_MCP_OK;
    }

    const cJSON *blk = NULL;
    cJSON_ArrayForEach(blk, content) {
        const cJSON *btype = cJSON_GetObjectItemCaseSensitive(blk, MCP_KEY_TYPE);
        const cJSON *btext = cJSON_GetObjectItemCaseSensitive(blk, MCP_KEY_TEXT);
        if (!cJSON_IsString(btype) || btype->valuestring == NULL ||
            strcmp(btype->valuestring, MCP_VAL_TYPE_TEXT) != 0 ||
            !cJSON_IsString(btext) || btext->valuestring == NULL) {
            continue;
        }

        if (!(*has_tbuf)) {
            if (lib_unlikely(lib_buf_init(tbuf, 256u, ctx->max_line_sz) != LIB_BUF_OK)) {
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating prompt buffer");
                return LIB_MCP_ERR_MEM;
            }
            *has_tbuf = 1;
        }

        if (role_str != NULL) {
            size_t rlen = strlen(role_str);
            if (lib_unlikely(lib_mcp_append_text(tbuf, "[", 1u, (tbuf->len > 0u) ? 1 : 0) != LIB_BUF_OK ||
                             lib_mcp_append_text(tbuf, role_str, rlen, 0) != LIB_BUF_OK ||
                             lib_mcp_append_text(tbuf, "]: ", 3u, 0) != LIB_BUF_OK)) {
                lib_buf_free(tbuf);
                lib_mcp_set_err(ctx, errbuf, errlen, "failed formatting prompt role");
                return LIB_MCP_ERR_MEM;
            }
        }

        lib_buf_status_t arc = lib_mcp_append_text(tbuf, btext->valuestring,
                                                   strlen(btext->valuestring),
                                                   (role_str == NULL && tbuf->len > 0u) ? 1 : 0);
        if (lib_unlikely(arc != LIB_BUF_OK)) {
            lib_buf_free(tbuf);
            lib_mcp_set_err(ctx, errbuf, errlen,
                            (arc == LIB_BUF_OVERFLOW) ? "prompt text exceeds max_line_sz"
                                                       : "out of memory building prompt text");
            return (arc == LIB_BUF_OVERFLOW) ? LIB_MCP_ERR_PROTO : LIB_MCP_ERR_MEM;
        }
    }

    return LIB_MCP_OK;
}

/* Aggregates message text from prompts/get result->messages into a single string. */
static lib_mcp_err_t lib_mcp_collect_prompt_text(lib_mcp_ctx_t *ctx,
                                                 const cJSON *res,
                                                 char **out_text,
                                                 char *errbuf,
                                                 size_t errlen)
{
    *out_text = NULL;

    const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_MESSAGES);
    if (!cJSON_IsArray(msgs)) {
        return LIB_MCP_OK;
    }

    lib_buf_t tbuf;
    memset(&tbuf, 0, sizeof(tbuf));
    int has_tbuf = 0;

    const cJSON *msg = NULL;
    cJSON_ArrayForEach(msg, msgs) {
        lib_mcp_err_t rc = lib_mcp_append_prompt_msg(ctx, &tbuf, &has_tbuf, msg, errbuf, errlen);
        if (lib_unlikely(rc != LIB_MCP_OK)) {
            return rc;
        }
    }

    return lib_mcp_finalize_text(ctx, &tbuf, has_tbuf, out_text, errbuf, errlen);
}

lib_mcp_ctx_t *lib_mcp_open(const lib_mcp_cfg_t *cfg, char *errbuf, size_t errlen)
{
    if (lib_unlikely(cfg == NULL || cfg->cmd == NULL || cfg->cmd[0] == '\0' ||
                     cfg->argv == NULL || cfg->argv[0] == NULL)) {
        lib_mcp_set_err(NULL, errbuf, errlen, "invalid lib_mcp_cfg_t");
        return NULL;
    }

    (void)signal(SIGPIPE, SIG_IGN);

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (lib_unlikely(pipe(to_child) != 0)) {
        lib_mcp_set_err(NULL, errbuf, errlen, "pipe(to_child) failed: %s", strerror(errno));
        return NULL;
    }
    if (lib_unlikely(pipe(from_child) != 0)) {
        lib_mcp_set_err(NULL, errbuf, errlen, "pipe(from_child) failed: %s", strerror(errno));
        lib_mcp_close_pipe(to_child);
        return NULL;
    }

    pid_t pid = fork();
    if (lib_unlikely(pid < 0)) {
        lib_mcp_set_err(NULL, errbuf, errlen, "fork() failed: %s", strerror(errno));
        lib_mcp_close_pipe(to_child);
        lib_mcp_close_pipe(from_child);
        return NULL;
    }

    if (pid == 0) {
        if (dup2(to_child[0], STDIN_FILENO) < 0 || dup2(from_child[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        lib_close_fd(&to_child[0]);
        lib_close_fd(&to_child[1]);
        lib_close_fd(&from_child[0]);
        lib_close_fd(&from_child[1]);

        if (cfg->envp != NULL) {
            execve(cfg->cmd, (char *const *)cfg->argv, (char *const *)cfg->envp);
        } else {
            execvp(cfg->cmd, (char *const *)cfg->argv);
        }
        _exit(127);
    }

    lib_close_fd(&to_child[0]);
    lib_close_fd(&from_child[1]);

    lib_mcp_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (lib_unlikely(ctx == NULL)) {
        lib_close_fd(&to_child[1]);
        lib_close_fd(&from_child[0]);
        (void)kill(pid, SIGTERM);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        lib_mcp_set_err(NULL, errbuf, errlen, "out of memory allocating lib_mcp_ctx_t");
        return NULL;
    }

    ctx->pid = pid;
    ctx->in_fd = to_child[1];
    ctx->out_fd = from_child[0];
    ctx->timeout_ms = (cfg->timeout_ms > 0) ? cfg->timeout_ms : LIB_MCP_DEF_TIMEOUT_MS;
    ctx->max_line_sz = (cfg->max_line_sz > 0) ? cfg->max_line_sz : LIB_MCP_DEF_MAX_LINE;
    ctx->next_id = 1;

    if (lib_unlikely(lib_buf_init(&ctx->rbuf, 4096u, ctx->max_line_sz) != LIB_BUF_OK)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "failed to initialize receive buffer: %s",
                        strerror(errno));
        lib_mcp_close(ctx);
        return NULL;
    }

    if (lib_unlikely(lib_set_nonblock(ctx->in_fd) != 0 || lib_set_nonblock(ctx->out_fd) != 0)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "failed to enable O_NONBLOCK: %s", strerror(errno));
        lib_mcp_close(ctx);
        return NULL;
    }

    if (lib_unlikely(lib_mcp_handshake(ctx, errbuf, errlen) != LIB_MCP_OK)) {
        lib_mcp_close(ctx);
        return NULL;
    }

    return ctx;
}

void lib_mcp_close(lib_mcp_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    lib_close_fd(&ctx->in_fd);
    lib_close_fd(&ctx->out_fd);

    if (ctx->pid > 0) {
        int status = 0;
        pid_t w = waitpid(ctx->pid, &status, WNOHANG);
        if (w == 0) {
            (void)kill(ctx->pid, SIGTERM);
            for (int i = 0; i < 20; i++) {
                w = waitpid(ctx->pid, &status, WNOHANG);
                if (w != 0) {
                    break;
                }

                struct timespec req = {
                    .tv_sec = 0,
                    .tv_nsec = 10000000L, /* 10 ms */
                };
                while (nanosleep(&req, &req) != 0 && errno == EINTR) {
                }
            }
            if (w == 0) {
                (void)kill(ctx->pid, SIGKILL);
                while (waitpid(ctx->pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
        ctx->pid = -1;
    }

    lib_buf_free(&ctx->rbuf);
    free(ctx);
}

void lib_mcp_free_tools(lib_mcp_tool_t *tools, size_t count)
{
    if (tools == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(tools[i].name);
        free(tools[i].description);
        free(tools[i].schema_json);
    }
    free(tools);
}

lib_mcp_err_t lib_mcp_tools(lib_mcp_ctx_t *ctx,
                            lib_mcp_tool_t **out_tools,
                            size_t *out_count,
                            char *errbuf,
                            size_t errlen)
{
    if (lib_unlikely(ctx == NULL || out_tools == NULL || out_count == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_tools");
        return LIB_MCP_ERR_ARG;
    }

    *out_tools = NULL;
    *out_count = 0;

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_TOOLS_LIST, NULL, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    lib_mcp_tool_t *list = NULL;
    size_t idx = 0;
    lib_mcp_err_t ret = LIB_MCP_OK;

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_TOOLS);
    if (lib_unlikely(!cJSON_IsArray(arr))) {
        lib_mcp_set_err(ctx, errbuf, errlen, "tools/list response missing tools array");
        ret = LIB_MCP_ERR_PROTO;
        goto fail;
    }

    size_t n = (size_t)cJSON_GetArraySize(arr);
    if (n > 0u) {
        list = calloc(n, sizeof(*list));
        if (lib_unlikely(list == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating tools array");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_NAME);
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_DESCRIPTION);
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_INPUT_SCHEMA);

        if (lib_unlikely(!cJSON_IsString(name) || name->valuestring == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "tool descriptor missing valid name");
            ret = LIB_MCP_ERR_PROTO;
            goto fail;
        }

        list[idx].name = strdup(name->valuestring);
        if (lib_unlikely(list[idx].name == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating tool name");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }

        if (cJSON_IsString(desc) && desc->valuestring != NULL) {
            list[idx].description = strdup(desc->valuestring);
            if (lib_unlikely(list[idx].description == NULL)) {
                idx++;
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating tool description");
                ret = LIB_MCP_ERR_MEM;
                goto fail;
            }
        }

        char *schema_raw = (schema != NULL) ? cJSON_PrintUnformatted(schema) : NULL;
        if (schema_raw != NULL) {
            list[idx].schema_json = strdup(schema_raw);
            free(schema_raw);
        } else {
            list[idx].schema_json = strdup("{}");
        }
        if (lib_unlikely(list[idx].schema_json == NULL)) {
            idx++;
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating tool schema");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }

        idx++;
    }

    cJSON_Delete(res);
    *out_tools = list;
    *out_count = n;
    return LIB_MCP_OK;

fail:
    lib_mcp_free_tools(list, idx);
    cJSON_Delete(res);
    return ret;
}

void lib_mcp_free_res(lib_mcp_res_t *res)
{
    if (res == NULL) {
        return;
    }
    free(res->text);
    free(res->raw_json);
    memset(res, 0, sizeof(*res));
}

lib_mcp_err_t lib_mcp_call(lib_mcp_ctx_t *ctx,
                           const char *name,
                           const char *args_json,
                           lib_mcp_res_t *out_res,
                           char *errbuf,
                           size_t errlen)
{
    if (lib_unlikely(ctx == NULL || name == NULL || name[0] == '\0' || out_res == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_call");
        return LIB_MCP_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    cJSON *args_obj = NULL;
    lib_mcp_err_t arc = lib_mcp_parse_args_obj(ctx, args_json, &args_obj, errbuf, errlen);
    if (lib_unlikely(arc != LIB_MCP_OK)) {
        return arc;
    }

    cJSON *params = cJSON_CreateObject();
    if (lib_unlikely(params == NULL)) {
        cJSON_Delete(args_obj);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating tools/call params");
        return LIB_MCP_ERR_MEM;
    }

    if (!cJSON_AddStringToObject(params, MCP_KEY_NAME, name)) {
        cJSON_Delete(args_obj);
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory adding tool name");
        return LIB_MCP_ERR_MEM;
    }
    cJSON_AddItemToObject(params, MCP_KEY_ARGUMENTS, args_obj);

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_TOOLS_CALL, params, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    lib_mcp_err_t ret = LIB_MCP_OK;

    out_res->raw_json = cJSON_PrintUnformatted(res);
    if (lib_unlikely(out_res->raw_json == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory serializing raw result");
        ret = LIB_MCP_ERR_MEM;
        goto fail;
    }

    const cJSON *is_err = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_IS_ERROR);
    out_res->is_error = (cJSON_IsBool(is_err) && cJSON_IsTrue(is_err)) ? 1 : 0;

    ret = lib_mcp_collect_tool_text(ctx, res, &out_res->text, errbuf, errlen);
    if (lib_unlikely(ret != LIB_MCP_OK)) {
        goto fail;
    }

    cJSON_Delete(res);
    return LIB_MCP_OK;

fail:
    lib_mcp_free_res(out_res);
    cJSON_Delete(res);
    return ret;
}

void lib_mcp_free_resources(lib_mcp_resource_t *resources, size_t count)
{
    if (resources == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(resources[i].uri);
        free(resources[i].name);
        free(resources[i].description);
        free(resources[i].mime_type);
    }
    free(resources);
}

lib_mcp_err_t lib_mcp_resources(lib_mcp_ctx_t *ctx,
                                lib_mcp_resource_t **out_resources,
                                size_t *out_count,
                                char *errbuf,
                                size_t errlen)
{
    if (lib_unlikely(ctx == NULL || out_resources == NULL || out_count == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_resources");
        return LIB_MCP_ERR_ARG;
    }

    *out_resources = NULL;
    *out_count = 0;

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_RESOURCES_LIST, NULL, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    lib_mcp_resource_t *list = NULL;
    size_t idx = 0;
    lib_mcp_err_t ret = LIB_MCP_OK;

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_RESOURCES);
    if (lib_unlikely(!cJSON_IsArray(arr))) {
        lib_mcp_set_err(ctx, errbuf, errlen, "resources/list response missing resources array");
        ret = LIB_MCP_ERR_PROTO;
        goto fail;
    }

    size_t n = (size_t)cJSON_GetArraySize(arr);
    if (n > 0u) {
        list = calloc(n, sizeof(*list));
        if (lib_unlikely(list == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating resources array");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *uri = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_URI);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_NAME);
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_DESCRIPTION);
        const cJSON *mime = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_MIME_TYPE);

        if (lib_unlikely(!cJSON_IsString(uri) || uri->valuestring == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "resource descriptor missing valid uri");
            ret = LIB_MCP_ERR_PROTO;
            goto fail;
        }

        list[idx].uri = strdup(uri->valuestring);
        if (lib_unlikely(list[idx].uri == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating resource uri");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }

        if (cJSON_IsString(name) && name->valuestring != NULL) {
            list[idx].name = strdup(name->valuestring);
            if (lib_unlikely(list[idx].name == NULL)) {
                idx++;
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating resource name");
                ret = LIB_MCP_ERR_MEM;
                goto fail;
            }
        }

        if (cJSON_IsString(desc) && desc->valuestring != NULL) {
            list[idx].description = strdup(desc->valuestring);
            if (lib_unlikely(list[idx].description == NULL)) {
                idx++;
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating resource description");
                ret = LIB_MCP_ERR_MEM;
                goto fail;
            }
        }

        if (cJSON_IsString(mime) && mime->valuestring != NULL) {
            list[idx].mime_type = strdup(mime->valuestring);
            if (lib_unlikely(list[idx].mime_type == NULL)) {
                idx++;
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating resource mimeType");
                ret = LIB_MCP_ERR_MEM;
                goto fail;
            }
        }

        idx++;
    }

    cJSON_Delete(res);
    *out_resources = list;
    *out_count = n;
    return LIB_MCP_OK;

fail:
    lib_mcp_free_resources(list, idx);
    cJSON_Delete(res);
    return ret;
}

lib_mcp_err_t lib_mcp_read_resource(lib_mcp_ctx_t *ctx,
                                    const char *uri,
                                    lib_mcp_res_t *out_res,
                                    char *errbuf,
                                    size_t errlen)
{
    if (lib_unlikely(ctx == NULL || uri == NULL || uri[0] == '\0' || out_res == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_read_resource");
        return LIB_MCP_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    cJSON *params = cJSON_CreateObject();
    if (lib_unlikely(params == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating resources/read params");
        return LIB_MCP_ERR_MEM;
    }

    if (!cJSON_AddStringToObject(params, MCP_KEY_URI, uri)) {
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory adding resource uri");
        return LIB_MCP_ERR_MEM;
    }

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_RESOURCES_READ, params, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    lib_mcp_err_t ret = LIB_MCP_OK;

    out_res->raw_json = cJSON_PrintUnformatted(res);
    if (lib_unlikely(out_res->raw_json == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory serializing resource result");
        ret = LIB_MCP_ERR_MEM;
        goto fail;
    }

    ret = lib_mcp_collect_resource_text(ctx, res, &out_res->text, errbuf, errlen);
    if (lib_unlikely(ret != LIB_MCP_OK)) {
        goto fail;
    }

    cJSON_Delete(res);
    return LIB_MCP_OK;

fail:
    lib_mcp_free_res(out_res);
    cJSON_Delete(res);
    return ret;
}

void lib_mcp_free_prompts(lib_mcp_prompt_t *prompts, size_t count)
{
    if (prompts == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(prompts[i].name);
        free(prompts[i].description);
        for (size_t j = 0; j < prompts[i].arg_count; j++) {
            free(prompts[i].arguments[j].name);
            free(prompts[i].arguments[j].description);
        }
        free(prompts[i].arguments);
    }
    free(prompts);
}

/* Parses a single prompt descriptor's optional arguments array. */
static lib_mcp_err_t lib_mcp_parse_prompt_args(lib_mcp_ctx_t *ctx,
                                               const cJSON *item,
                                               lib_mcp_prompt_t *out_prompt,
                                               char *errbuf,
                                               size_t errlen)
{
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_ARGUMENTS);
    if (!cJSON_IsArray(args)) {
        return LIB_MCP_OK;
    }

    size_t n = (size_t)cJSON_GetArraySize(args);
    if (n == 0u) {
        return LIB_MCP_OK;
    }

    lib_mcp_prompt_arg_t *arr = calloc(n, sizeof(*arr));
    if (lib_unlikely(arr == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating prompt arguments");
        return LIB_MCP_ERR_MEM;
    }

    size_t idx = 0;
    const cJSON *arg = NULL;
    cJSON_ArrayForEach(arg, args) {
        const cJSON *aname = cJSON_GetObjectItemCaseSensitive(arg, MCP_KEY_NAME);
        const cJSON *adesc = cJSON_GetObjectItemCaseSensitive(arg, MCP_KEY_DESCRIPTION);
        const cJSON *areq = cJSON_GetObjectItemCaseSensitive(arg, MCP_KEY_REQUIRED);

        if (lib_unlikely(!cJSON_IsString(aname) || aname->valuestring == NULL)) {
            for (size_t k = 0; k < idx; k++) {
                free(arr[k].name);
                free(arr[k].description);
            }
            free(arr);
            lib_mcp_set_err(ctx, errbuf, errlen, "prompt argument missing valid name");
            return LIB_MCP_ERR_PROTO;
        }

        arr[idx].name = strdup(aname->valuestring);
        if (lib_unlikely(arr[idx].name == NULL)) {
            for (size_t k = 0; k < idx; k++) {
                free(arr[k].name);
                free(arr[k].description);
            }
            free(arr);
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating prompt arg name");
            return LIB_MCP_ERR_MEM;
        }

        if (cJSON_IsString(adesc) && adesc->valuestring != NULL) {
            arr[idx].description = strdup(adesc->valuestring);
            if (lib_unlikely(arr[idx].description == NULL)) {
                free(arr[idx].name);
                for (size_t k = 0; k < idx; k++) {
                    free(arr[k].name);
                    free(arr[k].description);
                }
                free(arr);
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating prompt arg desc");
                return LIB_MCP_ERR_MEM;
            }
        }

        arr[idx].required = (cJSON_IsBool(areq) && cJSON_IsTrue(areq)) ? 1 : 0;
        idx++;
    }

    out_prompt->arguments = arr;
    out_prompt->arg_count = n;
    return LIB_MCP_OK;
}

lib_mcp_err_t lib_mcp_prompts(lib_mcp_ctx_t *ctx,
                              lib_mcp_prompt_t **out_prompts,
                              size_t *out_count,
                              char *errbuf,
                              size_t errlen)
{
    if (lib_unlikely(ctx == NULL || out_prompts == NULL || out_count == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_prompts");
        return LIB_MCP_ERR_ARG;
    }

    *out_prompts = NULL;
    *out_count = 0;

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_PROMPTS_LIST, NULL, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    lib_mcp_prompt_t *list = NULL;
    size_t idx = 0;
    lib_mcp_err_t ret = LIB_MCP_OK;

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(res, MCP_KEY_PROMPTS);
    if (lib_unlikely(!cJSON_IsArray(arr))) {
        lib_mcp_set_err(ctx, errbuf, errlen, "prompts/list response missing prompts array");
        ret = LIB_MCP_ERR_PROTO;
        goto fail;
    }

    size_t n = (size_t)cJSON_GetArraySize(arr);
    if (n > 0u) {
        list = calloc(n, sizeof(*list));
        if (lib_unlikely(list == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating prompts array");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_NAME);
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, MCP_KEY_DESCRIPTION);

        if (lib_unlikely(!cJSON_IsString(name) || name->valuestring == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "prompt descriptor missing valid name");
            ret = LIB_MCP_ERR_PROTO;
            goto fail;
        }

        list[idx].name = strdup(name->valuestring);
        if (lib_unlikely(list[idx].name == NULL)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating prompt name");
            ret = LIB_MCP_ERR_MEM;
            goto fail;
        }

        if (cJSON_IsString(desc) && desc->valuestring != NULL) {
            list[idx].description = strdup(desc->valuestring);
            if (lib_unlikely(list[idx].description == NULL)) {
                idx++;
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating prompt description");
                ret = LIB_MCP_ERR_MEM;
                goto fail;
            }
        }

        ret = lib_mcp_parse_prompt_args(ctx, item, &list[idx], errbuf, errlen);
        if (lib_unlikely(ret != LIB_MCP_OK)) {
            idx++;
            goto fail;
        }

        idx++;
    }

    cJSON_Delete(res);
    *out_prompts = list;
    *out_count = n;
    return LIB_MCP_OK;

fail:
    lib_mcp_free_prompts(list, idx);
    cJSON_Delete(res);
    return ret;
}

lib_mcp_err_t lib_mcp_get_prompt(lib_mcp_ctx_t *ctx,
                                 const char *name,
                                 const char *args_json,
                                 lib_mcp_res_t *out_res,
                                 char *errbuf,
                                 size_t errlen)
{
    if (lib_unlikely(ctx == NULL || name == NULL || name[0] == '\0' || out_res == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_get_prompt");
        return LIB_MCP_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    cJSON *args_obj = NULL;
    lib_mcp_err_t arc = lib_mcp_parse_args_obj(ctx, args_json, &args_obj, errbuf, errlen);
    if (lib_unlikely(arc != LIB_MCP_OK)) {
        return arc;
    }

    cJSON *params = cJSON_CreateObject();
    if (lib_unlikely(params == NULL)) {
        cJSON_Delete(args_obj);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating prompts/get params");
        return LIB_MCP_ERR_MEM;
    }

    if (!cJSON_AddStringToObject(params, MCP_KEY_NAME, name)) {
        cJSON_Delete(args_obj);
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory adding prompt name");
        return LIB_MCP_ERR_MEM;
    }
    cJSON_AddItemToObject(params, MCP_KEY_ARGUMENTS, args_obj);

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, LIB_MCP_METH_PROMPTS_GET, params, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    lib_mcp_err_t ret = LIB_MCP_OK;

    out_res->raw_json = cJSON_PrintUnformatted(res);
    if (lib_unlikely(out_res->raw_json == NULL)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory serializing prompt result");
        ret = LIB_MCP_ERR_MEM;
        goto fail;
    }

    ret = lib_mcp_collect_prompt_text(ctx, res, &out_res->text, errbuf, errlen);
    if (lib_unlikely(ret != LIB_MCP_OK)) {
        goto fail;
    }

    cJSON_Delete(res);
    return LIB_MCP_OK;

fail:
    lib_mcp_free_res(out_res);
    cJSON_Delete(res);
    return ret;
}

const char *lib_mcp_strerror(lib_mcp_err_t err)
{
    switch (err) {
    case LIB_MCP_OK:          return "ok";
    case LIB_MCP_ERR_ARG:     return "invalid argument";
    case LIB_MCP_ERR_MEM:     return "out of memory";
    case LIB_MCP_ERR_IO:      return "i/o error";
    case LIB_MCP_ERR_TIMEOUT: return "timed out";
    case LIB_MCP_ERR_PROTO:   return "protocol error";
    case LIB_MCP_ERR_RPC:     return "json-rpc error";
    case LIB_MCP_ERR_CLOSED:  return "connection closed";
    default:                  return "unknown error";
    }
}