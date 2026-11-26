/**
 * Copyright (c) 2026 yanrbts
 *
 * Tool registry and dispatcher for libagent.
 */

#ifndef LIBAGENT_LIB_TOOL_H
#define LIBAGENT_LIB_TOOL_H

#include "lib_export.h"

#include <stddef.h>
#include "lib_llm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Error codes returned by tool registry and dispatch operations.
 */
typedef enum lib_tool_err {
    LIB_TOOL_OK           =  0,  /* Operation succeeded */
    LIB_TOOL_ERR_ARG      = -1,  /* Invalid or NULL argument */
    LIB_TOOL_ERR_MEM      = -2,  /* Memory allocation failure */
    LIB_TOOL_ERR_EXIST    = -3,  /* Tool with the same name already registered */
    LIB_TOOL_ERR_NOTFOUND = -4,  /* Requested tool does not exist in registry */
    LIB_TOOL_ERR_DISABLED = -5,  /* Tool is currently disabled */
    LIB_TOOL_ERR_JSON     = -6,  /* Tool arguments failed JSON or schema object validation */
    LIB_TOOL_ERR_EXEC     = -7   /* Fatal execution error reported by tool callback */
} lib_tool_err_t;

/**
 * Execution context passed to a tool callback for a single invocation.
 */
typedef struct lib_tool_call {
    const char *call_id;    /* Optional provider-assigned tool call identifier */
    const char *name;       /* Target tool name resolved by the registry */
    const char *args_json;  /* Validated JSON object string (defaults to "{}" if empty/NULL) */
    void       *user_ctx;   /* Optional caller/session/agent context for this invocation */
} lib_tool_call_t;

/**
 * Output payload produced by a tool execution.
 */
typedef struct lib_tool_res {
    char *content;          /* Heap-allocated string returned as role="tool" message content */
    int   is_error;         /* Non-zero if content describes a handled tool/domain error */
} lib_tool_res_t;

/**
 * Callback signature for executing a registered tool.
 *
 * @param[in]  call      Invocation metadata and validated JSON arguments.
 * @param[in]  tool_ctx  Private context registered with the tool specification.
 * @param[out] out_res   Result structure populated with heap-allocated output content.
 * @param[out] errbuf    Optional buffer for human-readable error diagnostics.
 * @param[in]  errlen    Capacity of errbuf in bytes.
 * @return               LIB_TOOL_OK on completion (including handled errors with
 *                       out_res->is_error set), or a negative lib_tool_err_t code on
 *                       fatal failure.
 */
typedef lib_tool_err_t (*lib_tool_fn_t)(const lib_tool_call_t *call,
                                        void                  *tool_ctx,
                                        lib_tool_res_t        *out_res);

/**
 * Optional destructor for releasing private tool context state.
 *
 * @param[in] tool_ctx   Private context pointer previously supplied in lib_tool_spec_t.
 */
typedef void (*lib_tool_dtor_t)(void *tool_ctx);

/**
 * Registration specification describing a tool and its lifecycle callbacks.
 */
typedef struct lib_tool_spec {
    const char      *name;         /* Unique non-empty tool identifier exposed to the model */
    const char      *desc;         /* Natural-language description of tool behavior and usage */
    const char      *schema_json;  /* JSON Schema object string for arguments (NULL/blank -> "{}") */
    const char      *tag;          /* Optional grouping tag (e.g., skill or MCP source identifier) */
    int              enabled;      /* Initial activation state (non-zero enabled, zero disabled) */
    lib_tool_fn_t    fn;           /* Required execution handler callback */
    void            *tool_ctx;     /* Optional opaque context passed to fn and dtor */
    lib_tool_dtor_t  dtor;         /* Optional destructor invoked on removal or registry teardown */
} lib_tool_spec_t;

/**
 * Opaque tool registry handle.
 */
typedef struct lib_tool_reg lib_tool_reg_t;

/**
 * Convert a tool error code into a static descriptive string.
 *
 * @param[in] err        Error code to translate.
 * @return               Constant NUL-terminated description string.
 */
LIBAGENT_API const char *lib_tool_strerror(lib_tool_err_t err);

/**
 * Allocate and initialize an empty tool registry instance.
 *
 * @return Newly allocated registry handle, or NULL on failure.
 */
LIBAGENT_API lib_tool_reg_t *lib_tool_new(void);

/**
 * Destroy a tool registry, invoke registered destructors, and release all memory.
 *
 * @param[in] reg        Registry handle to destroy (NULL is a safe no-op).
 */
LIBAGENT_API void lib_tool_free(lib_tool_reg_t *reg);

/**
 * Return the total number of tools currently stored in the registry.
 *
 * @param[in] reg        Registry handle to query.
 * @return               Total registered tool count, or 0 if reg is NULL.
 */
LIBAGENT_API size_t lib_tool_count(const lib_tool_reg_t *reg);

/**
 * Return the number of currently enabled tools in the registry.
 *
 * @param[in] reg        Registry handle to query.
 * @return               Enabled tool count, or 0 if reg is NULL.
 */
LIBAGENT_API size_t lib_tool_enabled_cnt(const lib_tool_reg_t *reg);

/**
 * Check whether a tool with the given name exists in the registry.
 *
 * @param[in] reg        Registry handle to query.
 * @param[in] name       Tool name to look up.
 * @return               1 if present, 0 otherwise.
 */
LIBAGENT_API int lib_tool_has(const lib_tool_reg_t *reg, const char *name);

/**
 * Register a new tool in the registry.
 *
 * @param[in,out] reg    Target tool registry.
 * @param[in]     spec   Tool specification to copy and register.
 * @return               LIB_TOOL_OK on success, or an error code on failure.
 */
LIBAGENT_API lib_tool_err_t lib_tool_add(lib_tool_reg_t *reg,
                             const lib_tool_spec_t *spec);

/**
 * Unregister a tool by name and invoke its destructor if configured.
 *
 * @param[in,out] reg    Target tool registry.
 * @param[in]     name   Name of the tool to remove.
 * @return               LIB_TOOL_OK on success, or LIB_TOOL_ERR_NOTFOUND / error code.
 */
LIBAGENT_API lib_tool_err_t lib_tool_del(lib_tool_reg_t  *reg,
                                        const char       *name);

/**
 * Enable or disable a single registered tool without removing it.
 *
 * @param[in,out] reg     Target tool registry.
 * @param[in]     name    Name of the target tool.
 * @param[in]     enabled Non-zero to enable, zero to disable.
 * @return                LIB_TOOL_OK on success, or an error code on failure.
 */
LIBAGENT_API lib_tool_err_t lib_tool_enable(lib_tool_reg_t *reg,
                                            const char     *name,
                                            int             enabled);

/**
 * Enable or disable all tools matching a given grouping tag.
 *
 * @param[in,out] reg      Target tool registry.
 * @param[in]     tag      Non-empty tag string to match against registered tools.
 * @param[in]     enabled  Non-zero to enable matching tools, zero to disable.
 * @param[out]    out_cnt  Optional output count of tools whose state was matched.
 * @return                 LIB_TOOL_OK on success, or an error code on failure.
 */
LIBAGENT_API lib_tool_err_t lib_tool_enable_tag(lib_tool_reg_t  *reg,
                                    const char     *tag,
                                    int             enabled,
                                    size_t         *out_cnt);

/**
 * Build a heap-allocated array of LLM tool definitions from enabled tools.
 *
 * @param[in]  reg       Source tool registry.
 * @param[out] out_defs  Receives allocated array of lib_llm_tool_def_t (NULL if count is 0).
 * @param[out] out_cnt   Receives number of entries written to *out_defs.
 * @return               LIB_TOOL_OK on success, or LIB_TOOL_ERR_MEM / LIB_TOOL_ERR_ARG.
 */
LIBAGENT_API lib_tool_err_t lib_tool_llm_defs(const lib_tool_reg_t  *reg,
                                  lib_llm_tool_def_t   **out_defs,
                                  size_t                *out_cnt);

/**
 * Release an array of LLM tool definitions produced by lib_tool_llm_defs().
 *
 * @param[in] defs       Array of tool definitions to free (NULL is a safe no-op).
 * @param[in] count      Number of elements in defs.
 */
LIBAGENT_API void lib_tool_free_defs(lib_llm_tool_def_t *defs, size_t count);

/**
 * Validate arguments and dispatch a tool invocation by tool name.
 *
 * @param[in]  reg       Source tool registry.
 * @param[in]  call_id   Optional tool call identifier from the model response.
 * @param[in]  name      Target tool name.
 * @param[in]  args_json JSON-encoded argument string (NULL/blank normalized to "{}").
 * @param[in]  user_ctx  Optional caller/session context forwarded in lib_tool_call_t.
 * @param[out] out_res   Receives tool execution output on success.
 * @return               LIB_TOOL_OK on success, or a negative lib_tool_err_t on failure.
 */
LIBAGENT_API lib_tool_err_t lib_tool_exec(const lib_tool_reg_t  *reg,
                              const char           *call_id,
                              const char           *name,
                              const char           *args_json,
                              void                 *user_ctx,
                              lib_tool_res_t       *out_res);

/**
 * Dispatch a tool invocation from a parsed lib_llm_tool_call_t record.
 *
 * @param[in]  reg       Source tool registry.
 * @param[in]  call      Parsed tool call descriptor from lib_llm_res_t.
 * @param[in]  user_ctx  Optional caller/session context forwarded to the tool.
 * @param[out] out_res   Receives tool execution output on success.
 * @return               LIB_TOOL_OK on success, or a negative lib_tool_err_t on failure.
 */
LIBAGENT_API lib_tool_err_t lib_tool_exec_call(const lib_tool_reg_t *reg,
                                   const lib_llm_tool_call_t *call,
                                   void                      *user_ctx,
                                   lib_tool_res_t            *out_res);

/**
 * Release heap memory inside a lib_tool_res_t and zero-initialize the struct.
 *
 * @param[in,out] res    Tool result structure to reset (NULL is a safe no-op).
 */
LIBAGENT_API void lib_tool_res_reset(lib_tool_res_t *res);

#ifdef __cplusplus
}
#endif

#endif /* LIBAGENT_LIB_TOOL_H */