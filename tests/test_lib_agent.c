/**
 * Copyright (c) 2026 yanrbts
 *
 * Unit tests for lib_agent state machine and execution loop using Unity.
 */

#include "unity.h"
#include "lib_agent.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

typedef struct mock_agent_ctx {
    size_t                llm_calls;
    size_t                tool_calls_seen;
    size_t                step_hooks;
    lib_agent_turn_kind_t scripted_kinds[8];
    size_t                scripted_kind_cnt;
    lib_agent_err_t       scripted_llm_rc;
    lib_agent_err_t       scripted_tool_rc;
    size_t                scripted_calls_per_step;
    size_t                scripted_errs_per_step;
    int                   abort_on_step_hook;
    lib_agent_t          *cancel_target;
    size_t                cancel_after_llm_calls;
} mock_agent_ctx_t;

static mock_agent_ctx_t g_ctx;

static void reset_ctx(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.scripted_llm_rc         = LIB_AGENT_OK;
    g_ctx.scripted_tool_rc        = LIB_AGENT_OK;
    g_ctx.scripted_calls_per_step = 1u;
    g_ctx.scripted_errs_per_step  = 0u;
}

static lib_agent_err_t mock_on_llm(void                  *user_ctx,
                                   const char            *agent_name,
                                   size_t                 step_idx,
                                   lib_agent_turn_kind_t *out_kind,
                                   char                  *errbuf,
                                   size_t                 errlen)
{
    mock_agent_ctx_t *ctx = (mock_agent_ctx_t *)user_ctx;
    size_t            idx = 0u;

    (void)agent_name;
    (void)step_idx;

    if (lib_unlikely(ctx == NULL || out_kind == NULL)) {
        return LIB_AGENT_ERR_ARG;
    }

    if (lib_unlikely(ctx->scripted_llm_rc != LIB_AGENT_OK)) {
        if (errbuf != NULL && errlen > 0u) {
            (void)snprintf(errbuf, errlen, "mock llm failure");
        }
        return ctx->scripted_llm_rc;
    }

    idx = ctx->llm_calls;
    ctx->llm_calls++;

    if (idx < ctx->scripted_kind_cnt) {
        *out_kind = ctx->scripted_kinds[idx];
    } else {
        *out_kind = LIB_AGENT_TURN_FINAL;
    }

    if (ctx->cancel_target != NULL &&
        ctx->cancel_after_llm_calls > 0u &&
        ctx->llm_calls >= ctx->cancel_after_llm_calls) {
        lib_agent_cancel(ctx->cancel_target);
    }

    return LIB_AGENT_OK;
}

static lib_agent_err_t mock_on_tools(void       *user_ctx,
                                     const char *agent_name,
                                     size_t      step_idx,
                                     size_t     *out_calls,
                                     size_t     *out_errors,
                                     char       *errbuf,
                                     size_t      errlen)
{
    mock_agent_ctx_t *ctx = (mock_agent_ctx_t *)user_ctx;

    (void)agent_name;
    (void)step_idx;

    if (lib_unlikely(ctx == NULL || out_calls == NULL || out_errors == NULL)) {
        return LIB_AGENT_ERR_ARG;
    }

    if (lib_unlikely(ctx->scripted_tool_rc != LIB_AGENT_OK)) {
        if (errbuf != NULL && errlen > 0u) {
            (void)snprintf(errbuf, errlen, "mock tool failure");
        }
        return ctx->scripted_tool_rc;
    }

    *out_calls  = ctx->scripted_calls_per_step;
    *out_errors = ctx->scripted_errs_per_step;
    ctx->tool_calls_seen += ctx->scripted_calls_per_step;
    return LIB_AGENT_OK;
}

static int mock_on_step(void                        *user_ctx,
                        const char                  *agent_name,
                        const lib_agent_step_info_t *info)
{
    mock_agent_ctx_t *ctx = (mock_agent_ctx_t *)user_ctx;

    (void)agent_name;
    (void)info;

    if (ctx != NULL) {
        ctx->step_hooks++;
        if (lib_unlikely(ctx->abort_on_step_hook)) {
            return 1;
        }
    }
    return 0;
}

void setUp(void)
{
    reset_ctx();
}

void tearDown(void)
{
    reset_ctx();
}

static void test_lifecycle_defaults_and_helpers(void)
{
    lib_agent_t    *ag  = NULL;
    lib_agent_ops_t ops;
    char            errbuf[128];

    memset(&ops, 0, sizeof(ops));
    memset(errbuf, 0, sizeof(errbuf));

    TEST_ASSERT_NOT_NULL(lib_agent_strerror(LIB_AGENT_OK));
    TEST_ASSERT_NOT_NULL(lib_agent_strerror(LIB_AGENT_ERR_STEP));
    TEST_ASSERT_EQUAL_STRING("idle", lib_agent_state_str(LIB_AGENT_ST_IDLE));
    TEST_ASSERT_EQUAL_STRING("done", lib_agent_state_str(LIB_AGENT_ST_DONE));
    TEST_ASSERT_EQUAL_STRING("aborted", lib_agent_state_str(LIB_AGENT_ST_ABORTED));
    TEST_ASSERT_NULL(lib_agent_name(NULL));
    TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_agent_steps(NULL));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(NULL));

    /* Missing required callbacks must fail */
    TEST_ASSERT_NULL(lib_agent_new(NULL, NULL, errbuf, sizeof(errbuf)));
    TEST_ASSERT_TRUE(errbuf[0] != '\0');

    ops.on_llm = mock_on_llm;
    TEST_ASSERT_NULL(lib_agent_new(NULL, &ops, errbuf, sizeof(errbuf)));

    ops.on_tools = mock_on_tools;
    ops.on_step  = mock_on_step;

    /* NULL cfg applies default name and IDLE state */
    ag = lib_agent_new(NULL, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);
    TEST_ASSERT_EQUAL_STRING("agent", lib_agent_name(ag));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_IDLE, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_agent_steps(ag));

    lib_agent_free(ag);
    lib_agent_free(NULL);
    lib_agent_cancel(NULL);
}

static void test_multi_step_run_and_reset(void)
{
    lib_agent_t          *ag   = NULL;
    lib_agent_cfg_t       cfg;
    lib_agent_ops_t       ops;
    lib_agent_step_info_t info;
    lib_agent_err_t       rc   = LIB_AGENT_OK;
    char                  errbuf[128];

    memset(&cfg, 0, sizeof(cfg));
    memset(&ops, 0, sizeof(ops));
    memset(&info, 0, sizeof(info));
    memset(errbuf, 0, sizeof(errbuf));

    g_ctx.scripted_kinds[0]       = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kinds[1]       = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kinds[2]       = LIB_AGENT_TURN_FINAL;
    g_ctx.scripted_kind_cnt       = 3u;
    g_ctx.scripted_calls_per_step = 2u;

    cfg.name      = "planner";
    cfg.role_tag  = "skill:research";
    cfg.max_steps = 5u;
    cfg.user_ctx  = &g_ctx;

    ops.on_llm   = mock_on_llm;
    ops.on_tools = mock_on_tools;
    ops.on_step  = mock_on_step;

    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);
    TEST_ASSERT_EQUAL_STRING("planner", lib_agent_name(ag));

    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_OK, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_DONE, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_UINT64(3u, (unsigned long long)lib_agent_steps(ag));
    TEST_ASSERT_EQUAL_UINT64(3u, (unsigned long long)g_ctx.llm_calls);
    TEST_ASSERT_EQUAL_UINT64(4u, (unsigned long long)g_ctx.tool_calls_seen);
    TEST_ASSERT_EQUAL_UINT64(3u, (unsigned long long)g_ctx.step_hooks);
    TEST_ASSERT_EQUAL_UINT64(2u, (unsigned long long)info.step_idx);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_DONE, info.state);

    /* Running again from terminal state without reset must return ERR_STATE */
    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_STATE, rc);

    /* Reset allows clean reuse */
    rc = lib_agent_reset(ag, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_OK, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_IDLE, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_agent_steps(ag));

    lib_agent_free(ag);
}

static void test_step_limit_and_tool_error_policy(void)
{
    lib_agent_t          *ag   = NULL;
    lib_agent_cfg_t       cfg;
    lib_agent_ops_t       ops;
    lib_agent_step_info_t info;
    lib_agent_err_t       rc   = LIB_AGENT_OK;
    char                  errbuf[128];

    memset(&cfg, 0, sizeof(cfg));
    memset(&ops, 0, sizeof(ops));
    memset(&info, 0, sizeof(info));
    memset(errbuf, 0, sizeof(errbuf));

    /* Always ask for tools so max_steps=2 triggers LIB_AGENT_ERR_STEP */
    g_ctx.scripted_kinds[0]       = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kinds[1]       = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kinds[2]       = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kind_cnt       = 3u;
    g_ctx.scripted_calls_per_step = 1u;
    g_ctx.scripted_errs_per_step  = 1u;

    cfg.name             = "worker";
    cfg.max_steps        = 2u;
    cfg.stop_on_tool_err = 0;
    cfg.user_ctx         = &g_ctx;

    ops.on_llm   = mock_on_llm;
    ops.on_tools = mock_on_tools;

    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);

    /* stop_on_tool_err == 0 allows continuing across tool errors until max_steps */
    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_STEP, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_UINT64(2u, (unsigned long long)lib_agent_steps(ag));

    /* Reconfigure with stop_on_tool_err == 1 */
    lib_agent_free(ag);
    reset_ctx();
    g_ctx.scripted_kinds[0]      = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kind_cnt      = 1u;
    g_ctx.scripted_calls_per_step = 1u;
    g_ctx.scripted_errs_per_step  = 1u;

    cfg.stop_on_tool_err = 1;
    cfg.user_ctx         = &g_ctx;

    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);

    rc = lib_agent_step(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_TOOL, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_UINT64(1u, (unsigned long long)info.tool_errors);

    lib_agent_free(ag);
}

static void test_cancel_hook_and_callback_failures(void)
{
    lib_agent_t          *ag   = NULL;
    lib_agent_cfg_t       cfg;
    lib_agent_ops_t       ops;
    lib_agent_step_info_t info;
    lib_agent_err_t       rc   = LIB_AGENT_OK;
    char                  errbuf[128];

    memset(&cfg, 0, sizeof(cfg));
    memset(&ops, 0, sizeof(ops));
    memset(&info, 0, sizeof(info));
    memset(errbuf, 0, sizeof(errbuf));

    cfg.name     = "guard";
    cfg.user_ctx = &g_ctx;
    ops.on_llm   = mock_on_llm;
    ops.on_tools = mock_on_tools;
    ops.on_step  = mock_on_step;

    /* Abort requested inside on_step hook */
    g_ctx.abort_on_step_hook = 1;
    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);

    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_ABORT, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));
    lib_agent_free(ag);

    /* Cancellation requested during LLM callback */
    reset_ctx();
    memset(errbuf, 0, sizeof(errbuf));
    g_ctx.scripted_kinds[0]      = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kind_cnt      = 1u;
    g_ctx.cancel_after_llm_calls = 1u;

    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);
    g_ctx.cancel_target = ag;

    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_ABORT, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));
    lib_agent_free(ag);

    /* LLM callback fatal error */
    reset_ctx();
    memset(errbuf, 0, sizeof(errbuf));
    g_ctx.scripted_llm_rc = LIB_AGENT_ERR_LLM;
    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);

    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_LLM, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_STRING("mock llm failure", errbuf);
    lib_agent_free(ag);

    /* Tool callback fatal error */
    reset_ctx();
    memset(errbuf, 0, sizeof(errbuf));
    g_ctx.scripted_kinds[0] = LIB_AGENT_TURN_TOOLS;
    g_ctx.scripted_kind_cnt = 1u;
    g_ctx.scripted_tool_rc  = LIB_AGENT_ERR_TOOL;
    ag = lib_agent_new(&cfg, &ops, errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(ag);

    rc = lib_agent_run(ag, &info, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_TOOL, rc);
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));
    TEST_ASSERT_EQUAL_STRING("mock tool failure", errbuf);
    lib_agent_free(ag);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle_defaults_and_helpers);
    RUN_TEST(test_multi_step_run_and_reset);
    RUN_TEST(test_step_limit_and_tool_error_policy);
    RUN_TEST(test_cancel_hook_and_callback_failures);
    return UNITY_END();
}