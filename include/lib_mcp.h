/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_MCP_H__
#define __LIB_MCP_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LIB_MCP_OK          = 0,
    LIB_MCP_ERR_ARG     = -1,
    LIB_MCP_ERR_MEM     = -2,
    LIB_MCP_ERR_IO      = -3,
    LIB_MCP_ERR_TIMEOUT = -4,
    LIB_MCP_ERR_PROTO   = -5,
    LIB_MCP_ERR_RPC     = -6,
    LIB_MCP_ERR_CLOSED  = -7
} lib_mcp_err_t;

typedef struct lib_mcp_ctx lib_mcp_ctx_t;

typedef struct lib_mcp_cfg {
    const char        *cmd;         /* Executable path or command name passed to execvp */
    const char *const *argv;        /* NULL-terminated argument vector starting with argv[0] */
    const char *const *envp;        /* Optional NULL-terminated environment vector; NULL inherits */
    int                timeout_ms;  /* Default per-request timeout in milliseconds */
    size_t             max_line_sz; /* Maximum single JSON-RPC line size in bytes before failing */
} lib_mcp_cfg_t;

typedef struct lib_mcp_tool {
    char *name;                     /* Tool identifier exposed by the MCP server */
    char *description;              /* Human-readable description or NULL if omitted */
    char *schema_json;              /* Serialized JSON Schema string for tool arguments */
} lib_mcp_tool_t;

typedef struct lib_mcp_res {
    int   is_error;                 /* Non-zero when MCP tool execution reports isError=true */
    char *text;                     /* Concatenated text content blocks or NULL if none */
    char *raw_json;                 /* Serialized JSON string of the full result object */
} lib_mcp_res_t;

typedef struct lib_mcp_resource {
    char *uri;                      /* Resource URI identifier exposed by the MCP server */
    char *name;                     /* Human-readable resource name or NULL if omitted */
    char *description;              /* Human-readable resource description or NULL if omitted */
    char *mime_type;                /* Optional MIME media type string or NULL if omitted */
} lib_mcp_resource_t;

typedef struct lib_mcp_prompt_arg {
    char *name;                     /* Prompt argument name accepted by the template */
    char *description;              /* Human-readable argument description or NULL if omitted */
    int   required;                 /* Non-zero if the argument is required, 0 otherwise */
} lib_mcp_prompt_arg_t;

typedef struct lib_mcp_prompt {
    char                 *name;        /* Prompt template identifier exposed by the MCP server */
    char                 *description; /* Human-readable prompt description or NULL if omitted */
    lib_mcp_prompt_arg_t *arguments;   /* Array of declared prompt arguments or NULL if none */
    size_t                arg_count;   /* Number of elements in arguments */
} lib_mcp_prompt_t;

/*
 * Spawns an MCP stdio server subprocess, establishes bidirectional pipes,
 * performs the MCP initialize handshake, and returns an initialized client context.
 *
 * Parameters:
 *   cfg    - Non-NULL stdio server launch and transport configuration.
 *   errbuf - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   Pointer to a newly allocated lib_mcp_ctx_t on success, or NULL on failure.
 */
lib_mcp_ctx_t *lib_mcp_open(const lib_mcp_cfg_t *cfg, char *errbuf, size_t errlen);

/*
 * Closes transport descriptors, terminates and reaps the child server process
 * if still running, and frees all memory owned by the client context.
 *
 * Parameters:
 *   ctx - Client context handle returned by lib_mcp_open(); NULL is a no-op.
 *
 * Return:
 *   None.
 */
void lib_mcp_close(lib_mcp_ctx_t *ctx);

/*
 * Queries the connected MCP server via tools/list and allocates an array of
 * tool descriptors. Caller must release the array using lib_mcp_free_tools().
 *
 * Parameters:
 *   ctx       - Active MCP client context handle.
 *   out_tools - Non-NULL output pointer receiving the allocated tool descriptor array.
 *   out_count - Non-NULL output pointer receiving the number of elements in *out_tools.
 *   errbuf    - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen    - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   LIB_MCP_OK on success, or a negative lib_mcp_err_t code on error.
 */
lib_mcp_err_t lib_mcp_tools(lib_mcp_ctx_t *ctx,
                            lib_mcp_tool_t **out_tools,
                            size_t *out_count,
                            char *errbuf,
                            size_t errlen);

/*
 * Frees all heap memory owned by each element in tools and frees the array itself.
 *
 * Parameters:
 *   tools - Tool descriptor array allocated by lib_mcp_tools(); NULL is allowed.
 *   count - Number of elements contained in the tools array.
 *
 * Return:
 *   None.
 */
void lib_mcp_free_tools(lib_mcp_tool_t *tools, size_t count);

/*
 * Invokes a named MCP tool via tools/call with optional JSON object arguments
 * and populates out_res. Caller must release out_res fields via lib_mcp_free_res().
 *
 * Parameters:
 *   ctx       - Active MCP client context handle.
 *   name      - Non-empty name of the target MCP tool to invoke.
 *   args_json - Optional JSON object string specifying tool arguments; NULL or empty defaults to {}.
 *   out_res   - Non-NULL output structure receiving execution status and result payloads.
 *   errbuf    - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen    - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   LIB_MCP_OK on success, or a negative lib_mcp_err_t code on error.
 */
lib_mcp_err_t lib_mcp_call(lib_mcp_ctx_t *ctx,
                           const char *name,
                           const char *args_json,
                           lib_mcp_res_t *out_res,
                           char *errbuf,
                           size_t errlen);

/*
 * Releases heap buffers owned by res and resets all structure fields to zero.
 *
 * Parameters:
 *   res - Result structure to clean up; NULL is allowed.
 *
 * Return:
 *   None.
 */
void lib_mcp_free_res(lib_mcp_res_t *res);

/* Resources */

/*
 * Queries the connected MCP server via resources/list and allocates an array
 * of resource descriptors. Caller must release the array using lib_mcp_free_resources().
 *
 * Parameters:
 *   ctx           - Active MCP client context handle.
 *   out_resources - Non-NULL output pointer receiving the allocated resource array.
 *   out_count     - Non-NULL output pointer receiving the number of elements in *out_resources.
 *   errbuf        - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen        - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   LIB_MCP_OK on success, or a negative lib_mcp_err_t code on error.
 */
lib_mcp_err_t lib_mcp_resources(lib_mcp_ctx_t *ctx,
                                lib_mcp_resource_t **out_resources,
                                size_t *out_count,
                                char *errbuf,
                                size_t errlen);

/*
 * Frees all heap memory owned by each element in resources and frees the array itself.
 *
 * Parameters:
 *   resources - Resource descriptor array allocated by lib_mcp_resources(); NULL is allowed.
 *   count     - Number of elements contained in the resources array.
 *
 * Return:
 *   None.
 */
void lib_mcp_free_resources(lib_mcp_resource_t *resources, size_t count);

/*
 * Reads a resource identified by uri via resources/read and populates out_res.
 * Caller must release out_res fields via lib_mcp_free_res().
 *
 * Parameters:
 *   ctx     - Active MCP client context handle.
 *   uri     - Non-empty URI of the target MCP resource to read.
 *   out_res - Non-NULL output structure receiving aggregated content and raw JSON.
 *   errbuf  - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen  - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   LIB_MCP_OK on success, or a negative lib_mcp_err_t code on error.
 */
lib_mcp_err_t lib_mcp_read_resource(lib_mcp_ctx_t *ctx,
                                    const char *uri,
                                    lib_mcp_res_t *out_res,
                                    char *errbuf,
                                    size_t errlen);

/* Prompts */

/*
 * Queries the connected MCP server via prompts/list and allocates an array
 * of prompt descriptors. Caller must release the array using lib_mcp_free_prompts().
 *
 * Parameters:
 *   ctx         - Active MCP client context handle.
 *   out_prompts - Non-NULL output pointer receiving the allocated prompt descriptor array.
 *   out_count   - Non-NULL output pointer receiving the number of elements in *out_prompts.
 *   errbuf      - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen      - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   LIB_MCP_OK on success, or a negative lib_mcp_err_t code on error.
 */
lib_mcp_err_t lib_mcp_prompts(lib_mcp_ctx_t *ctx,
                              lib_mcp_prompt_t **out_prompts,
                              size_t *out_count,
                              char *errbuf,
                              size_t errlen);

/*
 * Frees all heap memory owned by each prompt element and its argument array,
 * then frees the top-level prompts array.
 *
 * Parameters:
 *   prompts - Prompt descriptor array allocated by lib_mcp_prompts(); NULL is allowed.
 *   count   - Number of elements contained in the prompts array.
 *
 * Return:
 *   None.
 */
void lib_mcp_free_prompts(lib_mcp_prompt_t *prompts, size_t count);

/*
 * Renders a named MCP prompt template via prompts/get with optional JSON object
 * arguments and populates out_res. Caller must release out_res fields via lib_mcp_free_res().
 *
 * Parameters:
 *   ctx       - Active MCP client context handle.
 *   name      - Non-empty name of the target MCP prompt template.
 *   args_json - Optional JSON object string specifying template arguments; NULL or empty defaults to {}.
 *   out_res   - Non-NULL output structure receiving rendered message text and raw JSON.
 *   errbuf    - Optional caller-provided buffer for formatted error diagnostics.
 *   errlen    - Capacity of errbuf in bytes, including the null terminator.
 *
 * Return:
 *   LIB_MCP_OK on success, or a negative lib_mcp_err_t code on error.
 */
lib_mcp_err_t lib_mcp_get_prompt(lib_mcp_ctx_t *ctx,
                                 const char *name,
                                 const char *args_json,
                                 lib_mcp_res_t *out_res,
                                 char *errbuf,
                                 size_t errlen);

/*
 * Maps an error code to a static human-readable diagnostic description.
 *
 * Parameters:
 *   err - Error code returned by a lib_mcp_* function.
 *
 * Return:
 *   Constant null-terminated string describing err; never NULL.
 */
const char *lib_mcp_strerror(lib_mcp_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* LIB_MCP_H */