/**
 * Copyright (c) 2026 yanrbts
 *
 * Synchronous HTTP client interface built on Mongoose with strict resource
 * bounds, header sanitization, and explicit diagnostic reporting.
 */
#ifndef __LIB_HTTP_H__
#define __LIB_HTTP_H__

#include "lib_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Result and error codes returned by lib_http operations.
 */
typedef enum lib_http_err {
    LIB_HTTP_OK          =  0, /**< Operation completed without error */
    LIB_HTTP_ERR_ARG     = -1, /**< Invalid argument, NULL pointer, or unsafe input */
    LIB_HTTP_ERR_INIT    = -2, /**< Context or transport initialization failed */
    LIB_HTTP_ERR_CONNECT = -3, /**< Failed to resolve or connect to remote endpoint */
    LIB_HTTP_ERR_SEND    = -4, /**< Failed while transmitting request headers or body */
    LIB_HTTP_ERR_RECV    = -5, /**< Failed or disconnected before receiving full response */
    LIB_HTTP_ERR_TIMEOUT = -6, /**< Request exceeded the configured deadline */
    LIB_HTTP_ERR_SIZE    = -7, /**< Response payload exceeded configured size limit */
    LIB_HTTP_ERR_PROTO   = -8, /**< Malformed or unexpected HTTP protocol framing */
    LIB_HTTP_ERR_NOMEM   = -9  /**< Memory allocation failed */
} lib_http_err_t;

/**
 * Single HTTP header key-value pair used for requests and captured responses.
 */
typedef struct lib_http_header {
    const char *name;  /**< Header field name (without trailing colon or CRLF) */
    const char *value; /**< Header field value */
} lib_http_header_t;

/**
 * Client configuration parameters applied when opening an HTTP context.
 */
typedef struct lib_http_cfg {
    int    timeout_ms;      /**< Default request timeout in milliseconds; non-positive uses default */
    size_t max_resp_sz;     /**< Maximum allowed response body size in bytes; 0 uses default */
    size_t max_hdr_cnt;     /**< Maximum number of response headers captured; 0 uses default */
    int    poll_step_ms;    /**< Event-loop poll interval in milliseconds; non-positive uses default */
    const char *ca_path;    /**< Optional CA bundle path for TLS verification; NULL uses system/default */
    int    tls_skip_verify; /**< Non-zero to skip peer certificate verification (testing only) */
} lib_http_cfg_t;

/**
 * Specification of a single outbound HTTP request.
 */
typedef struct lib_http_req {
    const char              *method;     /**< HTTP method token (e.g., GET, POST, PUT, PATCH, DELETE, HEAD) */
    const char              *url;        /**< Target endpoint URL (e.g., http://host:port/path) */
    const lib_http_header_t *headers;    /**< Optional array of custom request headers */
    size_t                   header_num; /**< Number of elements in headers array */
    const void              *body;       /**< Optional request payload buffer */
    size_t                   body_len;   /**< Length of request payload in bytes */
    int                      timeout_ms; /**< Per-request timeout in ms; non-positive inherits context default */
} lib_http_req_t;

/**
 * Owned HTTP response data populated upon successful request execution.
 */
typedef struct lib_http_res {
    int                status_code; /**< HTTP status code returned by server (e.g., 200, 404, 500) */
    char              *body;        /**< Heap-allocated body buffer with NUL terminator at body[body_len] */
    size_t             body_len;    /**< Exact byte length of response body */
    lib_http_header_t *headers;     /**< Heap-allocated array of captured response headers */
    size_t             header_num;  /**< Number of entries in headers array */
} lib_http_res_t;

/**
 * Opaque HTTP client context holding configuration and last diagnostic state.
 */
typedef struct lib_http_ctx lib_http_ctx_t;

/**
 * Allocates and initializes a new HTTP client context.
 *
 * @param[in]  cfg    Optional configuration overrides; NULL applies defaults.
 * @param[out] errbuf Optional buffer for human-readable error details.
 * @param[in]  errlen Capacity of errbuf in bytes.
 * @return            Newly allocated context on success, or NULL on failure.
 */
LIBAGENT_API lib_http_ctx_t *lib_http_open(const lib_http_cfg_t *cfg,
                                           char *errbuf,
                                           size_t errlen);

/**
 * Releases an HTTP client context. Safe to call with NULL.
 *
 * @param[in] ctx Context instance to destroy.
 */
LIBAGENT_API void lib_http_close(lib_http_ctx_t *ctx);

/**
 * Executes a synchronous HTTP request and populates out_res on success.
 *
 * @param[in]  ctx     Initialized HTTP client context.
 * @param[in]  req     Request specification to execute.
 * @param[out] out_res Output response structure (reset on entry; freed on error).
 * @param[out] errbuf  Optional buffer for human-readable error details.
 * @param[in]  errlen  Capacity of errbuf in bytes.
 * @return             LIB_HTTP_OK on success, or a negative lib_http_err_t code.
 */
LIBAGENT_API lib_http_err_t lib_http_exec(lib_http_ctx_t *ctx,
                                          const lib_http_req_t *req,
                                          lib_http_res_t *out_res,
                                          char *errbuf,
                                          size_t errlen);

/**
 * Executes a JSON request with standard JSON headers and optional Bearer token.
 *
 * @param[in]  ctx        Initialized HTTP client context.
 * @param[in]  method     HTTP method (e.g., "POST", "PUT", "PATCH"); NULL defaults to "POST".
 * @param[in]  url        Target endpoint URL.
 * @param[in]  bearer_tok Optional auth token; prepends "Bearer " unless already prefixed.
 * @param[in]  json_body  Non-NULL NUL-terminated JSON payload string.
 * @param[in]  timeout_ms Per-request timeout in ms; non-positive inherits context default.
 * @param[out] out_res    Output response structure.
 * @param[out] errbuf     Optional buffer for human-readable error details.
 * @param[in]  errlen     Capacity of errbuf in bytes.
 * @return                LIB_HTTP_OK on success, or a negative lib_http_err_t code.
 */
LIBAGENT_API lib_http_err_t lib_http_request_json(lib_http_ctx_t *ctx,
                                                  const char *method,
                                                  const char *url,
                                                  const char *bearer_tok,
                                                  const char *json_body,
                                                  int timeout_ms,
                                                  lib_http_res_t *out_res,
                                                  char *errbuf,
                                                  size_t errlen);

/**
 * Convenience wrapper for sending an HTTP POST request with a JSON payload.
 *
 * @param[in]  ctx        Initialized HTTP client context.
 * @param[in]  url        Target endpoint URL.
 * @param[in]  bearer_tok Optional auth token; prepends "Bearer " unless already prefixed.
 * @param[in]  json_body  Non-NULL NUL-terminated JSON payload string.
 * @param[in]  timeout_ms Per-request timeout in ms; non-positive inherits context default.
 * @param[out] out_res    Output response structure.
 * @param[out] errbuf     Optional buffer for human-readable error details.
 * @param[in]  errlen     Capacity of errbuf in bytes.
 * @return                LIB_HTTP_OK on success, or a negative lib_http_err_t code.
 */
LIBAGENT_API lib_http_err_t lib_http_post_json(lib_http_ctx_t *ctx,
                                                 const char *url,
                                                 const char *bearer_tok,
                                                 const char *json_body,
                                                 int timeout_ms,
                                                 lib_http_res_t *out_res,
                                                 char *errbuf,
                                                 size_t errlen);

/**
 * Releases all heap memory owned by res and resets its fields to zero.
 * Safe to call multiple times or with NULL.
 *
 * @param[in,out] res Response structure to clean up.
 */
LIBAGENT_API void lib_http_free_res(lib_http_res_t *res);

/**
 * Returns a static, human-readable description for a lib_http_err_t code.
 *
 * @param[in] err Error code to translate.
 * @return        Constant description string (never NULL).
 */
LIBAGENT_API const char *lib_http_strerror(lib_http_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* __LIB_HTTP_H__ */