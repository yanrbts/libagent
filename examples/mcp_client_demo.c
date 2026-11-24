#include "lib_mcp.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_DEFAULT_TIMEOUT_MS 15000
#define DEMO_DEFAULT_MAX_LINE   (8u * 1024u * 1024u)

typedef struct demo_opts {
    const char *cmd;           /* Command or executable path for MCP stdio server */
    const char *tool_name;     /* Target tool name to invoke via tools/call */
    const char *args_json;     /* JSON object string passed as tool arguments */
    const char *resource_uri;  /* Target resource URI to read via resources/read */
    const char *prompt_name;   /* Target prompt template name to get via prompts/get */
    const char *prompt_args;   /* JSON object string passed as prompt arguments */
    int         timeout_ms;    /* Per-request timeout in milliseconds */
    size_t      max_line;      /* Max single JSON-RPC frame size in bytes */
    int         list_only;     /* Non-zero to list tools and exit without calling */
    int         list_resources;/* Non-zero to list resources via resources/list */
    int         list_prompts;  /* Non-zero to list prompts via prompts/list */
    char      **child_argv;    /* Heap-allocated NULL-terminated argv for child */
    int         child_argc;    /* Number of elements in child_argv excluding NULL */
} demo_opts_t;

static volatile sig_atomic_t g_stop_requested = 0;

/* Handles SIGINT/SIGTERM for clean shutdown without unsafe calls. */
static void demo_on_signal(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

/* Installs process signal handlers for graceful termination. */
static int demo_install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = demo_on_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

static void demo_print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] [-- <server_cmd> [server_args...]]\n"
            "\n"
            "Options:\n"
            "  --tool <name>          Tool name to call (default: read_graph)\n"
            "  --args <json>          JSON object arguments for tool (default: {})\n"
            "  --list-resources       List server resources via resources/list\n"
            "  --read-resource <uri>  Read a resource URI via resources/read\n"
            "  --list-prompts         List server prompts via prompts/list\n"
            "  --prompt <name>        Prompt template name to fetch via prompts/get\n"
            "  --prompt-args <json>   JSON object arguments for prompt (default: {})\n"
            "  --timeout-ms <ms>      Request timeout in ms (default: %d)\n"
            "  --max-line <bytes>     Max JSON-RPC frame size (default: %zu)\n"
            "  --list-only            List tools and exit without invoking a tool\n"
            "  -h, --help             Show this help message\n"
            "\n"
            "Examples:\n"
            "  %s\n"
            "  %s --tool create_entities --args '{\"entities\":[{\"name\":\"AgentCore\",\"entityType\":\"module\",\"observations\":[\"stdio epoll transport\"]}]}'\n"
            "  %s --list-resources --list-prompts -- npx -y @modelcontextprotocol/server-memory\n"
            "  %s --read-resource file:///tmp/notes.txt -- /path/to/mcp-server\n"
            "  %s --prompt summarize_diff --prompt-args '{\"branch\":\"main\"}' -- /path/to/mcp-server\n",
            prog,
            DEMO_DEFAULT_TIMEOUT_MS,
            (size_t)DEMO_DEFAULT_MAX_LINE,
            prog,
            prog,
            prog,
            prog,
            prog);
}

static int demo_parse_int(const char *s, int *out_val)
{
    if (!s || s[0] == '\0' || !out_val) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v <= 0 || v > 86400000L) {
        return -1;
    }
    *out_val = (int)v;
    return 0;
}

static int demo_parse_size(const char *s, size_t *out_val)
{
    if (!s || s[0] == '\0' || !out_val) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v == 0ULL || v > 268435456ULL) {
        return -1;
    }
    *out_val = (size_t)v;
    return 0;
}

static void demo_free_opts(demo_opts_t *opts)
{
    if (!opts) {
        return;
    }
    free(opts->child_argv);
    opts->child_argv = NULL;
    opts->child_argc = 0;
}

/* Parses CLI arguments and builds the child server argv vector. */
static int demo_parse_args(int argc, char **argv, demo_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->tool_name = "read_graph";
    opts->args_json = "{}";
    opts->prompt_args = "{}";
    opts->timeout_ms = DEMO_DEFAULT_TIMEOUT_MS;
    opts->max_line = DEMO_DEFAULT_MAX_LINE;

    int sep_idx = argc;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            sep_idx = i;
            break;
        }
    }

    for (int i = 1; i < sep_idx; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            demo_print_usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "--list-only") == 0) {
            opts->list_only = 1;
        } else if (strcmp(argv[i], "--list-resources") == 0) {
            opts->list_resources = 1;
        } else if (strcmp(argv[i], "--list-prompts") == 0) {
            opts->list_prompts = 1;
        } else if (strcmp(argv[i], "--tool") == 0) {
            if (i + 1 >= sep_idx || argv[i + 1][0] == '\0') {
                fprintf(stderr, "[ERROR] --tool requires a non-empty name\n");
                return -1;
            }
            opts->tool_name = argv[++i];
        } else if (strcmp(argv[i], "--args") == 0) {
            if (i + 1 >= sep_idx) {
                fprintf(stderr, "[ERROR] --args requires a JSON string\n");
                return -1;
            }
            opts->args_json = argv[++i];
        } else if (strcmp(argv[i], "--read-resource") == 0) {
            if (i + 1 >= sep_idx || argv[i + 1][0] == '\0') {
                fprintf(stderr, "[ERROR] --read-resource requires a non-empty URI\n");
                return -1;
            }
            opts->resource_uri = argv[++i];
        } else if (strcmp(argv[i], "--prompt") == 0) {
            if (i + 1 >= sep_idx || argv[i + 1][0] == '\0') {
                fprintf(stderr, "[ERROR] --prompt requires a non-empty template name\n");
                return -1;
            }
            opts->prompt_name = argv[++i];
        } else if (strcmp(argv[i], "--prompt-args") == 0) {
            if (i + 1 >= sep_idx) {
                fprintf(stderr, "[ERROR] --prompt-args requires a JSON string\n");
                return -1;
            }
            opts->prompt_args = argv[++i];
        } else if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (i + 1 >= sep_idx || demo_parse_int(argv[i + 1], &opts->timeout_ms) != 0) {
                fprintf(stderr, "[ERROR] invalid --timeout-ms value\n");
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], "--max-line") == 0) {
            if (i + 1 >= sep_idx || demo_parse_size(argv[i + 1], &opts->max_line) != 0) {
                fprintf(stderr, "[ERROR] invalid --max-line value\n");
                return -1;
            }
            i++;
        } else {
            fprintf(stderr, "[ERROR] unknown option: %s\n", argv[i]);
            demo_print_usage(argv[0]);
            return -1;
        }
    }

    int extra_count = argc - (sep_idx + 1);
    if (extra_count > 0) {
        opts->child_argv = calloc((size_t)extra_count + 1u, sizeof(char *));
        if (!opts->child_argv) {
            fprintf(stderr, "[ERROR] out of memory allocating child_argv\n");
            return -1;
        }
        for (int i = 0; i < extra_count; i++) {
            opts->child_argv[i] = argv[sep_idx + 1 + i];
        }
        opts->child_argv[extra_count] = NULL;
        opts->child_argc = extra_count;
        opts->cmd = opts->child_argv[0];
    } else {
        /* Default to official MCP memory stdio server via npx */
        static const char *default_argv[] = {
            "npx", "-y", "@modelcontextprotocol/server-memory", NULL
        };
        opts->child_argv = calloc(4u, sizeof(char *));
        if (!opts->child_argv) {
            fprintf(stderr, "[ERROR] out of memory allocating default child_argv\n");
            return -1;
        }
        for (int i = 0; i < 3; i++) {
            opts->child_argv[i] = (char *)default_argv[i];
        }
        opts->child_argv[3] = NULL;
        opts->child_argc = 3;
        opts->cmd = opts->child_argv[0];
    }

    return 0;
}

static void demo_print_result(const char *kind, const char *target,
                              int64_t elapsed_ms, const lib_mcp_res_t *res)
{
    printf("[INFO] %s '%s' completed in %lld ms (is_error=%d)\n",
           kind, target, (long long)elapsed_ms, res->is_error);
    if (res->text) {
        printf("[TEXT]\n%s\n", res->text);
    }
    if (res->raw_json) {
        printf("[RAW_JSON]\n%s\n", res->raw_json);
    }
}

static int demo_handle_resources(lib_mcp_ctx_t *ctx, const demo_opts_t *opts,
                                 char *errbuf, size_t errlen)
{
    if (opts->list_resources && !g_stop_requested) {
        lib_mcp_resource_t *resources = NULL;
        size_t res_count = 0;
        int64_t t_start = lib_mono_ms();
        lib_mcp_err_t rrc = lib_mcp_resources(ctx, &resources, &res_count, errbuf, errlen);
        int64_t elapsed = lib_mono_ms() - t_start;

        if (rrc == LIB_MCP_OK) {
            printf("[INFO] Discovered %zu resource(s) in %lld ms:\n",
                   res_count, (long long)elapsed);
            for (size_t i = 0; i < res_count; i++) {
                printf("  [%zu] %s (%s) - %s\n",
                       i,
                       resources[i].uri ? resources[i].uri : "(no uri)",
                       resources[i].mime_type ? resources[i].mime_type : "unknown mime",
                       resources[i].description ? resources[i].description
                                                  : (resources[i].name ? resources[i].name
                                                                       : "(no description)"));
            }
            lib_mcp_free_resources(resources, res_count);
        } else if (rrc == LIB_MCP_ERR_RPC) {
            fprintf(stderr, "[WARN] resources/list not supported or rejected (%s): %s\n",
                    lib_mcp_strerror(rrc), errbuf);
        } else {
            fprintf(stderr, "[ERROR] lib_mcp_resources failed (%s): %s\n",
                    lib_mcp_strerror(rrc), errbuf);
            return 1;
        }
    }

    if (opts->resource_uri && !g_stop_requested) {
        lib_mcp_res_t rdata;
        memset(&rdata, 0, sizeof(rdata));
        int64_t t_start = lib_mono_ms();
        lib_mcp_err_t rrc = lib_mcp_read_resource(ctx, opts->resource_uri, &rdata,
                                                  errbuf, errlen);
        int64_t elapsed = lib_mono_ms() - t_start;

        if (lib_unlikely(rrc != LIB_MCP_OK)) {
            fprintf(stderr, "[ERROR] lib_mcp_read_resource('%s') failed (%s): %s\n",
                    opts->resource_uri, lib_mcp_strerror(rrc), errbuf);
            lib_mcp_free_res(&rdata);
            return 1;
        }
        demo_print_result("Resource", opts->resource_uri, elapsed, &rdata);
        lib_mcp_free_res(&rdata);
    }

    return 0;
}

static int demo_handle_prompts(lib_mcp_ctx_t *ctx, const demo_opts_t *opts,
                               char *errbuf, size_t errlen)
{
    if (opts->list_prompts && !g_stop_requested) {
        lib_mcp_prompt_t *prompts = NULL;
        size_t prompt_count = 0;
        int64_t t_start = lib_mono_ms();
        lib_mcp_err_t prc = lib_mcp_prompts(ctx, &prompts, &prompt_count, errbuf, errlen);
        int64_t elapsed = lib_mono_ms() - t_start;

        if (prc == LIB_MCP_OK) {
            printf("[INFO] Discovered %zu prompt(s) in %lld ms:\n",
                   prompt_count, (long long)elapsed);
            for (size_t i = 0; i < prompt_count; i++) {
                printf("  [%zu] %s (%zu arg(s)) - %s\n",
                       i,
                       prompts[i].name ? prompts[i].name : "(unnamed)",
                       prompts[i].arg_count,
                       prompts[i].description ? prompts[i].description : "(no description)");
            }
            lib_mcp_free_prompts(prompts, prompt_count);
        } else if (prc == LIB_MCP_ERR_RPC) {
            fprintf(stderr, "[WARN] prompts/list not supported or rejected (%s): %s\n",
                    lib_mcp_strerror(prc), errbuf);
        } else {
            fprintf(stderr, "[ERROR] lib_mcp_prompts failed (%s): %s\n",
                    lib_mcp_strerror(prc), errbuf);
            return 1;
        }
    }

    if (opts->prompt_name && !g_stop_requested) {
        lib_mcp_res_t pres;
        memset(&pres, 0, sizeof(pres));
        int64_t t_start = lib_mono_ms();
        lib_mcp_err_t prc = lib_mcp_get_prompt(ctx, opts->prompt_name, opts->prompt_args,
                                               &pres, errbuf, errlen);
        int64_t elapsed = lib_mono_ms() - t_start;

        if (lib_unlikely(prc != LIB_MCP_OK)) {
            fprintf(stderr, "[ERROR] lib_mcp_get_prompt('%s') failed (%s): %s\n",
                    opts->prompt_name, lib_mcp_strerror(prc), errbuf);
            lib_mcp_free_res(&pres);
            return 1;
        }
        demo_print_result("Prompt", opts->prompt_name, elapsed, &pres);
        lib_mcp_free_res(&pres);
    }

    return 0;
}

int main(int argc, char **argv)
{
    demo_opts_t opts;
    int prc = demo_parse_args(argc, argv, &opts);
    if (prc > 0) {
        demo_free_opts(&opts);
        return 0;
    }
    if (prc < 0) {
        demo_free_opts(&opts);
        return 2;
    }

    if (demo_install_signals() != 0) {
        fprintf(stderr, "[ERROR] failed to install signal handlers: %s\n", strerror(errno));
        demo_free_opts(&opts);
        return 1;
    }

    lib_mcp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmd = opts.cmd;
    cfg.argv = (const char *const *)opts.child_argv;
    cfg.envp = NULL;
    cfg.timeout_ms = opts.timeout_ms;
    cfg.max_line_sz = opts.max_line;

    char errbuf[256] = {0};
    int64_t t0 = lib_mono_ms();

    printf("[INFO] Spawning MCP stdio server: %s", cfg.cmd);
    for (int i = 1; i < opts.child_argc; i++) {
        printf(" %s", opts.child_argv[i]);
    }
    printf("\n");

    lib_mcp_ctx_t *ctx = lib_mcp_open(&cfg, errbuf, sizeof(errbuf));
    if (lib_unlikely(!ctx)) {
        fprintf(stderr, "[ERROR] lib_mcp_open failed: %s\n",
                errbuf[0] ? errbuf : "unknown error");
        demo_free_opts(&opts);
        return 1;
    }
    printf("[INFO] Handshake succeeded in %lld ms\n", (long long)(lib_mono_ms() - t0));

    int exit_code = 0;

    if (!g_stop_requested) {
        lib_mcp_tool_t *tools = NULL;
        size_t tool_count = 0;
        lib_mcp_err_t trc = lib_mcp_tools(ctx, &tools, &tool_count, errbuf, sizeof(errbuf));
        if (lib_unlikely(trc != LIB_MCP_OK)) {
            fprintf(stderr, "[ERROR] lib_mcp_tools failed (%s): %s\n",
                    lib_mcp_strerror(trc), errbuf);
            exit_code = 1;
        } else {
            printf("[INFO] Discovered %zu tool(s):\n", tool_count);
            for (size_t i = 0; i < tool_count; i++) {
                printf("  [%zu] %s - %s\n",
                       i,
                       tools[i].name ? tools[i].name : "(unnamed)",
                       tools[i].description ? tools[i].description : "(no description)");
            }
            lib_mcp_free_tools(tools, tool_count);
        }
    }

    if (exit_code == 0 && !g_stop_requested) {
        exit_code = demo_handle_resources(ctx, &opts, errbuf, sizeof(errbuf));
    }

    if (exit_code == 0 && !g_stop_requested) {
        exit_code = demo_handle_prompts(ctx, &opts, errbuf, sizeof(errbuf));
    }

    if (exit_code == 0 && !opts.list_only && !g_stop_requested) {
        lib_mcp_res_t res;
        memset(&res, 0, sizeof(res));

        int64_t call_start = lib_mono_ms();
        lib_mcp_err_t crc = lib_mcp_call(ctx, opts.tool_name, opts.args_json,
                                         &res, errbuf, sizeof(errbuf));
        int64_t call_elapsed = lib_mono_ms() - call_start;

        if (lib_unlikely(crc != LIB_MCP_OK)) {
            fprintf(stderr, "[ERROR] lib_mcp_call('%s') failed (%s): %s\n",
                    opts.tool_name, lib_mcp_strerror(crc), errbuf);
            exit_code = 1;
        } else {
            demo_print_result("Tool", opts.tool_name, call_elapsed, &res);
            if (res.is_error) {
                exit_code = 1;
            }
        }
        lib_mcp_free_res(&res);
    }

    lib_mcp_close(ctx);
    demo_free_opts(&opts);
    printf("[INFO] Closed MCP session cleanly (exit_code=%d)\n", exit_code);
    return exit_code;
}