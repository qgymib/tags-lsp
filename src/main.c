#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "method/__init__.h"
#include "utils/lsp_error.h"
#include "utils/lsp_msg.h"
#include "utils/io.h"
#include "utils/log.h"
#include "utils/lsp_work.h"
#include "utils/alloc.h"
#include "runtime.h"

#if defined(WIN32)
#   define sscanf(str, fmt, ...)    sscanf_s(str, fmt, ##__VA_ARGS__)
#   define strdup(s)                _strdup(s)
#endif

static const char* s_help =
TAGS_LSP_PROG_NAME " - Language server protocol wrapper for gtags.\n"
"Usage: " TAGS_LSP_PROG_NAME " [OPTIONS]\n"
"\n"
"OPTIONS:\n"
"  --stdio\n"
"    Uses stdio as the communication channel. If no option specific, use this\n"
"    as default option.\n"
"\n"
"  --pipe=[FILE]\n"
"    Use pipes (Windows) or socket files (Linux, Mac) as the communication\n"
"    channel.\n"
"\n"
"  --port=[NUMBER]\n"
"    Uses a socket as the communication channel.\n"
"\n"
"  --logdir=[PATH]\n"
"    The directory to store log. The logfile will be like tags-lsp.pid.log\n"
"\n"
"  --logfile=[PATH]\n"
"    The log file path. If both `--logdir` and `--logfile` exist, `--logfile`\n"
"    win.\n"
"\n"
"  -h, --help\n"
"    Show this help and exit.\n";

static const char* s_welcome =
TAGS_LSP_PROG_NAME " is a language server that provides IDE-like features to editors.\n"
"\n"
"Homepage: https://github.com/0xfdfd/tags-lsp\n"
"\n"
TAGS_LSP_PROG_NAME " accepts flags on the commandline. For more information, checkout\n"
"command line option `--help`.\n";

static void _cleanup_loop(void)
{
    if (g_tags.loop == NULL)
    {
        return;
    }

    /* Ensure all handle closed. */
    if (uv_run(g_tags.loop, UV_RUN_DEFAULT) != 0)
    {
        abort();
    }

    /* Close loop. */
    if (uv_loop_close(g_tags.loop) != 0)
    {
        abort();
    }

    lsp_free(g_tags.loop);
    g_tags.loop = NULL;
}

static void _at_exit_stage_1(void)
{
    tag_lsp_msg_exit();
    tag_lsp_io_exit();
    lsp_work_exit();

    lsp_log_exit();

    uv_close((uv_handle_t*)g_tags.sigint, NULL);
    uv_close((uv_handle_t*)g_tags.exit_notifier, NULL);
}

static void _at_exit_stage_2(void)
{
    if (g_tags.parser != NULL)
    {
        lsp_parser_destroy(g_tags.parser);
        g_tags.parser = NULL;
    }

    if (g_tags.config.logdir != NULL)
    {
        lsp_free(g_tags.config.logdir);
        g_tags.config.logdir = NULL;
    }

    if (g_tags.config.logfile != NULL)
    {
        lsp_free(g_tags.config.logfile);
        g_tags.config.logfile = NULL;
    }

    if (g_tags.sigint != NULL)
    {
        lsp_free(g_tags.sigint);
        g_tags.sigint = NULL;
    }

    if (g_tags.exit_notifier != NULL)
    {
        lsp_free(g_tags.exit_notifier);
        g_tags.exit_notifier = NULL;
    }

    tag_lsp_cleanup_workspace_folders();
    tag_lsp_cleanup_client_capabilities();
}

static void _wait_for_pending_task(void)
{
    size_t queue_size;
    while ((queue_size = lsp_work_queue_size()) != 0)
    {
        uv_run(g_tags.loop, UV_RUN_ONCE);
    }
}

static void _at_exit(void)
{
    /* Always set shutdown flag. */
    g_tags.flags.shutdown = 1;

    LSP_LOG(LSP_MSG_DEBUG, "cleanup...");

    lsp_msg_cancel_all_pending_request();
    _wait_for_pending_task();

    lsp_method_cleanup();

    _at_exit_stage_1();
    _cleanup_loop();
    _at_exit_stage_2();

    uv_library_shutdown();
}

static int _handle_request(cJSON* msg, void* arg)
{
    (void)arg;

    lsp_handle_msg(msg);

    return 0;
}

static void _on_io_in(const char* data, ssize_t size)
{
    if (size < 0)
    {
        uv_stop(g_tags.loop);
        return;
    }

    lsp_parser_execute(g_tags.parser, data, size);
}

static void _parser_command_line_options(tag_lsp_io_cfg_t* io_cfg, char* argv[])
{
    size_t i;
    int ret = 0;
    const char* opt = NULL;

    io_cfg->type = TAG_LSP_IO_STDIO;
    io_cfg->cb = _on_io_in;

    for (i = 0; argv[i] != NULL; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            fprintf(stderr, "%s", s_help);
            exit(EXIT_SUCCESS);
        }

        if (strcmp(argv[i], "--stdio") == 0)
        {
            io_cfg->type = TAG_LSP_IO_STDIO;
            continue;
        }

        opt = "--pipe=";
        if (strncmp(argv[i], opt, strlen(opt)) == 0)
        {
            opt = argv[i] + strlen(opt);
            io_cfg->type = TAG_LSP_IO_PIPE;
            io_cfg->data.file = opt;
            continue;
        }

        opt = "--port=";
        if (strncmp(argv[i], opt, strlen(opt)) == 0)
        {
            opt = argv[i] + strlen(opt);
            if (sscanf(opt, "%d", &ret) != 1)
            {
                fprintf(stderr, "invalid value for `--port`: %s.\n", opt);
                exit(EXIT_FAILURE);
            }
            io_cfg->type = TAG_LSP_IO_PORT;
            io_cfg->data.port = ret;
            continue;
        }

        opt = "--logdir=";
        if (strncmp(argv[i], opt, strlen(opt)) == 0)
        {
            opt = argv[i] + strlen(opt);
            if (g_tags.config.logdir != NULL)
            {
                free(g_tags.config.logdir);
            }
            g_tags.config.logdir = lsp_strdup(opt);
            continue;
        }

        opt = "--logfile=";
        if (strncmp(argv[i], opt, strlen(opt)) == 0)
        {
            opt = argv[i] + strlen(opt);
            if (g_tags.config.logfile != NULL)
            {
                lsp_free(g_tags.config.logfile);
            }
            g_tags.config.logfile = lsp_strdup(opt);
            continue;
        }
    }
}

static void _on_signal(uv_signal_t* handle, int signum)
{
    (void)handle; (void)signum;
    lsp_exit();
}

static void _signal_sigint_init()
{
    g_tags.sigint = lsp_malloc(sizeof(uv_signal_t));
    if (uv_signal_init(g_tags.loop, g_tags.sigint) != 0)
    {
        fprintf(stderr, "initialize sigint failed.\n");
        abort();
    }
    uv_signal_start(g_tags.sigint, _on_signal, SIGINT);
}

static void _show_welecome(void)
{
    lsp_direct_log(s_welcome);
    lsp_direct_log("\n");

    LSP_LOG(LSP_MSG_INFO, "PID: %" PRId64 ", PPID:%" PRId64,
        (int64_t)uv_os_getpid(), (int64_t)uv_os_getppid());
}

static void _on_exit_notify(uv_async_t* handle)
{
    (void)handle;
    uv_stop(g_tags.loop);
}

static void _init_exit_notifier(void)
{
    g_tags.exit_notifier = lsp_malloc(sizeof(uv_async_t));
    uv_async_init(g_tags.loop, g_tags.exit_notifier, _on_exit_notify);
}

static char** _initialize(int argc, char* argv[])
{
    argv = uv_setup_args(argc, argv);
    uv_disable_stdio_inheritance();

    g_tags.config.lsp_log_level = LSP_MSG_INFO;

    /* Initialize event loop. */
    g_tags.loop = lsp_malloc(sizeof(uv_loop_t));
    if (uv_loop_init(g_tags.loop) != 0)
    {
        fprintf(stderr, "initialize main event loop failed.\n");
        abort();
    }

    /* Initialize sigint handler. */
    _signal_sigint_init();

    _init_exit_notifier();

    tag_lsp_io_cfg_t io_cfg;
    _parser_command_line_options(&io_cfg, argv);

    /* Initialize log system. */
    lsp_log_init();

    /* Let's welcome user. */
    _show_welecome();

    /* Initialize IO layer. */
    tag_lsp_io_init(&io_cfg);

    lsp_msg_init();
    lsp_work_init();

    g_tags.parser = lsp_parser_create(_handle_request, NULL);

    return argv;
}

int main(int argc, char* argv[])
{
#if defined(SIGPIPE)
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Register global cleanup hook. */
    atexit(_at_exit);

    /* Global initialize. */
    argv = _initialize(argc, argv);

    //uv_sleep(10 * 1000);

    /* Run. */
    uv_run(g_tags.loop, UV_RUN_DEFAULT);

    LSP_LOG(LSP_MSG_INFO, "program about to exit.");

    return 0;
}
