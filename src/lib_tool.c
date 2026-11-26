/**
 * Copyright (c) 2026 yanrbts
 *
 * Tool registry and dispatcher implementation for libagent.
 */

#include "lib_tool.h"
#include "lib_error.h"
#include "util.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIB_TOOL_INIT_CAP 16u

/* Internal entry representing a single registered tool and its runtime state */
typedef struct lib_tool_entry {
    char            *name;         /* Heap-allocated unique tool name */
    char            *desc;         /* Heap-allocated tool description exposed to the LLM */
    char            *schema_json;  /* Heap-allocated normalized JSON Schema object string */
    char            *tag;          /* Optional heap-allocated group tag (NULL if untagged) */
    int              enabled;      /* Active flag: 1 if included in LLM defs and dispatchable, 0 otherwise */
    lib_tool_fn_t    fn;           /* Execution callback invoked on tool dispatch */
    void            *tool_ctx;     /* Opaque context pointer owned/managed per tool entry */
    lib_tool_dtor_t  dtor;         /* Optional destructor for tool_ctx on removal or registry teardown */
} lib_tool_entry_t;

/* Internal dynamic array backing the opaque lib_tool_reg_t handle */
struct lib_tool_reg {
    lib_tool_entry_t *entries;     /* Heap-allocated array of registered tool entries */
    size_t            count;       /* Number of currently valid entries in entries */
    size_t            cap;         /* Allocated capacity of entries in element count */
};

static int lib_tool_is_blank(const char *s)
{
    if (lib_unlikely(s == NULL)) {
        return 1;
    }
    while (*s != '\0') {
        if (!isspace((unsigned char)*s)) {
            return 0;
        }
        s++;
    }
    return 1;
}

static char *lib_tool_norm_schema(const char *schema_json,
                                   int        *oom)
{
    cJSON *parsed = NULL;

    if (lib_tool_is_blank(schema_json)) {
        return lib_strdup_ex("{}", oom);
    }

    parsed = cJSON_Parse(schema_json);
    if (lib_unlikely(parsed == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_PROTOCOL, "tool schema_json is not valid JSON");
        return NULL;
    }
    if (lib_unlikely(!cJSON_IsObject(parsed))) {
        cJSON_Delete(parsed);
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_PROTOCOL, "tool schema_json must be a JSON object");
        return NULL;
    }
    cJSON_Delete(parsed);

    return lib_strdup_ex(schema_json, oom);
}

static void lib_tool_entry_free(lib_tool_entry_t *e)
{
    if (lib_unlikely(e == NULL)) {
        return;
    }
    if (e->dtor != NULL) {
        e->dtor(e->tool_ctx);
    }
    free(e->name);
    free(e->desc);
    free(e->schema_json);
    free(e->tag);
    memset(e, 0, sizeof(*e));
}

static ssize_t lib_tool_find(const lib_tool_reg_t *reg, const char *name)
{
    size_t i = 0u;

    if (lib_unlikely(reg == NULL || name == NULL)) {
        return -1;
    }
    for (i = 0u; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

const char *lib_tool_strerror(lib_tool_err_t err)
{
    switch (err) {
    case LIB_TOOL_OK:           return "ok";
    case LIB_TOOL_ERR_ARG:      return "invalid argument";
    case LIB_TOOL_ERR_MEM:      return "out of memory";
    case LIB_TOOL_ERR_EXIST:    return "tool already exists";
    case LIB_TOOL_ERR_NOTFOUND: return "tool not found";
    case LIB_TOOL_ERR_DISABLED: return "tool is disabled";
    case LIB_TOOL_ERR_JSON:     return "invalid tool arguments json";
    case LIB_TOOL_ERR_EXEC:     return "tool execution failed";
    default:                    return "unknown tool error";
    }
}

lib_tool_reg_t *lib_tool_new(void)
{
    lib_tool_reg_t *reg = (lib_tool_reg_t *)calloc(1u, sizeof(*reg));

    if (lib_unlikely(reg == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "failed to allocate tool registry");
        return NULL;
    }
    reg->cap = LIB_TOOL_INIT_CAP;
    reg->entries = (lib_tool_entry_t *)calloc(reg->cap, sizeof(*reg->entries));
    if (lib_unlikely(reg->entries == NULL)) {
        free(reg);
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "failed to allocate tool entries");
        return NULL;
    }
    return reg;
}

void lib_tool_free(lib_tool_reg_t *reg)
{
    size_t i = 0u;

    if (lib_unlikely(reg == NULL)) {
        return;
    }
    for (i = 0u; i < reg->count; i++) {
        lib_tool_entry_free(&reg->entries[i]);
    }
    free(reg->entries);
    free(reg);
}

size_t lib_tool_count(const lib_tool_reg_t *reg)
{
    return (reg != NULL) ? reg->count : 0u;
}

size_t lib_tool_enabled_cnt(const lib_tool_reg_t *reg)
{
    size_t i = 0u;
    size_t n = 0u;

    if (lib_unlikely(reg == NULL)) {
        return 0u;
    }
    for (i = 0u; i < reg->count; i++) {
        if (reg->entries[i].enabled) {
            n++;
        }
    }
    return n;
}

int lib_tool_has(const lib_tool_reg_t *reg, const char *name)
{
    return (lib_tool_find(reg, name) >= 0) ? 1 : 0;
}

lib_tool_err_t lib_tool_add(lib_tool_reg_t        *reg,
                            const lib_tool_spec_t *spec)
{
    int               oom         = 0;
    size_t            new_cap     = 0u;
    char             *norm_schema = NULL;
    lib_tool_entry_t *grown       = NULL;
    lib_tool_entry_t  tmp;

    memset(&tmp, 0, sizeof(tmp));

    if (lib_unlikely(reg == NULL || spec == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "registry or tool spec is NULL");
        return LIB_TOOL_ERR_ARG;
    }
    if (lib_unlikely(spec->name == NULL || spec->name[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "tool name must be non-empty");
        return LIB_TOOL_ERR_ARG;
    }
    if (lib_unlikely(spec->fn == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "tool callback fn is required");
        return LIB_TOOL_ERR_ARG;
    }
    if (lib_unlikely(lib_tool_find(reg, spec->name) >= 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_PROTOCOL, "duplicate tool name: %s", spec->name);
        return LIB_TOOL_ERR_EXIST;
    }

    norm_schema = lib_tool_norm_schema(spec->schema_json, &oom);
    if (lib_unlikely(norm_schema == NULL)) {
        if (lib_unlikely(oom)) {
            lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "out of memory normalizing tool schema");
            return LIB_TOOL_ERR_MEM;
        }
        return LIB_TOOL_ERR_ARG;
    }

    if (lib_unlikely(reg->count == reg->cap)) {
        new_cap = reg->cap * 2u;
        if (lib_unlikely(new_cap <= reg->cap)) {
            free(norm_schema);
            lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "tool registry capacity overflow");
            return LIB_TOOL_ERR_MEM;
        }
        grown = (lib_tool_entry_t *)realloc(reg->entries, new_cap * sizeof(*grown));
        if (lib_unlikely(grown == NULL)) {
            free(norm_schema);
            lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "out of memory growing tool registry");
            return LIB_TOOL_ERR_MEM;
        }
        memset(grown + reg->cap, 0, (new_cap - reg->cap) * sizeof(*grown));
        reg->entries = grown;
        reg->cap     = new_cap;
    }

    tmp.name        = lib_strdup_ex(spec->name, &oom);
    tmp.desc        = lib_strdup_ex(spec->desc != NULL ? spec->desc : "", &oom);
    tmp.schema_json = norm_schema;
    tmp.tag         = lib_strdup_ex(spec->tag, &oom);
    tmp.enabled     = (spec->enabled != 0) ? 1 : 0;
    tmp.fn          = spec->fn;
    tmp.tool_ctx    = spec->tool_ctx;
    tmp.dtor        = spec->dtor;

    if (lib_unlikely(oom || tmp.name == NULL || tmp.desc == NULL ||
                     (spec->tag != NULL && tmp.tag == NULL))) {
        free(tmp.name);
        free(tmp.desc);
        free(tmp.schema_json);
        free(tmp.tag);
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "out of memory duplicating tool fields");
        return LIB_TOOL_ERR_MEM;
    }

    reg->entries[reg->count] = tmp;
    reg->count++;
    return LIB_TOOL_OK;
}

lib_tool_err_t lib_tool_del(lib_tool_reg_t *reg,
                            const char     *name)
{
    ssize_t idx = -1;
    size_t  i   = 0u;

    if (lib_unlikely(reg == NULL || name == NULL || name[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "invalid registry or tool name");
        return LIB_TOOL_ERR_ARG;
    }
    idx = lib_tool_find(reg, name);
    if (lib_unlikely(idx < 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOT_FOUND, "tool not found: %s", name);
        return LIB_TOOL_ERR_NOTFOUND;
    }

    lib_tool_entry_free(&reg->entries[(size_t)idx]);
    for (i = (size_t)idx; i + 1u < reg->count; i++) {
        reg->entries[i] = reg->entries[i + 1u];
    }
    reg->count--;
    memset(&reg->entries[reg->count], 0, sizeof(reg->entries[reg->count]));
    return LIB_TOOL_OK;
}

lib_tool_err_t lib_tool_enable(lib_tool_reg_t *reg,
                               const char     *name,
                               int             enabled)
{
    ssize_t idx = -1;

    if (lib_unlikely(reg == NULL || name == NULL || name[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "invalid registry or tool name");
        return LIB_TOOL_ERR_ARG;
    }
    idx = lib_tool_find(reg, name);
    if (lib_unlikely(idx < 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOT_FOUND, "tool not found: %s", name);
        return LIB_TOOL_ERR_NOTFOUND;
    }
    reg->entries[(size_t)idx].enabled = (enabled != 0) ? 1 : 0;
    return LIB_TOOL_OK;
}

lib_tool_err_t lib_tool_enable_tag(lib_tool_reg_t *reg,
                                   const char     *tag,
                                   int             enabled,
                                   size_t         *out_cnt)
{
    size_t i        = 0u;
    size_t affected = 0u;
    int    flag     = (enabled != 0) ? 1 : 0;

    if (lib_unlikely(reg == NULL || tag == NULL || tag[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "invalid registry or tag");
        return LIB_TOOL_ERR_ARG;
    }

    for (i = 0u; i < reg->count; i++) {
        if (reg->entries[i].tag != NULL && strcmp(reg->entries[i].tag, tag) == 0) {
            reg->entries[i].enabled = flag;
            affected++;
        }
    }

    if (out_cnt != NULL) {
        *out_cnt = affected;
    }
    return LIB_TOOL_OK;
}

lib_tool_err_t lib_tool_llm_defs(const lib_tool_reg_t  *reg,
                                 lib_llm_tool_def_t   **out_defs,
                                 size_t                *out_cnt)
{
    size_t              enabled_n = 0u;
    size_t              idx       = 0u;
    size_t              i         = 0u;
    int                 oom       = 0;
    lib_llm_tool_def_t *defs      = NULL;

    if (lib_unlikely(reg == NULL || out_defs == NULL || out_cnt == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "invalid arguments to lib_tool_llm_defs");
        return LIB_TOOL_ERR_ARG;
    }

    *out_defs = NULL;
    *out_cnt  = 0u;

    enabled_n = lib_tool_enabled_cnt(reg);
    if (enabled_n == 0u) {
        return LIB_TOOL_OK;
    }

    defs = (lib_llm_tool_def_t *)calloc(enabled_n, sizeof(*defs));
    if (lib_unlikely(defs == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "out of memory allocating llm tool defs");
        return LIB_TOOL_ERR_MEM;
    }

    for (i = 0u; i < reg->count; i++) {
        const lib_tool_entry_t *e = &reg->entries[i];

        if (!e->enabled) {
            continue;
        }
        defs[idx].name        = lib_strdup_ex(e->name, &oom);
        defs[idx].description = lib_strdup_ex(e->desc, &oom);
        defs[idx].schema_json = lib_strdup_ex(e->schema_json, &oom);
        if (lib_unlikely(oom || defs[idx].name == NULL ||
                         defs[idx].description == NULL || defs[idx].schema_json == NULL)) {
            lib_tool_free_defs(defs, enabled_n);
            lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOMEM, "out of memory duplicating llm tool def");
            return LIB_TOOL_ERR_MEM;
        }
        idx++;
    }

    *out_defs = defs;
    *out_cnt  = enabled_n;
    return LIB_TOOL_OK;
}

void lib_tool_free_defs(lib_llm_tool_def_t *defs, size_t count)
{
    size_t i = 0u;

    if (lib_unlikely(defs == NULL)) {
        return;
    }
    for (i = 0u; i < count; i++) {
        free((void *)defs[i].name);
        free((void *)defs[i].description);
        free((void *)defs[i].schema_json);
    }
    free(defs);
}

lib_tool_err_t lib_tool_exec(const lib_tool_reg_t *reg,
                             const char           *call_id,
                             const char           *name,
                             const char           *args_json,
                             void                 *user_ctx,
                             lib_tool_res_t       *out_res)
{
    ssize_t                 idx            = -1;
    const lib_tool_entry_t *entry          = NULL;
    const char             *effective_args = NULL;
    cJSON                  *parsed_args    = NULL;
    lib_tool_err_t          rc             = LIB_TOOL_OK;
    lib_tool_call_t         call_ctx;

    memset(&call_ctx, 0, sizeof(call_ctx));

    if (lib_unlikely(reg == NULL || name == NULL || name[0] == '\0' || out_res == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "invalid arguments to lib_tool_exec");
        return LIB_TOOL_ERR_ARG;
    }

    memset(out_res, 0, sizeof(*out_res));

    idx = lib_tool_find(reg, name);
    if (lib_unlikely(idx < 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_NOT_FOUND, "unknown tool: %s", name);
        return LIB_TOOL_ERR_NOTFOUND;
    }

    entry = &reg->entries[(size_t)idx];
    if (lib_unlikely(!entry->enabled)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INTERNAL, "tool is disabled: %s", name);
        return LIB_TOOL_ERR_DISABLED;
    }

    effective_args = lib_tool_is_blank(args_json) ? "{}" : args_json;
    parsed_args = cJSON_Parse(effective_args);
    if (lib_unlikely(parsed_args == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_PROTOCOL, "invalid JSON arguments for tool: %s", name);
        return LIB_TOOL_ERR_JSON;
    }
    if (lib_unlikely(!cJSON_IsObject(parsed_args))) {
        cJSON_Delete(parsed_args);
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_PROTOCOL, "tool arguments must be a JSON object: %s", name);
        return LIB_TOOL_ERR_JSON;
    }
    cJSON_Delete(parsed_args);

    call_ctx.call_id   = call_id;
    call_ctx.name      = entry->name;
    call_ctx.args_json = effective_args;
    call_ctx.user_ctx  = user_ctx;

    rc = entry->fn(&call_ctx, entry->tool_ctx, out_res);
    if (lib_unlikely(rc != LIB_TOOL_OK)) {
        lib_tool_res_reset(out_res);
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_TOOL_EXEC, lib_tool_strerror(rc));
        return rc;
    }

    return LIB_TOOL_OK;
}

lib_tool_err_t lib_tool_exec_call(const lib_tool_reg_t      *reg,
                                  const lib_llm_tool_call_t *call,
                                  void                      *user_ctx,
                                  lib_tool_res_t            *out_res)
{
    if (lib_unlikely(call == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG, "tool call is NULL");
        return LIB_TOOL_ERR_ARG;
    }
    return lib_tool_exec(reg,
                         call->id,
                         call->name,
                         call->args_json,
                         user_ctx,
                         out_res);
}

void lib_tool_res_reset(lib_tool_res_t *res)
{
    if (lib_unlikely(res == NULL)) {
        return;
    }
    free(res->content);
    memset(res, 0, sizeof(*res));
}