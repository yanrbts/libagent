/**
 * Copyright (c) 2026 yanrbts
 *
 * Thread-safe structured error reporting, automatic classification, and real-time
 * log sink dispatch for libagent subsystems.
 */
#ifndef __LIB_ERROR_H__
#define __LIB_ERROR_H__

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum byte length of a formatted error message, including null terminator. */
#ifndef LIB_ERR_MSG_MAX
#define LIB_ERR_MSG_MAX 256u
#endif

/** Semantic error categories shared across modules for control-flow decisions. */
typedef enum lib_err_code {
    LIB_ERR_OK = 0,         /**< No error; operation succeeded. */
    LIB_ERR_INVALID_ARG,    /**< Invalid or out-of-range input argument. */
    LIB_ERR_NOMEM,          /**< Memory allocation failure. */
    LIB_ERR_NOT_FOUND,      /**< Requested resource, entry, or endpoint was not found. */
    LIB_ERR_IO,             /**< Underlying I/O, socket, or descriptor failure. */
    LIB_ERR_TIMEOUT,        /**< Operation exceeded its deadline or timeout budget. */
    LIB_ERR_PROTOCOL,       /**< Wire, framing, HTTP, JSON, or MCP protocol violation. */
    LIB_ERR_TOOL_EXEC,      /**< Tool invocation or external execution failure. */
    LIB_ERR_STATE,          /**< Invalid lifecycle or state-machine transition. */
    LIB_ERR_INTERNAL        /**< Unexpected internal invariant or logic failure. */
} lib_err_code_t;

/** Subsystem or module domain from which an error originated. */
typedef enum lib_err_domain {
    LIB_ERR_DOM_NONE = 0,   /**< Unspecified or cross-cutting domain. */
    LIB_ERR_DOM_HTTP,       /**< HTTP transport and client/server layer. */
    LIB_ERR_DOM_LLM,        /**< LLM provider client and streaming layer. */
    LIB_ERR_DOM_TOOL,       /**< Tool registry and execution layer. */
    LIB_ERR_DOM_AGENT,      /**< Agent execution loop and state machine. */
    LIB_ERR_DOM_MCP,        /**< Model Context Protocol client/transport layer. */
    LIB_ERR_DOM_SKILL,      /**< Skill loading and execution layer. */
    LIB_ERR_DOM_LOOP        /**< Multi-agent scheduler and mailbox loop layer. */
} lib_err_domain_t;

/** Diagnostic severity level derived from error semantics and retryability. */
typedef enum lib_err_severity {
    LIB_ERR_SEV_WARN = 0,   /**< Transient or recoverable condition (typically retryable). */
    LIB_ERR_SEV_ERROR,      /**< Actionable runtime failure for the current operation. */
    LIB_ERR_SEV_FATAL       /**< Severe condition such as exhaustion of memory. */
} lib_err_severity_t;

/** Structured error snapshot stored per thread and passed to log sinks. */
typedef struct lib_err {
    lib_err_code_t     code;               /**< Unified semantic error code. */
    lib_err_domain_t   domain;             /**< Origin subsystem or module domain. */
    lib_err_severity_t severity;           /**< Inferred diagnostic severity level. */
    int                sys_errno;          /**< Captured POSIX errno value, or 0 if none. */
    int                status_code;        /**< External/protocol status code (e.g. HTTP), or 0. */
    int                retryable;          /**< Non-zero if retrying may succeed; 0 otherwise. */
    char               msg[LIB_ERR_MSG_MAX]; /**< Null-terminated human-readable message. */
} lib_err_t;

/**
 * Callback signature invoked synchronously when a structured error is emitted.
 *
 * @param[in] err      Pointer to the immutable structured error descriptor.
 * @param[in] userdata Opaque caller context pointer registered with the sink.
 */
typedef void (*lib_err_log_fn)(const lib_err_t *err, void *userdata);

/**
 * Register the error log sink callback for the calling thread.
 *
 * @param[in] fn       Log sink callback function, or NULL to disable sink dispatch.
 * @param[in] userdata Opaque pointer forwarded to @p fn on each error emission.
 */
void lib_err_set_log_sink(lib_err_log_fn fn, void *userdata);

/**
 * Retrieve a pointer to the most recent error recorded on the calling thread.
 *
 * The returned pointer remains valid until the calling thread emits another
 * error or calls lib_err_clear(); copy the struct by value if persistence is needed.
 *
 * @return Pointer to the thread-local lib_err_t record (never NULL).
 */
const lib_err_t *lib_err_last(void);

/**
 * Reset the calling thread's stored error record to a clean OK state.
 */
void lib_err_clear(void);

/**
 * Convert a semantic error code into a static uppercase diagnostic label.
 *
 * @param[in] code Semantic error code to format.
 * @return Non-NULL static string label.
 */
const char *lib_err_code_str(lib_err_code_t code);

/**
 * Convert a subsystem domain into a static uppercase diagnostic label.
 *
 * @param[in] domain Error domain to format.
 * @return Non-NULL static string label.
 */
const char *lib_err_domain_str(lib_err_domain_t domain);

/**
 * Convert a severity level into a static uppercase diagnostic label.
 *
 * @param[in] sev Severity level to format.
 * @return Non-NULL static string label.
 */
const char *lib_err_severity_str(lib_err_severity_t sev);

/**
 * Map a POSIX errno value to a unified semantic lib_err_code_t value.
 *
 * @param[in] sys_errno POSIX errno value to classify.
 * @return Corresponding semantic error code.
 */
lib_err_code_t lib_err_code_from_errno(int sys_errno);

/**
 * Infer whether an operation may succeed if retried.
 *
 * @param[in] code        Semantic error code.
 * @param[in] sys_errno   Associated POSIX errno value, or 0 if not applicable.
 * @param[in] status_code Associated protocol or HTTP status code, or 0 if none.
 * @return Non-zero if retryable, or 0 if non-retryable.
 */
int lib_err_infer_retryable(lib_err_code_t code, int sys_errno, int status_code);

/**
 * Infer diagnostic severity from semantic error code and retryability.
 *
 * @param[in] code      Semantic error code.
 * @param[in] retryable Non-zero if the error is considered retryable.
 * @return Inferred lib_err_severity_t value.
 */
lib_err_severity_t lib_err_infer_severity(lib_err_code_t code, int retryable);

/**
 * Build a structured error record, update thread-local state, and dispatch to the log sink.
 *
 * @param[in] domain      Origin subsystem domain.
 * @param[in] code        Unified semantic error code.
 * @param[in] sys_errno   POSIX errno value, or 0 if not applicable; when non-zero,
 *                        system error text and code are appended to the message.
 * @param[in] status_code External/protocol status code (e.g. HTTP status), or 0.
 * @param[in] fmt         printf-style format string for the error message, or NULL.
 * @param[in] ...         Arguments referenced by @p fmt.
 */
void lib_err_emit(lib_err_domain_t domain,
                  lib_err_code_t code,
                  int sys_errno,
                  int status_code,
                  const char *fmt, ...);

/**
 * Emit a system-originated error by classifying @p sys_errno automatically.
 *
 * @param[in] domain      Origin subsystem domain.
 * @param[in] sys_errno   POSIX errno value capturing the system failure.
 * @param[in] status_code External/protocol status code, or 0 if not applicable.
 * @param[in] fmt         printf-style format string for additional context, or NULL.
 * @param[in] ...         Arguments referenced by @p fmt.
 */
void lib_err_emit_sys(lib_err_domain_t domain,
                      int sys_errno,
                      int status_code,
                      const char *fmt, ...);

/**
 * va_list variant of error emission used by wrapper helpers.
 *
 * @param[in] domain      Origin subsystem domain.
 * @param[in] code        Unified semantic error code.
 * @param[in] sys_errno   POSIX errno value, or 0 if not applicable; when non-zero,
 *                        system error text and code are appended to the message.
 * @param[in] status_code External/protocol status code (e.g. HTTP status), or 0.
 * @param[in] fmt         printf-style format string for the error message, or NULL.
 * @param[in] ap          Initialized variadic argument list matching @p fmt.
 */
void lib_err_vemit(lib_err_domain_t domain,
                   lib_err_code_t code,
                   int sys_errno,
                   int status_code,
                   const char *fmt,
                   va_list ap);

/**
 * Emit a non-system, non-status structured error with fixed zero errno and status_code.
 *
 * @param[in] domain Origin subsystem domain.
 * @param[in] code   Unified semantic error code.
 * @param[in] fmt    printf-style format string for the error message, or NULL.
 * @param[in] ...    Arguments referenced by @p fmt.
 */
void lib_err_emit_msg(lib_err_domain_t domain,
                      lib_err_code_t code,
                      const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* LIB_ERROR_H */