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
    const char *cmd;        /* Command or executable path for MCP stdio server */
    const char *tool_name;  /* Target tool name to invoke via tools/call */
    const char *args_json;  /* JSON object string passed as tool arguments */
    int         timeout_ms; /* Per-request timeout in milliseconds */
    size_t      max_line;   /* Max single JSON-RPC frame size in bytes */
    int         list_only;  /* Non-zero to list tools and exit without calling */
    char      **child_argv; /* Heap-allocated NULL-terminated argv for child */
    int         child_argc; /* Number of elements in child_argv excluding NULL */
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
            "  --tool <name>       Tool name to call (default: read_graph)\n"
            "  --args <json>       JSON object arguments for tool (default: {})\n"
            "  --timeout-ms <ms>   Request timeout in ms (default: %d)\n"
            "  --max-line <bytes>  Max JSON-RPC frame size (default: %zu)\n"
            "  --list-only         List tools and exit without invoking a tool\n"
            "  -h, --help          Show this help message\n"
            "\n"
            "Examples:\n"
            "  %s\n"
            "  %s --tool create_entities --args '{\"entities\":[{\"name\":\"AgentCore\",\"entityType\":\"module\",\"observations\":[\"stdio epoll transport\"]}]}'\n"
            "  %s --tool read_graph -- npx -y @modelcontextprotocol/server-memory\n",
            prog,
            DEMO_DEFAULT_TIMEOUT_MS,
            (size_t)DEMO_DEFAULT_MAX_LINE,
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
            printf("[INFO] Tool '%s' completed in %lld ms (is_error=%d)\n",
                   opts.tool_name, (long long)call_elapsed, res.is_error);
            if (res.text) {
                printf("[TEXT]\n%s\n", res.text);
            }
            if (res.raw_json) {
                printf("[RAW_JSON]\n%s\n", res.raw_json);
            }
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