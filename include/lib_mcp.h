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
    LIB_MCP_OK = 0,
    LIB_MCP_ERR_ARG = -1,
    LIB_MCP_ERR_MEM = -2,
    LIB_MCP_ERR_IO = -3,
    LIB_MCP_ERR_TIMEOUT = -4,
    LIB_MCP_ERR_PROTO = -5,
    LIB_MCP_ERR_RPC = -6,
    LIB_MCP_ERR_CLOSED = -7
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
    char *name;        /* Tool identifier exposed by the MCP server */
    char *description; /* Human-readable description or NULL if omitted */
    char *schema_json; /* Serialized JSON Schema string for tool arguments */
} lib_mcp_tool_t;

typedef struct lib_mcp_res {
    int   is_error; /* Non-zero when MCP tool execution reports isError=true */
    char *text;     /* Concatenated text content blocks or NULL if none */
    char *raw_json; /* Serialized JSON string of the full result object */
} lib_mcp_res_t;

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
 *   res - Tool invocation result structure to clean up; NULL is allowed.
 *
 * Return:
 *   None.
 */
void lib_mcp_free_res(lib_mcp_res_t *res);

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