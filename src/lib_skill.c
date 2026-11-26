/**
 * Copyright (c) 2026 yanrbts
 *
 * Skill registry and prompt/tool-tag binding implementation for libagent.
 */
#include "lib_skill.h"
#include "lib_error.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LIB_SKILL_MAX_ITEMS 256u
#define LIB_SKILL_DEF_SEP   "\n\n"
#define LIB_SKILL_INIT_BUF  256u

typedef struct lib_skill_entry {
    char  *name;       /* Heap-allocated unique skill identifier */
    char  *desc;       /* Heap-allocated skill summary */
    char  *prompt;     /* Heap-allocated skill instruction prompt */
    size_t prompt_len; /* Cached byte length of prompt */
    char  *tool_tag;   /* Optional heap-allocated tool tag filter */
    int    enabled;    /* Non-zero if skill is currently enabled */
} lib_skill_entry_t;

struct lib_skill_reg {
    lib_skill_entry_t *items; /* Heap-allocated skill entry array */
    size_t             count; /* Active number of registered skills */
    size_t             cap;   /* Allocated capacity of items array */
};

static void lib_skill_entry_free(lib_skill_entry_t *ent)
{
    if (ent == NULL) {
        return;
    }
    free(ent->name);
    free(ent->desc);
    free(ent->prompt);
    free(ent->tool_tag);
    memset(ent, 0, sizeof(*ent));
}

static size_t lib_skill_find_idx(const lib_skill_reg_t *reg, const char *name)
{
    size_t i = 0u;

    if (reg == NULL || name == NULL) {
        return (size_t)-1;
    }
    for (i = 0u; i < reg->count; i++) {
        if (reg->items[i].name != NULL && strcmp(reg->items[i].name, name) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static void lib_skill_fill_info(const lib_skill_entry_t *ent, lib_skill_info_t *out)
{
    out->name     = ent->name;
    out->desc     = ent->desc;
    out->prompt   = ent->prompt;
    out->tool_tag = ent->tool_tag;
    out->enabled  = ent->enabled;
}

static int lib_skill_ensure_cap(lib_skill_reg_t *reg, size_t need)
{
    lib_skill_entry_t *next_items = NULL;
    size_t             next_cap   = 0u;

    if (need <= reg->cap) {
        return 0;
    }
    if (need > LIB_SKILL_MAX_ITEMS) {
        return -1;
    }

    next_cap = (reg->cap == 0u) ? 8u : reg->cap * 2u;
    while (next_cap < need) {
        next_cap *= 2u;
    }
    if (next_cap > LIB_SKILL_MAX_ITEMS) {
        next_cap = LIB_SKILL_MAX_ITEMS;
    }

    next_items = (lib_skill_entry_t *)realloc(reg->items, next_cap * sizeof(*next_items));
    if (lib_unlikely(next_items == NULL)) {
        return -1;
    }

    memset(next_items + reg->cap, 0, (next_cap - reg->cap) * sizeof(*next_items));
    reg->items = next_items;
    reg->cap   = next_cap;
    return 0;
}

static int lib_skill_buf_append(char       **buf,
                                size_t      *cap,
                                size_t      *len,
                                const char  *data,
                                size_t       dlen)
{
    size_t need     = 0u;
    size_t next_cap = 0u;
    char  *next_buf = NULL;

    if (dlen == 0u) {
        return 0;
    }

    if (*len > SIZE_MAX - dlen - 1u) {
        return -1;
    }
    need = *len + dlen + 1u;

    if (need > *cap) {
        next_cap = (*cap == 0u) ? LIB_SKILL_INIT_BUF : *cap;
        while (next_cap < need) {
            if (next_cap > (SIZE_MAX / 2u)) {
                return -1;
            }
            next_cap *= 2u;
        }
        next_buf = (char *)realloc(*buf, next_cap);
        if (lib_unlikely(next_buf == NULL)) {
            return -1;
        }
        *buf = next_buf;
        *cap = next_cap;
    }

    memcpy(*buf + *len, data, dlen);
    *len += dlen;
    (*buf)[*len] = '\0';
    return 0;
}

const char *lib_skill_strerror(lib_skill_err_t err)
{
    switch (err) {
    case LIB_SKILL_OK:        return "ok";
    case LIB_SKILL_ERR_ARG:   return "invalid argument";
    case LIB_SKILL_ERR_MEM:   return "out of memory";
    case LIB_SKILL_ERR_EXIST: return "skill already exists";
    case LIB_SKILL_ERR_NOENT: return "skill not found";
    case LIB_SKILL_ERR_FULL:  return "skill registry full";
    default:                  return "unknown skill error";
    }
}

lib_skill_reg_t *lib_skill_new(void)
{
    lib_skill_reg_t *reg = (lib_skill_reg_t *)calloc(1u, sizeof(*reg));

    if (lib_unlikely(reg == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                         "failed to allocate skill registry");
        return NULL;
    }
    return reg;
}

void lib_skill_free(lib_skill_reg_t *reg)
{
    size_t i = 0u;

    if (lib_unlikely(reg == NULL)) {
        return;
    }
    for (i = 0u; i < reg->count; i++) {
        lib_skill_entry_free(&reg->items[i]);
    }
    free(reg->items);
    free(reg);
}

lib_skill_err_t lib_skill_add(lib_skill_reg_t        *reg,
                              const lib_skill_spec_t *spec)
{
    lib_skill_entry_t ent;
    int               oom = 0;

    if (lib_unlikely(reg == NULL || spec == NULL ||
                     spec->name == NULL || spec->name[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_INVALID_ARG,
                         "skill registry and non-empty skill name are required");
        return LIB_SKILL_ERR_ARG;
    }

    if (lib_unlikely(lib_skill_find_idx(reg, spec->name) != (size_t)-1)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_STATE,
                         "skill '%s' already registered", spec->name);
        return LIB_SKILL_ERR_EXIST;
    }

    if (lib_unlikely(reg->count >= LIB_SKILL_MAX_ITEMS)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_STATE,
                         "skill registry reached capacity (%u)",
                         (unsigned)LIB_SKILL_MAX_ITEMS);
        return LIB_SKILL_ERR_FULL;
    }

    if (lib_unlikely(lib_skill_ensure_cap(reg, reg->count + 1u) != 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                         "failed to grow skill registry capacity");
        return LIB_SKILL_ERR_MEM;
    }

    memset(&ent, 0, sizeof(ent));
    ent.name = lib_strdup_ex(spec->name, &oom);
    if (lib_unlikely(oom || ent.name == NULL)) {
        lib_skill_entry_free(&ent);
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                         "out of memory duplicating skill name");
        return LIB_SKILL_ERR_MEM;
    }

    if (spec->desc != NULL) {
        ent.desc = lib_strdup_ex(spec->desc, &oom);
        if (lib_unlikely(oom || ent.desc == NULL)) {
            lib_skill_entry_free(&ent);
            lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                             "out of memory duplicating skill desc");
            return LIB_SKILL_ERR_MEM;
        }
    }

    if (spec->prompt != NULL) {
        ent.prompt = lib_strdup_ex(spec->prompt, &oom);
        if (lib_unlikely(oom || ent.prompt == NULL)) {
            lib_skill_entry_free(&ent);
            lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                             "out of memory duplicating skill prompt");
            return LIB_SKILL_ERR_MEM;
        }
        ent.prompt_len = strlen(ent.prompt);
    }

    if (spec->tool_tag != NULL && spec->tool_tag[0] != '\0') {
        ent.tool_tag = lib_strdup_ex(spec->tool_tag, &oom);
        if (lib_unlikely(oom || ent.tool_tag == NULL)) {
            lib_skill_entry_free(&ent);
            lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                             "out of memory duplicating skill tool_tag");
            return LIB_SKILL_ERR_MEM;
        }
    }

    ent.enabled = (spec->enabled != 0) ? 1 : 0;

    reg->items[reg->count] = ent;
    reg->count++;
    return LIB_SKILL_OK;
}

lib_skill_err_t lib_skill_remove(lib_skill_reg_t *reg,
                                 const char      *name)
{
    size_t idx  = 0u;
    size_t tail = 0u;

    if (lib_unlikely(reg == NULL || name == NULL || name[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_INVALID_ARG,
                         "skill registry and non-empty skill name are required");
        return LIB_SKILL_ERR_ARG;
    }

    idx = lib_skill_find_idx(reg, name);
    if (lib_unlikely(idx == (size_t)-1)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOT_FOUND,
                         "skill '%s' not found", name);
        return LIB_SKILL_ERR_NOENT;
    }

    lib_skill_entry_free(&reg->items[idx]);
    tail = reg->count - idx - 1u;
    if (tail > 0u) {
        memmove(&reg->items[idx], &reg->items[idx + 1u], tail * sizeof(*reg->items));
    }
    reg->count--;
    memset(&reg->items[reg->count], 0, sizeof(*reg->items));
    return LIB_SKILL_OK;
}

lib_skill_err_t lib_skill_set_enabled(lib_skill_reg_t *reg,
                                      const char      *name,
                                      int              enabled)
{
    size_t idx = 0u;

    if (lib_unlikely(reg == NULL || name == NULL || name[0] == '\0')) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_INVALID_ARG,
                         "skill registry and non-empty skill name are required");
        return LIB_SKILL_ERR_ARG;
    }

    idx = lib_skill_find_idx(reg, name);
    if (lib_unlikely(idx == (size_t)-1)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOT_FOUND,
                         "skill '%s' not found", name);
        return LIB_SKILL_ERR_NOENT;
    }

    reg->items[idx].enabled = (enabled != 0) ? 1 : 0;
    return LIB_SKILL_OK;
}

lib_skill_err_t lib_skill_get(const lib_skill_reg_t *reg,
                              const char            *name,
                              lib_skill_info_t      *out_info)
{
    size_t idx = 0u;

    if (lib_unlikely(reg == NULL || name == NULL || name[0] == '\0' || out_info == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_INVALID_ARG,
                         "invalid arguments to lib_skill_get");
        return LIB_SKILL_ERR_ARG;
    }

    idx = lib_skill_find_idx(reg, name);
    if (lib_unlikely(idx == (size_t)-1)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOT_FOUND,
                         "skill '%s' not found", name);
        return LIB_SKILL_ERR_NOENT;
    }

    lib_skill_fill_info(&reg->items[idx], out_info);
    return LIB_SKILL_OK;
}

size_t lib_skill_count(const lib_skill_reg_t *reg)
{
    return (reg != NULL) ? reg->count : 0u;
}

lib_skill_err_t lib_skill_list(const lib_skill_reg_t  *reg,
                               int                     enabled_only,
                               lib_skill_info_t      **out_items,
                               size_t                 *out_count)
{
    lib_skill_info_t *arr   = NULL;
    size_t            cnt   = 0u;
    size_t            i     = 0u;
    size_t            match = 0u;

    if (lib_unlikely(reg == NULL || out_items == NULL || out_count == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_INVALID_ARG,
                         "invalid arguments to lib_skill_list");
        return LIB_SKILL_ERR_ARG;
    }

    *out_items = NULL;
    *out_count = 0u;

    for (i = 0u; i < reg->count; i++) {
        if (!enabled_only || reg->items[i].enabled != 0) {
            cnt++;
        }
    }

    if (cnt == 0u) {
        return LIB_SKILL_OK;
    }

    arr = (lib_skill_info_t *)calloc(cnt, sizeof(*arr));
    if (lib_unlikely(arr == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                         "failed to allocate skill list");
        return LIB_SKILL_ERR_MEM;
    }

    for (i = 0u; i < reg->count; i++) {
        if (!enabled_only || reg->items[i].enabled != 0) {
            lib_skill_fill_info(&reg->items[i], &arr[match]);
            match++;
        }
    }

    *out_items = arr;
    *out_count = match;
    return LIB_SKILL_OK;
}

void lib_skill_free_list(lib_skill_info_t *items)
{
    free(items);
}

lib_skill_err_t lib_skill_build_prompt(const lib_skill_reg_t  *reg,
                                       const char             *sep,
                                       char                  **out_prompt)
{
    const char *eff_sep = NULL;
    size_t      sep_len = 0u;
    char       *buf     = NULL;
    size_t      cap     = 0u;
    size_t      len     = 0u;
    size_t      emitted = 0u;
    size_t      i       = 0u;

    if (lib_unlikely(reg == NULL || out_prompt == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_INVALID_ARG,
                         "invalid arguments to lib_skill_build_prompt");
        return LIB_SKILL_ERR_ARG;
    }

    *out_prompt = NULL;
    eff_sep = (sep != NULL) ? sep : LIB_SKILL_DEF_SEP;
    sep_len = strlen(eff_sep);

    buf = (char *)malloc(LIB_SKILL_INIT_BUF);
    if (lib_unlikely(buf == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                         "failed to allocate combined skill prompt");
        return LIB_SKILL_ERR_MEM;
    }
    cap = LIB_SKILL_INIT_BUF;
    buf[0] = '\0';

    for (i = 0u; i < reg->count; i++) {
        const lib_skill_entry_t *ent = &reg->items[i];

        if (!ent->enabled || ent->prompt == NULL || ent->prompt_len == 0u) {
            continue;
        }

        if (emitted > 0u && sep_len > 0u) {
            if (lib_unlikely(lib_skill_buf_append(&buf, &cap, &len, eff_sep, sep_len) != 0)) {
                free(buf);
                lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                                 "failed to append prompt separator");
                return LIB_SKILL_ERR_MEM;
            }
        }

        if (lib_unlikely(lib_skill_buf_append(&buf, &cap, &len, ent->prompt, ent->prompt_len) != 0)) {
            free(buf);
            lib_err_emit_msg(LIB_ERR_DOM_SKILL, LIB_ERR_NOMEM,
                             "failed to append skill prompt");
            return LIB_SKILL_ERR_MEM;
        }
        emitted++;
    }

    if (len + 1u < cap) {
        char *shrunk = (char *)realloc(buf, len + 1u);
        if (shrunk != NULL) {
            buf = shrunk;
        }
    }

    *out_prompt = buf;
    return LIB_SKILL_OK;
}