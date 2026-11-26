/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_LLM_H__
#define __LIB_LLM_H__

#include "lib_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OpenAI-compatible Chat Completions client (non-streaming).
 *
 * Threading: one lib_llm_ctx_t must not be used concurrently from multiple
 * threads without external synchronization (last_err is written on the ctx).
 *
 * Memory:
 *   - Request side (lib_llm_msg_t / lib_llm_tool_def_t): all pointers are
 *     borrowed; the library never frees them.
 *   - Response side (lib_llm_res_t): all heap fields are owned by the library;
 *     always call lib_llm_free_res() after lib_llm_chat(), including on failure
 *     if out_res was partially filled (lib_llm_chat clears it on entry and
 *     frees on parse failure).
 *   - Transport post(): on success must malloc *out_body; lib_llm_chat frees it.
 */

typedef enum {
    LIB_LLM_OK          = 0,
    LIB_LLM_ERR_ARG     = -1,
    LIB_LLM_ERR_MEM     = -2,
    LIB_LLM_ERR_IO      = -3,
    LIB_LLM_ERR_TIMEOUT = -4,
    LIB_LLM_ERR_PROTO   = -5,
    LIB_LLM_ERR_HTTP    = -6,
    LIB_LLM_ERR_API     = -7
} lib_llm_err_t;

typedef enum {
    LIB_LLM_ROLE_SYSTEM    = 0,
    LIB_LLM_ROLE_USER      = 1,
    LIB_LLM_ROLE_ASSISTANT = 2,
    LIB_LLM_ROLE_TOOL      = 3
} lib_llm_role_t;

/**
 * Transport POST hook.
 * Return LIB_LLM_OK when HTTP status/body were obtained (including non-2xx).
 * Return LIB_LLM_ERR_* on transport/timeout/memory failure before a usable response.
 * On LIB_LLM_OK, *out_body is malloc'd (or NULL if empty) and owned by caller.
 */
typedef lib_llm_err_t (*lib_llm_post_fn)(void *io_ctx,
                                         const char *url,
                                         const char *api_key,
                                         const char *body_json,
                                         size_t body_len,
                                         int timeout_ms,
                                         size_t max_resp_sz,
                                         char **out_body,
                                         size_t *out_len,
                                         int *out_status);

typedef struct lib_llm_io {
    lib_llm_post_fn post; /* required */
    void           *ctx;  /* opaque, owned by caller */
} lib_llm_io_t;

typedef struct lib_llm_ctx lib_llm_ctx_t;

typedef struct lib_llm_cfg {
    lib_llm_io_t io;
    const char  *endpoint;    /* required */
    const char  *api_key;     /* optional; NULL/empty => no Authorization */
    const char  *model;       /* required */
    int          timeout_ms;  /* <=0 => default */
    size_t       max_resp_sz; /* 0 => default */
    double       temperature; /* <0 => omit field */
    int          max_tokens;  /* <=0 => omit field */
} lib_llm_cfg_t;

typedef struct lib_llm_tool_call {
    char *id;
    char *name;
    char *args_json;
} lib_llm_tool_call_t;

typedef struct lib_llm_msg {
    lib_llm_role_t       role;
    const char          *content;
    const char          *tool_call_id; /* required when role == TOOL */
    const char          *name;
    lib_llm_tool_call_t *tool_calls;   /* borrowed; assistant tool turns */
    size_t               tool_count;
} lib_llm_msg_t;

typedef struct lib_llm_tool_def {
    const char *name;          /* required */
    const char *description;   /* optional */
    const char *schema_json;   /* JSON object string; NULL => {} */
} lib_llm_tool_def_t;

typedef struct lib_llm_usage {
    int64_t prompt_tokens;
    int64_t completion_tokens;
    int64_t total_tokens;
} lib_llm_usage_t;

typedef struct lib_llm_res {
    char                *text;
    char                *finish_reason;
    char                *raw_json;
    lib_llm_tool_call_t *tool_calls;
    size_t               tool_count;
    lib_llm_usage_t      usage;
} lib_llm_res_t;

/**
 * Allocate and initialize an LLM client context from a configuration snapshot.
 *
 * Deep-copies configuration strings and normalizes defaults (such as timeout
 * and response size limits) so the caller may safely modify or stack-dispose
 * cfg after return.
 *
 * @param cfg     Configuration snapshot; must not be NULL, and cfg->io.post,
 *                cfg->endpoint, and cfg->model must be non-NULL and non-empty.
 *
 * @return Initialized context pointer on success, or NULL on invalid input or
 *         memory allocation failure.
 */
LIBAGENT_API lib_llm_ctx_t *lib_llm_open(const lib_llm_cfg_t *cfg);

/**
 * Destroy an LLM client context and release all owned resources.
 *
 * Safe to call with NULL. Does not close or free caller-owned transport state
 * referenced via cfg.io.ctx.
 *
 * @param ctx  Context handle created by lib_llm_open(), or NULL.
 */
LIBAGENT_API void lib_llm_close(lib_llm_ctx_t *ctx);

/**
 * Execute a synchronous chat completion request with optional tool definitions.
 *
 * Serializes msgs and tools into a chat-completions JSON payload, invokes the
 * configured transport callback, and parses the response into out_res. On
 * failure, out_res is reset to a clean zeroed state.
 *
 * @param ctx         Initialized LLM client context; must not be NULL.
 * @param msgs        Array of conversation messages; must not be NULL.
 * @param msg_count   Number of elements in msgs; must be greater than 0.
 * @param tools       Optional array of callable tool definitions; may be NULL
 *                    when tool_count is 0.
 * @param tool_count  Number of elements in tools.
 * @param out_res     Output response structure populated on success and zeroed
 *                    on error; must not be NULL.
 *
 * @return LIB_LLM_OK on success, or a negative lib_llm_err_t error code on
 *         failure.
 */
LIBAGENT_API lib_llm_err_t  lib_llm_chat(lib_llm_ctx_t *ctx,
                                         const lib_llm_msg_t *msgs,
                                         size_t msg_count,
                                         const lib_llm_tool_def_t *tools,
                                         size_t tool_count,
                                         lib_llm_res_t *out_res);

/**
 * Release all heap buffers owned by a response structure and zero its fields.
 *
 * Safe to call with NULL or an already zero-initialized/freed structure.
 *
 * @param res  Response structure to clean up, or NULL.
 */
LIBAGENT_API void lib_llm_free_res(lib_llm_res_t *res);

/**
 * Release an array of tool_call entries and the array buffer itself.
 *
 * Safe to call with NULL calls or count == 0.
 *
 * @param calls  Heap-allocated array of tool calls, or NULL.
 * @param count  Number of initialized elements in calls.
 */
LIBAGENT_API void lib_llm_free_calls(lib_llm_tool_call_t *calls, size_t count);

/**
 * Return a static human-readable description for a lib_llm_err_t status code.
 *
 * @param err  Error status code to describe.
 *
 * @return NUL-terminated static description string (never NULL).
 */
LIBAGENT_API const char *lib_llm_strerror(lib_llm_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* __LIB_LLM_H__ */