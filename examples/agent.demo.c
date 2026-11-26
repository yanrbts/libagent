/**
 * Copyright (c) 2026 yanrbts
 *
 * Runnable agent example wired with lib_error, lib_http, lib_llm, lib_tool,
 * and lib_agent against a local Ollama endpoint.
 */

#include "lib_agent.h"
#include "lib_error.h"
#include "lib_http.h"
#include "lib_llm.h"
#include "lib_tool.h"
#include "util.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_OLLAMA_URL   "http://192.168.110.16:11434/v1/chat/completions"
#define DEMO_OLLAMA_MODEL "gemma4:e4b"
#define DEMO_MAX_MSGS     64u

typedef struct demo_msg_item {
    lib_llm_role_t       role;
    char                *content;
    char                *tool_call_id;
    char                *name;
    lib_llm_tool_call_t *tool_calls;
    size_t               tool_count;
} demo_msg_item_t;

typedef struct demo_ctx {
    lib_http_ctx_t      *http;
    lib_llm_ctx_t       *llm;
    lib_tool_reg_t      *tools;
    demo_msg_item_t      history[DEMO_MAX_MSGS];
    size_t               hist_cnt;
    lib_llm_tool_call_t *pending_calls;
    size_t               pending_cnt;
    char                 final_reply[4096];
} demo_ctx_t;

static void app_error_log_sink(const lib_err_t *err, void *userdata)
{
    FILE *out = stderr;

    (void)userdata;
    if (err == NULL) {
        return;
    }

    if (err->severity == LIB_ERR_SEV_WARN) {
        out = stdout;
    }

    (void)fprintf(out, "[%s][%s:%s] retry=%d errno=%d status=%d %s\n",
                  lib_err_severity_str(err->severity),
                  lib_err_domain_str(err->domain),
                  lib_err_code_str(err->code),
                  err->retryable,
                  err->sys_errno,
                  err->status_code,
                  err->msg);
}

static void demo_free_call_array(lib_llm_tool_call_t *calls, size_t cnt)
{
    size_t i = 0u;

    if (calls == NULL || cnt == 0u) {
        return;
    }
    for (i = 0u; i < cnt; i++) {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].args_json);
    }
    free(calls);
}

static lib_llm_tool_call_t *demo_dup_call_array(const lib_llm_tool_call_t *src, size_t cnt)
{
    lib_llm_tool_call_t *dst = NULL;
    size_t               i   = 0u;
    int                  oom = 0;

    if (src == NULL || cnt == 0u) {
        return NULL;
    }

    dst = (lib_llm_tool_call_t *)calloc(cnt, sizeof(*dst));
    if (lib_unlikely(dst == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_NOMEM,
                         "failed to allocate tool call array");
        return NULL;
    }

    for (i = 0u; i < cnt; i++) {
        if (src[i].id != NULL) {
            dst[i].id = lib_strdup_ex(src[i].id, &oom);
            if (lib_unlikely(oom || dst[i].id == NULL)) {
                demo_free_call_array(dst, cnt);
                return NULL;
            }
        }
        if (src[i].name != NULL) {
            dst[i].name = lib_strdup_ex(src[i].name, &oom);
            if (lib_unlikely(oom || dst[i].name == NULL)) {
                demo_free_call_array(dst, cnt);
                return NULL;
            }
        }
        if (src[i].args_json != NULL) {
            dst[i].args_json = lib_strdup_ex(src[i].args_json, &oom);
            if (lib_unlikely(oom || dst[i].args_json == NULL)) {
                demo_free_call_array(dst, cnt);
                return NULL;
            }
        }
    }

    return dst;
}

static void demo_free_history(demo_msg_item_t *items, size_t cnt)
{
    size_t i = 0u;

    if (items == NULL) {
        return;
    }
    for (i = 0u; i < cnt; i++) {
        free(items[i].content);
        free(items[i].tool_call_id);
        free(items[i].name);
        demo_free_call_array(items[i].tool_calls, items[i].tool_count);
    }
}

static int demo_add_msg(demo_ctx_t                *ctx,
                        lib_llm_role_t             role,
                        const char                *content,
                        const char                *tool_call_id,
                        const char                *name,
                        const lib_llm_tool_call_t *calls,
                        size_t                     call_cnt)
{
    demo_msg_item_t *item = NULL;
    int              oom  = 0;

    if (lib_unlikely(ctx == NULL)) {
        return -1;
    }
    if (lib_unlikely(ctx->hist_cnt >= DEMO_MAX_MSGS)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_STATE,
                         "message history reached max capacity (%u)",
                         (unsigned)DEMO_MAX_MSGS);
        return -1;
    }

    item = &ctx->history[ctx->hist_cnt];
    memset(item, 0, sizeof(*item));
    item->role = role;

    if (content != NULL) {
        item->content = lib_strdup_ex(content, &oom);
        if (lib_unlikely(oom || item->content == NULL)) {
            demo_free_history(item, 1u);
            return -1;
        }
    }
    if (tool_call_id != NULL) {
        item->tool_call_id = lib_strdup_ex(tool_call_id, &oom);
        if (lib_unlikely(oom || item->tool_call_id == NULL)) {
            demo_free_history(item, 1u);
            return -1;
        }
    }
    if (name != NULL) {
        item->name = lib_strdup_ex(name, &oom);
        if (lib_unlikely(oom || item->name == NULL)) {
            demo_free_history(item, 1u);
            return -1;
        }
    }
    if (calls != NULL && call_cnt > 0u) {
        item->tool_calls = demo_dup_call_array(calls, call_cnt);
        if (lib_unlikely(item->tool_calls == NULL)) {
            demo_free_history(item, 1u);
            return -1;
        }
        item->tool_count = call_cnt;
    }

    ctx->hist_cnt++;
    return 0;
}

/* Bridge lib_http_ctx_t into lib_llm_io_t transport callback */
static lib_llm_err_t demo_llm_post_cb(void          *io_ctx,
                                      const char    *url,
                                      const char    *api_key,
                                      const char    *body_json,
                                      size_t         body_len,
                                      int            timeout_ms,
                                      size_t         max_resp_sz,
                                      char         **out_body,
                                      size_t        *out_len,
                                      int           *out_status)
{
    lib_http_ctx_t *http = (lib_http_ctx_t *)io_ctx;
    lib_http_res_t  hres;
    lib_http_err_t  hrc  = LIB_HTTP_OK;
    char           *copy = NULL;

    (void)body_len;
    (void)max_resp_sz;

    memset(&hres, 0, sizeof(hres));

    if (lib_unlikely(http == NULL || out_body == NULL || out_len == NULL || out_status == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG,
                         "invalid arguments in demo_llm_post_cb");
        return LIB_LLM_ERR_ARG;
    }

    *out_body   = NULL;
    *out_len    = 0u;
    *out_status = 0;

    hrc = lib_http_post_json(http, url, api_key, body_json, timeout_ms, &hres);
    if (lib_unlikely(hrc != LIB_HTTP_OK)) {
        lib_http_free_res(&hres);
        if (hrc == LIB_HTTP_ERR_TIMEOUT) {
            return LIB_LLM_ERR_TIMEOUT;
        }
        if (hrc == LIB_HTTP_ERR_NOMEM) {
            return LIB_LLM_ERR_MEM;
        }
        return LIB_LLM_ERR_IO;
    }

    *out_status = hres.status_code;
    if (hres.body != NULL && hres.body_len > 0u) {
        copy = (char *)malloc(hres.body_len + 1u);
        if (lib_unlikely(copy == NULL)) {
            lib_http_free_res(&hres);
            lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_NOMEM,
                             "failed to copy HTTP response body");
            return LIB_LLM_ERR_MEM;
        }
        memcpy(copy, hres.body, hres.body_len);
        copy[hres.body_len] = '\0';
        *out_body = copy;
        *out_len  = hres.body_len;
    }

    lib_http_free_res(&hres);
    return LIB_LLM_OK;
}

static lib_tool_err_t demo_tool_host_status(const lib_tool_call_t *call,
                                            void                  *tool_ctx,
                                            lib_tool_res_t        *out_res)
{
    int   oom = 0;
    char *buf = NULL;

    (void)call;
    (void)tool_ctx;

    if (lib_unlikely(out_res == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG,
                         "host_status out_res must not be NULL");
        return LIB_TOOL_ERR_ARG;
    }

    buf = lib_strdup_ex("{\"status\":\"ok\",\"load_1m\":0.35,\"free_mem_mb\":8192}", &oom);
    if (lib_unlikely(oom || buf == NULL)) {
        return LIB_TOOL_ERR_MEM;
    }

    out_res->content  = buf;
    out_res->is_error = 0;
    return LIB_TOOL_OK;
}

static lib_tool_err_t demo_tool_add(const lib_tool_call_t *call,
                                    void                  *tool_ctx,
                                    lib_tool_res_t        *out_res)
{
    cJSON *root = NULL;
    cJSON *ja   = NULL;
    cJSON *jb   = NULL;
    char   tmp[128];
    int    oom  = 0;
    char  *res  = NULL;

    (void)tool_ctx;

    if (lib_unlikely(call == NULL || out_res == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG,
                         "add tool received NULL argument");
        return LIB_TOOL_ERR_ARG;
    }

    root = cJSON_Parse(call->args_json != NULL ? call->args_json : "{}");
    if (lib_unlikely(root == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_PROTOCOL,
                         "add tool received invalid JSON arguments");
        res = lib_strdup_ex("{\"error\":\"invalid json arguments\"}", &oom);
        if (lib_unlikely(oom || res == NULL)) {
            return LIB_TOOL_ERR_MEM;
        }
        out_res->content  = res;
        out_res->is_error = 1;
        return LIB_TOOL_OK;
    }

    ja = cJSON_GetObjectItemCaseSensitive(root, "a");
    jb = cJSON_GetObjectItemCaseSensitive(root, "b");
    if (lib_unlikely(!cJSON_IsNumber(ja) || !cJSON_IsNumber(jb))) {
        cJSON_Delete(root);
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG,
                         "add tool requires numeric fields 'a' and 'b'");
        res = lib_strdup_ex("{\"error\":\"a and b must be numbers\"}", &oom);
        if (lib_unlikely(oom || res == NULL)) {
            return LIB_TOOL_ERR_MEM;
        }
        out_res->content  = res;
        out_res->is_error = 1;
        return LIB_TOOL_OK;
    }

    (void)snprintf(tmp, sizeof(tmp), "{\"result\":%.6g}", ja->valuedouble + jb->valuedouble);
    cJSON_Delete(root);

    res = lib_strdup_ex(tmp, &oom);
    if (lib_unlikely(oom || res == NULL)) {
        return LIB_TOOL_ERR_MEM;
    }

    out_res->content  = res;
    out_res->is_error = 0;
    return LIB_TOOL_OK;
}

static lib_agent_err_t demo_on_llm(void                  *user_ctx,
                                   const char            *agent_name,
                                   size_t                 step_idx,
                                   lib_agent_turn_kind_t *out_kind)
{
    demo_ctx_t         *ctx     = (demo_ctx_t *)user_ctx;
    lib_llm_msg_t      *msgs    = NULL;
    lib_llm_tool_def_t *defs    = NULL;
    size_t              def_cnt = 0u;
    lib_llm_res_t       res;
    lib_tool_err_t      trc     = LIB_TOOL_OK;
    lib_llm_err_t       lrc     = LIB_LLM_OK;
    size_t              i       = 0u;

    (void)agent_name;
    (void)step_idx;

    memset(&res, 0, sizeof(res));

    if (lib_unlikely(ctx == NULL || ctx->llm == NULL || ctx->tools == NULL || out_kind == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "demo_on_llm received NULL context or output pointer");
        return LIB_AGENT_ERR_ARG;
    }

    if (ctx->pending_calls != NULL) {
        demo_free_call_array(ctx->pending_calls, ctx->pending_cnt);
        ctx->pending_calls = NULL;
        ctx->pending_cnt   = 0u;
    }

    if (lib_unlikely(ctx->hist_cnt == 0u)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_STATE,
                         "demo_on_llm invoked with empty message history");
        return LIB_AGENT_ERR_STATE;
    }

    msgs = (lib_llm_msg_t *)calloc(ctx->hist_cnt, sizeof(*msgs));
    if (lib_unlikely(msgs == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_NOMEM,
                         "failed to allocate LLM message array");
        return LIB_AGENT_ERR_MEM;
    }

    for (i = 0u; i < ctx->hist_cnt; i++) {
        msgs[i].role         = ctx->history[i].role;
        msgs[i].content      = ctx->history[i].content;
        msgs[i].tool_call_id = ctx->history[i].tool_call_id;
        msgs[i].name         = ctx->history[i].name;
        msgs[i].tool_calls   = ctx->history[i].tool_calls;
        msgs[i].tool_count   = ctx->history[i].tool_count;
    }

    trc = lib_tool_llm_defs(ctx->tools, &defs, &def_cnt);
    if (lib_unlikely(trc != LIB_TOOL_OK)) {
        free(msgs);
        return LIB_AGENT_ERR_LLM;
    }

    lrc = lib_llm_chat(ctx->llm, msgs, ctx->hist_cnt, defs, def_cnt, &res);
    lib_tool_free_defs(defs, def_cnt);
    free(msgs);

    if (lib_unlikely(lrc != LIB_LLM_OK)) {
        lib_llm_free_res(&res);
        return LIB_AGENT_ERR_LLM;
    }

    if (res.tool_calls != NULL && res.tool_count > 0u) {
        char   names_buf[256];
        size_t pos = 0u;
        size_t k   = 0u;

        names_buf[0] = '\0';
        for (k = 0u; k < res.tool_count; k++) {
            const char *tname = (res.tool_calls[k].name != NULL &&
                                 res.tool_calls[k].name[0] != '\0')
                                    ? res.tool_calls[k].name
                                    : "<unnamed>";
            int n = snprintf(names_buf + pos, sizeof(names_buf) - pos,
                             "%s%s", (k > 0u) ? ", " : "", tname);
            if (n < 0 || (size_t)n >= sizeof(names_buf) - pos) {
                break;
            }
            pos += (size_t)n;
        }

        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_OK,
                         "demo_on_llm: tool_calls (%zu): [%s]",
                         res.tool_count, names_buf);

        if (lib_unlikely(demo_add_msg(ctx, LIB_LLM_ROLE_ASSISTANT, res.text,
                                      NULL, NULL, res.tool_calls, res.tool_count) != 0)) {
            lib_llm_free_res(&res);
            return LIB_AGENT_ERR_MEM;
        }
        ctx->pending_calls = demo_dup_call_array(res.tool_calls, res.tool_count);
        ctx->pending_cnt   = (ctx->pending_calls != NULL) ? res.tool_count : 0u;
        lib_llm_free_res(&res);
        if (lib_unlikely(ctx->pending_calls == NULL)) {
            return LIB_AGENT_ERR_MEM;
        }
        *out_kind = LIB_AGENT_TURN_TOOLS;
        return LIB_AGENT_OK;
    }

    if (res.text != NULL) {
        (void)snprintf(ctx->final_reply, sizeof(ctx->final_reply), "%s", res.text);
        (void)demo_add_msg(ctx, LIB_LLM_ROLE_ASSISTANT, res.text,
                           NULL, NULL, NULL, 0u);
    }
    lib_llm_free_res(&res);
    *out_kind = LIB_AGENT_TURN_FINAL;
    return LIB_AGENT_OK;
}

static lib_agent_err_t demo_on_tools(void       *user_ctx,
                                     const char *agent_name,
                                     size_t      step_idx,
                                     size_t     *out_calls,
                                     size_t     *out_errors)
{
    demo_ctx_t    *ctx    = (demo_ctx_t *)user_ctx;
    size_t         i      = 0u;
    size_t         calls  = 0u;
    size_t         errors = 0u;

    (void)agent_name;
    (void)step_idx;

    if (lib_unlikely(ctx == NULL || ctx->tools == NULL ||
                     out_calls == NULL || out_errors == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "demo_on_tools received NULL pointer");
        return LIB_AGENT_ERR_ARG;
    }

    for (i = 0u; i < ctx->pending_cnt; i++) {
        lib_tool_res_t t_res;
        lib_tool_err_t trc = LIB_TOOL_OK;

        memset(&t_res, 0, sizeof(t_res));
        trc = lib_tool_exec_call(ctx->tools, &ctx->pending_calls[i], ctx, &t_res);
        calls++;

        if (lib_unlikely(trc != LIB_TOOL_OK || t_res.is_error)) {
            errors++;
        }

        if (lib_unlikely(demo_add_msg(ctx, LIB_LLM_ROLE_TOOL,
                                      t_res.content != NULL ? t_res.content : "{\"error\":\"no content\"}",
                                      ctx->pending_calls[i].id,
                                      ctx->pending_calls[i].name,
                                      NULL, 0u) != 0)) {
            lib_tool_res_reset(&t_res);
            demo_free_call_array(ctx->pending_calls, ctx->pending_cnt);
            ctx->pending_calls = NULL;
            ctx->pending_cnt   = 0u;
            return LIB_AGENT_ERR_MEM;
        }

        lib_tool_res_reset(&t_res);
    }

    demo_free_call_array(ctx->pending_calls, ctx->pending_cnt);
    ctx->pending_calls = NULL;
    ctx->pending_cnt   = 0u;

    *out_calls  = calls;
    *out_errors = errors;
    return LIB_AGENT_OK;
}

static int demo_on_step(void                        *user_ctx,
                        const char                  *agent_name,
                        const lib_agent_step_info_t *info)
{
    (void)user_ctx;
    if (info != NULL) {
        (void)printf("[%s] step=%zu state=%s calls=%zu errs=%zu\n",
                     agent_name != NULL ? agent_name : "agent",
                     info->step_idx,
                     lib_agent_state_str(info->state),
                     info->tool_calls,
                     info->tool_errors);
    }
    return 0;
}

int main(int argc, char **argv)
{
    demo_ctx_t            ctx;
    lib_http_cfg_t        hcfg;
    lib_llm_cfg_t         lcfg;
    lib_tool_reg_t       *reg      = NULL;
    lib_tool_spec_t       spec;
    lib_agent_t          *ag       = NULL;
    lib_agent_cfg_t       acfg;
    lib_agent_ops_t       aops;
    lib_agent_step_info_t info;
    lib_agent_err_t       arc      = LIB_AGENT_OK;
    const char           *model    = DEMO_OLLAMA_MODEL;
    const char           *endpoint = DEMO_OLLAMA_URL;
    const char           *prompt   = "Check host status and compute 12 + 34.";
    const char           *env_str  = NULL;

    memset(&ctx, 0, sizeof(ctx));
    memset(&hcfg, 0, sizeof(hcfg));
    memset(&lcfg, 0, sizeof(lcfg));
    memset(&spec, 0, sizeof(spec));
    memset(&acfg, 0, sizeof(acfg));
    memset(&aops, 0, sizeof(aops));
    memset(&info, 0, sizeof(info));

    lib_err_set_log_sink(app_error_log_sink, NULL);

    env_str = getenv("OLLAMA_MODEL");
    if (env_str != NULL && env_str[0] != '\0') {
        model = env_str;
    }
    env_str = getenv("OLLAMA_URL");
    if (env_str != NULL && env_str[0] != '\0') {
        endpoint = env_str;
    }
    if (argc > 1 && argv[1][0] != '\0') {
        model = argv[1];
    }
    if (argc > 2 && argv[2][0] != '\0') {
        prompt = argv[2];
    }

    hcfg.timeout_ms = 30000;
    ctx.http = lib_http_open(&hcfg);
    if (lib_unlikely(ctx.http == NULL)) {
        return 1;
    }

    lcfg.io.post     = demo_llm_post_cb;
    lcfg.io.ctx      = ctx.http;
    lcfg.endpoint    = endpoint;
    lcfg.model       = model;
    lcfg.timeout_ms  = 30000;
    lcfg.temperature = 0.2;

    ctx.llm = lib_llm_open(&lcfg);
    if (lib_unlikely(ctx.llm == NULL)) {
        lib_http_close(ctx.http);
        return 1;
    }

    reg = lib_tool_new();
    if (lib_unlikely(reg == NULL)) {
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    spec.name        = "host_status";
    spec.desc        = "Query host load and free memory";
    spec.schema_json = "{\"type\":\"object\",\"properties\":{}}";
    spec.tag         = "ops";
    spec.enabled     = 1;
    spec.fn          = demo_tool_host_status;
    if (lib_unlikely(lib_tool_add(reg, &spec) != LIB_TOOL_OK)) {
        lib_tool_free(reg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    memset(&spec, 0, sizeof(spec));
    spec.name        = "add";
    spec.desc        = "Add two numbers a and b";
    spec.schema_json = "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"}},\"required\":[\"a\",\"b\"]}";
    spec.tag         = "math";
    spec.enabled     = 1;
    spec.fn          = demo_tool_add;
    if (lib_unlikely(lib_tool_add(reg, &spec) != LIB_TOOL_OK)) {
        lib_tool_free(reg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    ctx.tools = reg;

    if (lib_unlikely(demo_add_msg(&ctx, LIB_LLM_ROLE_SYSTEM,
                                  "You are a helpful assistant. Call tools when needed.",
                                  NULL, NULL, NULL, 0u) != 0 ||
                     demo_add_msg(&ctx, LIB_LLM_ROLE_USER, prompt,
                                  NULL, NULL, NULL, 0u) != 0)) {
        demo_free_history(ctx.history, ctx.hist_cnt);
        lib_tool_free(reg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    acfg.name      = "ollama-agent";
    acfg.max_steps = 8u;
    acfg.user_ctx  = &ctx;

    aops.on_llm   = demo_on_llm;
    aops.on_tools = demo_on_tools;
    aops.on_step  = demo_on_step;

    ag = lib_agent_new(&acfg, &aops);
    if (lib_unlikely(ag == NULL)) {
        demo_free_history(ctx.history, ctx.hist_cnt);
        lib_tool_free(reg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    arc = lib_agent_run(ag, &info);
    if (lib_unlikely(arc != LIB_AGENT_OK)) {
        (void)fprintf(stderr, "agent aborted (%s): %s\n",
                      lib_agent_strerror(arc),
                      lib_err_last()->msg);
    } else if (ctx.final_reply[0] != '\0') {
        (void)printf("\n=== Final Reply ===\n%s\n", ctx.final_reply);
    }

    demo_free_call_array(ctx.pending_calls, ctx.pending_cnt);
    demo_free_history(ctx.history, ctx.hist_cnt);
    lib_agent_free(ag);
    lib_tool_free(reg);
    lib_llm_close(ctx.llm);
    lib_http_close(ctx.http);
    return (arc == LIB_AGENT_OK) ? 0 : 1;
}