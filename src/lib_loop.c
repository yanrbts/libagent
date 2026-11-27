/**
 * Copyright (c) 2026 yanrbts
 *
 * Multi-agent scheduler, resource coordinator, and mailbox loop implementation.
 */
#include "lib_loop.h"
#include "lib_error.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define LIB_LOOP_MAX_SLOTS     64u
#define LIB_LOOP_DEF_MAX_STEPS 64u
#define LIB_LOOP_DEF_MAX_MSGS  128u

typedef struct lib_loop_qnode {
    char                  *from;               /* Heap-allocated sender identifier */
    char                  *to;                 /* Heap-allocated recipient name or "*" */
    lib_loop_msg_kind_t    kind;               /* Message classification tag */
    void                  *payload;            /* Owned or borrowed message payload buffer */
    size_t                 len;                /* Payload size in bytes */
    void                 (*free_fn)(void *payload); /* Optional custom payload destructor */
    struct lib_loop_qnode *next;               /* Next queued message node in FIFO order */
} lib_loop_qnode_t;

typedef struct lib_loop_slot {
    uint32_t          slot_id;                 /* Zero-based slot index assigned on attach */
    int               priority;                /* Scheduling weight; higher runs first */
    lib_agent_t      *agent;                   /* Managed agent instance (borrowed pointer) */
    lib_loop_qnode_t *inbox_head;              /* Oldest queued message in agent inbox */
    lib_loop_qnode_t *inbox_tail;              /* Newest queued message in agent inbox */
    size_t            inbox_len;               /* Current number of messages in inbox */
} lib_loop_slot_t;

struct lib_loop {
    char                  *name;               /* Heap-allocated loop instance name */
    lib_loop_slot_t        slots[LIB_LOOP_MAX_SLOTS]; /* Fixed-capacity table of attached agents */
    size_t                 slot_count;         /* Number of currently attached agent slots */
    size_t                 rr_cursor;          /* Round-robin tie-breaker cursor across slots */
    size_t                 max_total_steps;    /* Global step limit before budget abort */
    size_t                 max_inbox_msgs;     /* Per-agent inbox queue depth ceiling */
    size_t                 total_steps;        /* Cumulative agent steps in current run */
    int                    stop_on_err;        /* Non-zero aborts loop on agent step failure */
    int                    cancel_req;         /* Non-zero cancellation flag requested externally */
    lib_loop_state_t       state;              /* Current scheduler lifecycle state */
    const lib_skill_reg_t *skills;             /* Shared skill registry (borrowed pointer) */
    void                  *shared_ctx;         /* Shared context passed to agents/hooks */
    void                  *user_ctx;           /* Caller context forwarded to loop callbacks */
    lib_loop_ops_t         ops;                /* Registered loop observer/control hooks */
};

static void lib_loop_payload_free(void *payload, size_t len, void (*free_fn)(void *))
{
    (void)len;
    if (payload == NULL) {
        return;
    }
    if (free_fn != NULL) {
        free_fn(payload);
    } else {
        free(payload);
    }
}

static void lib_loop_qnode_free(lib_loop_qnode_t *node)
{
    if (node == NULL) {
        return;
    }
    free(node->from);
    free(node->to);
    lib_loop_payload_free(node->payload, node->len, node->free_fn);
    free(node);
}

static void lib_loop_slot_clear_inbox(lib_loop_slot_t *slot)
{
    lib_loop_qnode_t *cur  = NULL;
    lib_loop_qnode_t *next = NULL;

    if (slot == NULL) {
        return;
    }
    cur = slot->inbox_head;
    while (cur != NULL) {
        next = cur->next;
        lib_loop_qnode_free(cur);
        cur = next;
    }
    slot->inbox_head = NULL;
    slot->inbox_tail = NULL;
    slot->inbox_len  = 0u;
}

static int lib_loop_find_slot_by_name(const lib_loop_t *loop, const char *name, size_t *out_idx)
{
    size_t i = 0u;

    if (loop == NULL || name == NULL || name[0] == '\0' || out_idx == NULL) {
        return -1;
    }
    for (i = 0u; i < loop->slot_count; i++) {
        const char *aname = lib_agent_name(loop->slots[i].agent);
        if (aname != NULL && strcmp(aname, name) == 0) {
            *out_idx = i;
            return 0;
        }
    }
    return -1;
}

static void lib_loop_broadcast_cancel(lib_loop_t *loop)
{
    size_t i = 0u;

    if (loop == NULL) {
        return;
    }
    for (i = 0u; i < loop->slot_count; i++) {
        lib_agent_cancel(loop->slots[i].agent);
    }
}

static int lib_loop_enqueue_one(lib_loop_t          *loop,
                                lib_loop_slot_t     *dst,
                                const char          *from,
                                const char          *to,
                                lib_loop_msg_kind_t  kind,
                                const void          *payload,
                                size_t               len,
                                int                  copy_payload,
                                void               (*free_fn)(void *payload),
                                int                  owns_payload)
{
    lib_loop_qnode_t *node = NULL;
    int               oom  = 0;

    if (dst->inbox_len >= loop->max_inbox_msgs) {
        if (owns_payload && !copy_payload) {
            lib_loop_payload_free((void *)payload, len, free_fn);
        }
        return -1;
    }

    node = (lib_loop_qnode_t *)calloc(1u, sizeof(*node));
    if (lib_unlikely(node == NULL)) {
        if (owns_payload && !copy_payload) {
            lib_loop_payload_free((void *)payload, len, free_fn);
        }
        return -1;
    }

    node->from = lib_strdup_ex((from != NULL && from[0] != '\0') ? from : "system", &oom);
    if (lib_unlikely(oom || node->from == NULL)) {
        if (owns_payload && !copy_payload) {
            lib_loop_payload_free((void *)payload, len, free_fn);
        }
        node->payload = NULL;
        lib_loop_qnode_free(node);
        return -1;
    }

    node->to = lib_strdup_ex((to != NULL && to[0] != '\0') ? to : "*", &oom);
    if (lib_unlikely(oom || node->to == NULL)) {
        if (owns_payload && !copy_payload) {
            lib_loop_payload_free((void *)payload, len, free_fn);
        }
        node->payload = NULL;
        lib_loop_qnode_free(node);
        return -1;
    }

    node->kind = kind;
    node->len  = len;

    if (payload != NULL && len > 0u) {
        if (copy_payload) {
            void *dup_buf = malloc(len);
            if (lib_unlikely(dup_buf == NULL)) {
                lib_loop_qnode_free(node);
                return -1;
            }
            memcpy(dup_buf, payload, len);
            node->payload = dup_buf;
            node->free_fn = free;
        } else {
            node->payload = (void *)payload;
            node->free_fn = owns_payload ? free_fn : NULL;
        }
    }

    if (dst->inbox_tail != NULL) {
        dst->inbox_tail->next = node;
        dst->inbox_tail = node;
    } else {
        dst->inbox_head = node;
        dst->inbox_tail = node;
    }
    dst->inbox_len++;
    return 0;
}

/**
 * Return non-zero if candidate slot @p s should replace the current best slot.
 *
 * Selection rules (evaluated in order):
 * 1. First runnable slot wins (!found).
 * 2. Higher priority always preempts lower priority (s->priority > best_prio).
 * 3. Within equal priority, a slot with pending inbox messages beats an idle-queue
 *    slot (has_msg && !best_has_msg). When both have messages or neither does,
 *    keep the earlier one in round-robin scan order from rr_cursor.
 */
static int lib_loop_slot_is_better(int found,
                                   int cand_prio,
                                   int cand_has_msg,
                                   int best_prio,
                                   int best_has_msg)
{
    if (!found) {
        return 1;
    }
    if (cand_prio != best_prio) {
        return (cand_prio > best_prio) ? 1 : 0;
    }
    /* Equal priority: prefer a slot that currently has pending messages to consume */
    if (cand_has_msg != best_has_msg) {
        return (cand_has_msg && !best_has_msg) ? 1 : 0;
    }
    /* Equal priority and equal message readiness: preserve rr_cursor scan order */
    return 0;
}

static int lib_loop_pick_next_slot(lib_loop_t *loop, size_t *out_idx)
{
    size_t i            = 0u;
    int    found        = 0;
    int    best_prio    = 0;
    int    best_has_msg = 0;
    size_t best_idx     = 0u;

    if (loop == NULL || loop->slot_count == 0u || out_idx == NULL) {
        return 0;
    }

    for (i = 0u; i < loop->slot_count; i++) {
        size_t                 idx     = (loop->rr_cursor + i) % loop->slot_count;
        const lib_loop_slot_t *s       = &loop->slots[idx];
        lib_agent_state_t      st      = lib_agent_state(s->agent);
        int                    has_msg = (s->inbox_len > 0u) ? 1 : 0;

        /* Skip agents already in terminal states; they can no longer advance */
        if (st == LIB_AGENT_ST_DONE || st == LIB_AGENT_ST_ABORTED) {
            continue;
        }

        if (lib_loop_slot_is_better(found, s->priority, has_msg,
                                    best_prio, best_has_msg)) {
            found        = 1;
            best_prio    = s->priority;
            best_has_msg = has_msg;
            best_idx     = idx;
        }
    }

    if (!found) {
        return 0;
    }

    *out_idx = best_idx;
    /* Advance round-robin cursor past the chosen slot to avoid starving peers */
    loop->rr_cursor = (best_idx + 1u) % loop->slot_count;
    return 1;
}

const char *lib_loop_strerror(lib_loop_err_t err)
{
    switch (err) {
    case LIB_LOOP_OK:         return "ok";
    case LIB_LOOP_ERR_ARG:    return "invalid argument";
    case LIB_LOOP_ERR_MEM:    return "out of memory";
    case LIB_LOOP_ERR_STATE:  return "invalid loop state";
    case LIB_LOOP_ERR_EXIST:  return "agent already attached";
    case LIB_LOOP_ERR_NOENT:  return "agent or message not found";
    case LIB_LOOP_ERR_FULL:   return "loop capacity full";
    case LIB_LOOP_ERR_BUDGET: return "loop step budget exceeded";
    case LIB_LOOP_ERR_AGENT:  return "managed agent failed";
    case LIB_LOOP_ERR_ABORT:  return "loop aborted";
    default:                  return "unknown loop error";
    }
}

const char *lib_loop_state_str(lib_loop_state_t state)
{
    switch (state) {
    case LIB_LOOP_ST_IDLE:    return "idle";
    case LIB_LOOP_ST_RUNNING: return "running";
    case LIB_LOOP_ST_DONE:    return "done";
    case LIB_LOOP_ST_ABORTED: return "aborted";
    default:                  return "unknown";
    }
}

lib_loop_t *lib_loop_new(const lib_loop_cfg_t *cfg,
                         const lib_loop_ops_t *ops)
{
    lib_loop_t *loop = NULL;
    int         oom  = 0;

    loop = (lib_loop_t *)calloc(1u, sizeof(*loop));
    if (lib_unlikely(loop == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOMEM,
                         "failed to allocate lib_loop instance");
        return NULL;
    }

    if (cfg != NULL && cfg->name != NULL && cfg->name[0] != '\0') {
        loop->name = lib_strdup_ex(cfg->name, &oom);
    } else {
        loop->name = lib_strdup_ex("loop", &oom);
    }
    if (lib_unlikely(oom || loop->name == NULL)) {
        lib_loop_free(loop);
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOMEM,
                         "out of memory allocating loop name");
        return NULL;
    }

    loop->max_total_steps = (cfg != NULL && cfg->max_total_steps > 0u)
                                ? cfg->max_total_steps
                                : LIB_LOOP_DEF_MAX_STEPS;
    loop->max_inbox_msgs  = (cfg != NULL && cfg->max_inbox_msgs > 0u)
                                ? cfg->max_inbox_msgs
                                : LIB_LOOP_DEF_MAX_MSGS;
    loop->stop_on_err     = (cfg != NULL && cfg->stop_on_err != 0) ? 1 : 0;
    loop->skills          = (cfg != NULL) ? cfg->skills : NULL;
    loop->shared_ctx      = (cfg != NULL) ? cfg->shared_ctx : NULL;
    loop->user_ctx        = (cfg != NULL) ? cfg->user_ctx : NULL;
    loop->state           = LIB_LOOP_ST_IDLE;

    if (ops != NULL) {
        loop->ops = *ops;
    }

    return loop;
}

void lib_loop_free(lib_loop_t *loop)
{
    size_t i = 0u;

    if (lib_unlikely(loop == NULL)) {
        return;
    }
    for (i = 0u; i < loop->slot_count; i++) {
        lib_loop_slot_clear_inbox(&loop->slots[i]);
    }
    free(loop->name);
    free(loop);
}

lib_loop_err_t lib_loop_reset(lib_loop_t *loop)
{
    size_t          i   = 0u;
    lib_agent_err_t arc = LIB_AGENT_OK;

    if (lib_unlikely(loop == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "loop instance is NULL");
        return LIB_LOOP_ERR_ARG;
    }

    for (i = 0u; i < loop->slot_count; i++) {
        lib_loop_slot_clear_inbox(&loop->slots[i]);
        arc = lib_agent_reset(loop->slots[i].agent);
        if (lib_unlikely(arc != LIB_AGENT_OK)) {
            lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                             "failed to reset agent in slot %zu", i);
            return LIB_LOOP_ERR_STATE;
        }
    }

    loop->total_steps = 0u;
    loop->rr_cursor   = 0u;
    loop->cancel_req  = 0;
    loop->state       = LIB_LOOP_ST_IDLE;
    return LIB_LOOP_OK;
}

void lib_loop_cancel(lib_loop_t *loop)
{
    if (lib_unlikely(loop == NULL)) {
        return;
    }
    loop->cancel_req = 1;
    lib_loop_broadcast_cancel(loop);
}

lib_loop_err_t lib_loop_set_skills(lib_loop_t            *loop,
                                   const lib_skill_reg_t *skills)
{
    if (lib_unlikely(loop == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "loop instance is NULL");
        return LIB_LOOP_ERR_ARG;
    }
    loop->skills = skills;
    return LIB_LOOP_OK;
}

lib_loop_err_t lib_loop_set_shared(lib_loop_t *loop, void *shared_ctx)
{
    if (lib_unlikely(loop == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "loop instance is NULL");
        return LIB_LOOP_ERR_ARG;
    }
    loop->shared_ctx = shared_ctx;
    return LIB_LOOP_OK;
}

const lib_skill_reg_t *lib_loop_skills(const lib_loop_t *loop)
{
    return (loop != NULL) ? loop->skills : NULL;
}

void *lib_loop_shared(const lib_loop_t *loop)
{
    return (loop != NULL) ? loop->shared_ctx : NULL;
}

lib_loop_err_t lib_loop_attach(lib_loop_t *loop,
                               lib_agent_t *agent,
                               int          priority,
                               uint32_t    *out_slot_id)
{
    const char *aname = NULL;
    size_t      dup_idx = 0u;
    uint32_t    sid = 0u;

    if (lib_unlikely(loop == NULL || agent == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "loop and agent are required");
        return LIB_LOOP_ERR_ARG;
    }

    if (lib_unlikely(loop->slot_count >= LIB_LOOP_MAX_SLOTS)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                         "loop reached max slot capacity (%u)",
                         (unsigned)LIB_LOOP_MAX_SLOTS);
        return LIB_LOOP_ERR_FULL;
    }

    aname = lib_agent_name(agent);
    if (aname != NULL && lib_loop_find_slot_by_name(loop, aname, &dup_idx) == 0) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                         "duplicate agent '%s' in loop", aname);
        return LIB_LOOP_ERR_EXIST;
    }

    sid = (uint32_t)loop->slot_count;
    memset(&loop->slots[sid], 0, sizeof(loop->slots[sid]));
    loop->slots[sid].slot_id  = sid;
    loop->slots[sid].priority = priority;
    loop->slots[sid].agent    = agent;
    loop->slot_count++;

    if (out_slot_id != NULL) {
        *out_slot_id = sid;
    }
    return LIB_LOOP_OK;
}

lib_loop_err_t lib_loop_send(lib_loop_t          *loop,
                             const char          *from,
                             const char          *to,
                             lib_loop_msg_kind_t  kind,
                             const void          *payload,
                             size_t               len,
                             int                  copy_payload,
                             void               (*free_fn)(void *payload))
{
    size_t target_idx = 0u;
    size_t i          = 0u;

    if (lib_unlikely(loop == NULL || kind == 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "invalid arguments to lib_loop_send");
        return LIB_LOOP_ERR_ARG;
    }

    /* Broadcast message delivery when @p to is NULL, empty, or wildcard "*".
     *
     * Key design points:
     * 1. Guard empty loop early: if no agent slots exist and caller transferred
     *    ownership (!copy_payload), release @p payload immediately to prevent leaks.
     * 2. Single-owner lifetime rule: when !copy_payload, only the final slot takes
     *    payload destruction ownership (owns = 1) so shared payload is freed once.
     * 3. Partial-enqueue trade-off: if an earlier slot succeeds and a later slot
     *    fails (e.g., inbox full/OOM), earlier nodes remain in their inboxes until
     *    lib_loop_reset() or lib_loop_free() drains them. */
    if (to == NULL || to[0] == '\0' || strcmp(to, "*") == 0) {
        if (lib_unlikely(loop->slot_count == 0u)) {
            if (!copy_payload) {
                lib_loop_payload_free((void *)payload, len, free_fn);
            }
            lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOT_FOUND,
                             "no agents attached for broadcast");
            return LIB_LOOP_ERR_NOENT;
        }

        for (i = 0u; i < loop->slot_count; i++) {
            /* Only the last recipient slot takes payload ownership 
             * (!copy_payload) so payload is freed once */
            int owns = (!copy_payload && i == (loop->slot_count - 1u)) ? 1 : 0;

            if (lib_unlikely(lib_loop_enqueue_one(loop, &loop->slots[i], from, "*",
                                                  kind, payload, len, copy_payload,
                                                  free_fn, owns) != 0)) {
                lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOMEM,
                                 "failed to enqueue broadcast message to slot %zu", i);
                return LIB_LOOP_ERR_FULL;
            }
        }
        return LIB_LOOP_OK;
    }

    /* Unicast delivery: verify target agent exists before enqueueing */
    if (lib_unlikely(lib_loop_find_slot_by_name(loop, to, &target_idx) != 0)) {
        if (!copy_payload) {
            lib_loop_payload_free((void *)payload, len, free_fn);
        }
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOT_FOUND,
                         "target agent '%s' not attached", to);
        return LIB_LOOP_ERR_NOENT;
    }

    if (lib_unlikely(lib_loop_enqueue_one(loop, &loop->slots[target_idx], from, to,
                                          kind, payload, len, copy_payload,
                                          free_fn, 1) != 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOMEM,
                         "failed to enqueue message to agent '%s'", to);
        return LIB_LOOP_ERR_FULL;
    }

    return LIB_LOOP_OK;
}

lib_loop_err_t lib_loop_pop(lib_loop_t     *loop,
                            const char     *agent_name,
                            lib_loop_msg_t *out_msg)
{
    size_t            idx  = 0u;
    lib_loop_slot_t  *slot = NULL;
    lib_loop_qnode_t *node = NULL;

    if (lib_unlikely(loop == NULL || agent_name == NULL ||
                     agent_name[0] == '\0' || out_msg == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "invalid arguments to lib_loop_pop");
        return LIB_LOOP_ERR_ARG;
    }

    if (lib_unlikely(lib_loop_find_slot_by_name(loop, agent_name, &idx) != 0)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_NOT_FOUND,
                         "agent '%s' not found in loop", agent_name);
        return LIB_LOOP_ERR_NOENT;
    }

    slot = &loop->slots[idx];
    if (slot->inbox_head == NULL) {
        return LIB_LOOP_ERR_NOENT;
    }

    node = slot->inbox_head;
    slot->inbox_head = node->next;
    if (slot->inbox_head == NULL) {
        slot->inbox_tail = NULL;
    }
    slot->inbox_len--;

    out_msg->from    = node->from;
    out_msg->to      = node->to;
    out_msg->kind    = node->kind;
    out_msg->payload = node->payload;
    out_msg->len     = node->len;
    out_msg->free_fn = node->free_fn;
    free(node);
    return LIB_LOOP_OK;
}

void lib_loop_msg_cleanup(lib_loop_msg_t *msg)
{
    if (lib_unlikely(msg == NULL)) {
        return;
    }
    free(msg->from);
    free(msg->to);
    lib_loop_payload_free(msg->payload, msg->len, msg->free_fn);
    memset(msg, 0, sizeof(*msg));
}

lib_loop_err_t lib_loop_tick(lib_loop_t           *loop,
                           lib_loop_tick_info_t *out_info)
{
    size_t                sel_idx = 0u;
    lib_loop_slot_t      *slot    = NULL;
    lib_agent_step_info_t sinfo;
    lib_agent_err_t       arc     = LIB_AGENT_OK;
    lib_loop_tick_info_t  tinfo;

    if (lib_unlikely(loop == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "loop instance is NULL");
        return LIB_LOOP_ERR_ARG;
    }

    if (lib_unlikely(loop->state == LIB_LOOP_ST_DONE ||
                     loop->state == LIB_LOOP_ST_ABORTED)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                         "loop is in terminal state; call lib_loop_reset first");
        return LIB_LOOP_ERR_STATE;
    }

    memset(&sinfo, 0, sizeof(sinfo));
    memset(&tinfo, 0, sizeof(tinfo));

    if (lib_unlikely(loop->cancel_req)) {
        lib_loop_broadcast_cancel(loop);
        loop->state = LIB_LOOP_ST_ABORTED;
        tinfo.state = loop->state;
        tinfo.total_steps = loop->total_steps;
        if (out_info != NULL) {
            *out_info = tinfo;
        }
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE, "loop cancelled");
        return LIB_LOOP_ERR_ABORT;
    }

    if (lib_unlikely(loop->total_steps >= loop->max_total_steps)) {
        lib_loop_broadcast_cancel(loop);
        loop->state = LIB_LOOP_ST_ABORTED;
        tinfo.state = loop->state;
        tinfo.total_steps = loop->total_steps;
        if (out_info != NULL) {
            *out_info = tinfo;
        }
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                         "loop exceeded max_total_steps (%zu)",
                         loop->max_total_steps);
        return LIB_LOOP_ERR_BUDGET;
    }

    loop->state = LIB_LOOP_ST_RUNNING;

    if (!lib_loop_pick_next_slot(loop, &sel_idx)) {
        loop->state = LIB_LOOP_ST_DONE;
        tinfo.state = loop->state;
        tinfo.total_steps = loop->total_steps;
        if (out_info != NULL) {
            *out_info = tinfo;
        }
        return LIB_LOOP_OK;
    }

    slot = &loop->slots[sel_idx];
    arc = lib_agent_step(slot->agent, &sinfo);
    loop->total_steps++;

    tinfo.slot_id     = slot->slot_id;
    tinfo.agent_name  = lib_agent_name(slot->agent);
    tinfo.step_info   = sinfo;
    tinfo.total_steps = loop->total_steps;

    if (lib_unlikely(arc != LIB_AGENT_OK)) {
        if (loop->stop_on_err) {
            lib_loop_broadcast_cancel(loop);
            loop->state = LIB_LOOP_ST_ABORTED;
            tinfo.state = loop->state;
            if (out_info != NULL) {
                *out_info = tinfo;
            }
            if (lib_err_last()->code == LIB_ERR_OK) {
                lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_PROTOCOL,
                                 "agent '%s' failed with %s",
                                 tinfo.agent_name ? tinfo.agent_name : "?",
                                 lib_agent_strerror(arc));
            }
            return LIB_LOOP_ERR_AGENT;
        }
    }

    if (loop->ops.on_step != NULL) {
        tinfo.state = loop->state;
        if (lib_unlikely(loop->ops.on_step(loop->user_ctx, loop->shared_ctx,
                                           loop->skills, &tinfo) != 0)) {
            lib_loop_broadcast_cancel(loop);
            loop->state = LIB_LOOP_ST_ABORTED;
            tinfo.state = loop->state;
            if (out_info != NULL) {
                *out_info = tinfo;
            }
            lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                             "aborted by on_step hook");
            return LIB_LOOP_ERR_ABORT;
        }
    }

    /* If every slot reached a terminal state, transition loop to DONE */
    if (!lib_loop_pick_next_slot(loop, &sel_idx)) {
        loop->state = LIB_LOOP_ST_DONE;
    } else {
        loop->state = LIB_LOOP_ST_IDLE;
    }
    tinfo.state = loop->state;

    if (out_info != NULL) {
        *out_info = tinfo;
    }
    return LIB_LOOP_OK;
}

lib_loop_err_t lib_loop_run(lib_loop_t           *loop,
                          lib_loop_tick_info_t *out_info)
{
    lib_loop_err_t       rc = LIB_LOOP_OK;
    lib_loop_tick_info_t last_info;

    if (lib_unlikely(loop == NULL)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_INVALID_ARG,
                         "loop instance is NULL");
        return LIB_LOOP_ERR_ARG;
    }

    if (lib_unlikely(loop->state == LIB_LOOP_ST_DONE ||
                     loop->state == LIB_LOOP_ST_ABORTED)) {
        lib_err_emit_msg(LIB_ERR_DOM_LOOP, LIB_ERR_STATE,
                         "loop is in terminal state; call lib_loop_reset first");
        return LIB_LOOP_ERR_STATE;
    }

    memset(&last_info, 0, sizeof(last_info));

    while (loop->state == LIB_LOOP_ST_IDLE || loop->state == LIB_LOOP_ST_RUNNING) {
        rc = lib_loop_tick(loop, &last_info);
        if (lib_unlikely(rc != LIB_LOOP_OK)) {
            if (out_info != NULL) {
                *out_info = last_info;
            }
            return rc;
        }
        if (loop->state == LIB_LOOP_ST_DONE) {
            break;
        }
    }

    if (out_info != NULL) {
        *out_info = last_info;
    }
    return LIB_LOOP_OK;
}

lib_loop_state_t lib_loop_state(const lib_loop_t *loop)
{
    return (loop != NULL) ? loop->state : LIB_LOOP_ST_ABORTED;
}

size_t lib_loop_total_steps(const lib_loop_t *loop)
{
    return (loop != NULL) ? loop->total_steps : 0u;
}