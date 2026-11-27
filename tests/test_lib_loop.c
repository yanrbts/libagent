/**
 * Copyright (c) 2026 yanrbts
 *
 * Unity unit tests for lib_loop scheduler, shared resources, and mailbox routing.
 */
#include "lib_loop.h"
#include "lib_agent.h"
#include "lib_skill.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_ag_ctx {
    lib_loop_t           *loop;
    const char           *self_name;
    const char           *forward_to;
    size_t                remaining_tool_turns;
    int                   fail_on_llm;
    int                   fail_on_tool;
    size_t                llm_calls;
    size_t                tool_calls;
    size_t                msgs_received;
    char                  last_msg[128];
    lib_loop_msg_kind_t   last_kind;
} test_ag_ctx_t;

typedef struct test_obs_ctx {
    size_t step_events;
    int    abort_at_step;
} test_obs_ctx_t;

static lib_agent_err_t mock_on_llm(void                  *user_ctx,
                                   const char            *agent_name,
                                   size_t                 step_idx,
                                   lib_agent_turn_kind_t *out_kind)
{
    test_ag_ctx_t *ctx = (test_ag_ctx_t *)user_ctx;
    lib_loop_msg_t msg;

    (void)agent_name;
    (void)step_idx;

    if (ctx == NULL || out_kind == NULL) {
        return LIB_AGENT_ERR_ARG;
    }

    ctx->llm_calls++;

    if (ctx->loop != NULL && ctx->self_name != NULL) {
        memset(&msg, 0, sizeof(msg));
        if (lib_loop_pop(ctx->loop, ctx->self_name, &msg) == LIB_LOOP_OK) {
            size_t copy_len = (msg.len < sizeof(ctx->last_msg) - 1u)
                                  ? msg.len
                                  : (sizeof(ctx->last_msg) - 1u);
            ctx->msgs_received++;
            ctx->last_kind = msg.kind;
            if (msg.payload != NULL && copy_len > 0u) {
                memcpy(ctx->last_msg, msg.payload, copy_len);
            }
            ctx->last_msg[copy_len] = '\0';
            lib_loop_msg_cleanup(&msg);
        }
    }

    if (ctx->fail_on_llm) {
        return LIB_AGENT_ERR_LLM;
    }

    if (ctx->remaining_tool_turns > 0u) {
        *out_kind = LIB_AGENT_TURN_TOOLS;
    } else {
        *out_kind = LIB_AGENT_TURN_FINAL;
    }
    return LIB_AGENT_OK;
}

static lib_agent_err_t mock_on_tools(void       *user_ctx,
                                     const char *agent_name,
                                     size_t      step_idx,
                                     size_t     *out_calls,
                                     size_t     *out_errors)
{
    test_ag_ctx_t *ctx = (test_ag_ctx_t *)user_ctx;

    (void)agent_name;
    (void)step_idx;

    if (ctx == NULL || out_calls == NULL || out_errors == NULL) {
        return LIB_AGENT_ERR_ARG;
    }

    *out_calls = 1u;
    *out_errors = 0u;
    ctx->tool_calls++;

    if (ctx->fail_on_tool) {
        *out_errors = 1u;
        ctx->remaining_tool_turns = 0u;
        return LIB_AGENT_ERR_TOOL;
    }

    if (ctx->remaining_tool_turns > 0u) {
        ctx->remaining_tool_turns--;
    }

    if (ctx->remaining_tool_turns == 0u &&
        ctx->loop != NULL &&
        ctx->forward_to != NULL &&
        ctx->forward_to[0] != '\0') {
        const char *handoff = "handoff-payload";
        (void)lib_loop_send(ctx->loop, ctx->self_name, ctx->forward_to,
                            LIB_LOOP_MSG_HANDOFF, handoff, strlen(handoff),
                            1, NULL);
    }

    return LIB_AGENT_OK;
}

static int mock_on_step_hook(void                       *user_ctx,
                             void                       *shared_ctx,
                             const lib_skill_reg_t      *skills,
                             const lib_loop_tick_info_t *tick_info)
{
    test_obs_ctx_t *obs = (test_obs_ctx_t *)user_ctx;

    (void)shared_ctx;
    (void)skills;
    (void)tick_info;

    if (obs == NULL) {
        return 0;
    }
    obs->step_events++;
    if (obs->abort_at_step > 0 && obs->step_events >= (size_t)obs->abort_at_step) {
        return 1;
    }
    return 0;
}

static void reset_ag_ctx(test_ag_ctx_t *ctx,
                         lib_loop_t    *loop,
                         const char    *self_name,
                         const char    *forward_to,
                         size_t         tool_turns)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->loop                 = loop;
    ctx->self_name            = self_name;
    ctx->forward_to           = forward_to;
    ctx->remaining_tool_turns = tool_turns;
}

static lib_agent_t *make_test_agent(const char    *name,
                                    const char    *role_tag,
                                    size_t         max_steps,
                                    int            stop_on_tool_err,
                                    test_ag_ctx_t *ctx)
{
    lib_agent_cfg_t acfg;
    lib_agent_ops_t aops;

    memset(&acfg, 0, sizeof(acfg));
    acfg.name             = name;
    acfg.role_tag         = role_tag;
    acfg.max_steps        = max_steps;
    acfg.stop_on_tool_err = stop_on_tool_err;
    acfg.user_ctx         = ctx;

    memset(&aops, 0, sizeof(aops));
    aops.on_llm   = mock_on_llm;
    aops.on_tools = mock_on_tools;

    return lib_agent_new(&acfg, &aops);
}

void setUp(void)
{
}

void tearDown(void)
{
    lib_loop_free(NULL);
}

void test_lib_loop_lifecycle_and_strings(void)
{
    lib_loop_t *loop = NULL;

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_ABORTED, lib_loop_state(NULL));
    TEST_ASSERT_EQUAL_UINT(0u, lib_loop_total_steps(NULL));
    TEST_ASSERT_NULL(lib_loop_skills(NULL));
    TEST_ASSERT_NULL(lib_loop_shared(NULL));

    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_OK));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_ARG));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_MEM));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_STATE));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_EXIST));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_NOENT));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_FULL));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_BUDGET));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_AGENT));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror(LIB_LOOP_ERR_ABORT));
    TEST_ASSERT_NOT_NULL(lib_loop_strerror((lib_loop_err_t)-999));

    TEST_ASSERT_NOT_NULL(lib_loop_state_str(LIB_LOOP_ST_IDLE));
    TEST_ASSERT_NOT_NULL(lib_loop_state_str(LIB_LOOP_ST_RUNNING));
    TEST_ASSERT_NOT_NULL(lib_loop_state_str(LIB_LOOP_ST_DONE));
    TEST_ASSERT_NOT_NULL(lib_loop_state_str(LIB_LOOP_ST_ABORTED));
    TEST_ASSERT_NOT_NULL(lib_loop_state_str((lib_loop_state_t)99));

    loop = lib_loop_new(NULL, NULL);
    TEST_ASSERT_NOT_NULL(loop);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_IDLE, lib_loop_state(loop));
    TEST_ASSERT_EQUAL_UINT(0u, lib_loop_total_steps(loop));

    lib_loop_free(loop);
}

void test_lib_loop_resources_and_attach_validation(void)
{
    lib_loop_t     *loop   = NULL;
    lib_skill_reg_t *skills = NULL;
    lib_agent_t    *ag1    = NULL;
    lib_agent_t    *ag2    = NULL;
    test_ag_ctx_t   ctx1;
    test_ag_ctx_t   ctx2;
    uint32_t        sid1   = 99u;
    uint32_t        sid2   = 99u;
    int             shared_marker = 42;

    loop = lib_loop_new(NULL, NULL);
    skills = lib_skill_new();
    TEST_ASSERT_NOT_NULL(loop);
    TEST_ASSERT_NOT_NULL(skills);

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_ARG, lib_loop_set_skills(NULL, skills));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_ARG, lib_loop_set_shared(NULL, &shared_marker));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_ARG, lib_loop_attach(NULL, NULL, 0, &sid1));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_ARG, lib_loop_attach(loop, NULL, 0, &sid1));

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_set_skills(loop, skills));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_set_shared(loop, &shared_marker));
    TEST_ASSERT_TRUE(lib_loop_skills(loop) == skills);
    TEST_ASSERT_TRUE(lib_loop_shared(loop) == &shared_marker);

    reset_ag_ctx(&ctx1, loop, "planner", NULL, 0u);
    reset_ag_ctx(&ctx2, loop, "planner", NULL, 0u);
    ag1 = make_test_agent("planner", "plan", 8u, 0, &ctx1);
    ag2 = make_test_agent("planner", "plan", 8u, 0, &ctx2);
    TEST_ASSERT_NOT_NULL(ag1);
    TEST_ASSERT_NOT_NULL(ag2);

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag1, 10, &sid1));
    TEST_ASSERT_EQUAL_UINT32(0u, sid1);

    /* Duplicate agent name must be rejected */
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_EXIST, lib_loop_attach(loop, ag2, 5, &sid2));

    lib_agent_free(ag2);
    lib_agent_free(ag1);
    lib_skill_free(skills);
    lib_loop_free(loop);
}

void test_lib_loop_mailbox_send_pop_and_broadcast(void)
{
    lib_loop_t    *loop = NULL;
    lib_agent_t   *ag_a = NULL;
    lib_agent_t   *ag_b = NULL;
    test_ag_ctx_t  ctx_a;
    test_ag_ctx_t  ctx_b;
    lib_loop_msg_t msg;
    const char    *p1 = "task-1";
    const char    *p2 = "broadcast-msg";

    loop = lib_loop_new(NULL, NULL);
    TEST_ASSERT_NOT_NULL(loop);

    reset_ag_ctx(&ctx_a, loop, "worker_a", NULL, 0u);
    reset_ag_ctx(&ctx_b, loop, "worker_b", NULL, 0u);
    ag_a = make_test_agent("worker_a", "w", 4u, 0, &ctx_a);
    ag_b = make_test_agent("worker_b", "w", 4u, 0, &ctx_b);
    TEST_ASSERT_NOT_NULL(ag_a);
    TEST_ASSERT_NOT_NULL(ag_b);

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag_a, 1, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag_b, 1, NULL));

    /* Pop on empty inbox returns LIB_LOOP_ERR_NOENT */
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_NOENT, lib_loop_pop(loop, "worker_a", &msg));

    /* Targeted send with copied payload */
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK,
                          lib_loop_send(loop, "orchestrator", "worker_a",
                                        LIB_LOOP_MSG_TASK, p1, strlen(p1), 1, NULL));

    /* Broadcast to all attached agents */
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK,
                          lib_loop_send(loop, "orchestrator", "*",
                                        LIB_LOOP_MSG_RESULT, p2, strlen(p2), 1, NULL));

    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_pop(loop, "worker_a", &msg));
    TEST_ASSERT_EQUAL_STRING("orchestrator", msg.from);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_MSG_TASK, msg.kind);
    TEST_ASSERT_EQUAL_UINT(strlen(p1), msg.len);
    TEST_ASSERT_EQUAL_MEMORY(p1, msg.payload, msg.len);
    lib_loop_msg_cleanup(&msg);

    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_pop(loop, "worker_a", &msg));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_MSG_RESULT, msg.kind);
    TEST_ASSERT_EQUAL_MEMORY(p2, msg.payload, msg.len);
    lib_loop_msg_cleanup(&msg);

    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_pop(loop, "worker_b", &msg));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_MSG_RESULT, msg.kind);
    TEST_ASSERT_EQUAL_MEMORY(p2, msg.payload, msg.len);
    lib_loop_msg_cleanup(&msg);

    lib_loop_msg_cleanup(NULL);
    lib_agent_free(ag_b);
    lib_agent_free(ag_a);
    lib_loop_free(loop);
}

void test_lib_loop_multi_agent_handoff_and_reset(void)
{
    lib_loop_cfg_t       lcfg;
    lib_loop_ops_t       lops;
    test_obs_ctx_t       obs;
    lib_loop_t          *loop = NULL;
    lib_agent_t         *planner = NULL;
    lib_agent_t         *executor = NULL;
    test_ag_ctx_t        ctx_plan;
    test_ag_ctx_t        ctx_exec;
    lib_loop_tick_info_t tinfo;
    const char          *init_task = "audit-service";

    memset(&obs, 0, sizeof(obs));
    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.name            = "pipeline_loop";
    lcfg.max_total_steps = 16u;
    lcfg.stop_on_err     = 1;
    lcfg.user_ctx        = &obs;

    memset(&lops, 0, sizeof(lops));
    lops.on_step = mock_on_step_hook;

    loop = lib_loop_new(&lcfg, &lops);
    TEST_ASSERT_NOT_NULL(loop);

    /* planner runs 1 tool turn then sends LIB_LOOP_MSG_HANDOFF to executor */
    reset_ag_ctx(&ctx_plan, loop, "planner", "executor", 1u);
    reset_ag_ctx(&ctx_exec, loop, "executor", NULL, 0u);

    planner  = make_test_agent("planner", "plan", 4u, 1, &ctx_plan);
    executor = make_test_agent("executor", "exec", 4u, 1, &ctx_exec);
    TEST_ASSERT_NOT_NULL(planner);
    TEST_ASSERT_NOT_NULL(executor);

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, planner, 10, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, executor, 5, NULL));

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK,
                          lib_loop_send(loop, "user", "planner",
                                        LIB_LOOP_MSG_TASK, init_task, strlen(init_task),
                                        1, NULL));

    memset(&tinfo, 0, sizeof(tinfo));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_run(loop, &tinfo));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_DONE, lib_loop_state(loop));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_DONE, lib_agent_state(planner));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_DONE, lib_agent_state(executor));

    TEST_ASSERT_EQUAL_UINT(1u, ctx_plan.msgs_received);
    TEST_ASSERT_EQUAL_STRING("audit-service", ctx_plan.last_msg);
    TEST_ASSERT_EQUAL_UINT(1u, ctx_exec.msgs_received);
    TEST_ASSERT_EQUAL_STRING("handoff-payload", ctx_exec.last_msg);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_MSG_HANDOFF, ctx_exec.last_kind);
    TEST_ASSERT_TRUE(obs.step_events >= 2u);

    /* Reset and re-run cleanly */
    ctx_plan.remaining_tool_turns = 1u;
    ctx_exec.remaining_tool_turns = 0u;
    ctx_plan.msgs_received = 0u;
    ctx_exec.msgs_received = 0u;
    obs.step_events = 0u;

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_reset(loop));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_IDLE, lib_loop_state(loop));
    TEST_ASSERT_EQUAL_UINT(0u, lib_loop_total_steps(loop));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK,
                          lib_loop_send(loop, "user", "planner",
                                        LIB_LOOP_MSG_TASK, init_task, strlen(init_task),
                                        1, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_run(loop, &tinfo));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_DONE, lib_loop_state(loop));

    lib_agent_free(executor);
    lib_agent_free(planner);
    lib_loop_free(loop);
}

void test_lib_loop_budget_and_cancel_propagation(void)
{
    lib_loop_cfg_t lcfg;
    lib_loop_t    *loop = NULL;
    lib_agent_t   *ag   = NULL;
    test_ag_ctx_t  ctx;

    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.max_total_steps = 2u;

    loop = lib_loop_new(&lcfg, NULL);
    TEST_ASSERT_NOT_NULL(loop);

    reset_ag_ctx(&ctx, loop, "looping_agent", NULL, 10u);
    ag = make_test_agent("looping_agent", NULL, 10u, 0, &ctx);
    TEST_ASSERT_NOT_NULL(ag);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag, 1, NULL));

    /* Budget exhaustion aborts the loop and broadcasts cancel_req to attached agents */
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_BUDGET, lib_loop_run(loop, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_ABORTED, lib_loop_state(loop));

    /* Stepping ag consumes the propagated cancel_req and transitions it to ABORTED */
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_ABORT, lib_agent_step(ag, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));

    /* Test external cancellation propagation */
    lib_loop_free(loop);
    lib_agent_free(ag);

    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.max_total_steps = 32u;
    loop = lib_loop_new(&lcfg, NULL);
    TEST_ASSERT_NOT_NULL(loop);

    reset_ag_ctx(&ctx, loop, "cancellable_agent", NULL, 10u);
    ag = make_test_agent("cancellable_agent", NULL, 10u, 0, &ctx);
    TEST_ASSERT_NOT_NULL(ag);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag, 1, NULL));

    lib_loop_cancel(loop);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_ABORT, lib_loop_tick(loop, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_ABORTED, lib_loop_state(loop));

    /* Step ag so it observes the propagated cancel_req and transitions to ABORTED */
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ERR_ABORT, lib_agent_step(ag, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));

    lib_agent_free(ag);
    lib_loop_free(loop);
}

void test_lib_loop_agent_error_and_hook_abort(void)
{
    lib_loop_cfg_t lcfg;
    lib_loop_ops_t lops;
    test_obs_ctx_t obs;
    lib_loop_t    *loop = NULL;
    lib_agent_t   *ag   = NULL;
    test_ag_ctx_t  ctx;

    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.stop_on_err = 1;
    loop = lib_loop_new(&lcfg, NULL);
    TEST_ASSERT_NOT_NULL(loop);

    reset_ag_ctx(&ctx, loop, "failing_agent", NULL, 1u);
    ctx.fail_on_tool = 1;
    ag = make_test_agent("failing_agent", NULL, 4u, 1, &ctx);
    TEST_ASSERT_NOT_NULL(ag);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag, 1, NULL));

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_AGENT, lib_loop_run(loop, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_ABORTED, lib_loop_state(loop));
    TEST_ASSERT_EQUAL_INT(LIB_AGENT_ST_ABORTED, lib_agent_state(ag));

    lib_agent_free(ag);
    lib_loop_free(loop);

    /* Hook returning non-zero aborts loop */
    memset(&obs, 0, sizeof(obs));
    obs.abort_at_step = 1;

    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.user_ctx = &obs;
    memset(&lops, 0, sizeof(lops));
    lops.on_step = mock_on_step_hook;

    loop = lib_loop_new(&lcfg, &lops);
    TEST_ASSERT_NOT_NULL(loop);

    reset_ag_ctx(&ctx, loop, "hook_abort_agent", NULL, 0u);
    ag = make_test_agent("hook_abort_agent", NULL, 4u, 0, &ctx);
    TEST_ASSERT_NOT_NULL(ag);
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_OK, lib_loop_attach(loop, ag, 1, NULL));

    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ERR_ABORT, lib_loop_run(loop, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_LOOP_ST_ABORTED, lib_loop_state(loop));

    lib_agent_free(ag);
    lib_loop_free(loop);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lib_loop_lifecycle_and_strings);
    RUN_TEST(test_lib_loop_resources_and_attach_validation);
    RUN_TEST(test_lib_loop_mailbox_send_pop_and_broadcast);
    RUN_TEST(test_lib_loop_multi_agent_handoff_and_reset);
    RUN_TEST(test_lib_loop_budget_and_cancel_propagation);
    RUN_TEST(test_lib_loop_agent_error_and_hook_abort);
    return UNITY_END();
}