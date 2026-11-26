/**
 * Copyright (c) 2026 yanrbts
 *
 * Unity unit tests for lib_skill registry and prompt builder.
 */
#include "lib_skill.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

static lib_skill_reg_t *g_reg = NULL;

void setUp(void)
{
    g_reg = lib_skill_new();
    TEST_ASSERT_NOT_NULL(g_reg);
}

void tearDown(void)
{
    lib_skill_free(g_reg);
    g_reg = NULL;
    /* Safe to call on NULL pointer */
    lib_skill_free(NULL);
}

void test_lib_skill_strerror_known_and_unknown(void)
{
    TEST_ASSERT_NOT_NULL(lib_skill_strerror(LIB_SKILL_OK));
    TEST_ASSERT_NOT_NULL(lib_skill_strerror(LIB_SKILL_ERR_ARG));
    TEST_ASSERT_NOT_NULL(lib_skill_strerror(LIB_SKILL_ERR_MEM));
    TEST_ASSERT_NOT_NULL(lib_skill_strerror(LIB_SKILL_ERR_EXIST));
    TEST_ASSERT_NOT_NULL(lib_skill_strerror(LIB_SKILL_ERR_NOENT));
    TEST_ASSERT_NOT_NULL(lib_skill_strerror(LIB_SKILL_ERR_FULL));
    TEST_ASSERT_NOT_NULL(lib_skill_strerror((lib_skill_err_t)-999));
}

void test_lib_skill_add_get_count_and_duplicate(void)
{
    lib_skill_spec_t spec = {
        .name     = "appsec",
        .desc     = "Application security testing workflow",
        .prompt   = "Map assets before scanning.",
        .tool_tag = "sec",
        .enabled  = 1
    };
    lib_skill_info_t info;
    lib_skill_err_t  rc;

    TEST_ASSERT_EQUAL_UINT(0u, lib_skill_count(NULL));
    TEST_ASSERT_EQUAL_UINT(0u, lib_skill_count(g_reg));

    rc = lib_skill_add(g_reg, &spec);
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, rc);
    TEST_ASSERT_EQUAL_UINT(1u, lib_skill_count(g_reg));

    /* Duplicate name must be rejected without mutating state */
    rc = lib_skill_add(g_reg, &spec);
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_EXIST, rc);
    TEST_ASSERT_EQUAL_UINT(1u, lib_skill_count(g_reg));

    memset(&info, 0, sizeof(info));
    rc = lib_skill_get(g_reg, "appsec", &info);
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, rc);
    TEST_ASSERT_EQUAL_STRING("appsec", info.name);
    TEST_ASSERT_EQUAL_STRING("Application security testing workflow", info.desc);
    TEST_ASSERT_EQUAL_STRING("Map assets before scanning.", info.prompt);
    TEST_ASSERT_EQUAL_STRING("sec", info.tool_tag);
    TEST_ASSERT_NOT_EQUAL(0, info.enabled);

    /* Missing skill */
    rc = lib_skill_get(g_reg, "nonexistent", &info);
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_NOENT, rc);
}

void test_lib_skill_invalid_arguments(void)
{
    lib_skill_spec_t valid_spec = {
        .name     = "valid_skill",
        .desc     = "desc",
        .prompt   = "prompt",
        .tool_tag = NULL,
        .enabled  = 1
    };
    lib_skill_spec_t empty_name_spec = {
        .name     = "",
        .desc     = "desc",
        .prompt   = "prompt",
        .tool_tag = NULL,
        .enabled  = 1
    };
    lib_skill_spec_t null_name_spec = {
        .name     = NULL,
        .desc     = "desc",
        .prompt   = "prompt",
        .tool_tag = NULL,
        .enabled  = 1
    };
    lib_skill_info_t  info;
    lib_skill_info_t *list = NULL;
    size_t            cnt  = 99u;
    char             *prompt = NULL;

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_add(NULL, &valid_spec));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_add(g_reg, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_add(g_reg, &empty_name_spec));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_add(g_reg, &null_name_spec));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_remove(NULL, "valid_skill"));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_remove(g_reg, NULL));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_remove(g_reg, ""));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_set_enabled(NULL, "valid_skill", 1));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_set_enabled(g_reg, NULL, 1));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_set_enabled(g_reg, "", 1));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_get(NULL, "valid_skill", &info));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_get(g_reg, NULL, &info));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_get(g_reg, "valid_skill", NULL));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_list(NULL, 0, &list, &cnt));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_list(g_reg, 0, NULL, &cnt));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_list(g_reg, 0, &list, NULL));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_build_prompt(NULL, NULL, &prompt));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_ARG, lib_skill_build_prompt(g_reg, NULL, NULL));
}

void test_lib_skill_enable_toggle_and_list_filter(void)
{
    lib_skill_spec_t s1 = { "s1", "d1", "P1", NULL, 1 };
    lib_skill_spec_t s2 = { "s2", "d2", "P2", NULL, 0 };
    lib_skill_spec_t s3 = { "s3", "d3", "P3", NULL, 1 };
    lib_skill_info_t *items = NULL;
    size_t            count = 0u;
    lib_skill_info_t  info;

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s1));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s2));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s3));

    /* All items (enabled_only = 0) */
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_list(g_reg, 0, &items, &count));
    TEST_ASSERT_EQUAL_UINT(3u, count);
    TEST_ASSERT_NOT_NULL(items);
    TEST_ASSERT_EQUAL_STRING("s1", items[0].name);
    TEST_ASSERT_EQUAL_STRING("s2", items[1].name);
    TEST_ASSERT_EQUAL_STRING("s3", items[2].name);
    lib_skill_free_list(items);
    items = NULL;
    count = 0u;

    /* Enabled only (enabled_only = 1) */
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_list(g_reg, 1, &items, &count));
    TEST_ASSERT_EQUAL_UINT(2u, count);
    TEST_ASSERT_EQUAL_STRING("s1", items[0].name);
    TEST_ASSERT_EQUAL_STRING("s3", items[1].name);
    lib_skill_free_list(items);
    items = NULL;
    count = 0u;

    /* Enable s2, disable s1 */
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_set_enabled(g_reg, "s2", 1));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_set_enabled(g_reg, "s1", 0));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_NOENT, lib_skill_set_enabled(g_reg, "missing", 1));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_get(g_reg, "s2", &info));
    TEST_ASSERT_NOT_EQUAL(0, info.enabled);
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_get(g_reg, "s1", &info));
    TEST_ASSERT_EQUAL_INT(0, info.enabled);

    lib_skill_free_list(NULL);
}

void test_lib_skill_remove_preserves_order_and_frees_entry(void)
{
    lib_skill_spec_t s1 = { "first",  "d1", "P1", NULL, 1 };
    lib_skill_spec_t s2 = { "second", "d2", "P2", NULL, 1 };
    lib_skill_spec_t s3 = { "third",  "d3", "P3", NULL, 1 };
    lib_skill_info_t *items = NULL;
    size_t            count = 0u;

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_NOENT, lib_skill_remove(g_reg, "first"));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s1));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s2));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s3));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_remove(g_reg, "second"));
    TEST_ASSERT_EQUAL_UINT(2u, lib_skill_count(g_reg));

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_list(g_reg, 0, &items, &count));
    TEST_ASSERT_EQUAL_UINT(2u, count);
    TEST_ASSERT_EQUAL_STRING("first", items[0].name);
    TEST_ASSERT_EQUAL_STRING("third", items[1].name);
    lib_skill_free_list(items);
}

void test_lib_skill_build_prompt_single_pass_and_edge_cases(void)
{
    lib_skill_spec_t s_on1      = { "on1",      NULL, "Rule A",    NULL, 1 };
    lib_skill_spec_t s_off      = { "off",      NULL, "Rule Off",  NULL, 0 };
    lib_skill_spec_t s_empty_p  = { "empty_p",  NULL, "",          NULL, 1 };
    lib_skill_spec_t s_null_p   = { "null_p",   NULL, NULL,        NULL, 1 };
    lib_skill_spec_t s_on2      = { "on2",      NULL, "Rule B",    NULL, 1 };
    char            *prompt     = NULL;

    /* Empty registry yields empty NUL-terminated string */
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_build_prompt(g_reg, NULL, &prompt));
    TEST_ASSERT_NOT_NULL(prompt);
    TEST_ASSERT_EQUAL_STRING("", prompt);
    free(prompt);
    prompt = NULL;

    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s_on1));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s_off));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s_empty_p));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s_null_p));
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_add(g_reg, &s_on2));

    /* Default separator "\n\n" */
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_build_prompt(g_reg, NULL, &prompt));
    TEST_ASSERT_NOT_NULL(prompt);
    TEST_ASSERT_EQUAL_STRING("Rule A\n\nRule B", prompt);
    free(prompt);
    prompt = NULL;

    /* Custom separator " | " */
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, lib_skill_build_prompt(g_reg, " | ", &prompt));
    TEST_ASSERT_NOT_NULL(prompt);
    TEST_ASSERT_EQUAL_STRING("Rule A | Rule B", prompt);
    free(prompt);
    prompt = NULL;
}

void test_lib_skill_capacity_limit(void)
{
    size_t           i = 0u;
    char             name_buf[32];
    lib_skill_spec_t spec;
    lib_skill_err_t  rc;

    for (i = 0u; i < 256u; i++) {
        (void)snprintf(name_buf, sizeof(name_buf), "skill_%u", (unsigned)i);
        memset(&spec, 0, sizeof(spec));
        spec.name    = name_buf;
        spec.prompt  = "body";
        spec.enabled = 1;

        rc = lib_skill_add(g_reg, &spec);
        TEST_ASSERT_EQUAL_INT(LIB_SKILL_OK, rc);
    }

    TEST_ASSERT_EQUAL_UINT(256u, lib_skill_count(g_reg));

    memset(&spec, 0, sizeof(spec));
    spec.name    = "skill_overflow";
    spec.prompt  = "body";
    spec.enabled = 1;

    rc = lib_skill_add(g_reg, &spec);
    TEST_ASSERT_EQUAL_INT(LIB_SKILL_ERR_FULL, rc);
    TEST_ASSERT_EQUAL_UINT(256u, lib_skill_count(g_reg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lib_skill_strerror_known_and_unknown);
    RUN_TEST(test_lib_skill_add_get_count_and_duplicate);
    RUN_TEST(test_lib_skill_invalid_arguments);
    RUN_TEST(test_lib_skill_enable_toggle_and_list_filter);
    RUN_TEST(test_lib_skill_remove_preserves_order_and_frees_entry);
    RUN_TEST(test_lib_skill_build_prompt_single_pass_and_edge_cases);
    RUN_TEST(test_lib_skill_capacity_limit);
    return UNITY_END();
}