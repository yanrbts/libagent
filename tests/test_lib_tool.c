/**
 * Copyright (c) 2026 yanrbts
 *
 * Unit tests for lib_tool registry and dispatcher using Unity.
 */

#include "unity.h"
#include "lib_tool.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
void        *__wrap_malloc(size_t size);
void        *__wrap_calloc(size_t nmemb, size_t size);
#endif

static int g_mock_malloc_fail_after = -1; /* -1: never fail; >=0: fail when remaining count reaches 0 */
static int g_mock_calloc_fail_after = -1; /* -1: never fail; >=0: fail when remaining count reaches 0 */
static int g_dtor_calls             = 0;
static int g_exec_calls             = 0;

void *__wrap_malloc(size_t size)
{
    if (g_mock_malloc_fail_after == 0) {
        return NULL;
    }
    if (g_mock_malloc_fail_after > 0) {
        g_mock_malloc_fail_after--;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __real_malloc(size);
#else
    return malloc(size);
#endif
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (g_mock_calloc_fail_after == 0) {
        return NULL;
    }
    if (g_mock_calloc_fail_after > 0) {
        g_mock_calloc_fail_after--;
    }
#if defined(__GNUC__) || defined(__clang__)
    return __real_calloc(nmemb, size);
#else
    return calloc(nmemb, size);
#endif
}

static void reset_mocks(void)
{
    g_mock_malloc_fail_after = -1;
    g_mock_calloc_fail_after = -1;
    g_dtor_calls             = 0;
    g_exec_calls             = 0;
}

static void dummy_dtor(void *ctx)
{
    int *flag = (int *)ctx;

    g_dtor_calls++;
    if (flag != NULL) {
        *flag += 1;
    }
}

static lib_tool_err_t echo_tool_fn(const lib_tool_call_t *call,
                                   void                  *tool_ctx,
                                   lib_tool_res_t        *out_res,
                                   char                  *errbuf,
                                   size_t                 errlen)
{
    int   oom = 0;
    char *copy = NULL;

    (void)tool_ctx;
    (void)errbuf;
    (void)errlen;

    g_exec_calls++;
    if (lib_unlikely(call == NULL || out_res == NULL)) {
        return LIB_TOOL_ERR_ARG;
    }

    copy = lib_strdup_ex(call->args_json, &oom);
    if (lib_unlikely(oom || copy == NULL)) {
        free(copy);
        return LIB_TOOL_ERR_MEM;
    }

    out_res->content  = copy;
    out_res->is_error = 0;
    return LIB_TOOL_OK;
}

static lib_tool_err_t domain_err_tool_fn(const lib_tool_call_t *call,
                                         void                  *tool_ctx,
                                         lib_tool_res_t        *out_res,
                                         char                  *errbuf,
                                         size_t                 errlen)
{
    int   oom = 0;
    char *msg = NULL;

    (void)call;
    (void)tool_ctx;
    (void)errbuf;
    (void)errlen;

    g_exec_calls++;
    msg = lib_strdup_ex("{\"error\":\"permission denied\"}", &oom);
    if (lib_unlikely(oom || msg == NULL)) {
        free(msg);
        return LIB_TOOL_ERR_MEM;
    }

    out_res->content  = msg;
    out_res->is_error = 1;
    return LIB_TOOL_OK;
}

static lib_tool_err_t fatal_err_tool_fn(const lib_tool_call_t *call,
                                        void                  *tool_ctx,
                                        lib_tool_res_t        *out_res,
                                        char                  *errbuf,
                                        size_t                 errlen)
{
    (void)call;
    (void)tool_ctx;
    (void)out_res;

    g_exec_calls++;
    if (errbuf != NULL && errlen > 0u) {
        (void)snprintf(errbuf, errlen, "backend crash");
    }
    return LIB_TOOL_ERR_EXEC;
}

void setUp(void)
{
    reset_mocks();
}

void tearDown(void)
{
    reset_mocks();
}

static void test_reg_lifecycle_and_add_del(void)
{
    lib_tool_reg_t *reg      = NULL;
    lib_tool_spec_t spec;
    lib_tool_err_t  rc       = LIB_TOOL_OK;
    int             ctx_flag = 0;
    char            errbuf[128];

    memset(&spec, 0, sizeof(spec));
    memset(errbuf, 0, sizeof(errbuf));

    reg = lib_tool_new(errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(reg);
    TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_tool_count(reg));
    TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_tool_enabled_cnt(reg));
    TEST_ASSERT_EQUAL_INT(0, lib_tool_has(reg, "echo"));

    spec.name        = "echo";
    spec.desc        = "Echo input JSON";
    spec.schema_json = "{\"type\":\"object\"}";
    spec.tag         = "builtin";
    spec.enabled     = 1;
    spec.fn          = echo_tool_fn;
    spec.tool_ctx    = &ctx_flag;
    spec.dtor        = dummy_dtor;

    rc = lib_tool_add(reg, &spec, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);
    TEST_ASSERT_EQUAL_UINT64(1u, (unsigned long long)lib_tool_count(reg));
    TEST_ASSERT_EQUAL_UINT64(1u, (unsigned long long)lib_tool_enabled_cnt(reg));
    TEST_ASSERT_EQUAL_INT(1, lib_tool_has(reg, "echo"));

    /* Duplicate registration must fail */
    rc = lib_tool_add(reg, &spec, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_EXIST, rc);
    TEST_ASSERT_TRUE(errbuf[0] != '\0');

    /* Delete invokes destructor */
    rc = lib_tool_del(reg, "echo", errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, g_dtor_calls);
    TEST_ASSERT_EQUAL_INT(1, ctx_flag);
    TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_tool_count(reg));
    TEST_ASSERT_EQUAL_INT(0, lib_tool_has(reg, "echo"));

    /* Deleting non-existent tool returns NOTFOUND */
    rc = lib_tool_del(reg, "echo", errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_NOTFOUND, rc);

    lib_tool_free(reg);
    lib_tool_free(NULL);
}

static void test_add_validation_and_schema_normalization(void)
{
    lib_tool_reg_t     *reg       = NULL;
    lib_tool_spec_t     spec;
    lib_tool_err_t      rc        = LIB_TOOL_OK;
    lib_llm_tool_def_t *defs      = NULL;
    size_t              def_cnt   = 0u;
    char                errbuf[128];

    memset(&spec, 0, sizeof(spec));
    memset(errbuf, 0, sizeof(errbuf));

    reg = lib_tool_new(NULL, 0u);
    TEST_ASSERT_NOT_NULL(reg);

    /* Invalid arguments */
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_ARG, lib_tool_add(NULL, &spec, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_ARG, lib_tool_add(reg, NULL, errbuf, sizeof(errbuf)));

    spec.name = "";
    spec.fn   = echo_tool_fn;
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_ARG, lib_tool_add(reg, &spec, errbuf, sizeof(errbuf)));

    spec.name = "bad_fn";
    spec.fn   = NULL;
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_ARG, lib_tool_add(reg, &spec, errbuf, sizeof(errbuf)));

    /* Invalid JSON schema */
    spec.name        = "bad_json_schema";
    spec.fn          = echo_tool_fn;
    spec.schema_json = "{not valid json";
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_ARG, lib_tool_add(reg, &spec, errbuf, sizeof(errbuf)));

    /* Non-object JSON schema */
    spec.schema_json = "[\"array\"]";
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_ARG, lib_tool_add(reg, &spec, errbuf, sizeof(errbuf)));

    /* Blank schema normalizes to "{}" */
    spec.name        = "blank_schema";
    spec.schema_json = "   \t\n ";
    spec.enabled     = 1;
    rc = lib_tool_add(reg, &spec, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);

    rc = lib_tool_llm_defs(reg, &defs, &def_cnt, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);
    TEST_ASSERT_EQUAL_UINT64(1u, (unsigned long long)def_cnt);
    TEST_ASSERT_NOT_NULL(defs);
    TEST_ASSERT_EQUAL_STRING("blank_schema", defs[0].name);
    TEST_ASSERT_EQUAL_STRING("{}", defs[0].schema_json);

    lib_tool_free_defs(defs, def_cnt);
    lib_tool_free_defs(NULL, 0u);
    lib_tool_free(reg);
}

static void test_enable_and_tag_scoping(void)
{
    lib_tool_reg_t     *reg      = NULL;
    lib_tool_spec_t     s1;
    lib_tool_spec_t     s2;
    lib_tool_spec_t     s3;
    lib_llm_tool_def_t *defs     = NULL;
    size_t              def_cnt  = 0u;
    size_t              affected = 0u;
    char                errbuf[128];

    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));
    memset(&s3, 0, sizeof(s3));
    memset(errbuf, 0, sizeof(errbuf));

    reg = lib_tool_new(errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(reg);

    s1.name    = "core_ping";
    s1.tag     = "builtin";
    s1.enabled = 1;
    s1.fn      = echo_tool_fn;

    s2.name    = "git_status";
    s2.tag     = "skill:git";
    s2.enabled = 0;
    s2.fn      = echo_tool_fn;

    s3.name    = "git_diff";
    s3.tag     = "skill:git";
    s3.enabled = 0;
    s3.fn      = echo_tool_fn;

    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s1, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s2, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s3, errbuf, sizeof(errbuf)));

    TEST_ASSERT_EQUAL_UINT64(3u, (unsigned long long)lib_tool_count(reg));
    TEST_ASSERT_EQUAL_UINT64(1u, (unsigned long long)lib_tool_enabled_cnt(reg));

    /* Enable all tools under skill:git */
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK,
                          lib_tool_enable_tag(reg, "skill:git", 1, &affected, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_UINT64(2u, (unsigned long long)affected);
    TEST_ASSERT_EQUAL_UINT64(3u, (unsigned long long)lib_tool_enabled_cnt(reg));

    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK,
                          lib_tool_llm_defs(reg, &defs, &def_cnt, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_UINT64(3u, (unsigned long long)def_cnt);
    lib_tool_free_defs(defs, def_cnt);
    defs = NULL;
    def_cnt = 0u;

    /* Disable single tool */
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK,
                          lib_tool_enable(reg, "git_diff", 0, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_UINT64(2u, (unsigned long long)lib_tool_enabled_cnt(reg));

    /* Disable skill:git tag */
    affected = 0u;
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK,
                          lib_tool_enable_tag(reg, "skill:git", 0, &affected, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_UINT64(2u, (unsigned long long)affected);
    TEST_ASSERT_EQUAL_UINT64(1u, (unsigned long long)lib_tool_enabled_cnt(reg));

    lib_tool_free(reg);
}

static void test_exec_and_error_handling(void)
{
    lib_tool_reg_t    *reg       = NULL;
    lib_tool_spec_t    s_ok;
    lib_tool_spec_t    s_off;
    lib_tool_spec_t    s_dom;
    lib_tool_spec_t    s_fat;
    lib_tool_res_t     res;
    lib_llm_tool_call_t llm_call;
    lib_tool_err_t     rc        = LIB_TOOL_OK;
    int                user_data = 42;
    char               errbuf[128];

    memset(&s_ok, 0, sizeof(s_ok));
    memset(&s_off, 0, sizeof(s_off));
    memset(&s_dom, 0, sizeof(s_dom));
    memset(&s_fat, 0, sizeof(s_fat));
    memset(&res, 0, sizeof(res));
    memset(&llm_call, 0, sizeof(llm_call));
    memset(errbuf, 0, sizeof(errbuf));

    reg = lib_tool_new(errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(reg);

    s_ok.name  = "echo";
    s_ok.enabled = 1;
    s_ok.fn    = echo_tool_fn;

    s_off.name = "disabled_tool";
    s_off.enabled = 0;
    s_off.fn   = echo_tool_fn;

    s_dom.name = "domain_fail";
    s_dom.enabled = 1;
    s_dom.fn   = domain_err_tool_fn;

    s_fat.name = "fatal_fail";
    s_fat.enabled = 1;
    s_fat.fn   = fatal_err_tool_fn;

    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s_ok, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s_off, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s_dom, errbuf, sizeof(errbuf)));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_add(reg, &s_fat, errbuf, sizeof(errbuf)));

    /* Normal execution via lib_llm_tool_call_t */
    llm_call.id        = "call_001";
    llm_call.name      = "echo";
    llm_call.args_json = "{\"q\":\"hello\"}";

    rc = lib_tool_exec_call(reg, &llm_call, &user_data, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, res.is_error);
    TEST_ASSERT_EQUAL_STRING("{\"q\":\"hello\"}", res.content);
    TEST_ASSERT_EQUAL_INT(1, g_exec_calls);
    lib_tool_res_reset(&res);
    TEST_ASSERT_NULL(res.content);

    /* Blank args normalize to "{}" */
    rc = lib_tool_exec(reg, "call_002", "echo", "   ", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);
    TEST_ASSERT_EQUAL_STRING("{}", res.content);
    lib_tool_res_reset(&res);

    /* Unknown tool */
    rc = lib_tool_exec(reg, "call_003", "missing", "{}", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_NOTFOUND, rc);

    /* Disabled tool */
    rc = lib_tool_exec(reg, "call_004", "disabled_tool", "{}", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_DISABLED, rc);

    /* Invalid JSON args */
    rc = lib_tool_exec(reg, "call_005", "echo", "{broken", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_JSON, rc);

    /* Non-object JSON args */
    rc = lib_tool_exec(reg, "call_006", "echo", "[1, 2, 3]", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_JSON, rc);

    /* Handled domain error: returns LIB_TOOL_OK with is_error == 1 */
    rc = lib_tool_exec(reg, "call_007", "domain_fail", "{}", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, res.is_error);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"permission denied\"}", res.content);
    lib_tool_res_reset(&res);

    /* Fatal tool callback error */
    rc = lib_tool_exec(reg, "call_008", "fatal_fail", "{}", NULL, &res, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_ERR_EXEC, rc);
    TEST_ASSERT_NULL(res.content);
    TEST_ASSERT_EQUAL_STRING("backend crash", errbuf);

    lib_tool_free(reg);
}

static void test_oom_paths(void)
{
    lib_tool_reg_t     *reg     = NULL;
    lib_tool_spec_t     spec;
    lib_tool_err_t      rc      = LIB_TOOL_OK;
    lib_llm_tool_def_t *defs    = NULL;
    size_t              def_cnt = 0u;
    int                 hit_mem = 0;
    int                 step    = 0;
    char                errbuf[128];

    memset(&spec, 0, sizeof(spec));
    memset(errbuf, 0, sizeof(errbuf));

    /* Fail initial reg allocation (calloc #1) */
    g_mock_calloc_fail_after = 0;
    reg = lib_tool_new(errbuf, sizeof(errbuf));
    TEST_ASSERT_NULL(reg);
    g_mock_calloc_fail_after = -1;

    /* Fail entries array allocation (calloc #2) */
    g_mock_calloc_fail_after = 1;
    reg = lib_tool_new(errbuf, sizeof(errbuf));
    TEST_ASSERT_NULL(reg);
    g_mock_calloc_fail_after = -1;

    reg = lib_tool_new(errbuf, sizeof(errbuf));
    TEST_ASSERT_NOT_NULL(reg);

    spec.name        = "oom_tool";
    spec.desc        = "desc";
    spec.schema_json = "{\"type\":\"object\"}";
    spec.tag         = "tag";
    spec.enabled     = 1;
    spec.fn          = echo_tool_fn;

    /* Sweep malloc failure across steps 0..16 so earlier cJSON allocs do not mask LIB_TOOL_ERR_MEM */
    for (step = 0; step <= 16; step++) {
        memset(errbuf, 0, sizeof(errbuf));
        g_mock_malloc_fail_after = step;
        rc = lib_tool_add(reg, &spec, errbuf, sizeof(errbuf));
        g_mock_malloc_fail_after = -1;

        if (rc == LIB_TOOL_ERR_MEM) {
            TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)lib_tool_count(reg));
            hit_mem = 1;
            break;
        }
        if (rc == LIB_TOOL_OK) {
            TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, lib_tool_del(reg, "oom_tool", errbuf, sizeof(errbuf)));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(hit_mem, "expected LIB_TOOL_ERR_MEM during lib_tool_add");

    /* Add valid tool, then sweep malloc failure during lib_tool_llm_defs */
    rc = lib_tool_add(reg, &spec, errbuf, sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(LIB_TOOL_OK, rc);

    hit_mem = 0;
    for (step = 0; step <= 8; step++) {
        memset(errbuf, 0, sizeof(errbuf));
        g_mock_malloc_fail_after = step;
        rc = lib_tool_llm_defs(reg, &defs, &def_cnt, errbuf, sizeof(errbuf));
        g_mock_malloc_fail_after = -1;

        if (rc == LIB_TOOL_ERR_MEM) {
            TEST_ASSERT_NULL(defs);
            TEST_ASSERT_EQUAL_UINT64(0u, (unsigned long long)def_cnt);
            hit_mem = 1;
            break;
        }
        if (rc == LIB_TOOL_OK) {
            lib_tool_free_defs(defs, def_cnt);
            defs = NULL;
            def_cnt = 0u;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(hit_mem, "expected LIB_TOOL_ERR_MEM during lib_tool_llm_defs");

    lib_tool_free(reg);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reg_lifecycle_and_add_del);
    RUN_TEST(test_add_validation_and_schema_normalization);
    RUN_TEST(test_enable_and_tag_scoping);
    RUN_TEST(test_exec_and_error_handling);
    RUN_TEST(test_oom_paths);
    return UNITY_END();
}