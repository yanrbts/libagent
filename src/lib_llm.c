/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */

#include "lib_llm.h"
#include "lib_error.h"
#include "util.h"
#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIB_LLM_DEF_TIMEOUT_MS 30000
#define LIB_LLM_DEF_MAX_RESP   (16u * 1024u * 1024u)

#define LLM_VAL_TOOL_TYPE_FN "function"
#define LLM_VAL_EMPTY_JSON   "{}"
#define LLM_VAL_EMPTY_STR    ""

typedef enum {
    LLM_KEY_MODEL = 0,
    LLM_KEY_TEMPERATURE,
    LLM_KEY_MAX_TOKENS,
    LLM_KEY_MESSAGES,
    LLM_KEY_ROLE,
    LLM_KEY_CONTENT,
    LLM_KEY_NAME,
    LLM_KEY_TOOL_CALL_ID,
    LLM_KEY_TOOL_CALLS,
    LLM_KEY_ID,
    LLM_KEY_TYPE,
    LLM_KEY_FUNCTION,
    LLM_KEY_ARGUMENTS,
    LLM_KEY_TOOLS,
    LLM_KEY_DESCRIPTION,
    LLM_KEY_PARAMETERS,
    LLM_KEY_USAGE,
    LLM_KEY_PROMPT_TOKENS,
    LLM_KEY_COMPLETION_TOKENS,
    LLM_KEY_TOTAL_TOKENS,
    LLM_KEY_ERROR,
    LLM_KEY_MESSAGE,
    LLM_KEY_CHOICES,
    LLM_KEY_FINISH_REASON,
    LLM_KEY_COUNT
} llm_json_key_t;

static const char *const k_llm_json_keys[LLM_KEY_COUNT] = {
    [LLM_KEY_MODEL]             = "model",
    [LLM_KEY_TEMPERATURE]       = "temperature",
    [LLM_KEY_MAX_TOKENS]        = "max_tokens",
    [LLM_KEY_MESSAGES]          = "messages",
    [LLM_KEY_ROLE]              = "role",
    [LLM_KEY_CONTENT]           = "content",
    [LLM_KEY_NAME]              = "name",
    [LLM_KEY_TOOL_CALL_ID]      = "tool_call_id",
    [LLM_KEY_TOOL_CALLS]        = "tool_calls",
    [LLM_KEY_ID]                = "id",
    [LLM_KEY_TYPE]              = "type",
    [LLM_KEY_FUNCTION]          = "function",
    [LLM_KEY_ARGUMENTS]         = "arguments",
    [LLM_KEY_TOOLS]             = "tools",
    [LLM_KEY_DESCRIPTION]       = "description",
    [LLM_KEY_PARAMETERS]        = "parameters",
    [LLM_KEY_USAGE]             = "usage",
    [LLM_KEY_PROMPT_TOKENS]     = "prompt_tokens",
    [LLM_KEY_COMPLETION_TOKENS] = "completion_tokens",
    [LLM_KEY_TOTAL_TOKENS]      = "total_tokens",
    [LLM_KEY_ERROR]             = "error",
    [LLM_KEY_MESSAGE]           = "message",
    [LLM_KEY_CHOICES]           = "choices",
    [LLM_KEY_FINISH_REASON]     = "finish_reason",
};

#define LLM_K(k) k_llm_json_keys[(k)]

static const char *const k_llm_wire_roles[] = {
    [LIB_LLM_ROLE_SYSTEM]    = "system",
    [LIB_LLM_ROLE_USER]      = "user",
    [LIB_LLM_ROLE_ASSISTANT] = "assistant",
    [LIB_LLM_ROLE_TOOL]      = "tool",
};

struct lib_llm_ctx {
    lib_llm_cfg_t cfg;
};

static const char *lib_llm_role_str(lib_llm_role_t role)
{
    switch (role) {
    case LIB_LLM_ROLE_SYSTEM:
    case LIB_LLM_ROLE_USER:
    case LIB_LLM_ROLE_ASSISTANT:
    case LIB_LLM_ROLE_TOOL:
        return k_llm_wire_roles[role];
    default:
        return NULL;
    }
}

static int lib_llm_has_non_ws(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p == NULL) {
        return 0;
    }
    while (*p != '\0') {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            return 1;
        }
        p++;
    }
    return 0;
}

static int lib_llm_attach_obj(cJSON *parent, llm_json_key_t key, cJSON *child)
{
    if (lib_unlikely(parent == NULL || child == NULL)) {
        cJSON_Delete(child);
        return 0;
    }
    if (lib_unlikely(!cJSON_AddItemToObject(parent, LLM_K(key), child))) {
        cJSON_Delete(child);
        return 0;
    }
    return 1;
}

static int lib_llm_push_array(cJSON *arr, cJSON *item)
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

static cJSON *lib_llm_encode_call_item(const lib_llm_tool_call_t *tc)
{
    cJSON      *item = NULL;
    cJSON      *fn   = NULL;
    const char *args;

    if (lib_unlikely(tc == NULL || tc->id == NULL || tc->id[0] == '\0' ||
                     tc->name == NULL || tc->name[0] == '\0')) {
        return NULL;
    }

    item = cJSON_CreateObject();
    fn   = cJSON_CreateObject();
    if (lib_unlikely(item == NULL || fn == NULL)) {
        goto fail;
    }

    args = (tc->args_json != NULL && tc->args_json[0] != '\0')
               ? tc->args_json
               : LLM_VAL_EMPTY_JSON;

    if (!cJSON_AddStringToObject(item, LLM_K(LLM_KEY_ID), tc->id) ||
        !cJSON_AddStringToObject(item, LLM_K(LLM_KEY_TYPE), LLM_VAL_TOOL_TYPE_FN) ||
        !cJSON_AddStringToObject(fn, LLM_K(LLM_KEY_NAME), tc->name) ||
        !cJSON_AddStringToObject(fn, LLM_K(LLM_KEY_ARGUMENTS), args)) {
        goto fail;
    }

    /* transfers ownership of fn */
    if (!lib_llm_attach_obj(item, LLM_KEY_FUNCTION, fn)) {
        fn = NULL;
        goto fail;
    }
    fn = NULL;
    return item;

fail:
    cJSON_Delete(fn);
    cJSON_Delete(item);
    return NULL;
}

static cJSON *lib_llm_encode_msg(const lib_llm_msg_t *msg)
{
    cJSON      *mobj = NULL;
    cJSON      *arr  = NULL;
    const char *rstr;
    size_t      i;

    if (lib_unlikely(msg == NULL)) {
        return NULL;
    }

    rstr = lib_llm_role_str(msg->role);
    if (lib_unlikely(rstr == NULL)) {
        return NULL;
    }

    if (lib_unlikely(msg->tool_calls != NULL && msg->tool_count == 0u)) {
        return NULL;
    }

    mobj = cJSON_CreateObject();
    if (lib_unlikely(mobj == NULL)) {
        return NULL;
    }

    if (!cJSON_AddStringToObject(mobj, LLM_K(LLM_KEY_ROLE), rstr)) {
        goto fail;
    }

    if (msg->content != NULL) {
        if (!cJSON_AddStringToObject(mobj, LLM_K(LLM_KEY_CONTENT), msg->content)) {
            goto fail;
        }
    } else if (msg->role == LIB_LLM_ROLE_TOOL) {
        if (!cJSON_AddStringToObject(mobj, LLM_K(LLM_KEY_CONTENT), LLM_VAL_EMPTY_STR)) {
            goto fail;
        }
    } else if (msg->role == LIB_LLM_ROLE_ASSISTANT && msg->tool_count > 0u) {
        if (!cJSON_AddNullToObject(mobj, LLM_K(LLM_KEY_CONTENT))) {
            goto fail;
        }
    }

    if (msg->name != NULL && msg->name[0] != '\0') {
        if (!cJSON_AddStringToObject(mobj, LLM_K(LLM_KEY_NAME), msg->name)) {
            goto fail;
        }
    }

    if (msg->role == LIB_LLM_ROLE_TOOL) {
        if (lib_unlikely(msg->tool_call_id == NULL || msg->tool_call_id[0] == '\0')) {
            goto fail;
        }
        if (!cJSON_AddStringToObject(mobj, LLM_K(LLM_KEY_TOOL_CALL_ID),
                                     msg->tool_call_id)) {
            goto fail;
        }
    }

    if (msg->role == LIB_LLM_ROLE_ASSISTANT && msg->tool_count > 0u) {
        if (lib_unlikely(msg->tool_calls == NULL)) {
            goto fail;
        }
        arr = cJSON_CreateArray();
        if (lib_unlikely(arr == NULL)) {
            goto fail;
        }
        for (i = 0; i < msg->tool_count; i++) {
            cJSON *call_obj = lib_llm_encode_call_item(&msg->tool_calls[i]);
            if (lib_unlikely(call_obj == NULL || !lib_llm_push_array(arr, call_obj))) {
                goto fail;
            }
        }
        if (!lib_llm_attach_obj(mobj, LLM_KEY_TOOL_CALLS, arr)) {
            arr = NULL;
            goto fail;
        }
        arr = NULL;
    }

    return mobj;

fail:
    cJSON_Delete(arr);
    cJSON_Delete(mobj);
    return NULL;
}

static cJSON *lib_llm_encode_tool_def(const lib_llm_tool_def_t *tdef)
{
    cJSON *schema = NULL;
    cJSON *item   = NULL;
    cJSON *fn     = NULL;

    if (lib_unlikely(tdef == NULL || tdef->name == NULL || tdef->name[0] == '\0')) {
        return NULL;
    }

    if (lib_llm_has_non_ws(tdef->schema_json)) {
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

    item = cJSON_CreateObject();
    fn   = cJSON_CreateObject();
    if (lib_unlikely(item == NULL || fn == NULL)) {
        goto fail;
    }

    if (!cJSON_AddStringToObject(item, LLM_K(LLM_KEY_TYPE), LLM_VAL_TOOL_TYPE_FN) ||
        !cJSON_AddStringToObject(fn, LLM_K(LLM_KEY_NAME), tdef->name)) {
        goto fail;
    }

    if (tdef->description != NULL && tdef->description[0] != '\0') {
        if (!cJSON_AddStringToObject(fn, LLM_K(LLM_KEY_DESCRIPTION),
                                     tdef->description)) {
            goto fail;
        }
    }

    if (!lib_llm_attach_obj(fn, LLM_KEY_PARAMETERS, schema)) {
        schema = NULL;
        goto fail;
    }
    schema = NULL;

    if (!lib_llm_attach_obj(item, LLM_KEY_FUNCTION, fn)) {
        fn = NULL;
        goto fail;
    }
    fn = NULL;
    return item;

fail:
    cJSON_Delete(schema);
    cJSON_Delete(fn);
    cJSON_Delete(item);
    return NULL;
}

static char *lib_llm_build_req_json(const lib_llm_ctx_t *ctx,
                                    const lib_llm_msg_t *msgs,
                                    size_t msg_count,
                                    const lib_llm_tool_def_t *tools,
                                    size_t tool_count,
                                    int *bad_arg,
                                    int *bad_is_tool,
                                    size_t *bad_index)
{
    cJSON *root = NULL;
    cJSON *marr = NULL;
    cJSON *tarr = NULL;
    char  *body = NULL;
    size_t i;

    *bad_arg = 0;
    if (bad_is_tool != NULL) {
        *bad_is_tool = 0;
    }
    if (bad_index != NULL) {
        *bad_index = (size_t)-1;
    }

    root = cJSON_CreateObject();
    marr = cJSON_CreateArray();
    if (lib_unlikely(root == NULL || marr == NULL)) {
        goto fail;
    }

    if (!cJSON_AddStringToObject(root, LLM_K(LLM_KEY_MODEL), ctx->cfg.model)) {
        goto fail;
    }

    if (ctx->cfg.temperature >= 0.0) {
        if (!cJSON_AddNumberToObject(root, LLM_K(LLM_KEY_TEMPERATURE),
                                     ctx->cfg.temperature)) {
            goto fail;
        }
    }

    if (ctx->cfg.max_tokens > 0) {
        if (!cJSON_AddNumberToObject(root, LLM_K(LLM_KEY_MAX_TOKENS),
                                     ctx->cfg.max_tokens)) {
            goto fail;
        }
    }

    for (i = 0; i < msg_count; i++) {
        cJSON *mobj = lib_llm_encode_msg(&msgs[i]);
        if (lib_unlikely(mobj == NULL)) {
            *bad_arg = 1;
            if (bad_is_tool != NULL) {
                *bad_is_tool = 0;
            }
            if (bad_index != NULL) {
                *bad_index = i;
            }
            goto fail;
        }
        if (lib_unlikely(!lib_llm_push_array(marr, mobj))) {
            goto fail;
        }
    }

    if (lib_unlikely(!lib_llm_attach_obj(root, LLM_KEY_MESSAGES, marr))) {
        marr = NULL;
        goto fail;
    }
    marr = NULL;

    if (tools != NULL && tool_count > 0u) {
        tarr = cJSON_CreateArray();
        if (lib_unlikely(tarr == NULL)) {
            goto fail;
        }
        for (i = 0; i < tool_count; i++) {
            cJSON *tobj = lib_llm_encode_tool_def(&tools[i]);
            if (lib_unlikely(tobj == NULL)) {
                *bad_arg = 1;
                if (bad_is_tool != NULL) {
                    *bad_is_tool = 1;
                }
                if (bad_index != NULL) {
                    *bad_index = i;
                }
                goto fail;
            }
            if (lib_unlikely(!lib_llm_push_array(tarr, tobj))) {
                goto fail;
            }
        }
        if (lib_unlikely(!lib_llm_attach_obj(root, LLM_KEY_TOOLS, tarr))) {
            tarr = NULL;
            goto fail;
        }
        tarr = NULL;
    }

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;

fail:
    cJSON_Delete(tarr);
    cJSON_Delete(marr);
    cJSON_Delete(root);
    return NULL;
}

static void lib_llm_extract_http_err_detail(const char *body, char *out, size_t out_sz)
{
    cJSON       *root;
    const cJSON *err_obj;
    const cJSON *msg;

    if (out == NULL || out_sz == 0u) {
        return;
    }
    out[0] = '\0';
    if (body == NULL || body[0] == '\0') {
        return;
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        return;
    }

    err_obj = cJSON_GetObjectItemCaseSensitive(root, LLM_K(LLM_KEY_ERROR));
    if (cJSON_IsObject(err_obj)) {
        msg = cJSON_GetObjectItemCaseSensitive(err_obj, LLM_K(LLM_KEY_MESSAGE));
        if (cJSON_IsString(msg) && msg->valuestring != NULL &&
            msg->valuestring[0] != '\0') {
            (void)snprintf(out, out_sz, ": %s", msg->valuestring);
        }
    }

    cJSON_Delete(root);
}

static lib_llm_err_t lib_llm_parse_calls(lib_llm_ctx_t *ctx,
                                         const cJSON *calls_arr,
                                         lib_llm_res_t *out_res)
{
    (void)ctx;

    size_t               n;
    size_t               idx;
    lib_llm_tool_call_t *list;
    const cJSON         *item;

    if (!cJSON_IsArray(calls_arr)) {
        return LIB_LLM_OK;
    }

    n = (size_t)cJSON_GetArraySize(calls_arr);
    if (n == 0u) {
        return LIB_LLM_OK;
    }

    list = (lib_llm_tool_call_t *)calloc(n, sizeof(*list));
    if (lib_unlikely(list == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory allocating tool_calls");
        return LIB_LLM_ERR_MEM;
    }

    idx = 0;
    cJSON_ArrayForEach(item, calls_arr) {
        const cJSON *id_j;
        const cJSON *fn_j;
        const cJSON *name_j;
        const cJSON *args_j;
        const char  *id_str;
        int          oom = 0;

        if (lib_unlikely(!cJSON_IsObject(item))) {
            lib_llm_free_calls(list, idx);
            lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_PROTOCOL, "tool_calls[%zu] is not an object", idx);
            return LIB_LLM_ERR_PROTO;
        }

        id_j   = cJSON_GetObjectItemCaseSensitive(item, LLM_K(LLM_KEY_ID));
        fn_j   = cJSON_GetObjectItemCaseSensitive(item, LLM_K(LLM_KEY_FUNCTION));
        name_j = cJSON_IsObject(fn_j)
                     ? cJSON_GetObjectItemCaseSensitive(fn_j, LLM_K(LLM_KEY_NAME))
                     : NULL;
        args_j = cJSON_IsObject(fn_j)
                     ? cJSON_GetObjectItemCaseSensitive(fn_j, LLM_K(LLM_KEY_ARGUMENTS))
                     : NULL;

        if (lib_unlikely(!cJSON_IsString(name_j) || name_j->valuestring == NULL ||
                         name_j->valuestring[0] == '\0')) {
            lib_llm_free_calls(list, idx);
            lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOT_FOUND,
                            "tool_calls[%zu] missing function.name", idx);
            return LIB_LLM_ERR_PROTO;
        }

        id_str = (cJSON_IsString(id_j) && id_j->valuestring != NULL)
                     ? id_j->valuestring
                     : LLM_VAL_EMPTY_STR;

        list[idx].id   = lib_strdup_ex(id_str, &oom);
        list[idx].name = lib_strdup_ex(name_j->valuestring, &oom);

        if (cJSON_IsString(args_j) && args_j->valuestring != NULL) {
            list[idx].args_json = lib_strdup_ex(args_j->valuestring, &oom);
        } else if (cJSON_IsObject(args_j)) {
            char *raw_args = cJSON_PrintUnformatted(args_j);
            if (raw_args == NULL) {
                oom = 1;
            } else {
                list[idx].args_json = raw_args;
            }
        } else {
            list[idx].args_json = lib_strdup_ex(LLM_VAL_EMPTY_JSON, &oom);
        }

        if (lib_unlikely(oom != 0)) {
            lib_llm_free_calls(list, idx + 1u);
            lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM,
                            "out of memory parsing tool_calls[%zu]", idx);
            return LIB_LLM_ERR_MEM;
        }
        idx++;
    }

    out_res->tool_calls = list;
    out_res->tool_count = n;
    return LIB_LLM_OK;
}

static void lib_llm_parse_usage(const cJSON *root, lib_llm_usage_t *usage)
{
    const cJSON *u;
    const cJSON *p;
    const cJSON *c;
    const cJSON *t;

    u = cJSON_GetObjectItemCaseSensitive(root, LLM_K(LLM_KEY_USAGE));
    if (!cJSON_IsObject(u)) {
        return;
    }

    p = cJSON_GetObjectItemCaseSensitive(u, LLM_K(LLM_KEY_PROMPT_TOKENS));
    c = cJSON_GetObjectItemCaseSensitive(u, LLM_K(LLM_KEY_COMPLETION_TOKENS));
    t = cJSON_GetObjectItemCaseSensitive(u, LLM_K(LLM_KEY_TOTAL_TOKENS));

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

static lib_llm_err_t lib_llm_parse_resp(lib_llm_ctx_t *ctx,
                                        const char *raw_body,
                                        lib_llm_res_t *out_res)
{
    cJSON        *root = NULL;
    const cJSON  *api_err;
    const cJSON  *choices;
    const cJSON  *first;
    const cJSON  *finish;
    const cJSON  *msg;
    const cJSON  *content;
    const cJSON  *calls;
    int           oom = 0;
    lib_llm_err_t crc;

    root = cJSON_Parse(raw_body);
    if (lib_unlikely(root == NULL || !cJSON_IsObject(root))) {
        cJSON_Delete(root);
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_PROTOCOL, "invalid JSON in LLM response");
        return LIB_LLM_ERR_PROTO;
    }

    api_err = cJSON_GetObjectItemCaseSensitive(root, LLM_K(LLM_KEY_ERROR));
    if (cJSON_IsObject(api_err)) {
        const cJSON *emsg = cJSON_GetObjectItemCaseSensitive(api_err, LLM_K(LLM_KEY_MESSAGE));
        out_res->raw_json = lib_strdup_ex(raw_body, NULL);
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_PROTOCOL, "LLM API error: %s",
                        (cJSON_IsString(emsg) && emsg->valuestring != NULL)
                            ? emsg->valuestring
                            : "unknown error");
        cJSON_Delete(root);
        return LIB_LLM_ERR_API;
    }

    out_res->raw_json = lib_strdup_ex(raw_body, &oom);
    if (lib_unlikely(oom != 0)) {
        cJSON_Delete(root);
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory storing raw_json");
        return LIB_LLM_ERR_MEM;
    }

    lib_llm_parse_usage(root, &out_res->usage);

    choices = cJSON_GetObjectItemCaseSensitive(root, LLM_K(LLM_KEY_CHOICES));
    first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (lib_unlikely(!cJSON_IsObject(first))) {
        cJSON_Delete(root);
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_PROTOCOL, "LLM response missing choices[0]");
        return LIB_LLM_ERR_PROTO;
    }

    finish = cJSON_GetObjectItemCaseSensitive(first, LLM_K(LLM_KEY_FINISH_REASON));
    if (cJSON_IsString(finish) && finish->valuestring != NULL) {
        out_res->finish_reason = lib_strdup_ex(finish->valuestring, &oom);
        if (lib_unlikely(oom != 0)) {
            cJSON_Delete(root);
            lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory parsing finish_reason");
            return LIB_LLM_ERR_MEM;
        }
    }

    msg = cJSON_GetObjectItemCaseSensitive(first, LLM_K(LLM_KEY_MESSAGE));
    if (lib_unlikely(!cJSON_IsObject(msg))) {
        cJSON_Delete(root);
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_PROTOCOL, "LLM choice missing message object");
        return LIB_LLM_ERR_PROTO;
    }

    content = cJSON_GetObjectItemCaseSensitive(msg, LLM_K(LLM_KEY_CONTENT));
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        out_res->text = lib_strdup_ex(content->valuestring, &oom);
        if (lib_unlikely(oom != 0)) {
            cJSON_Delete(root);
            lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory copying message content");
            return LIB_LLM_ERR_MEM;
        }
    }

    calls = cJSON_GetObjectItemCaseSensitive(msg, LLM_K(LLM_KEY_TOOL_CALLS));
    crc   = lib_llm_parse_calls(ctx, calls, out_res);
    cJSON_Delete(root);
    return crc;
}

static int lib_llm_cfg_init_owned(lib_llm_cfg_t *dst, const lib_llm_cfg_t *src, int *oom)
{
    memset(dst, 0, sizeof(*dst));

    dst->io          = src->io;
    dst->timeout_ms  = (src->timeout_ms > 0) ? src->timeout_ms : LIB_LLM_DEF_TIMEOUT_MS;
    dst->max_resp_sz = (src->max_resp_sz > 0u) ? src->max_resp_sz : LIB_LLM_DEF_MAX_RESP;
    dst->temperature = src->temperature;
    dst->max_tokens  = src->max_tokens;

    dst->endpoint = lib_strdup_ex(src->endpoint, oom);
    dst->model    = lib_strdup_ex(src->model, oom);
    if (src->api_key != NULL && src->api_key[0] != '\0') {
        dst->api_key = lib_strdup_ex(src->api_key, oom);
    }

    if (*oom != 0 || dst->endpoint == NULL || dst->model == NULL) {
        *oom = 1;
        return -1;
    }
    return 0;
}

lib_llm_ctx_t *lib_llm_open(const lib_llm_cfg_t *cfg)
{
    lib_llm_ctx_t *ctx;
    int            oom = 0;

    if (lib_unlikely(cfg == NULL || cfg->io.post == NULL ||
                     cfg->endpoint == NULL || cfg->endpoint[0] == '\0' ||
                     cfg->model == NULL || cfg->model[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG, "invalid lib_llm_cfg_t");
        return NULL;
    }

    ctx = (lib_llm_ctx_t *)calloc(1, sizeof(*ctx));
    if (lib_unlikely(ctx == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory allocating lib_llm_ctx_t");
        return NULL;
    }

    if (lib_unlikely(lib_llm_cfg_init_owned(&ctx->cfg, cfg, &oom) != 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory duplicating config strings");
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
    free((void *)ctx->cfg.endpoint);
    free((void *)ctx->cfg.model);
    free((void *)ctx->cfg.api_key);
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

void lib_llm_free_calls(lib_llm_tool_call_t *calls, size_t count)
{
    size_t i;

    if (calls == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].args_json);
        calls[i].id = calls[i].name = calls[i].args_json = NULL;
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
                           lib_llm_res_t *out_res)
{
    char         *req_json    = NULL;
    char         *resp_body   = NULL;
    size_t        req_len;
    size_t        resp_len    = 0u;
    size_t        bad_index   = (size_t)-1;
    int           http_status = 0;
    int           bad_arg     = 0;
    int           bad_is_tool = 0;
    lib_llm_err_t irc;
    lib_llm_err_t prc;
    const char   *body;

    if (lib_unlikely(ctx == NULL || ctx->cfg.io.post == NULL ||
                     msgs == NULL || msg_count == 0u || out_res == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG, "invalid arguments to lib_llm_chat");
        return LIB_LLM_ERR_ARG;
    }

    if (lib_unlikely(tools == NULL && tool_count > 0u)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG, "tools is NULL but tool_count > 0");
        return LIB_LLM_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    req_json = lib_llm_build_req_json(ctx, msgs, msg_count, tools, tool_count,
                                      &bad_arg, &bad_is_tool, &bad_index);
    if (lib_unlikely(req_json == NULL)) {
        if (bad_arg) {
            if (bad_is_tool) {
                lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG, "invalid tool definition at index %zu", bad_index);
            } else {
                lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG, "invalid message at index %zu", bad_index);
            }
            return LIB_LLM_ERR_ARG;
        }
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM, "out of memory serializing chat request");
        return LIB_LLM_ERR_MEM;
    }

    req_len = strlen(req_json);
    irc = ctx->cfg.io.post(ctx->cfg.io.ctx,
                           ctx->cfg.endpoint,
                           ctx->cfg.api_key,
                           req_json,
                           req_len,
                           ctx->cfg.timeout_ms,
                           ctx->cfg.max_resp_sz,
                           &resp_body,
                           &resp_len,
                           &http_status);
    free(req_json);
    req_json = NULL;

    if (lib_unlikely(irc != LIB_LLM_OK)) {
        free(resp_body);
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INTERNAL,
                        "transport failed for '%s': %s",
                        ctx->cfg.endpoint,
                        lib_llm_strerror(irc));
        return irc;
    }

    if (lib_unlikely(http_status < 200 || http_status >= 300)) {
        char detail[192];

        lib_llm_extract_http_err_detail(resp_body, detail, sizeof(detail));
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_PROTOCOL,
                        "HTTP status %d from '%s'%s",
                        http_status, ctx->cfg.endpoint, detail);
        free(resp_body);
        return LIB_LLM_ERR_HTTP;
    }

    body = (resp_body != NULL) ? resp_body : LLM_VAL_EMPTY_STR;
    prc  = lib_llm_parse_resp(ctx, body, out_res);
    free(resp_body);

    if (lib_unlikely(prc != LIB_LLM_OK)) {
        lib_llm_free_res(out_res);
    }
    return prc;
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