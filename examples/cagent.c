/**
 * Copyright (c) 2026 yanrbts
 *
 * Industrial-grade penetration-testing agent wired with lib_error, lib_http,
 * lib_llm, lib_tool, lib_skill, lib_agent, and lib_loop against Docker sandbox.
 */
#include "lib_agent.h"
#include "lib_error.h"
#include "lib_http.h"
#include "lib_llm.h"
#include "lib_loop.h"
#include "lib_skill.h"
#include "lib_tool.h"
#include "util.h"
#include "cJSON.h"

#include <errno.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PENTEST_OLLAMA_URL       "http://192.168.110.16:11434/v1/chat/completions"
#define PENTEST_OLLAMA_MODEL     "gemma4:e4b"
#define PENTEST_CONTAINER_ID     "strix-pentest-sandbox"
#define PENTEST_ALLOWED_SCOPE    "192.168.56.10"

#define PENTEST_MAX_MSGS         48u
#define PENTEST_TOOL_MAX_OUT     2048u
#define PENTEST_DEFAULT_CMD_TO_S 90
#define PENTEST_DEFAULT_LLM_TO_M 180000
#define PENTEST_DEFAULT_HTT_TO_M 180000

typedef struct pentest_msg_item {
    lib_llm_role_t       role;
    char                *content;
    char                *tool_call_id;
    char                *name;
    lib_llm_tool_call_t *tool_calls;
    size_t               tool_count;
} pentest_msg_item_t;

typedef struct pentest_app_ctx {
    lib_loop_t            *loop;
    const lib_skill_reg_t *skills;
    lib_http_ctx_t        *http;
    lib_llm_ctx_t         *llm;
    lib_tool_reg_t        *tools;

    const char            *agent_name;
    const char            *role_tag;
    const char            *next_agent;
    const char            *container_id;
    const char            *allowed_scope;
    int                    cmd_timeout_sec;

    int                    task_loaded;
    int                    finalized;
    char                   target[256];
    pentest_msg_item_t     history[PENTEST_MAX_MSGS];
    size_t                 hist_cnt;
    lib_llm_tool_call_t   *pending_calls;
    size_t                 pending_cnt;
    char                   final_report[8192];
} pentest_app_ctx_t;

static int pentest_parse_positive_int(const char *str, int default_val, int min_val, int max_val)
{
    char *endptr = NULL;
    long  val    = 0L;

    if (str == NULL || str[0] == '\0') {
        return default_val;
    }
    errno = 0;
    val = strtol(str, &endptr, 10);
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return default_val;
    }
    if (val < (long)min_val) {
        return min_val;
    }
    if (val > (long)max_val) {
        return max_val;
    }
    return (int)val;
}

static void pentest_trim_str(char *buf)
{
    size_t len   = 0u;
    size_t start = 0u;

    if (buf == NULL || buf[0] == '\0') {
        return;
    }
    len = strlen(buf);
    while (len > 0u && (buf[len - 1u] == '\n' || buf[len - 1u] == '\r' ||
                        buf[len - 1u] == ' '  || buf[len - 1u] == '\t')) {
        buf[--len] = '\0';
    }
    while (buf[start] == ' ' || buf[start] == '\t' ||
           buf[start] == '\r' || buf[start] == '\n') {
        start++;
    }
    if (start > 0u) {
        memmove(buf, buf + start, len - start + 1u);
    }
}

static void pentest_err_sink(const lib_err_t *err, void *userdata)
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

static void pentest_free_calls(lib_llm_tool_call_t *calls, size_t cnt)
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

static lib_llm_tool_call_t *pentest_dup_calls(const lib_llm_tool_call_t *src, size_t cnt)
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
                pentest_free_calls(dst, cnt);
                return NULL;
            }
        }
        if (src[i].name != NULL) {
            dst[i].name = lib_strdup_ex(src[i].name, &oom);
            if (lib_unlikely(oom || dst[i].name == NULL)) {
                pentest_free_calls(dst, cnt);
                return NULL;
            }
        }
        if (src[i].args_json != NULL) {
            dst[i].args_json = lib_strdup_ex(src[i].args_json, &oom);
            if (lib_unlikely(oom || dst[i].args_json == NULL)) {
                pentest_free_calls(dst, cnt);
                return NULL;
            }
        }
    }
    return dst;
}

static void pentest_free_msgs(pentest_msg_item_t *items, size_t cnt)
{
    size_t i = 0u;

    if (items == NULL) {
        return;
    }
    for (i = 0u; i < cnt; i++) {
        free(items[i].content);
        free(items[i].tool_call_id);
        free(items[i].name);
        pentest_free_calls(items[i].tool_calls, items[i].tool_count);
    }
}

/* Evict oldest complete turn(s) after keep_head so assistant(tool_calls) and
 * corresponding tool messages stay paired. */
static int pentest_compact_history(pentest_app_ctx_t *ctx)
{
    size_t keep_head = (ctx->hist_cnt >= 2u) ? 2u : ctx->hist_cnt;
    size_t drop_cnt  = 0u;
    size_t tail_cnt  = 0u;

    if (ctx->hist_cnt < PENTEST_MAX_MSGS) {
        return 0;
    }
    if (ctx->hist_cnt <= keep_head + 1u) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_STATE,
                         "message history full without evictable turns");
        return -1;
    }

    /* Advance drop_cnt until we land at a non-TOOL message boundary */
    drop_cnt = 1u;
    while (keep_head + drop_cnt < ctx->hist_cnt &&
           ctx->history[keep_head + drop_cnt].role == LIB_LLM_ROLE_TOOL) {
        drop_cnt++;
    }
    if (keep_head + drop_cnt >= ctx->hist_cnt) {
        drop_cnt = ctx->hist_cnt - keep_head - 1u;
    }

    pentest_free_msgs(&ctx->history[keep_head], drop_cnt);

    tail_cnt = ctx->hist_cnt - (keep_head + drop_cnt);
    if (tail_cnt > 0u) {
        memmove(&ctx->history[keep_head],
                &ctx->history[keep_head + drop_cnt],
                tail_cnt * sizeof(ctx->history[0]));
    }
    ctx->hist_cnt = keep_head + tail_cnt;
    memset(&ctx->history[ctx->hist_cnt], 0,
           (PENTEST_MAX_MSGS - ctx->hist_cnt) * sizeof(ctx->history[0]));
    return 0;
}

static int pentest_add_msg(pentest_app_ctx_t          *ctx,
                           lib_llm_role_t               role,
                           const char                  *content,
                           const char                  *tool_call_id,
                           const char                  *name,
                           const lib_llm_tool_call_t   *calls,
                           size_t                       call_cnt)
{
    pentest_msg_item_t *item = NULL;
    int                 oom  = 0;

    if (lib_unlikely(ctx == NULL)) {
        return -1;
    }
    if (ctx->hist_cnt >= PENTEST_MAX_MSGS && pentest_compact_history(ctx) != 0) {
        return -1;
    }

    item = &ctx->history[ctx->hist_cnt];
    memset(item, 0, sizeof(*item));
    item->role = role;

    if (content != NULL) {
        item->content = lib_strdup_ex(content, &oom);
        if (lib_unlikely(oom || item->content == NULL)) {
            pentest_free_msgs(item, 1u);
            return -1;
        }
    }
    if (tool_call_id != NULL) {
        item->tool_call_id = lib_strdup_ex(tool_call_id, &oom);
        if (lib_unlikely(oom || item->tool_call_id == NULL)) {
            pentest_free_msgs(item, 1u);
            return -1;
        }
    }
    if (name != NULL) {
        item->name = lib_strdup_ex(name, &oom);
        if (lib_unlikely(oom || item->name == NULL)) {
            pentest_free_msgs(item, 1u);
            return -1;
        }
    }
    if (calls != NULL && call_cnt > 0u) {
        item->tool_calls = pentest_dup_calls(calls, call_cnt);
        if (lib_unlikely(item->tool_calls == NULL)) {
            pentest_free_msgs(item, 1u);
            return -1;
        }
        item->tool_count = call_cnt;
    }

    ctx->hist_cnt++;
    return 0;
}

static void pentest_reset_run_state(pentest_app_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pentest_free_calls(ctx->pending_calls, ctx->pending_cnt);
    ctx->pending_calls = NULL;
    ctx->pending_cnt   = 0u;
    pentest_free_msgs(ctx->history, ctx->hist_cnt);
    memset(ctx->history, 0, sizeof(ctx->history));
    ctx->hist_cnt        = 0u;
    ctx->task_loaded     = 0;
    ctx->finalized       = 0;
    ctx->target[0]       = '\0';
    ctx->final_report[0] = '\0';
}

/* Bridge lib_http_ctx_t into lib_llm_io_t */
static lib_llm_err_t pentest_llm_post_cb(void          *io_ctx,
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
    if (lib_unlikely(http == NULL || body_json == NULL ||
                     out_body == NULL || out_len == NULL || out_status == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LLM, LIB_ERR_INVALID_ARG,
                         "invalid arguments in pentest_llm_post_cb");
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
                             "failed to copy LLM HTTP response");
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

static int pentest_decode_wait_status(int raw_status)
{
    if (raw_status < 0) {
        return -1;
    }
    if (WIFEXITED(raw_status)) {
        return WEXITSTATUS(raw_status);
    }
    if (WIFSIGNALED(raw_status)) {
        return 128 + WTERMSIG(raw_status);
    }
    return -1;
}

static int pentest_pclose_retry(FILE *fp)
{
    int status = -1;

    if (fp == NULL) {
        return -1;
    }
    do {
        errno  = 0;
        status = pclose(fp);
    } while (status < 0 && errno == EINTR);
    return status;
}

/* Tool registered in lib_tool: runs a scoped command inside the Docker sandbox */
static lib_tool_err_t pentest_tool_sandbox_exec(const lib_tool_call_t *call,
                                                void                  *tool_ctx,
                                                lib_tool_res_t        *out_res)
{
    pentest_app_ctx_t *ctx = (pentest_app_ctx_t *)tool_ctx;
    cJSON             *root = NULL;
    cJSON             *jcmd = NULL;
    const char        *cmd  = NULL;
    char               quoted[2048];
    char               docker_cmd[2800];
    char               out_buf[PENTEST_TOOL_MAX_OUT];
    FILE              *fp   = NULL;
    size_t             used = 0u;
    size_t             i    = 0u;
    size_t             q    = 0u;
    int                exit_code = -1;
    int                oom  = 0;
    char              *payload = NULL;

    if (ctx == NULL && call != NULL) {
        ctx = (pentest_app_ctx_t *)call->user_ctx;
    }
    if (lib_unlikely(call == NULL || out_res == NULL || ctx == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG,
                        "sandbox_exec received NULL call, out_res, or context");
        return LIB_TOOL_ERR_ARG;
    }

    if (lib_unlikely(ctx->container_id == NULL || ctx->container_id[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_INVALID_ARG,
                         "sandbox_exec missing container_id in tool_ctx");
        return LIB_TOOL_ERR_ARG;
    }

    root = cJSON_Parse(call->args_json != NULL ? call->args_json : "{}");
    jcmd = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "cmd") : NULL;
    if (lib_unlikely(!cJSON_IsString(jcmd) || jcmd->valuestring == NULL ||
                     jcmd->valuestring[0] == '\0')) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        payload = lib_strdup_ex("{\"error\":\"missing non-empty 'cmd' string\"}", &oom);
        if (lib_unlikely(oom || payload == NULL)) {
            return LIB_TOOL_ERR_MEM;
        }
        out_res->content  = payload;
        out_res->is_error = 1;
        return LIB_TOOL_OK;
    }
    cmd = jcmd->valuestring;

    /* Enforce target scope so sandbox commands stay within authorized target */
    if (ctx->allowed_scope == NULL || ctx->allowed_scope[0] == '\0' ||
        strstr(cmd, ctx->allowed_scope) == NULL) {
        cJSON_Delete(root);
        (void)snprintf(out_buf, sizeof(out_buf),
                       "{\"error\":\"command outside allowed scope\",\"scope\":\"%s\"}",
                       ctx->allowed_scope ? ctx->allowed_scope : "");
        payload = lib_strdup_ex(out_buf, &oom);
        if (lib_unlikely(oom || payload == NULL)) {
            return LIB_TOOL_ERR_MEM;
        }
        out_res->content  = payload;
        out_res->is_error = 1;
        return LIB_TOOL_OK;
    }

    for (i = 0u; cmd[i] != '\0' && q + 4u < sizeof(quoted); i++) {
        if (cmd[i] == '\'') {
            memcpy(&quoted[q], "'\\''", 4u);
            q += 4u;
        } else {
            quoted[q++] = cmd[i];
        }
    }
    quoted[q] = '\0';
    cJSON_Delete(root);

    (void)snprintf(docker_cmd, sizeof(docker_cmd),
                   "timeout %d docker exec %s /bin/sh -c '%s' 2>&1",
                   ctx->cmd_timeout_sec > 0 ? ctx->cmd_timeout_sec : PENTEST_DEFAULT_CMD_TO_S,
                   ctx->container_id,
                   quoted);

    fp = popen(docker_cmd, "r");
    if (lib_unlikely(fp == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_TOOL, LIB_ERR_IO,
                         "failed to invoke docker exec for sandbox");
        return LIB_TOOL_ERR_EXEC;
    }

    out_buf[0] = '\0';
    while (!feof(fp) && used + 1u < sizeof(out_buf)) {
        size_t n = fread(out_buf + used, 1u, sizeof(out_buf) - 1u - used, fp);
        if (n == 0u) {
            if (ferror(fp) && errno == EINTR) {
                clearerr(fp);
                continue;
            }
            break;
        }
        used += n;
        out_buf[used] = '\0';
    }

    exit_code = pentest_decode_wait_status(pentest_pclose_retry(fp));

    {
        cJSON *wrap     = cJSON_CreateObject();
        char  *json_str = NULL;

        if (wrap != NULL) {
            cJSON_AddNumberToObject(wrap, "exit_code", exit_code);
            cJSON_AddStringToObject(wrap, "output", out_buf);
            json_str = cJSON_PrintUnformatted(wrap);
            cJSON_Delete(wrap);
        }
        if (json_str != NULL) {
            payload = lib_strdup_ex(json_str, &oom);
            free(json_str);
        } else {
            payload = lib_strdup_ex(out_buf, &oom);
        }
    }

    if (lib_unlikely(oom || payload == NULL)) {
        return LIB_TOOL_ERR_MEM;
    }

    out_res->content  = payload;
    out_res->is_error = (exit_code == 0) ? 0 : 1;
    return LIB_TOOL_OK;
}

/* Pull initial TASK / HANDOFF from this agent's loop inbox and seed conversation */
static int pentest_consume_inbox_if_needed(pentest_app_ctx_t *ctx)
{
    lib_loop_msg_t msg;
    char           sys_buf[2048];
    char           user_buf[1024];
    const char    *skill_hint = "";

    if (ctx->task_loaded || ctx->loop == NULL || ctx->agent_name == NULL) {
        return 0;
    }

    memset(&msg, 0, sizeof(msg));
    if (lib_loop_pop(ctx->loop, ctx->agent_name, &msg) != LIB_LOOP_OK) {
        return 0;
    }

    {
        size_t n = (msg.len < sizeof(ctx->target) - 1u)
                       ? msg.len
                       : (sizeof(ctx->target) - 1u);
        if (msg.payload != NULL && n > 0u) {
            memcpy(ctx->target, msg.payload, n);
        }
        ctx->target[n] = '\0';
        pentest_trim_str(ctx->target);
    }
    lib_loop_msg_cleanup(&msg);

    if (ctx->target[0] == '\0') {
        return 0;
    }

    if (ctx->skills != NULL) {
        skill_hint = ctx->role_tag ? ctx->role_tag : "pentest";
    }

    (void)snprintf(sys_buf, sizeof(sys_buf),
                   "You are a disciplined penetration testing agent (%s). "
                   "Authorized scope: %s. Use sandbox_exec for authorized probes, "
                   "keep commands concise and targeted, and provide a structured "
                   "summary when complete.",
                   skill_hint,
                   ctx->allowed_scope ? ctx->allowed_scope : "");

    (void)snprintf(user_buf, sizeof(user_buf),
                   "Target input: %s\nAuthorized scope: %s",
                   ctx->target,
                   ctx->allowed_scope ? ctx->allowed_scope : "");

    if (pentest_add_msg(ctx, LIB_LLM_ROLE_SYSTEM, sys_buf, NULL, NULL, NULL, 0u) != 0 ||
        pentest_add_msg(ctx, LIB_LLM_ROLE_USER, user_buf, NULL, NULL, NULL, 0u) != 0) {
        return -1;
    }

    ctx->task_loaded = 1;
    return 0;
}

static lib_agent_err_t pentest_on_llm(void                  *user_ctx,
                                      const char            *agent_name,
                                      size_t                 step_idx,
                                      lib_agent_turn_kind_t *out_kind)
{
    pentest_app_ctx_t  *ctx     = (pentest_app_ctx_t *)user_ctx;
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

    if (lib_unlikely(ctx == NULL || ctx->llm == NULL ||
                     ctx->tools == NULL || out_kind == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "pentest_on_llm received NULL pointer");
        return LIB_AGENT_ERR_ARG;
    }

    if (ctx->pending_calls != NULL) {
        pentest_free_calls(ctx->pending_calls, ctx->pending_cnt);
        ctx->pending_calls = NULL;
        ctx->pending_cnt   = 0u;
    }

    if (lib_unlikely(pentest_consume_inbox_if_needed(ctx) != 0)) {
        return LIB_AGENT_ERR_MEM;
    }

    if (!ctx->task_loaded || ctx->finalized || ctx->hist_cnt == 0u) {
        *out_kind = LIB_AGENT_TURN_FINAL;
        return LIB_AGENT_OK;
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
        if (lib_unlikely(pentest_add_msg(ctx, LIB_LLM_ROLE_ASSISTANT, res.text,
                                         NULL, NULL, res.tool_calls,
                                         res.tool_count) != 0)) {
            lib_llm_free_res(&res);
            return LIB_AGENT_ERR_MEM;
        }
        ctx->pending_calls = pentest_dup_calls(res.tool_calls, res.tool_count);
        ctx->pending_cnt   = (ctx->pending_calls != NULL) ? res.tool_count : 0u;
        lib_llm_free_res(&res);
        if (lib_unlikely(ctx->pending_calls == NULL)) {
            return LIB_AGENT_ERR_MEM;
        }
        *out_kind = LIB_AGENT_TURN_TOOLS;
        return LIB_AGENT_OK;
    }

    if (res.text != NULL) {
        (void)snprintf(ctx->final_report, sizeof(ctx->final_report), "%s", res.text);
        (void)pentest_add_msg(ctx, LIB_LLM_ROLE_ASSISTANT, res.text,
                              NULL, NULL, NULL, 0u);
        if (ctx->loop != NULL && ctx->next_agent != NULL && ctx->next_agent[0] != '\0') {
            (void)lib_loop_send(ctx->loop, ctx->agent_name, ctx->next_agent,
                                LIB_LOOP_MSG_HANDOFF, ctx->final_report,
                                strlen(ctx->final_report), 1, NULL);
        }
    }
    lib_llm_free_res(&res);
    ctx->finalized = 1;
    *out_kind = LIB_AGENT_TURN_FINAL;
    return LIB_AGENT_OK;
}

static lib_agent_err_t pentest_on_tools(void       *user_ctx,
                                        const char *agent_name,
                                        size_t      step_idx,
                                        size_t     *out_calls,
                                        size_t     *out_errors)
{
    pentest_app_ctx_t *ctx    = (pentest_app_ctx_t *)user_ctx;
    size_t             i      = 0u;
    size_t             calls  = 0u;
    size_t             errors = 0u;

    (void)agent_name;
    (void)step_idx;

    if (lib_unlikely(ctx == NULL || ctx->tools == NULL ||
                     out_calls == NULL || out_errors == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "pentest_on_tools received NULL pointer");
        return LIB_AGENT_ERR_ARG;
    }

    for (i = 0u; i < ctx->pending_cnt; i++) {
        lib_tool_res_t t_res;
        lib_tool_err_t trc = LIB_TOOL_OK;
        const char    *content = "{\"error\":\"empty tool result\"}";

        memset(&t_res, 0, sizeof(t_res));
        trc = lib_tool_exec_call(ctx->tools, &ctx->pending_calls[i], ctx, &t_res);
        calls++;

        if (lib_unlikely(trc != LIB_TOOL_OK || t_res.is_error)) {
            errors++;
        }
        if (t_res.content != NULL && t_res.content[0] != '\0') {
            content = t_res.content;
        }

        if (lib_unlikely(pentest_add_msg(ctx, LIB_LLM_ROLE_TOOL, content,
                                         ctx->pending_calls[i].id,
                                         ctx->pending_calls[i].name,
                                         NULL, 0u) != 0)) {
            lib_tool_res_reset(&t_res);
            pentest_free_calls(ctx->pending_calls, ctx->pending_cnt);
            ctx->pending_calls = NULL;
            ctx->pending_cnt   = 0u;
            return LIB_AGENT_ERR_MEM;
        }
        lib_tool_res_reset(&t_res);
    }

    pentest_free_calls(ctx->pending_calls, ctx->pending_cnt);
    ctx->pending_calls = NULL;
    ctx->pending_cnt   = 0u;

    *out_calls  = calls;
    *out_errors = errors;
    return LIB_AGENT_OK;
}

static int pentest_on_step(void                        *user_ctx,
                           const char                  *agent_name,
                           const lib_agent_step_info_t *info)
{
    (void)user_ctx;
    if (info != NULL) {
        (void)printf("[%s] step=%zu state=%s calls=%zu errs=%zu\n",
                     agent_name != NULL ? agent_name : "pentest_agent",
                     info->step_idx,
                     lib_agent_state_str(info->state),
                     info->tool_calls,
                     info->tool_errors);
    }
    return 0;
}

int main(int argc, char **argv)
{
    pentest_app_ctx_t ctx;
    lib_http_cfg_t    hcfg;
    lib_llm_cfg_t     lcfg;
    lib_tool_reg_t   *treg      = NULL;
    lib_tool_spec_t   tspec;
    lib_agent_t      *agent     = NULL;
    lib_agent_cfg_t   acfg;
    lib_agent_ops_t   aops;
    lib_loop_t       *loop      = NULL;
    lib_loop_cfg_t    loop_cfg;
    lib_loop_ops_t    loop_ops;
    lib_loop_err_t    lrc       = LIB_LOOP_OK;

    const char       *model     = PENTEST_OLLAMA_MODEL;
    const char       *endpoint  = PENTEST_OLLAMA_URL;
    const char       *container = PENTEST_CONTAINER_ID;
    const char       *scope     = PENTEST_ALLOWED_SCOPE;
    const char       *target    = PENTEST_ALLOWED_SCOPE;
    const char       *env_str   = NULL;
    int               http_to_m = PENTEST_DEFAULT_HTT_TO_M;
    int               llm_to_m  = PENTEST_DEFAULT_LLM_TO_M;
    int               cmd_to_s  = PENTEST_DEFAULT_CMD_TO_S;

    memset(&ctx, 0, sizeof(ctx));
    memset(&hcfg, 0, sizeof(hcfg));
    memset(&lcfg, 0, sizeof(lcfg));
    memset(&tspec, 0, sizeof(tspec));
    memset(&acfg, 0, sizeof(acfg));
    memset(&aops, 0, sizeof(aops));
    memset(&loop_cfg, 0, sizeof(loop_cfg));
    memset(&loop_ops, 0, sizeof(loop_ops));

    lib_err_set_log_sink(pentest_err_sink, NULL);

    if ((env_str = getenv("OLLAMA_MODEL")) != NULL && env_str[0] != '\0') {
        model = env_str;
    }
    if ((env_str = getenv("OLLAMA_URL")) != NULL && env_str[0] != '\0') {
        endpoint = env_str;
    }
    if ((env_str = getenv("PENTEST_CONTAINER")) != NULL && env_str[0] != '\0') {
        container = env_str;
    }
    if ((env_str = getenv("PENTEST_SCOPE")) != NULL && env_str[0] != '\0') {
        scope = env_str;
    }
    http_to_m = pentest_parse_positive_int(getenv("HTTP_TIMEOUT_MS"),
                                           PENTEST_DEFAULT_HTT_TO_M, 1000, 600000);
    llm_to_m  = pentest_parse_positive_int(getenv("OLLAMA_TIMEOUT_MS"),
                                           PENTEST_DEFAULT_LLM_TO_M, 1000, 600000);
    cmd_to_s  = pentest_parse_positive_int(getenv("PENTEST_CMD_TIMEOUT_S"),
                                           PENTEST_DEFAULT_CMD_TO_S, 1, 3600);
    if (argc > 1 && argv[1][0] != '\0') {
        target = argv[1];
    }

    ctx.agent_name      = "pentest_worker";
    ctx.role_tag        = "pentest_recon";
    ctx.next_agent      = NULL;
    ctx.container_id    = container;
    ctx.allowed_scope   = scope;
    ctx.cmd_timeout_sec = cmd_to_s;

    hcfg.timeout_ms = http_to_m;
    ctx.http = lib_http_open(&hcfg);
    if (lib_unlikely(ctx.http == NULL)) {
        return 1;
    }

    lcfg.io.post     = pentest_llm_post_cb;
    lcfg.io.ctx      = ctx.http;
    lcfg.endpoint    = endpoint;
    lcfg.model       = model;
    lcfg.timeout_ms  = llm_to_m;
    lcfg.temperature = 0.1;

    ctx.llm = lib_llm_open(&lcfg);
    if (lib_unlikely(ctx.llm == NULL)) {
        lib_http_close(ctx.http);
        return 1;
    }

    treg = lib_tool_new();
    if (lib_unlikely(treg == NULL)) {
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    /* If lib_tool_spec_t carries a tool-specific context field (e.g. .ctx / .user_ctx),
     * bind &ctx there as well in case lib_tool_exec_call ignores its 3rd argument. */
    tspec.name        = "sandbox_exec";
    tspec.desc        = "Execute an authorized command inside the isolated Docker pentest sandbox";
    tspec.schema_json = "{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\",\"description\":\"Shell command to run in sandbox\"}},\"required\":[\"cmd\"]}";
    tspec.tag         = "pentest";
    tspec.enabled     = 1;
    tspec.fn          = pentest_tool_sandbox_exec;
    tspec.tool_ctx    = &ctx;

    if (lib_unlikely(lib_tool_add(treg, &tspec) != LIB_TOOL_OK)) {
        lib_tool_free(treg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }
    ctx.tools = treg;

    loop_cfg.max_total_steps = 24u;
    loop = lib_loop_new(&loop_cfg, &loop_ops);
    if (lib_unlikely(loop == NULL)) {
        lib_tool_free(treg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }
    ctx.loop   = loop;
    ctx.skills = lib_loop_skills(loop);

    acfg.name             = ctx.agent_name;
    acfg.role_tag         = ctx.role_tag;
    acfg.max_steps        = 12u;
    acfg.stop_on_tool_err = 0;
    acfg.user_ctx         = &ctx;

    aops.on_llm   = pentest_on_llm;
    aops.on_tools = pentest_on_tools;
    aops.on_step  = pentest_on_step;

    agent = lib_agent_new(&acfg, &aops);
    if (lib_unlikely(agent == NULL)) {
        lib_loop_free(loop);
        lib_tool_free(treg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    if (lib_unlikely(lib_loop_attach(loop, agent, 1, NULL) != LIB_LOOP_OK)) {
        lib_agent_free(agent);
        lib_loop_free(loop);
        lib_tool_free(treg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    if (lib_unlikely(lib_loop_send(loop, "orchestrator", ctx.agent_name,
                                   LIB_LOOP_MSG_TASK, target, strlen(target),
                                   1, NULL) != LIB_LOOP_OK)) {
        lib_agent_free(agent);
        lib_loop_free(loop);
        lib_tool_free(treg);
        lib_llm_close(ctx.llm);
        lib_http_close(ctx.http);
        return 1;
    }

    lrc = lib_loop_run(loop, NULL);
    (void)printf("loop finished: rc=%d agent_state=%s\n",
                 (int)lrc, lib_agent_state_str(lib_agent_state(agent)));
    if (ctx.final_report[0] != '\0') {
        (void)printf("\n=== Pentest Report ===\n%s\n", ctx.final_report);
    }

    pentest_reset_run_state(&ctx);
    lib_agent_free(agent);
    lib_loop_free(loop);
    lib_tool_free(treg);
    lib_llm_close(ctx.llm);
    lib_http_close(ctx.http);
    return (lrc == LIB_LOOP_OK) ? 0 : 1;
}