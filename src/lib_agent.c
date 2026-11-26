/**
 * Copyright (c) 2026 yanrbts
 *
 * Agent state machine and execution loop implementation for libagent.
 */
#include "lib_agent.h"
#include "lib_error.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIB_AGENT_DEF_MAX_STEPS 16u

struct lib_agent {
    char             *name;      /* Heap-allocated agent name */
    char             *role_tag;  /* Optional heap-allocated role/skill tag */
    size_t            max_steps; /* Step budget before forcing abort */
    size_t            step_cnt;  /* Number of completed loop iterations */
    int               stop_on_err; /* Non-zero aborts immediately on tool error */
    int               cancel_req;  /* Non-zero cancellation flag set externally */
    lib_agent_state_t state;     /* Current execution state */
    lib_agent_ops_t   ops;       /* Bound execution and observation callbacks */
    void             *user_ctx;  /* Caller context forwarded to callbacks */
};

static void lib_agent_fill_info(const lib_agent_t     *ag,
                                size_t                 calls,
                                size_t                 errors,
                                lib_agent_step_info_t *out_info)
{
    if (out_info == NULL) {
        return;
    }
    out_info->step_idx    = (ag->step_cnt > 0u) ? (ag->step_cnt - 1u) : 0u;
    out_info->state       = ag->state;
    out_info->tool_calls  = calls;
    out_info->tool_errors = errors;
}

static lib_agent_err_t lib_agent_abort(lib_agent_t     *ag,
                                       lib_agent_err_t  err,
                                       lib_err_code_t   ecode,
                                       const char      *msg)
{
    ag->state = LIB_AGENT_ST_ABORTED;
    lib_err_emit_msg(LIB_ERR_DOM_AGENT, ecode, "%s", msg);
    return err;
}

const char *lib_agent_strerror(lib_agent_err_t err)
{
    switch (err) {
    case LIB_AGENT_OK:        return "ok";
    case LIB_AGENT_ERR_ARG:   return "invalid argument";
    case LIB_AGENT_ERR_MEM:   return "out of memory";
    case LIB_AGENT_ERR_STATE: return "invalid agent state";
    case LIB_AGENT_ERR_STEP:  return "agent max steps exceeded";
    case LIB_AGENT_ERR_LLM:   return "agent llm step failed";
    case LIB_AGENT_ERR_TOOL:  return "agent tool step failed";
    case LIB_AGENT_ERR_ABORT: return "agent execution aborted";
    default:                  return "unknown agent error";
    }
}

const char *lib_agent_state_str(lib_agent_state_t state)
{
    switch (state) {
    case LIB_AGENT_ST_IDLE:      return "idle";
    case LIB_AGENT_ST_LLM_CALL:  return "llm_call";
    case LIB_AGENT_ST_TOOL_EXEC: return "tool_exec";
    case LIB_AGENT_ST_DONE:      return "done";
    case LIB_AGENT_ST_ABORTED:   return "aborted";
    default:                     return "unknown";
    }
}

lib_agent_t *lib_agent_new(const lib_agent_cfg_t *cfg,
                           const lib_agent_ops_t *ops)
{
    lib_agent_t *ag  = NULL;
    int          oom = 0;

    if (lib_unlikely(ops == NULL || ops->on_llm == NULL || ops->on_tools == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "agent ops, on_llm, and on_tools are required");
        return NULL;
    }

    ag = (lib_agent_t *)calloc(1u, sizeof(*ag));
    if (lib_unlikely(ag == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_NOMEM,
                         "failed to allocate agent instance");
        return NULL;
    }

    if (cfg != NULL && cfg->name != NULL && cfg->name[0] != '\0') {
        ag->name = lib_strdup_ex(cfg->name, &oom);
        if (lib_unlikely(oom || ag->name == NULL)) {
            lib_agent_free(ag);
            lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_NOMEM,
                             "out of memory duplicating agent name");
            return NULL;
        }
    } else {
        ag->name = lib_strdup_ex("agent", &oom);
        if (lib_unlikely(oom || ag->name == NULL)) {
            lib_agent_free(ag);
            lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_NOMEM,
                             "out of memory setting default agent name");
            return NULL;
        }
    }

    if (cfg != NULL && cfg->role_tag != NULL && cfg->role_tag[0] != '\0') {
        ag->role_tag = lib_strdup_ex(cfg->role_tag, &oom);
        if (lib_unlikely(oom || ag->role_tag == NULL)) {
            lib_agent_free(ag);
            lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_NOMEM,
                             "out of memory duplicating agent role_tag");
            return NULL;
        }
    }

    ag->max_steps   = (cfg != NULL && cfg->max_steps > 0u)
                          ? cfg->max_steps
                          : LIB_AGENT_DEF_MAX_STEPS;
    ag->stop_on_err = (cfg != NULL && cfg->stop_on_tool_err != 0) ? 1 : 0;
    ag->user_ctx    = (cfg != NULL) ? cfg->user_ctx : NULL;
    ag->ops         = *ops;
    ag->state       = LIB_AGENT_ST_IDLE;
    ag->step_cnt    = 0u;
    ag->cancel_req  = 0;

    return ag;
}

void lib_agent_free(lib_agent_t *ag)
{
    if (lib_unlikely(ag == NULL)) {
        return;
    }
    free(ag->name);
    free(ag->role_tag);
    free(ag);
}

lib_agent_err_t lib_agent_reset(lib_agent_t *ag)
{
    if (lib_unlikely(ag == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "agent instance is NULL");
        return LIB_AGENT_ERR_ARG;
    }
    ag->state      = LIB_AGENT_ST_IDLE;
    ag->step_cnt   = 0u;
    ag->cancel_req = 0;
    return LIB_AGENT_OK;
}

void lib_agent_cancel(lib_agent_t *ag)
{
    if (lib_unlikely(ag != NULL)) {
        ag->cancel_req = 1;
    }
}

lib_agent_state_t lib_agent_state(const lib_agent_t *ag)
{
    return (ag != NULL) ? ag->state : LIB_AGENT_ST_ABORTED;
}

size_t lib_agent_steps(const lib_agent_t *ag)
{
    return (ag != NULL) ? ag->step_cnt : 0u;
}

const char *lib_agent_name(const lib_agent_t *ag)
{
    return (ag != NULL) ? ag->name : NULL;
}

lib_agent_err_t lib_agent_step(lib_agent_t           *ag,
                               lib_agent_step_info_t *out_info)
{
    lib_agent_turn_kind_t kind     = LIB_AGENT_TURN_FINAL;
    lib_agent_err_t       rc       = LIB_AGENT_OK;
    size_t                calls    = 0u;
    size_t                errors   = 0u;
    size_t                cur_step = 0u;
    lib_agent_step_info_t snap;

    if (lib_unlikely(ag == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "agent instance is NULL");
        return LIB_AGENT_ERR_ARG;
    }

    if (lib_unlikely(ag->state == LIB_AGENT_ST_DONE ||
                     ag->state == LIB_AGENT_ST_ABORTED)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_STATE,
                         "agent is in terminal state; call lib_agent_reset first");
        return LIB_AGENT_ERR_STATE;
    }

    if (lib_unlikely(ag->cancel_req)) {
        rc = lib_agent_abort(ag, LIB_AGENT_ERR_ABORT, LIB_ERR_STATE,
                             "agent cancelled");
        lib_agent_fill_info(ag, 0u, 0u, out_info);
        return rc;
    }

    if (lib_unlikely(ag->step_cnt >= ag->max_steps)) {
        rc = lib_agent_abort(ag, LIB_AGENT_ERR_STEP, LIB_ERR_STATE,
                             "agent reached max_steps");
        lib_agent_fill_info(ag, 0u, 0u, out_info);
        return rc;
    }

    cur_step = ag->step_cnt;
    ag->state = LIB_AGENT_ST_LLM_CALL;

    /* Clear stale TLS error before callback so we preserve any specific error it emits. */
    lib_err_clear();
    rc = ag->ops.on_llm(ag->user_ctx, ag->name, cur_step, &kind);
    if (lib_unlikely(rc != LIB_AGENT_OK)) {
        ag->state = LIB_AGENT_ST_ABORTED;
        if (lib_err_last()->code == LIB_ERR_OK) {
            lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_PROTOCOL,
                             "agent '%s' on_llm failed at step %zu",
                             ag->name, cur_step);
        }
        lib_agent_fill_info(ag, 0u, 0u, out_info);
        return LIB_AGENT_ERR_LLM;
    }

    if (lib_unlikely(ag->cancel_req)) {
        rc = lib_agent_abort(ag, LIB_AGENT_ERR_ABORT, LIB_ERR_STATE,
                             "agent cancelled after llm");
        lib_agent_fill_info(ag, 0u, 0u, out_info);
        return rc;
    }

    if (kind == LIB_AGENT_TURN_FINAL) {
        ag->step_cnt++;
        ag->state = LIB_AGENT_ST_DONE;
        lib_agent_fill_info(ag, 0u, 0u, out_info);
        if (ag->ops.on_step != NULL) {
            lib_agent_fill_info(ag, 0u, 0u, &snap);
            if (lib_unlikely(ag->ops.on_step(ag->user_ctx, ag->name, &snap) != 0)) {
                return lib_agent_abort(ag, LIB_AGENT_ERR_ABORT, LIB_ERR_STATE,
                                       "aborted by on_step hook");
            }
        }
        return LIB_AGENT_OK;
    }

    ag->state = LIB_AGENT_ST_TOOL_EXEC;
    lib_err_clear();
    rc = ag->ops.on_tools(ag->user_ctx, ag->name, cur_step, &calls, &errors);
    if (lib_unlikely(rc != LIB_AGENT_OK)) {
        ag->state = LIB_AGENT_ST_ABORTED;
        if (lib_err_last()->code == LIB_ERR_OK) {
            lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_TOOL_EXEC,
                             "agent '%s' on_tools failed at step %zu",
                             ag->name, cur_step);
        }
        lib_agent_fill_info(ag, calls, errors, out_info);
        return LIB_AGENT_ERR_TOOL;
    }

    if (lib_unlikely(ag->stop_on_err && errors > 0u)) {
        rc = lib_agent_abort(ag, LIB_AGENT_ERR_TOOL, LIB_ERR_TOOL_EXEC,
                             "aborted due to tool error");
        lib_agent_fill_info(ag, calls, errors, out_info);
        return rc;
    }

    ag->step_cnt++;
    ag->state = LIB_AGENT_ST_IDLE;

    if (lib_unlikely(ag->cancel_req)) {
        rc = lib_agent_abort(ag, LIB_AGENT_ERR_ABORT, LIB_ERR_STATE,
                             "agent cancelled after tools");
        lib_agent_fill_info(ag, calls, errors, out_info);
        return rc;
    }

    if (ag->ops.on_step != NULL) {
        lib_agent_fill_info(ag, calls, errors, &snap);
        if (lib_unlikely(ag->ops.on_step(ag->user_ctx, ag->name, &snap) != 0)) {
            rc = lib_agent_abort(ag, LIB_AGENT_ERR_ABORT, LIB_ERR_STATE,
                                 "aborted by on_step hook");
            lib_agent_fill_info(ag, calls, errors, out_info);
            return rc;
        }
    }

    lib_agent_fill_info(ag, calls, errors, out_info);
    return LIB_AGENT_OK;
}

lib_agent_err_t lib_agent_run(lib_agent_t           *ag,
                              lib_agent_step_info_t *out_info)
{
    lib_agent_err_t rc = LIB_AGENT_OK;

    if (lib_unlikely(ag == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_INVALID_ARG,
                         "agent instance is NULL");
        return LIB_AGENT_ERR_ARG;
    }

    if (lib_unlikely(ag->state == LIB_AGENT_ST_DONE ||
                     ag->state == LIB_AGENT_ST_ABORTED)) {
        lib_err_emit_msg(LIB_ERR_DOM_AGENT, LIB_ERR_STATE,
                         "agent is in terminal state; call lib_agent_reset first");
        return LIB_AGENT_ERR_STATE;
    }

    while (ag->state == LIB_AGENT_ST_IDLE) {
        rc = lib_agent_step(ag, out_info);
        if (lib_unlikely(rc != LIB_AGENT_OK)) {
            return rc;
        }
        if (ag->state == LIB_AGENT_ST_DONE) {
            return LIB_AGENT_OK;
        }
    }

    return LIB_AGENT_OK;
}