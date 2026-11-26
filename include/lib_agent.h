/**
 * Copyright (c) 2026 yanrbts
 *
 * Agent state machine and execution loop interface for libagent.
 */

#ifndef LIBAGENT_LIB_AGENT_H
#define LIBAGENT_LIB_AGENT_H

#include <stddef.h>
#include "lib_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Error codes returned by lib_agent APIs and execution callbacks.
 */
typedef enum lib_agent_err {
    LIB_AGENT_OK         =  0,  /* Operation succeeded */
    LIB_AGENT_ERR_ARG    = -1,  /* Invalid or NULL argument */
    LIB_AGENT_ERR_MEM    = -2,  /* Memory allocation failure */
    LIB_AGENT_ERR_STATE  = -3,  /* Invalid state machine transition or terminal state */
    LIB_AGENT_ERR_STEP   = -4,  /* Step limit exceeded before reaching completion */
    LIB_AGENT_ERR_LLM    = -5,  /* LLM turn callback failed */
    LIB_AGENT_ERR_TOOL   = -6,  /* Tool execution callback failed or policy aborted */
    LIB_AGENT_ERR_ABORT  = -7   /* Execution cancelled via API or step hook */
} lib_agent_err_t;

/**
 * Execution states of an agent state machine instance.
 */
typedef enum lib_agent_state {
    LIB_AGENT_ST_IDLE = 0,      /* Ready for a new run or next loop iteration */
    LIB_AGENT_ST_LLM_CALL,      /* Invoking LLM backend with current context and tools */
    LIB_AGENT_ST_TOOL_EXEC,     /* Dispatching tool calls produced by the LLM turn */
    LIB_AGENT_ST_DONE,          /* Completed normally with a final assistant response */
    LIB_AGENT_ST_ABORTED        /* Terminated early due to error, step limit, or cancel */
} lib_agent_state_t;

/**
 * Classification of the output produced by a single LLM turn.
 */
typedef enum lib_agent_turn_kind {
    LIB_AGENT_TURN_FINAL = 0,   /* Model emitted a final response without tool calls */
    LIB_AGENT_TURN_TOOLS        /* Model requested one or more tool invocations */
} lib_agent_turn_kind_t;

/**
 * Telemetry and transition snapshot recorded for an agent step.
 */
typedef struct lib_agent_step_info {
    size_t            step_idx;     /* Zero-based index of the step being reported */
    lib_agent_state_t state;        /* State machine state after the step transition */
    size_t            tool_calls;   /* Total tool invocations handled in this step */
    size_t            tool_errors;  /* Tool invocations that failed or returned error */
} lib_agent_step_info_t;

/**
 * Configuration parameters for constructing a lib_agent_t instance.
 */
typedef struct lib_agent_cfg {
    const char *name;           /* Logical agent identifier for routing and diagnostics */
    const char *role_tag;       /* Optional skill or tool group tag associated with role */
    size_t      max_steps;      /* Maximum loop steps per run (0 uses default limit) */
    int         stop_on_tool_err;/* Non-zero to abort immediately when tool_errors > 0 */
    void       *user_ctx;       /* Caller-owned context pointer passed to callbacks */
} lib_agent_cfg_t;

/**
 * Callback table decoupling the core state machine from LLM inference,
 * tool dispatch, conversation history, and multi-agent orchestration.
 */
typedef struct lib_agent_ops {
    /**
     * Execute a single LLM inference turn and append assistant output to history.
     *
     * @param user_ctx   Caller-owned context from lib_agent_cfg_t.
     * @param agent_name Logical name of the executing agent instance.
     * @param step_idx   Zero-based index of the current loop step.
     * @param out_kind   Output turn classification (FINAL or TOOLS).
     * @param errbuf     Optional diagnostic buffer populated on failure.
     * @param errlen     Capacity of errbuf in bytes.
     * @return           LIB_AGENT_OK on success, or a negative error code.
     */
    lib_agent_err_t (*on_llm)(void                  *user_ctx,
                              const char            *agent_name,
                              size_t                 step_idx,
                              lib_agent_turn_kind_t *out_kind);

    /**
     * Dispatch pending tool calls from the latest LLM turn and record results.
     *
     * @param user_ctx   Caller-owned context from lib_agent_cfg_t.
     * @param agent_name Logical name of the executing agent instance.
     * @param step_idx   Zero-based index of the current loop step.
     * @param out_calls  Output count of tool calls dispatched in this step.
     * @param out_errors Output count of tool calls that errored in this step.
     * @param errbuf     Optional diagnostic buffer populated on failure.
     * @param errlen     Capacity of errbuf in bytes.
     * @return           LIB_AGENT_OK on success, or a negative error code.
     */
    lib_agent_err_t (*on_tools)(void        *user_ctx,
                                const char  *agent_name,
                                size_t       step_idx,
                                size_t      *out_calls,
                                size_t      *out_errors);

    /**
     * Optional post-step observation and control hook.
     *
     * @param user_ctx   Caller-owned context from lib_agent_cfg_t.
     * @param agent_name Logical name of the executing agent instance.
     * @param info       Read-only snapshot of the completed step.
     * @return           0 to continue execution, or non-zero to request abort.
     */
    int (*on_step)(void                        *user_ctx,
                   const char                  *agent_name,
                   const lib_agent_step_info_t *info);
} lib_agent_ops_t;

typedef struct lib_agent lib_agent_t;

/**
 * Convert an agent error code into a static, human-readable string.
 *
 * @param err Error code to describe.
 * @return    Constant null-terminated string describing err.
 */
LIBAGENT_API const char *lib_agent_strerror(lib_agent_err_t err);

/**
 * Convert an agent state enum value into a static symbolic label.
 *
 * @param state State machine state value.
 * @return      Constant null-terminated state name.
 */
LIBAGENT_API const char *lib_agent_state_str(lib_agent_state_t state);

/**
 * Allocate and initialize a new agent state machine instance.
 *
 * @param cfg    Optional configuration struct (NULL applies defaults).
 * @param ops    Required callback table with non-NULL on_llm and on_tools.
 * @param errbuf Optional diagnostic buffer populated on failure.
 * @param errlen Capacity of errbuf in bytes.
 * @return       Initialized lib_agent_t handle, or NULL on error.
 */
LIBAGENT_API lib_agent_t *lib_agent_new(const lib_agent_cfg_t *cfg,
                                        const lib_agent_ops_t *ops);

/**
 * Release all resources owned by an agent instance.
 *
 * @param ag Agent handle to destroy (NULL is a safe no-op).
 */
LIBAGENT_API void lib_agent_free(lib_agent_t *ag);

/**
 * Reset an agent instance to IDLE with zeroed step counter for a new run.
 *
 * @param ag     Target agent instance.
 * @return       LIB_AGENT_OK on success, or LIB_AGENT_ERR_ARG if ag is NULL.
 */
LIBAGENT_API lib_agent_err_t lib_agent_reset(lib_agent_t *ag);

/**
 * Request early termination of the active or upcoming run.
 *
 * @param ag Target agent instance (NULL is a safe no-op).
 */
LIBAGENT_API void lib_agent_cancel(lib_agent_t *ag);

/**
 * Query the current state of the agent state machine.
 *
 * @param ag Agent instance to inspect.
 * @return   Current lib_agent_state_t, or LIB_AGENT_ST_ABORTED if ag is NULL.
 */
LIBAGENT_API lib_agent_state_t lib_agent_state(const lib_agent_t *ag);

/**
 * Query the number of completed steps in the current or last run.
 *
 * @param ag Agent instance to inspect.
 * @return   Completed step count, or 0 if ag is NULL.
 */
LIBAGENT_API size_t lib_agent_steps(const lib_agent_t *ag);

/**
 * Query the configured logical name of the agent instance.
 *
 * @param ag Agent instance to inspect.
 * @return   Null-terminated agent name, or NULL if ag is NULL.
 */
LIBAGENT_API const char *lib_agent_name(const lib_agent_t *ag);

/**
 * Advance the agent state machine by one step (LLM turn and optional tool turn).
 *
 * @param ag       Target agent instance.
 * @param out_info Optional output snapshot of the executed step.
 * @return         LIB_AGENT_OK on step completion, or a negative error code.
 */
LIBAGENT_API lib_agent_err_t lib_agent_step(lib_agent_t           *ag,
                                            lib_agent_step_info_t *out_info);

/**
 * Run the agent loop until reaching DONE, ABORTED, max_steps, or an error.
 *
 * @param ag       Target agent instance.
 * @param out_info Optional output snapshot of the final or last step.
 * @param errbuf   Optional diagnostic buffer populated on failure.
 * @param errlen   Capacity of errbuf in bytes.
 * @return         LIB_AGENT_OK when DONE is reached, or a negative error code.
 */
LIBAGENT_API lib_agent_err_t lib_agent_run(lib_agent_t           *ag,
                                           lib_agent_step_info_t *out_info);

#ifdef __cplusplus
}
#endif

#endif /* LIBAGENT_LIB_AGENT_H */