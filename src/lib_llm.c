/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */

#include "lib_llm.h"
#include "lib_http.h"
#include "util.h"
#include "cJSON.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIB_LLM_DEF_TIMEOUT_MS 30000
#define LIB_LLM_DEF_MAX_RESP   (16u * 1024u * 1024u)

struct lib_llm_ctx {
    char           *endpoint;    /* Target chat completions endpoint URL */
    char           *api_key;     /* Optional API key or Bearer token */
    char           *model;       /* Default model identifier */
    int             timeout_ms;  /* Per-request timeout in milliseconds */
    size_t          max_resp_sz; /* Maximum HTTP response payload size */
    double          temperature; /* Sampling temperature (< 0 omits field) */
    int             max_tokens;  /* Maximum completion tokens (<= 0 omits field) */
    lib_http_ctx_t *http;        /* Underlying synchronous HTTP/HTTPS client */
    char            last_err[256]; /* Last diagnostic error message */
};

/* Formats and stores a diagnostic message into ctx and optional errbuf. */
static void lib_llm_set_err(lib_llm_ctx_t *ctx, char *errbuf, size_t errlen,
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

/* Converts an enum role to its OpenAI-compatible wire role string. */
static const char *lib_llm_role_str(lib_llm_role_t role)
{
    switch (role) {
    case LIB_LLM_ROLE_SYSTEM:    return "system";
    case LIB_LLM_ROLE_USER:      return "user";
    case LIB_LLM_ROLE_ASSISTANT: return "assistant";
    case LIB_LLM_ROLE_TOOL:      return "tool";
    default:                     return NULL;
    }
}

/* Duplicates a non-NULL string safely; sets *oom on allocation failure. */
static char *lib_llm_strdup_safe(const char *s, int *oom)
{
    if (s == NULL) {
        return NULL;
    }
    char *copy = strdup(s);
    if (lib_unlikely(copy == NULL)) {
        if (oom != NULL) {
            *oom = 1;
        }
        return NULL;
    }
    return copy;
}

/* Attaches child JSON node to parent, freeing child if attachment fails. */
static int lib_llm_attach_item(cJSON *parent, const char *key, cJSON *child)
{
    if (lib_unlikely(parent == NULL || child == NULL)) {
        cJSON_Delete(child);
        return 0;
    }
    if (lib_unlikely(!cJSON_AddItemToObject(parent, key, child))) {
        cJSON_Delete(child);
        return 0;
    }
    return 1;
}

/* Appends item to JSON array, freeing item if append fails. */
static int lib_llm_push_item(cJSON *arr, cJSON *item)
{
    if (lib_unlikely(arr == NULL || item == NULL)) {
        cJSON_Delete(item);
        return 0;
    }
    if (lib_unlikely(!cJSON_AddItemToArray(arr, item))) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

/* Serializes a single lib_llm_tool_call_t into a cJSON tool_calls item. */
static cJSON *lib_llm_encode_call_item(const lib_llm_tool_call_t *tc)
{
    if (tc == NULL || tc->id == NULL || tc->id[0] == '\0' ||
        tc->name == NULL || tc->name[0] == '\0') {
        return NULL;
    }

    cJSON *item = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    if (lib_unlikely(item == NULL || fn == NULL)) {
        cJSON_Delete(item);
        cJSON_Delete(fn);
        return NULL;
    }

    const char *args = (tc->args_json != NULL && tc->args_json[0] != '\0')
                           ? tc->args_json
                           : "{}";

    if (!cJSON_AddStringToObject(item, "id", tc->id) ||
        !cJSON_AddStringToObject(item, "type", "function") ||
        !cJSON_AddStringToObject(fn, "name", tc->name) ||
        !cJSON_AddStringToObject(fn, "arguments", args) ||
        !lib_llm_attach_item(item, "function", fn)) {
        cJSON_Delete(item);
        return NULL;
    }

    return item;
}

/* Serializes one conversation message into a cJSON chat message object. */
static cJSON *lib_llm_encode_msg(const lib_llm_msg_t *msg)
{
    if (lib_unlikely(msg == NULL)) {
        return NULL;
    }

    const char *rstr = lib_llm_role_str(msg->role);
    if (lib_unlikely(rstr == NULL)) {
        return NULL;
    }

    cJSON *mobj = cJSON_CreateObject();
    if (lib_unlikely(mobj == NULL)) {
        return NULL;
    }

    if (!cJSON_AddStringToObject(mobj, "role", rstr)) {
        cJSON_Delete(mobj);
        return NULL;
    }

    if (msg->content != NULL) {
        if (!cJSON_AddStringToObject(mobj, "content", msg->content)) {
            cJSON_Delete(mobj);
            return NULL;
        }
    } else if (msg->role == LIB_LLM_ROLE_TOOL) {
        if (!cJSON_AddStringToObject(mobj, "content", "")) {
            cJSON_Delete(mobj);
            return NULL;
        }
    } else if (msg->role == LIB_LLM_ROLE_ASSISTANT && msg->tool_count > 0u) {
        if (!cJSON_AddNullToObject(mobj, "content")) {
            cJSON_Delete(mobj);
            return NULL;
        }
    }

    if (msg->name != NULL && msg->name[0] != '\0') {
        if (!cJSON_AddStringToObject(mobj, "name", msg->name)) {
            cJSON_Delete(mobj);
            return NULL;
        }
    }

    if (msg->role == LIB_LLM_ROLE_TOOL) {
        if (lib_unlikely(msg->tool_call_id == NULL || msg->tool_call_id[0] == '\0')) {
            cJSON_Delete(mobj);
            return NULL;
        }
        if (!cJSON_AddStringToObject(mobj, "tool_call_id", msg->tool_call_id)) {
            cJSON_Delete(mobj);
            return NULL;
        }
    }

    if (msg->role == LIB_LLM_ROLE_ASSISTANT && msg->tool_count > 0u) {
        if (lib_unlikely(msg->tool_calls == NULL)) {
            cJSON_Delete(mobj);
            return NULL;
        }
        cJSON *arr = cJSON_CreateArray();
        if (lib_unlikely(arr == NULL)) {
            cJSON_Delete(mobj);
            return NULL;
        }
        for (size_t i = 0; i < msg->tool_count; i++) {
            cJSON *call_obj = lib_llm_encode_call_item(&msg->tool_calls[i]);
            if (lib_unlikely(call_obj == NULL || !lib_llm_push_item(arr, call_obj))) {
                cJSON_Delete(arr);
                cJSON_Delete(mobj);
                return NULL;
            }
        }
        if (!lib_llm_attach_item(mobj, "tool_calls", arr)) {
            cJSON_Delete(mobj);
            return NULL;
        }
    }

    return mobj;
}

/* Serializes a single tool definition into an OpenAI-compatible function tool object. */
static cJSON *lib_llm_encode_tool_def(const lib_llm_tool_def_t *tdef)
{
    if (tdef == NULL || tdef->name == NULL || tdef->name[0] == '\0') {
        return NULL;
    }

    cJSON *schema = NULL;
    if (tdef->schema_json != NULL && tdef->schema_json[0] != '\0') {
        schema = cJSON_Parse(tdef->schema_json);
        if (lib_unlikely(schema == NULL || !cJSON_IsObject(schema))) {
            cJSON_Delete(schema);
            return NULL;
        }
    } else {
        schema = cJSON_CreateObject();
        if (lib_unlikely(schema == NULL)) {
            return NULL;
        }
    }

    cJSON *item = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    if (lib_unlikely(item == NULL || fn == NULL)) {
        cJSON_Delete(schema);
        cJSON_Delete(item);
        cJSON_Delete(fn);
        return NULL;
    }

    if (!cJSON_AddStringToObject(item, "type", "function") ||
        !cJSON_AddStringToObject(fn, "name", tdef->name)) {
        cJSON_Delete(schema);
        cJSON_Delete(item);
        cJSON_Delete(fn);
        return NULL;
    }

    if (tdef->description != NULL && tdef->description[0] != '\0') {
        if (!cJSON_AddStringToObject(fn, "description", tdef->description)) {
            cJSON_Delete(schema);
            cJSON_Delete(item);
            cJSON_Delete(fn);
            return NULL;
        }
    }

    if (!lib_llm_attach_item(fn, "parameters", schema) ||
        !lib_llm_attach_item(item, "function", fn)) {
        cJSON_Delete(item);
        return NULL;
    }

    return item;
}

/* Builds the JSON request body string for chat completions. */
static char *lib_llm_build_req_json(const lib_llm_ctx_t *ctx,
                                    const lib_llm_msg_t *msgs,
                                    size_t msg_count,
                                    const lib_llm_tool_def_t *tools,
                                    size_t tool_count,
                                    int *bad_arg)
{
    *bad_arg = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *marr = cJSON_CreateArray();
    if (lib_unlikely(root == NULL || marr == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(marr);
        return NULL;
    }

    if (!cJSON_AddStringToObject(root, "model", ctx->model)) {
        cJSON_Delete(marr);
        cJSON_Delete(root);
        return NULL;
    }

    if (ctx->temperature >= 0.0) {
        if (!cJSON_AddNumberToObject(root, "temperature", ctx->temperature)) {
            cJSON_Delete(marr);
            cJSON_Delete(root);
            return NULL;
        }
    }

    if (ctx->max_tokens > 0) {
        if (!cJSON_AddNumberToObject(root, "max_tokens", ctx->max_tokens)) {
            cJSON_Delete(marr);
            cJSON_Delete(root);
            return NULL;
        }
    }

    for (size_t i = 0; i < msg_count; i++) {
        cJSON *mobj = lib_llm_encode_msg(&msgs[i]);
        if (lib_unlikely(mobj == NULL)) {
            *bad_arg = 1;
            cJSON_Delete(marr);
            cJSON_Delete(root);
            return NULL;
        }
        if (lib_unlikely(!lib_llm_push_item(marr, mobj))) {
            cJSON_Delete(marr);
            cJSON_Delete(root);
            return NULL;
        }
    }

    if (lib_unlikely(!lib_llm_attach_item(root, "messages", marr))) {
        cJSON_Delete(root);
        return NULL;
    }

    if (tools != NULL && tool_count > 0u) {
        cJSON *tarr = cJSON_CreateArray();
        if (lib_unlikely(tarr == NULL)) {
            cJSON_Delete(root);
            return NULL;
        }
        for (size_t i = 0; i < tool_count; i++) {
            cJSON *tobj = lib_llm_encode_tool_def(&tools[i]);
            if (lib_unlikely(tobj == NULL)) {
                *bad_arg = 1;
                cJSON_Delete(tarr);
                cJSON_Delete(root);
                return NULL;
            }
            if (lib_unlikely(!lib_llm_push_item(tarr, tobj))) {
                cJSON_Delete(tarr);
                cJSON_Delete(root);
                return NULL;
            }
        }
        if (lib_unlikely(!lib_llm_attach_item(root, "tools", tarr))) {
            cJSON_Delete(root);
            return NULL;
        }
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* Maps lib_http_err_t to a corresponding lib_llm_err_t code. */
static lib_llm_err_t lib_llm_map_http_err(lib_http_err_t hrc)
{
    switch (hrc) {
    case LIB_HTTP_ERR_ARG:     return LIB_LLM_ERR_ARG;
    case LIB_HTTP_ERR_NOMEM:   return LIB_LLM_ERR_MEM;
    case LIB_HTTP_ERR_TIMEOUT: return LIB_LLM_ERR_TIMEOUT;
    case LIB_HTTP_ERR_PROTO:   return LIB_LLM_ERR_PROTO;
    case LIB_HTTP_ERR_INIT:
    case LIB_HTTP_ERR_CONNECT:
    case LIB_HTTP_ERR_SEND:
    case LIB_HTTP_ERR_RECV:
    case LIB_HTTP_ERR_SIZE:
    default:                   return LIB_LLM_ERR_IO;
    }
}

/* Extracts optional error.message from a non-2xx JSON response body. */
static void lib_llm_extract_http_err_detail(const char *body, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (body == NULL || body[0] == '\0' || out_sz == 0u) {
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return;
    }

    const cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(err_obj)) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(err_obj, "message");
        if (cJSON_IsString(msg) && msg->valuestring != NULL && msg->valuestring[0] != '\0') {
            snprintf(out, out_sz, ": %s", msg->valuestring);
        }
    }

    cJSON_Delete(root);
}

/* Executes an HTTP POST with JSON payload and Bearer auth via lib_http. */
static lib_llm_err_t lib_llm_http_post_json(lib_llm_ctx_t *ctx,
                                            const char *req_json,
                                            lib_http_res_t *out_http_res,
                                            char *errbuf,
                                            size_t errlen)
{
    char http_err[256] = {0};
    lib_http_err_t hrc = lib_http_post_json(ctx->http,
                                            ctx->endpoint,
                                            ctx->api_key,
                                            req_json,
                                            ctx->timeout_ms,
                                            out_http_res,
                                            http_err,
                                            sizeof(http_err));
    if (lib_unlikely(hrc != LIB_HTTP_OK)) {
        const char *detail = (http_err[0] != '\0')
                                 ? http_err
                                 : lib_http_strerror(hrc);
        lib_llm_set_err(ctx, errbuf, errlen,
                        "HTTP request to '%s' failed: %s",
                        ctx->endpoint, detail);
        return lib_llm_map_http_err(hrc);
    }

    if (lib_unlikely(out_http_res->status_code < 200 || out_http_res->status_code >= 300)) {
        char detail[192];
        lib_llm_extract_http_err_detail(out_http_res->body, detail, sizeof(detail));
        lib_llm_set_err(ctx, errbuf, errlen,
                        "HTTP status %d from '%s'%s",
                        out_http_res->status_code, ctx->endpoint, detail);
        lib_http_free_res(out_http_res);
        return LIB_LLM_ERR_HTTP;
    }

    return LIB_LLM_OK;
}

/* Parses tool_calls array from an assistant message object into out_res. */
static lib_llm_err_t lib_llm_parse_calls(lib_llm_ctx_t *ctx,
                                         const cJSON *calls_arr,
                                         lib_llm_res_t *out_res,
                                         char *errbuf,
                                         size_t errlen)
{
    if (!cJSON_IsArray(calls_arr)) {
        return LIB_LLM_OK;
    }

    size_t n = (size_t)cJSON_GetArraySize(calls_arr);
    if (n == 0u) {
        return LIB_LLM_OK;
    }

    lib_llm_tool_call_t *list = calloc(n, sizeof(*list));
    if (lib_unlikely(list == NULL)) {
        lib_llm_set_err(ctx, errbuf, errlen, "out of memory allocating tool_calls");
        return LIB_LLM_ERR_MEM;
    }

    size_t idx = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, calls_arr) {
        if (lib_unlikely(!cJSON_IsObject(item))) {
            lib_llm_free_calls(list, idx);
            lib_llm_set_err(ctx, errbuf, errlen, "tool_calls entry is not an object");
            return LIB_LLM_ERR_PROTO;
        }

        const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *fn_j = cJSON_GetObjectItemCaseSensitive(item, "function");
        const cJSON *name_j = cJSON_IsObject(fn_j)
                                  ? cJSON_GetObjectItemCaseSensitive(fn_j, "name")
                                  : NULL;
        const cJSON *args_j = cJSON_IsObject(fn_j)
                                  ? cJSON_GetObjectItemCaseSensitive(fn_j, "arguments")
                                  : NULL;

        const char *id_str = (cJSON_IsString(id_j) && id_j->valuestring != NULL)
                                 ? id_j->valuestring
                                 : "";
        if (lib_unlikely(!cJSON_IsString(name_j) || name_j->valuestring == NULL ||
                         name_j->valuestring[0] == '\0')) {
            lib_llm_free_calls(list, idx);
            lib_llm_set_err(ctx, errbuf, errlen, "tool_call missing function name");
            return LIB_LLM_ERR_PROTO;
        }

        int oom = 0;
        list[idx].id = lib_llm_strdup_safe(id_str, &oom);
        list[idx].name = lib_llm_strdup_safe(name_j->valuestring, &oom);

        if (cJSON_IsString(args_j) && args_j->valuestring != NULL) {
            list[idx].args_json = lib_llm_strdup_safe(args_j->valuestring, &oom);
        } else if (cJSON_IsObject(args_j)) {
            char *raw_args = cJSON_PrintUnformatted(args_j);
            if (raw_args == NULL) {
                oom = 1;
            } else {
                list[idx].args_json = raw_args;
            }
        } else {
            list[idx].args_json = lib_llm_strdup_safe("{}", &oom);
        }

        if (lib_unlikely(oom)) {
            idx++;
            lib_llm_free_calls(list, idx);
            lib_llm_set_err(ctx, errbuf, errlen, "out of memory parsing tool_call fields");
            return LIB_LLM_ERR_MEM;
        }
        idx++;
    }

    out_res->tool_calls = list;
    out_res->tool_count = n;
    return LIB_LLM_OK;
}

/* Extracts token usage statistics when present in the response object. */
static void lib_llm_parse_usage(const cJSON *root, lib_llm_usage_t *usage)
{
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (!cJSON_IsObject(u)) {
        return;
    }

    const cJSON *p = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(u, "completion_tokens");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(u, "total_tokens");

    if (cJSON_IsNumber(p)) {
        usage->prompt_tokens = (int64_t)p->valuedouble;
    }
    if (cJSON_IsNumber(c)) {
        usage->completion_tokens = (int64_t)c->valuedouble;
    }
    if (cJSON_IsNumber(t)) {
        usage->total_tokens = (int64_t)t->valuedouble;
    }
}

/* Parses an OpenAI-compatible chat completion JSON body into out_res. */
static lib_llm_err_t lib_llm_parse_resp(lib_llm_ctx_t *ctx,
                                        const char *raw_body,
                                        lib_llm_res_t *out_res,
                                        char *errbuf,
                                        size_t errlen)
{
    cJSON *root = cJSON_Parse(raw_body);
    if (lib_unlikely(root == NULL || !cJSON_IsObject(root))) {
        cJSON_Delete(root);
        lib_llm_set_err(ctx, errbuf, errlen, "invalid JSON in LLM response");
        return LIB_LLM_ERR_PROTO;
    }

    const cJSON *api_err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(api_err)) {
        const cJSON *emsg = cJSON_GetObjectItemCaseSensitive(api_err, "message");
        lib_llm_set_err(ctx, errbuf, errlen, "LLM API error: %s",
                        (cJSON_IsString(emsg) && emsg->valuestring != NULL)
                            ? emsg->valuestring
                            : "unknown error");
        cJSON_Delete(root);
        return LIB_LLM_ERR_API;
    }

    int oom = 0;
    out_res->raw_json = lib_llm_strdup_safe(raw_body, &oom);
    if (lib_unlikely(oom)) {
        cJSON_Delete(root);
        lib_llm_free_res(out_res);
        lib_llm_set_err(ctx, errbuf, errlen, "out of memory storing raw_json");
        return LIB_LLM_ERR_MEM;
    }

    lib_llm_parse_usage(root, &out_res->usage);

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (lib_unlikely(!cJSON_IsObject(first))) {
        cJSON_Delete(root);
        lib_llm_free_res(out_res);
        lib_llm_set_err(ctx, errbuf, errlen, "LLM response missing choices[0]");
        return LIB_LLM_ERR_PROTO;
    }

    const cJSON *finish = cJSON_GetObjectItemCaseSensitive(first, "finish_reason");
    if (cJSON_IsString(finish) && finish->valuestring != NULL) {
        out_res->finish_reason = lib_llm_strdup_safe(finish->valuestring, &oom);
        if (lib_unlikely(oom)) {
            cJSON_Delete(root);
            lib_llm_free_res(out_res);
            lib_llm_set_err(ctx, errbuf, errlen, "out of memory parsing finish_reason");
            return LIB_LLM_ERR_MEM;
        }
    }

    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(first, "message");
    if (lib_unlikely(!cJSON_IsObject(msg))) {
        cJSON_Delete(root);
        lib_llm_free_res(out_res);
        lib_llm_set_err(ctx, errbuf, errlen, "LLM choice missing message object");
        return LIB_LLM_ERR_PROTO;
    }

    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        out_res->text = lib_llm_strdup_safe(content->valuestring, &oom);
        if (lib_unlikely(oom)) {
            cJSON_Delete(root);
            lib_llm_free_res(out_res);
            lib_llm_set_err(ctx, errbuf, errlen, "out of memory copying message content");
            return LIB_LLM_ERR_MEM;
        }
    }

    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
    lib_llm_err_t crc = lib_llm_parse_calls(ctx, calls, out_res, errbuf, errlen);
    cJSON_Delete(root);
    if (lib_unlikely(crc != LIB_LLM_OK)) {
        lib_llm_free_res(out_res);
        return crc;
    }

    return LIB_LLM_OK;
}

lib_llm_ctx_t *lib_llm_open(const lib_llm_cfg_t *cfg, char *errbuf, size_t errlen)
{
    if (lib_unlikely(cfg == NULL || cfg->endpoint == NULL || cfg->endpoint[0] == '\0' ||
                     cfg->model == NULL || cfg->model[0] == '\0')) {
        lib_llm_set_err(NULL, errbuf, errlen, "invalid lib_llm_cfg_t");
        return NULL;
    }

    lib_llm_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (lib_unlikely(ctx == NULL)) {
        lib_llm_set_err(NULL, errbuf, errlen, "out of memory allocating lib_llm_ctx_t");
        return NULL;
    }

    int oom = 0;
    ctx->endpoint = lib_llm_strdup_safe(cfg->endpoint, &oom);
    ctx->model = lib_llm_strdup_safe(cfg->model, &oom);
    if (cfg->api_key != NULL && cfg->api_key[0] != '\0') {
        ctx->api_key = lib_llm_strdup_safe(cfg->api_key, &oom);
    }

    if (lib_unlikely(oom)) {
        lib_llm_set_err(ctx, errbuf, errlen, "out of memory duplicating config strings");
        lib_llm_close(ctx);
        return NULL;
    }

    ctx->timeout_ms = (cfg->timeout_ms > 0) ? cfg->timeout_ms : LIB_LLM_DEF_TIMEOUT_MS;
    ctx->max_resp_sz = (cfg->max_resp_sz > 0u) ? cfg->max_resp_sz : LIB_LLM_DEF_MAX_RESP;
    ctx->temperature = cfg->temperature;
    ctx->max_tokens = cfg->max_tokens;

    lib_http_cfg_t hcfg;
    memset(&hcfg, 0, sizeof(hcfg));
    hcfg.timeout_ms = ctx->timeout_ms;
    hcfg.max_resp_sz = ctx->max_resp_sz;

    char http_err[256] = {0};
    ctx->http = lib_http_open(&hcfg, http_err, sizeof(http_err));
    if (lib_unlikely(ctx->http == NULL)) {
        lib_llm_set_err(ctx, errbuf, errlen,
                        "failed to open HTTP client: %s",
                        http_err[0] != '\0' ? http_err : "unknown error");
        lib_llm_close(ctx);
        return NULL;
    }

    return ctx;
}

void lib_llm_close(lib_llm_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    lib_http_close(ctx->http);
    free(ctx->endpoint);
    free(ctx->api_key);
    free(ctx->model);
    free(ctx);
}

void lib_llm_free_calls(lib_llm_tool_call_t *calls, size_t count)
{
    if (calls == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].args_json);
    }
    free(calls);
}

void lib_llm_free_res(lib_llm_res_t *res)
{
    if (res == NULL) {
        return;
    }
    free(res->text);
    free(res->finish_reason);
    free(res->raw_json);
    lib_llm_free_calls(res->tool_calls, res->tool_count);
    memset(res, 0, sizeof(*res));
}

lib_llm_err_t lib_llm_chat(lib_llm_ctx_t *ctx,
                           const lib_llm_msg_t *msgs,
                           size_t msg_count,
                           const lib_llm_tool_def_t *tools,
                           size_t tool_count,
                           lib_llm_res_t *out_res,
                           char *errbuf,
                           size_t errlen)
{
    if (lib_unlikely(ctx == NULL || ctx->http == NULL ||
                     msgs == NULL || msg_count == 0u || out_res == NULL)) {
        lib_llm_set_err(ctx, errbuf, errlen, "invalid arguments to lib_llm_chat");
        return LIB_LLM_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    int bad_arg = 0;
    char *req_json = lib_llm_build_req_json(ctx, msgs, msg_count, tools, tool_count, &bad_arg);
    if (lib_unlikely(req_json == NULL)) {
        lib_llm_set_err(ctx, errbuf, errlen,
                        bad_arg ? "invalid message or tool definition"
                                : "out of memory serializing chat request");
        return bad_arg ? LIB_LLM_ERR_ARG : LIB_LLM_ERR_MEM;
    }

    lib_http_res_t http_res;
    memset(&http_res, 0, sizeof(http_res));

    lib_llm_err_t hrc = lib_llm_http_post_json(ctx, req_json, &http_res, errbuf, errlen);
    free(req_json);
    if (lib_unlikely(hrc != LIB_LLM_OK)) {
        return hrc;
    }

    const char *body = (http_res.body != NULL) ? http_res.body : "";
    lib_llm_err_t prc = lib_llm_parse_resp(ctx, body, out_res, errbuf, errlen);
    lib_http_free_res(&http_res);
    return prc;
}

const char *lib_llm_errstr(const lib_llm_ctx_t *ctx)
{
    if (ctx == NULL) {
        return "";
    }
    return ctx->last_err;
}

const char *lib_llm_strerror(lib_llm_err_t err)
{
    switch (err) {
    case LIB_LLM_OK:          return "ok";
    case LIB_LLM_ERR_ARG:     return "invalid argument";
    case LIB_LLM_ERR_MEM:     return "out of memory";
    case LIB_LLM_ERR_IO:      return "i/o error";
    case LIB_LLM_ERR_TIMEOUT: return "timed out";
    case LIB_LLM_ERR_PROTO:   return "protocol error";
    case LIB_LLM_ERR_HTTP:    return "http error";
    case LIB_LLM_ERR_API:     return "llm api error";
    default:                  return "unknown error";
    }
}