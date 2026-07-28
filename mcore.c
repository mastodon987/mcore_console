/*
 * mcore -- MeshCore console client
 *
 * Connects to a running meshcored instance via its Unix domain socket and
 * provides an interactive CLI.
 *
 * Build:
 *   gcc -O2 -o mcore mcore.c -lreadline
 *   # or without readline (basic line editing only):
 *   gcc -O2 -DNO_READLINE -o mcore mcore.c
 *
 * Usage:
 *   mcore                                        # /run/meshcored/meshcored-default.sock
 *   mcore -s /run/meshcored/meshcored-foo.sock   # specific appliance socket
 *   mcore -i mynode                              # /run/meshcored/meshcored-mynode.sock
 *   mcore -e "get name"                          # non-interactive, one command
 *   mcore -e "set name X" -e "get name"
 *
 * Install:
 *   sudo cp mcore /usr/local/bin/
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#ifndef NO_READLINE
  #include <readline/readline.h>
  #include <readline/history.h>
  #define HAS_READLINE 1
#else
  #define HAS_READLINE 0
#endif

/* ------------------------------------------------------------------ */
/* ANSI colour codes                                                    */
/* ------------------------------------------------------------------ */
#define COL_BANNER  "\033[1;32m"   /* bold green  */
#define COL_PROMPT  "\033[0;36m"   /* cyan        */
#define COL_ERROR   "\033[1;31m"   /* bold red    */
#define COL_RESET   "\033[0m"

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */
#define DEFAULT_INSTANCE  "default"
#define SOCK_PATH_MAX     256
#define CMD_MAX           512
#define BUF_MAX           8192
#define MAX_EXEC_CMDS     64
#define HISTORY_FILE_MAX  512

static int  g_sock        = -1;
static int  g_use_color   = 1;
static char g_history_path[HISTORY_FILE_MAX];

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static void sock_path_from_instance(char *out, size_t outlen)
{
    snprintf(out, outlen, "/run/meshcored/meshcored.sock");
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int do_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("mcore: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) {
        fprintf(stderr, "mcore: socket path too long: %s\n", path);
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, plen + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "%smcore: socket not found: %s%s\n",
                    g_use_color ? COL_ERROR : "", path,
                    g_use_color ? COL_RESET : "");
            fprintf(stderr, "       Is meshcored running with WITH_MC_CONSOLE=1?\n");
        } else {
            fprintf(stderr, "%smcore: cannot connect to %s: %s%s\n",
                    g_use_color ? COL_ERROR : "", path,
                    strerror(errno),
                    g_use_color ? COL_RESET : "");
        }
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    return fd;
}

/*
 * Drain all data available on fd within `timeout_ms` milliseconds of idle.
 * Returns number of bytes written into buf (null-terminated).
 */
static int recv_pending(int fd, char *buf, int bufsz, int timeout_ms)
{
    int total = 0;
    struct timeval tv;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
            break;

        int space = bufsz - total - 1;
        if (space <= 0)
            break;

        ssize_t n = read(fd, buf + total, space);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                total = -1; /* server closed */
            break;
        }
        total += (int)n;

        /* give a very short grace window for more data to arrive */
        timeout_ms = 50;
    }

    if (total > 0)
        buf[total] = '\0';
    return total;
}

static void send_line(int fd, const char *line)
{
    char buf[CMD_MAX + 2];
    int  n = snprintf(buf, sizeof(buf), "%s\n", line);
    ssize_t r = write(fd, buf, n);
    (void)r;
}

static void print_server_data(const char *data)
{
    /*
     * The server prefixes every line with "mcore> ".
     * Strip that prefix before printing so we don't get doubled prompts
     * (e.g. "mcore> > 64" becomes "> 64" without stripping, or just "64"
     * if the value reply itself starts with "> ").
     */
    static const char PREFIX[] = "mcore> ";
    static const int  PREFIX_LEN = 7; /* strlen("mcore> ") */

    const char *p = data;
    while (*p) {
        /* find end of this line */
        const char *eol = strchr(p, '\n');
        int line_len = eol ? (int)(eol - p) : (int)strlen(p);

        /* strip the "mcore> " prefix if present */
        const char *line = p;
        if (line_len >= PREFIX_LEN && memcmp(line, PREFIX, PREFIX_LEN) == 0) {
            line     += PREFIX_LEN;
            line_len -= PREFIX_LEN;
        }

        /* strip a leading "> " from bare value replies like "> 64" */
        if (line_len >= 2 && line[0] == '>' && line[1] == ' ') {
            line     += 2;
            line_len -= 2;
        }

        fwrite(line, 1, line_len, stdout);
        fputc('\n', stdout);

        if (!eol) break;
        p = eol + 1;
    }
    fflush(stdout);
}

static void cleanup_history(void)
{
#if HAS_READLINE
    if (g_history_path[0])
        write_history(g_history_path);
#endif
}

static void sigint_handler(int sig)
{
    (void)sig;
    cleanup_history();
    (void)!write(STDOUT_FILENO, "\n", 1);
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* Interactive mode                                                      */
/* ------------------------------------------------------------------ */
static void run_interactive(int fd)
{
    /* print banner */
    char banner[BUF_MAX];
    int n = recv_pending(fd, banner, sizeof(banner), 1000);
    if (n > 0) {
        if (g_use_color)
            printf("%s%s%s\n", COL_BANNER, banner, COL_RESET);
        else
            printf("%s\n", banner);
        fflush(stdout);
    } else if (n < 0) {
        fprintf(stderr, "%smcore: server closed immediately.%s\n",
                g_use_color ? COL_ERROR : "",
                g_use_color ? COL_RESET : "");
        return;
    }

#if HAS_READLINE
    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_history_path, sizeof(g_history_path), "%s/.mcore_history", home);
        read_history(g_history_path);
        stifle_history(500);
    }
#endif

    signal(SIGINT, sigint_handler);

    char prompt[64];
    if (g_use_color)
        snprintf(prompt, sizeof(prompt), COL_PROMPT "mcore" COL_RESET "> ");
    else
        snprintf(prompt, sizeof(prompt), "mcore> ");

    for (;;) {
        /* Check for any server-pushed data before showing prompt */
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {0, 0};
            if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
                char tmp[BUF_MAX];
                int m = recv_pending(fd, tmp, sizeof(tmp), 50);
                if (m > 0)
                    print_server_data(tmp);
                else if (m < 0) {
                    fprintf(stderr, "%smcore: server closed the connection.%s\n",
                            g_use_color ? COL_ERROR : "",
                            g_use_color ? COL_RESET : "");
                    break;
                }
            }
        }

        char *line = NULL;

#if HAS_READLINE
        line = readline(prompt);
        if (!line) {
            /* Ctrl-D / EOF */
            printf("\n");
            break;
        }
        if (line[0])
            add_history(line);
#else
        char linebuf[CMD_MAX];
        fputs(prompt, stdout);
        fflush(stdout);
        if (!fgets(linebuf, sizeof(linebuf), stdin)) {
            printf("\n");
            break;
        }
        /* strip trailing newline */
        linebuf[strcspn(linebuf, "\r\n")] = '\0';
        line = linebuf;
#endif

        /* trim leading whitespace */
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        if (!*cmd) {
#if HAS_READLINE
            free(line);
#endif
            continue;
        }

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            send_line(fd, cmd);
            usleep(100000);
#if HAS_READLINE
            free(line);
#endif
            break;
        }

        send_line(fd, cmd);

        /* read response */
        char reply[BUF_MAX];
        int r = recv_pending(fd, reply, sizeof(reply), 2000);
        if (r > 0) {
            print_server_data(reply);
        } else if (r < 0) {
            fprintf(stderr, "%smcore: server closed the connection.%s\n",
                    g_use_color ? COL_ERROR : "",
                    g_use_color ? COL_RESET : "");
#if HAS_READLINE
            free(line);
#endif
            break;
        }

#if HAS_READLINE
        free(line);
#endif
    }

    cleanup_history();
}

/* ------------------------------------------------------------------ */
/* Batch / non-interactive mode                                          */
/* ------------------------------------------------------------------ */
static void run_batch(int fd, char **cmds, int ncmds)
{
    /* consume banner silently */
    char banner[BUF_MAX];
    recv_pending(fd, banner, sizeof(banner), 1000);

    for (int i = 0; i < ncmds; i++) {
        send_line(fd, cmds[i]);
        char reply[BUF_MAX];
        int r = recv_pending(fd, reply, sizeof(reply), 3000);
        if (r > 0) {
            fputs(reply, stdout);
            if (reply[strlen(reply)-1] != '\n')
                fputc('\n', stdout);
            fflush(stdout);
        } else if (r < 0) {
            fprintf(stderr, "mcore: server closed unexpectedly.\n");
            break;
        }
    }

    send_line(fd, "exit");
    usleep(100000);
}

/* ------------------------------------------------------------------ */
/* Usage                                                                 */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS]\n"
        "\n"
        "MeshCore console client. Connects to a running meshcored instance\n"
        "via its Unix domain socket.\n"
        "\n"
        "Options:\n"
        "  -s PATH       Connect to socket at PATH\n"
        "  -i NAME       Shorthand: connect to /run/meshcored/meshcored-NAME.sock\n"
        "                Default instance name: " DEFAULT_INSTANCE "\n"
        "  -e CMD        Execute CMD non-interactively and exit.\n"
        "                May be repeated: -e 'get name' -e 'get freq'\n"
        "  -n            Disable ANSI colour output\n"
        "  -h, --help    Show this help\n"
        "\n"
        "Sockets:\n"
        "  meshcored creates a Unix domain socket for each running instance.\n"
        "  Default location: /run/meshcored/meshcored-" DEFAULT_INSTANCE ".sock\n"
        "  List all available sockets with:\n"
        "    ls /run/meshcored/meshcored-*.sock\n"
        "\n"
        "Examples:\n"
        "  %s                                        # connect to default socket\n"
        "  %s -s /run/meshcored/meshcored-node2.sock # connect to different appliance\n"
        "  %s -i repeater1                           # /run/meshcored/meshcored-repeater1.sock\n"
        "  %s -e 'get name' -e 'get freq'            # scripted, no interaction\n"
        "\n"
        "Keyboard shortcuts (interactive):\n"
        "  Ctrl-C / Ctrl-D / 'exit' / 'quit'  -- disconnect and exit\n"
        "  Up/Down arrows                       -- command history\n",
        prog, prog, prog, prog, prog
    );
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    char  sock_path[SOCK_PATH_MAX] = "";
    char  instance[64]             = DEFAULT_INSTANCE;
    char *exec_cmds[MAX_EXEC_CMDS];
    int   n_exec = 0;
    int   opt;

    /* handle GNU-style long options before getopt sees them */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    while ((opt = getopt(argc, argv, "s:i:e:nh")) != -1) {
        switch (opt) {
        case 's':
            strncpy(sock_path, optarg, sizeof(sock_path) - 1);
            break;
        case 'i':
            strncpy(instance, optarg, sizeof(instance) - 1);
            break;
        case 'e':
            if (n_exec < MAX_EXEC_CMDS)
                exec_cmds[n_exec++] = optarg;
            break;
        case 'n':
            g_use_color = 0;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            fprintf(stderr, "Try '%s -h' for help.\n", argv[0]);
            return 1;
        }
    }

    /* resolve socket path */
    if (!sock_path[0])
        sock_path_from_instance(sock_path, sizeof(sock_path));

    g_sock = do_connect(sock_path);
    if (g_sock < 0)
        return 1;

    if (n_exec > 0)
        run_batch(g_sock, exec_cmds, n_exec);
    else
        run_interactive(g_sock);

    close(g_sock);
    return 0;
}
