/**
 * Copyright (c) 2026 yanrbts
 *
 * Skill registry and prompt/tool-tag binding module for libagent.
 */
#ifndef __LIB_SKILL_H__
#define __LIB_SKILL_H__

#include "lib_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Result codes returned by lib_skill operations.
 */
typedef enum lib_skill_err {
    LIB_SKILL_OK        =  0, /* Operation succeeded */
    LIB_SKILL_ERR_ARG   = -1, /* Invalid argument or NULL pointer */
    LIB_SKILL_ERR_MEM   = -2, /* Memory allocation failure */
    LIB_SKILL_ERR_EXIST = -3, /* Skill with the same name already exists */
    LIB_SKILL_ERR_NOENT = -4, /* Requested skill was not found */
    LIB_SKILL_ERR_FULL  = -5  /* Registry reached maximum capacity */
} lib_skill_err_t;

/**
 * Specification describing a skill to register into a lib_skill_reg_t.
 */
typedef struct lib_skill_spec {
    const char *name;        /* Unique skill identifier (non-empty) */
    const char *desc;        /* Short human-readable summary of the skill */
    const char *prompt;      /* System or task instructions injected when active */
    const char *tool_tag;    /* Optional tool registry tag filter (NULL or empty for all) */
    int         enabled;     /* Non-zero if enabled by default, zero if disabled */
} lib_skill_spec_t;

/**
 * Read-only view of a registered skill entry.
 */
typedef struct lib_skill_info {
    const char *name;        /* Skill identifier */
    const char *desc;        /* Skill summary */
    const char *prompt;      /* Skill instruction prompt */
    const char *tool_tag;    /* Associated tool tag filter, or NULL if unrestricted */
    int         enabled;     /* Non-zero if currently enabled */
} lib_skill_info_t;

/**
 * Opaque skill registry handle.
 */
typedef struct lib_skill_reg lib_skill_reg_t;

/**
 * Convert a lib_skill_err_t code into a human-readable string.
 *
 * @param[in] err Error code to format.
 * @return Constant string describing @p err.
 */
LIBAGENT_API const char *lib_skill_strerror(lib_skill_err_t err);

/**
 * Create a new skill registry instance.
 *
 * @return Newly allocated registry handle, or NULL on allocation failure.
 */
LIBAGENT_API lib_skill_reg_t *lib_skill_new(void);

/**
 * Destroy a skill registry and release all owned resources.
 *
 * @param[in] reg Registry handle to free; NULL is allowed and ignored.
 */
LIBAGENT_API void lib_skill_free(lib_skill_reg_t *reg);

/**
 * Register a new skill in the registry by copying its specification fields.
 *
 * @param[in,out] reg  Target skill registry.
 * @param[in]     spec Skill specification to insert.
 * @return LIB_SKILL_OK on success, or a negative lib_skill_err_t code on error.
 */
LIBAGENT_API lib_skill_err_t lib_skill_add(lib_skill_reg_t *reg,
                              const lib_skill_spec_t *spec);

/**
 * Remove a skill from the registry by name.
 *
 * @param[in,out] reg  Target skill registry.
 * @param[in]     name Name of the skill to remove.
 * @return LIB_SKILL_OK on success, LIB_SKILL_ERR_NOENT if missing, or another error code.
 */
LIBAGENT_API lib_skill_err_t lib_skill_remove(lib_skill_reg_t *reg,
                                 const char *name);

/**
 * Enable or disable a registered skill by name.
 *
 * @param[in,out] reg     Target skill registry.
 * @param[in]     name    Target skill name.
 * @param[in]     enabled Non-zero to enable, zero to disable.
 * @return LIB_SKILL_OK on success, LIB_SKILL_ERR_NOENT if missing, or another error code.
 */
LIBAGENT_API lib_skill_err_t lib_skill_set_enabled(lib_skill_reg_t *reg,
                                      const char *name,
                                      int enabled);

/**
 * Query metadata for a skill by name.
 *
 * @param[in]  reg      Source skill registry.
 * @param[in]  name     Target skill name.
 * @param[out] out_info Output snapshot struct populated on success.
 * @return LIB_SKILL_OK on success, LIB_SKILL_ERR_NOENT if missing, or another error code.
 */
LIBAGENT_API lib_skill_err_t lib_skill_get(const lib_skill_reg_t *reg,
                              const char *name,
                              lib_skill_info_t *out_info);

/**
 * Return the number of skills currently stored in the registry.
 *
 * @param[in] reg Target skill registry; NULL returns 0.
 * @return Number of registered skills.
 */
LIBAGENT_API size_t lib_skill_count(const lib_skill_reg_t *reg);

/**
 * Collect snapshots of matching skills into a caller-owned array.
 *
 * @param[in]  reg          Source skill registry.
 * @param[in]  enabled_only Non-zero to include only enabled skills; zero for all.
 * @param[out] out_items    Receives heap-allocated array of lib_skill_info_t items.
 * @param[out] out_count    Receives number of entries in @p *out_items.
 * @return LIB_SKILL_OK on success, or a negative lib_skill_err_t code on error.
 */
LIBAGENT_API lib_skill_err_t lib_skill_list(const lib_skill_reg_t *reg,
                               int enabled_only,
                               lib_skill_info_t **out_items,
                               size_t *out_count);

/**
 * Free an array previously returned by lib_skill_list().
 *
 * @param[in] items Array pointer to free; NULL is allowed and ignored.
 */
LIBAGENT_API void lib_skill_free_list(lib_skill_info_t *items);

/**
 * Build a combined prompt string from enabled skills.
 *
 * @param[in]  reg       Source skill registry.
 * @param[in]  sep       Separator inserted between skill prompts; NULL defaults to "\n\n".
 * @param[out] out_prompt Receives newly allocated NUL-terminated prompt string.
 * @return LIB_SKILL_OK on success, or a negative lib_skill_err_t code on error.
 */
LIBAGENT_API lib_skill_err_t lib_skill_build_prompt(const lib_skill_reg_t *reg,
                                       const char *sep,
                                       char **out_prompt);

#ifdef __cplusplus
}
#endif

#endif /* __LIB_SKILL_H__ */