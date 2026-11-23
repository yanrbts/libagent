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
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LIB_MCP_DEF_TIMEOUT_MS 10000
#define LIB_MCP_DEF_MAX_LINE   (16u * 1024u * 1024u)
#define LIB_MCP_PROTO_VER      "2025-06-18"

struct lib_mcp_ctx {
    pid_t     pid;          /* Spawned MCP server child process ID */
    int       in_fd;        /* Write end connected to child stdin */
    int       out_fd;       /* Read end connected to child stdout */
    int       timeout_ms;   /* Default per-request timeout in milliseconds */
    size_t    max_line_sz;  /* Maximum single JSON-RPC frame size in bytes */
    int64_t   next_id;      /* Monotonically increasing JSON-RPC request ID */
    lib_buf_t rbuf;         /* Receive frame buffer for child stdout */
    char      last_err[256];/* Last diagnostic error message */
};

/* Formats and stores a diagnostic message into ctx and optional errbuf. */
static void lib_mcp_set_err(lib_mcp_ctx_t *ctx, char *errbuf, size_t errlen,
                            const char *fmt, ...)
{
    char tmp[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (ctx) {
        snprintf(ctx->last_err, sizeof(ctx->last_err), "%s", tmp);
    }
    if (errbuf && errlen > 0) {
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

/* Closes a pair of pipe file descriptors safely. */
static void lib_mcp_close_pipe(int pfd[2]) {
    if (!pfd) {
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

/* Serializes and writes a single newline-delimited JSON-RPC message. */
static lib_mcp_err_t lib_mcp_send_msg(lib_mcp_ctx_t *ctx,
                                      const cJSON *msg,
                                      int timeout_ms,
                                      char *errbuf,
                                      size_t errlen)
{
    if (lib_unlikely(!ctx || !msg)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to send_msg");
        return LIB_MCP_ERR_ARG;
    }

    char *raw = cJSON_PrintUnformatted(msg);
    if (lib_unlikely(!raw)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "failed to serialize JSON-RPC message");
        return LIB_MCP_ERR_MEM;
    }

    size_t raw_len = strlen(raw);
    if (lib_unlikely(raw_len > ctx->max_line_sz || raw_len + 1u > ctx->max_line_sz)) {
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
                                     const char *method,
                                     cJSON *params,
                                     int timeout_ms,
                                     cJSON **out_result,
                                     char *errbuf,
                                     size_t errlen)
{
    *out_result = NULL;

    int eff_timeout = (timeout_ms > 0) ? timeout_ms : ctx->timeout_ms;
    int64_t deadline = lib_mcp_make_deadline(eff_timeout);
    int64_t req_id = ctx->next_id++;

    cJSON *req = cJSON_CreateObject();
    if (lib_unlikely(!req)) {
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating JSON-RPC request");
        return LIB_MCP_ERR_MEM;
    }

    if (!cJSON_AddStringToObject(req, "jsonrpc", "2.0") ||
        !cJSON_AddNumberToObject(req, "id", (double)req_id) ||
        !cJSON_AddStringToObject(req, "method", method)) {
        cJSON_Delete(params);
        cJSON_Delete(req);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory populating JSON-RPC request");
        return LIB_MCP_ERR_MEM;
    }

    if (params) {
        cJSON_AddItemToObject(req, "params", params);
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
        if (lib_unlikely(!doc)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "invalid JSON frame from MCP server");
            return LIB_MCP_ERR_PROTO;
        }

        if (lib_unlikely(!cJSON_IsObject(doc))) {
            cJSON_Delete(doc);
            lib_mcp_set_err(ctx, errbuf, errlen, "JSON-RPC message is not an object");
            return LIB_MCP_ERR_PROTO;
        }

        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(doc, "id");
        if (!cJSON_IsNumber(id_item) || (int64_t)id_item->valuedouble != req_id) {
            cJSON_Delete(doc);
            continue;
        }

        cJSON *err_item = cJSON_GetObjectItemCaseSensitive(doc, "error");
        if (cJSON_IsObject(err_item)) {
            cJSON *emsg = cJSON_GetObjectItemCaseSensitive(err_item, "message");
            cJSON *ecode = cJSON_GetObjectItemCaseSensitive(err_item, "code");
            lib_mcp_set_err(ctx, errbuf, errlen, "RPC error (%d): %s",
                            cJSON_IsNumber(ecode) ? ecode->valueint : -1,
                            (cJSON_IsString(emsg) && emsg->valuestring) ? emsg->valuestring : "unknown");
            cJSON_Delete(doc);
            return LIB_MCP_ERR_RPC;
        }

        cJSON *res_item = cJSON_GetObjectItemCaseSensitive(doc, "result");
        if (lib_unlikely(!res_item)) {
            cJSON_Delete(doc);
            lib_mcp_set_err(ctx, errbuf, errlen, "JSON-RPC response missing result");
            return LIB_MCP_ERR_PROTO;
        }

        *out_result = cJSON_Duplicate(res_item, 1);
        cJSON_Delete(doc);
        if (lib_unlikely(!*out_result)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory cloning RPC result");
            return LIB_MCP_ERR_MEM;
        }
        return LIB_MCP_OK;
    }
}

/* Executes the MCP initialize handshake and sends notifications/initialized. */
static lib_mcp_err_t lib_mcp_handshake(lib_mcp_ctx_t *ctx, char *errbuf, size_t errlen)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *caps = cJSON_CreateObject();
    cJSON *info = cJSON_CreateObject();
    if (lib_unlikely(!params || !caps || !info)) {
        cJSON_Delete(params);
        cJSON_Delete(caps);
        cJSON_Delete(info);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory building initialize params");
        return LIB_MCP_ERR_MEM;
    }

    cJSON_AddStringToObject(info, "name", "lib_mcp_c_client");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddStringToObject(params, "protocolVersion", LIB_MCP_PROTO_VER);
    cJSON_AddItemToObject(params, "capabilities", caps);
    cJSON_AddItemToObject(params, "clientInfo", info);

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, "initialize", params, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    cJSON_Delete(res);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    cJSON *notif = cJSON_CreateObject();
    if (lib_unlikely(!notif)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory building initialized notification");
        return LIB_MCP_ERR_MEM;
    }
    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "notifications/initialized");

    rc = lib_mcp_send_msg(ctx, notif, ctx->timeout_ms, errbuf, errlen);
    cJSON_Delete(notif);
    return rc;
}

/* Appends a string slice to a lib_buf_t, optionally inserting a newline first. */
static lib_buf_status_t lib_mcp_append_text(lib_buf_t *tbuf,
                                            const char *src,
                                            size_t src_len,
                                            int need_nl)
{
    size_t extra = src_len + (need_nl ? 1u : 0u);
    if (extra > SIZE_MAX - tbuf->len) {
        errno = EOVERFLOW;
        return LIB_BUF_ERR;
    }

    lib_buf_status_t rrc = lib_buf_reserve(tbuf, tbuf->len + extra + 1u);
    if (lib_unlikely(rrc != LIB_BUF_OK)) {
        return rrc;
    }

    if (need_nl) {
        tbuf->data[tbuf->len++] = '\n';
    }
    if (src_len > 0) {
        memcpy(tbuf->data + tbuf->len, src, src_len);
        tbuf->len += src_len;
    }
    tbuf->data[tbuf->len] = '\0';
    return LIB_BUF_OK;
}

lib_mcp_ctx_t *lib_mcp_open(const lib_mcp_cfg_t *cfg, char *errbuf, size_t errlen)
{
    if (lib_unlikely(!cfg || !cfg->cmd || cfg->cmd[0] == '\0' || !cfg->argv || !cfg->argv[0])) {
        lib_mcp_set_err(NULL, errbuf, errlen, "invalid lib_mcp_cfg_t");
        return NULL;
    }

    signal(SIGPIPE, SIG_IGN);

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

        if (cfg->envp) {
            execve(cfg->cmd, (char *const *)cfg->argv, (char *const *)cfg->envp);
        } else {
            execvp(cfg->cmd, (char *const *)cfg->argv);
        }
        _exit(127);
    }

    lib_close_fd(&to_child[0]);
    lib_close_fd(&from_child[1]);

    lib_mcp_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (lib_unlikely(!ctx)) {
        lib_close_fd(&to_child[1]);
        lib_close_fd(&from_child[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
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
    if (!ctx) {
        return;
    }

    lib_close_fd(&ctx->in_fd);
    lib_close_fd(&ctx->out_fd);

    if (ctx->pid > 0) {
        int status = 0;
        pid_t w = waitpid(ctx->pid, &status, WNOHANG);
        if (w == 0) {
            kill(ctx->pid, SIGTERM);
            while (waitpid(ctx->pid, &status, 0) < 0) {
                if (errno != EINTR) {
                    break;
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
    if (!tools) {
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
    if (lib_unlikely(!ctx || !out_tools || !out_count)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_tools");
        return LIB_MCP_ERR_ARG;
    }

    *out_tools = NULL;
    *out_count = 0;

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, "tools/list", NULL, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(res, "tools");
    if (lib_unlikely(!cJSON_IsArray(arr))) {
        cJSON_Delete(res);
        lib_mcp_set_err(ctx, errbuf, errlen, "tools/list response missing tools array");
        return LIB_MCP_ERR_PROTO;
    }

    size_t n = (size_t)cJSON_GetArraySize(arr);
    lib_mcp_tool_t *list = (n > 0) ? calloc(n, sizeof(*list)) : NULL;
    if (lib_unlikely(n > 0 && !list)) {
        cJSON_Delete(res);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating tools array");
        return LIB_MCP_ERR_MEM;
    }

    size_t idx = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, "description");
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(item, "inputSchema");

        if (lib_unlikely(!cJSON_IsString(name) || !name->valuestring)) {
            lib_mcp_free_tools(list, idx);
            cJSON_Delete(res);
            lib_mcp_set_err(ctx, errbuf, errlen, "tool descriptor missing valid name");
            return LIB_MCP_ERR_PROTO;
        }

        list[idx].name = strdup(name->valuestring);
        if (lib_unlikely(!list[idx].name)) {
            lib_mcp_free_tools(list, idx);
            cJSON_Delete(res);
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating tool name");
            return LIB_MCP_ERR_MEM;
        }

        if (cJSON_IsString(desc) && desc->valuestring) {
            list[idx].description = strdup(desc->valuestring);
            if (lib_unlikely(!list[idx].description)) {
                lib_mcp_free_tools(list, idx + 1u);
                cJSON_Delete(res);
                lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating tool description");
                return LIB_MCP_ERR_MEM;
            }
        }

        char *schema_raw = schema ? cJSON_PrintUnformatted(schema) : NULL;
        if (schema_raw) {
            list[idx].schema_json = strdup(schema_raw);
            free(schema_raw);
        } else {
            list[idx].schema_json = strdup("{}");
        }
        if (lib_unlikely(!list[idx].schema_json)) {
            lib_mcp_free_tools(list, idx + 1u);
            cJSON_Delete(res);
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory duplicating tool schema");
            return LIB_MCP_ERR_MEM;
        }

        idx++;
    }

    cJSON_Delete(res);
    *out_tools = list;
    *out_count = n;
    return LIB_MCP_OK;
}

void lib_mcp_free_res(lib_mcp_res_t *res)
{
    if (!res) {
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
    if (lib_unlikely(!ctx || !name || name[0] == '\0' || !out_res)) {
        lib_mcp_set_err(ctx, errbuf, errlen, "invalid arguments to lib_mcp_call");
        return LIB_MCP_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    cJSON *args_obj = NULL;
    if (args_json && args_json[0] != '\0') {
        args_obj = cJSON_Parse(args_json);
        if (lib_unlikely(!args_obj || !cJSON_IsObject(args_obj))) {
            cJSON_Delete(args_obj);
            lib_mcp_set_err(ctx, errbuf, errlen, "args_json must parse to a JSON object");
            return LIB_MCP_ERR_ARG;
        }
    } else {
        args_obj = cJSON_CreateObject();
        if (lib_unlikely(!args_obj)) {
            lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating arguments object");
            return LIB_MCP_ERR_MEM;
        }
    }

    cJSON *params = cJSON_CreateObject();
    if (lib_unlikely(!params)) {
        cJSON_Delete(args_obj);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory creating tools/call params");
        return LIB_MCP_ERR_MEM;
    }

    if (!cJSON_AddStringToObject(params, "name", name)) {
        cJSON_Delete(args_obj);
        cJSON_Delete(params);
        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory adding tool name");
        return LIB_MCP_ERR_MEM;
    }
    cJSON_AddItemToObject(params, "arguments", args_obj);

    cJSON *res = NULL;
    lib_mcp_err_t rc = lib_mcp_rpc_req(ctx, "tools/call", params, ctx->timeout_ms,
                                       &res, errbuf, errlen);
    if (lib_unlikely(rc != LIB_MCP_OK)) {
        return rc;
    }

    out_res->raw_json = cJSON_PrintUnformatted(res);
    cJSON *is_err = cJSON_GetObjectItemCaseSensitive(res, "isError");
    out_res->is_error = (cJSON_IsBool(is_err) && cJSON_IsTrue(is_err)) ? 1 : 0;

    lib_buf_t tbuf;
    memset(&tbuf, 0, sizeof(tbuf));
    int has_tbuf = 0;

    cJSON *content = cJSON_GetObjectItemCaseSensitive(res, "content");
    if (cJSON_IsArray(content)) {
        cJSON *blk = NULL;
        cJSON_ArrayForEach(blk, content) {
            cJSON *btype = cJSON_GetObjectItemCaseSensitive(blk, "type");
            cJSON *btext = cJSON_GetObjectItemCaseSensitive(blk, "text");
            if (cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0 &&
                cJSON_IsString(btext) && btext->valuestring) {
                if (!has_tbuf) {
                    if (lib_unlikely(lib_buf_init(&tbuf, 256u, ctx->max_line_sz) != LIB_BUF_OK)) {
                        lib_mcp_free_res(out_res);
                        cJSON_Delete(res);
                        lib_mcp_set_err(ctx, errbuf, errlen, "out of memory allocating text buffer");
                        return LIB_MCP_ERR_MEM;
                    }
                    has_tbuf = 1;
                }

                lib_buf_status_t arc = lib_mcp_append_text(&tbuf,
                                                           btext->valuestring,
                                                           strlen(btext->valuestring),
                                                           tbuf.len > 0 ? 1 : 0);
                if (lib_unlikely(arc != LIB_BUF_OK)) {
                    lib_buf_free(&tbuf);
                    lib_mcp_free_res(out_res);
                    cJSON_Delete(res);
                    lib_mcp_set_err(ctx, errbuf, errlen,
                                    (arc == LIB_BUF_OVERFLOW) ? "tool text exceeds max_line_sz"
                                                               : "out of memory building tool text");
                    return (arc == LIB_BUF_OVERFLOW) ? LIB_MCP_ERR_PROTO : LIB_MCP_ERR_MEM;
                }
            }
        }
    }

    if (has_tbuf) {
        /* Transfer ownership of the heap buffer directly to out_res->text */
        out_res->text = tbuf.data;
        tbuf.data = NULL;
        lib_buf_free(&tbuf);
    }

    cJSON_Delete(res);
    return LIB_MCP_OK;
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