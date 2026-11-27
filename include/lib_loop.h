/**
 * Copyright (c) 2026 yanrbts
 *
 * Multi-agent scheduler, resource coordinator, and mailbox loop for libagent.
 *
 * +---------------------------------------------------------------------------+
 * |                       Caller / Orchestrator Layer                         |
 * |  lib_loop_new()  lib_loop_attach()  lib_loop_send()  lib_loop_run()       |
 * +-------------------------------------+-------------------------------------+
 *                                       |
 *                                       v
 * +---------------------------------------------------------------------------+
 * |                            lib_loop Coordinator                           |
 * |  +-------------------+  +-----------------------+  +--------------------+ |
 * |  | Shared Resources  |  | Priority + RR Picker  |  |  Global Guardrails | |
 * |  | - lib_skill_reg_t |  | - priority (desc)     |  |  - max_total_steps | |
 * |  | - shared_ctx      |  | - inbox_len tie-break |  |  - stop_on_err     | |
 * |  | - on_step hook    |  | - rr_cursor advance   |  |  - cancel broadcast| |
 * |  +-------------------+  +-----------+-----------+  +--------------------+ |
 * +-------------------------------------+-------------------------------------+
 *                                       |
 *            +--------------------------+--------------------------+
 *            v                                                     v
 * +-----------------------+                             +-----------------------+
 * |  Agent Slot A (prio)  |                             |  Agent Slot B (prio)  |
 * |  +-----------------+  |                             |  +-----------------+  |
 * |  | Inbox Queue     |  |                             |  | Inbox Queue     |  |
 * |  | TASK/RESULT/etc |  |                             |  | TASK/RESULT/etc |  |
 * |  +--------+--------+  |                             |  +--------+--------+  |
 * |           |           |                             |           |           |
 * |           v           |                             |           v           |
 * |  lib_loop_pop()       |                             |  lib_loop_pop()       |
 * |           |           |                             |           |           |
 * |           v           |                             |           v           |
 * |  lib_agent_step()     |                             |  lib_agent_step()     |
 * |  (IDLE->LLM->TOOL)    |                             |  (IDLE->LLM->TOOL)    |
 * |           |           |                             |           |           |
 * |           +-------- lib_loop_send() (handoff / result) -------+             |
 * +-----------------------+                             +-----------------------+
 */
#ifndef __LIB_LOOP_H__
#define __LIB_LOOP_H__

#include "lib_export.h"
#include "lib_agent.h"
#include "lib_skill.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Error codes returned by lib_loop operations.
 */
typedef enum lib_loop_err {
    LIB_LOOP_OK         =  0, /* Operation succeeded */
    LIB_LOOP_ERR_ARG    = -1, /* Invalid argument or NULL pointer */
    LIB_LOOP_ERR_MEM    = -2, /* Memory allocation failure */
    LIB_LOOP_ERR_STATE  = -3, /* Invalid loop or agent state */
    LIB_LOOP_ERR_EXIST  = -4, /* Agent name or slot already registered */
    LIB_LOOP_ERR_NOENT  = -5, /* Agent or message not found */
    LIB_LOOP_ERR_FULL   = -6, /* Loop slot or mailbox queue full */
    LIB_LOOP_ERR_BUDGET = -7, /* Global step or handoff budget exceeded */
    LIB_LOOP_ERR_AGENT  = -8, /* Managed agent step failed */
    LIB_LOOP_ERR_ABORT  = -9  /* Loop execution cancelled or aborted */
} lib_loop_err_t;

/**
 * High-level execution state of lib_loop.
 */
typedef enum lib_loop_state {
    LIB_LOOP_ST_IDLE    = 0, /* Ready to run or awaiting next tick */
    LIB_LOOP_ST_RUNNING = 1, /* Actively dispatching agent steps */
    LIB_LOOP_ST_DONE    = 2, /* All managed agents finished cleanly */
    LIB_LOOP_ST_ABORTED = 3  /* Stopped due to error, budget, or cancel */
} lib_loop_state_t;

/**
 * Message categories exchanged across coordinated agents.
 */
typedef enum lib_loop_msg_kind {
    LIB_LOOP_MSG_TASK    = 1, /* Work item or prompt dispatched to target agent */
    LIB_LOOP_MSG_RESULT  = 2, /* Output artifact produced by an agent */
    LIB_LOOP_MSG_HANDOFF = 3, /* Control/context handoff to next pipeline stage */
    LIB_LOOP_MSG_ERROR   = 4  /* Error or diagnostic notification */
} lib_loop_msg_kind_t;

/**
 * A message popped from an agent's inbox.
 *
 * Caller owns payload memory after lib_loop_pop() succeeds and must free it
 * via free_fn (or free() when free_fn is NULL).
 */
typedef struct lib_loop_msg {
    char               *from;                      /* Sender agent name (heap string, caller frees) */
    char               *to;                        /* Recipient agent name or "*" (heap string, caller frees) */
    lib_loop_msg_kind_t kind;                      /* Message category */
    void               *payload;                   /* Message payload bytes */
    size_t              len;                       /* Payload byte length */
    void              (*free_fn)(void *payload);   /* Optional custom payload destructor */
} lib_loop_msg_t;

/**
 * Snapshot emitted after a single scheduler tick.
 */
typedef struct lib_loop_tick_info {
    lib_loop_state_t      state;                   /* Current loop state */
    size_t                total_steps;             /* Total agent steps executed in this run */
    uint32_t              slot_id;                 /* Selected agent slot index, if any */
    const char           *agent_name;              /* Selected agent name, or NULL if idle/done */
    lib_agent_step_info_t step_info;               /* Latest step details from lib_agent_step */
} lib_loop_tick_info_t;

/**
 * Hook invoked after an agent completes one step.
 *
 * @param[in] user_ctx   Caller context pointer passed in lib_loop_cfg_t.
 * @param[in] shared_ctx Shared application context bound to the loop.
 * @param[in] skills     Shared skill registry bound to the loop (may be NULL).
 * @param[in] tick_info  Read-only snapshot of the step just executed.
 * @return 0 to continue scheduling, or non-zero to abort the loop.
 */
typedef int (*lib_loop_on_step_fn)(void                       *user_ctx,
                                   void                       *shared_ctx,
                                   const lib_skill_reg_t      *skills,
                                   const lib_loop_tick_info_t *tick_info);

/**
 * Optional hooks attached to lib_loop.
 */
typedef struct lib_loop_ops {
    lib_loop_on_step_fn on_step;                   /* Optional per-tick observer hook */
} lib_loop_ops_t;

/**
 * Configuration parameters for creating a lib_loop instance.
 */
typedef struct lib_loop_cfg {
    const char            *name;                   /* Optional loop identifier */
    size_t                 max_total_steps;        /* Total step cap across all agents (0 = default) */
    size_t                 max_inbox_msgs;         /* Max queued messages per agent (0 = default) */
    int                    stop_on_err;            /* Non-zero aborts loop when an agent step fails */
    void                  *shared_ctx;             /* Shared context pointer visible to hooks/agents */
    const lib_skill_reg_t *skills;                 /* Shared skill registry bound to the loop */
    void                  *user_ctx;               /* Caller context forwarded to callbacks */
} lib_loop_cfg_t;

/**
 * Opaque loop coordinator handle.
 */
typedef struct lib_loop lib_loop_t;

/**
 * Convert a lib_loop_err_t code to a human-readable string.
 *
 * @param[in] err Error code to format.
 * @return Constant NUL-terminated string describing @p err.
 */
LIBAGENT_API const char *lib_loop_strerror(lib_loop_err_t err);

/**
 * Convert a lib_loop_state_t enum to a human-readable string.
 *
 * @param[in] state Loop state value to format.
 * @return Constant NUL-terminated string describing @p state.
 */
LIBAGENT_API const char *lib_loop_state_str(lib_loop_state_t state);

/**
 * Create a new multi-agent loop coordinator.
 *
 * @param[in] cfg Optional loop configuration; NULL applies defaults.
 * @param[in] ops Optional observer hooks; NULL disables hooks.
 * @return Newly allocated loop handle, or NULL on allocation failure.
 */
LIBAGENT_API lib_loop_t *lib_loop_new(const lib_loop_cfg_t *cfg,
                                      const lib_loop_ops_t *ops);

/**
 * Destroy a loop coordinator and free all loop-owned memory.
 *
 * Does not free attached lib_agent_t instances or external lib_skill_reg_t.
 *
 * @param[in] loop Loop handle to destroy; NULL is allowed and ignored.
 */
LIBAGENT_API void lib_loop_free(lib_loop_t *loop);

/**
 * Reset loop counters, state, and queued messages for a fresh run.
 *
 * Also resets attached lib_agent_t instances to IDLE.
 *
 * @param[in,out] loop Target loop handle.
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_reset(lib_loop_t *loop);

/**
 * Request cancellation and propagate cancel to all attached agents.
 *
 * @param[in,out] loop Target loop handle; NULL is allowed and ignored.
 */
LIBAGENT_API void lib_loop_cancel(lib_loop_t *loop);

/**
 * Bind or replace the shared skill registry pointer.
 *
 * @param[in,out] loop   Target loop handle.
 * @param[in]     skills Skill registry pointer (may be NULL to unbind).
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_set_skills(lib_loop_t            *loop,
                                                const lib_skill_reg_t *skills);

/**
 * Bind or replace the shared context pointer.
 *
 * @param[in,out] loop       Target loop handle.
 * @param[in]     shared_ctx Opaque shared context pointer (may be NULL).
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_set_shared(lib_loop_t *loop,
                                                void       *shared_ctx);

/**
 * Retrieve the bound shared skill registry pointer (or NULL).
 *
 * @param[in] loop Target loop handle; NULL returns NULL.
 * @return Bound skill registry pointer, or NULL if unset.
 */
LIBAGENT_API const lib_skill_reg_t *lib_loop_skills(const lib_loop_t *loop);

/**
 * Retrieve the bound shared context pointer (or NULL).
 *
 * @param[in] loop Target loop handle; NULL returns NULL.
 * @return Bound shared context pointer, or NULL if unset.
 */
LIBAGENT_API void *lib_loop_shared(const lib_loop_t *loop);

/**
 * Attach an existing lib_agent_t into the loop scheduler.
 *
 * @param[in,out] loop        Target loop handle.
 * @param[in,out] agent       Agent instance to schedule (caller retains ownership).
 * @param[in]     priority    Scheduling priority (higher values scheduled earlier).
 * @param[out]    out_slot_id Optional output receiving assigned slot index.
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_attach(lib_loop_t  *loop,
                                            lib_agent_t *agent,
                                            int          priority,
                                            uint32_t    *out_slot_id);

/**
 * Enqueue a message to a named agent, or to all agents when @p to is "*" or NULL.
 *
 * If @p copy_payload is non-zero, @p payload is copied into a heap buffer and
 * @p free_fn is ignored; otherwise @p free_fn (or free) takes ownership of @p payload.
 *
 * @param[in,out] loop         Target loop handle.
 * @param[in]     from         Optional sender name; NULL or empty defaults to "system".
 * @param[in]     to           Recipient agent name, or "*" / NULL for broadcast.
 * @param[in]     kind         Message category (lib_loop_msg_kind_t).
 * @param[in]     payload      Message payload pointer (may be NULL when @p len is 0).
 * @param[in]     len          Payload byte length.
 * @param[in]     copy_payload Non-zero to deep-copy @p payload, zero to transfer ownership.
 * @param[in]     free_fn      Optional destructor for @p payload when taking ownership.
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_send(lib_loop_t        *loop,
                                        const char          *from,
                                        const char          *to,
                                        lib_loop_msg_kind_t  kind,
                                        const void          *payload,
                                        size_t               len,
                                        int                  copy_payload,
                                        void               (*free_fn)(void *payload));

/**
 * Pop the next pending message from @p agent_name's inbox.
 *
 * @param[in,out] loop       Target loop handle.
 * @param[in]     agent_name Target agent name owning the inbox.
 * @param[out]    out_msg    Receives popped message; caller frees via lib_loop_msg_cleanup().
 * @return LIB_LOOP_OK on success, LIB_LOOP_ERR_NOENT if empty/missing, or error code.
 */
LIBAGENT_API lib_loop_err_t lib_loop_pop(lib_loop_t   *loop,
                                       const char     *agent_name,
                                       lib_loop_msg_t *out_msg);

/**
 * Release fields inside a message returned by lib_loop_pop().
 *
 * @param[in,out] msg Message struct to clean up; NULL is allowed and ignored.
 */
LIBAGENT_API void lib_loop_msg_cleanup(lib_loop_msg_t *msg);

/**
 * Execute a single scheduling tick across attached agents.
 *
 * @param[in,out] loop     Target loop handle.
 * @param[out]    out_info Optional snapshot of the executed tick.
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_tick(lib_loop_t           *loop,
                                          lib_loop_tick_info_t *out_info);

/**
 * Run the loop until completion, budget exhaustion, error, or cancellation.
 *
 * @param[in,out] loop     Target loop handle.
 * @param[out]    out_info Optional snapshot of the final or stopping tick.
 * @return LIB_LOOP_OK on success, or a negative lib_loop_err_t code on error.
 */
LIBAGENT_API lib_loop_err_t lib_loop_run(lib_loop_t           *loop,
                                         lib_loop_tick_info_t *out_info);

/**
 * Current loop state.
 *
 * @param[in] loop Target loop handle; NULL returns LIB_LOOP_ST_ABORTED.
 * @return Current lib_loop_state_t value.
 */
LIBAGENT_API lib_loop_state_t lib_loop_state(const lib_loop_t *loop);

/**
 * Total steps executed in current or last run.
 *
 * @param[in] loop Target loop handle; NULL returns 0.
 * @return Number of agent steps executed so far.
 */
LIBAGENT_API size_t lib_loop_total_steps(const lib_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* LIBAGENT_LIB_LOOP_H */