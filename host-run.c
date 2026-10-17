/* host-run: run a command on the host from inside the Flatpak sandbox.
 *
 * This talks to the org.freedesktop.Flatpak.Development.HostCommand
 * D-Bus API (the same one "flatpak-spawn --host" uses), so the app needs
 * --talk-name=org.freedesktop.Flatpak.
 *
 *   host-run [--env=NAME[=VALUE]]... [--] COMMAND [ARG]...
 *   host-shell [ARG]...      the user's login shell on the host
 *   NAME [ARG]...            when installed as a symlink called NAME
 *                            (e.g. podman, toolbox), runs the host's NAME
 *
 * If stdin and stdout/stderr are a terminal the command gets a fresh
 * pseudo-terminal of its own, which it can make its controlling terminal,
 * so job control, Ctrl-C and window resizes behave as in a host terminal.
 * Otherwise our stdin/stdout/stderr are handed to the command directly,
 * so pipes, EOF and a separate stderr work as expected.
 *
 * The exit status of the host command is passed through; if it was killed
 * by a signal we die from the same signal.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define FLATPAK_BUS   "org.freedesktop.Flatpak"
#define FLATPAK_PATH  "/org/freedesktop/Flatpak/Development"
#define FLATPAK_IFACE "org.freedesktop.Flatpak.Development"

/* Kill the host command if our bus connection goes away. */
#define HOST_COMMAND_FLAGS_WATCH_BUS (1 << 1)

/* Exit code for our own failures, as env(1) and podman use. */
#define EXIT_HOST_RUN 125

/* The host command starts with the host session's environment, not the
 * sandbox's; only these are carried over (plus any --env). */
static const char *const forward_env[] = {
    "TERM", "COLORTERM", "TERMINFO", "INSIDE_EMACS", "EMACS_VTERM_PATH", NULL
};

static const int forward_signals[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2
};

static const char host_shell_script[] =
    "shell=$(getent passwd \"$(id -un)\" | cut -d: -f7)\n"
    "exec \"${shell:-/bin/sh}\" -l \"$@\"\n";

static struct {
    sd_bus *bus;
    sd_event *event;
    uint32_t pid;
    bool exited;
    int wait_status;
    int pty_master;             /* -1 unless running on a pty */
    int pty_out;                /* where output from the pty is copied to */
    bool termios_saved;
    struct termios termios;     /* our terminal's settings, to restore */
} st = { .pty_master = -1 };

static void restore_terminal(void)
{
    if (st.termios_saved)
        tcsetattr(STDIN_FILENO, TCSADRAIN, &st.termios);
    st.termios_saved = false;
}

static void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...)
{
    va_list ap;

    restore_terminal();
    fputs("host-run: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_HOST_RUN);
}

static void usage(FILE *out)
{
    fputs("usage: host-run [--env=NAME[=VALUE]]... [--] COMMAND [ARG]...\n"
          "       host-shell [ARG]...\n"
          "Run COMMAND on the host, outside the Flatpak sandbox.\n", out);
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= n;
    }
}

/* Copy whatever is waiting on the pty to our output.  Returns false once
 * the pty has hung up (every copy of the slave side is closed). */
static bool copy_pty_output(void)
{
    char buf[8192];
    ssize_t n = read(st.pty_master, buf, sizeof(buf));

    if (n > 0) {
        write_all(st.pty_out, buf, n);
        return true;
    }
    return n < 0 && (errno == EINTR || errno == EAGAIN);
}

static void finish(void)
{
    /* Anything the command wrote before exiting is already in the pty. */
    if (st.pty_master >= 0) {
        struct pollfd p = { .fd = st.pty_master, .events = POLLIN };
        while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN) && copy_pty_output())
            ;
    }
    sd_event_exit(st.event, 0);
}

static int on_exited(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    uint32_t pid, status;

    if (sd_bus_message_read(m, "uu", &pid, &status) < 0 || pid != st.pid)
        return 0;
    st.exited = true;
    st.wait_status = status;
    finish();
    return 0;
}

static void signal_host(int sig)
{
    sd_bus_call_method_async(st.bus, NULL, FLATPAK_BUS, FLATPAK_PATH, FLATPAK_IFACE,
                             "HostCommandSignal", NULL, NULL,
                             "uub", st.pid, (uint32_t)sig, 1);
}

static void copy_window_size(void)
{
    struct winsize ws;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(st.pty_master, TIOCSWINSZ, &ws);
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata)
{
    if (si->ssi_signo == SIGWINCH)
        copy_window_size();
    else
        signal_host(si->ssi_signo);
    return 0;
}

static int on_stdin(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n > 0) {
        write_all(st.pty_master, buf, n);
        return 0;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return 0;
    /* Our terminal went away: hang up the command, as a terminal would. */
    sd_event_source_set_enabled(s, SD_EVENT_OFF);
    signal_host(SIGHUP);
    return 0;
}

static int on_pty(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    if (!copy_pty_output())
        sd_event_source_set_enabled(s, SD_EVENT_OFF);
    return 0;
}

static bool fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

/* Set up a pty for the command.  Returns the slave fd to hand over. */
static int setup_pty(void)
{
    struct termios raw;
    int master, slave;

    if (tcgetattr(STDIN_FILENO, &st.termios) < 0)
        die("tcgetattr: %m");
    if (openpty(&master, &slave, NULL, &st.termios, NULL) < 0)
        die("openpty: %m");
    fcntl(master, F_SETFD, FD_CLOEXEC);
    st.pty_master = master;
    st.pty_out = isatty(STDOUT_FILENO) ? STDOUT_FILENO : STDERR_FILENO;
    copy_window_size();

    raw = st.termios;
    cfmakeraw(&raw);
    st.termios_saved = true;
    atexit(restore_terminal);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    return slave;
}

/* The cwd as the host sees it: --filesystem=host puts host directories
 * that clash with the sandbox (e.g. /usr) under /run/host. */
static char *host_cwd(void)
{
    char *cwd = get_current_dir_name();

    if (!cwd)
        return strdup(getenv("HOME") ? getenv("HOME") : "/");
    if (strncmp(cwd, "/run/host/", 10) == 0)
        memmove(cwd, cwd + 9, strlen(cwd + 9) + 1);
    return cwd;
}

static void append_env(sd_bus_message *m, const char *name, const char *value)
{
    if (sd_bus_message_append(m, "{ss}", name, value) < 0)
        die("cannot build message");
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    const char **extra_env = calloc(argc, sizeof(char *));
    int n_extra_env = 0;
    const char *prefix[4];
    int n_prefix = 0, first_arg = 1;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sigset_t mask;
    int slave = -1, r;

    if (strcmp(name, "host-run") == 0) {
        for (; first_arg < argc && argv[first_arg][0] == '-'; first_arg++) {
            const char *arg = argv[first_arg];
            if (strcmp(arg, "--") == 0) {
                first_arg++;
                break;
            } else if (strncmp(arg, "--env=", 6) == 0 && arg[6] && arg[6] != '=') {
                extra_env[n_extra_env++] = arg + 6;
            } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
                usage(stdout);
                return 0;
            } else {
                usage(stderr);
                return EXIT_HOST_RUN;
            }
        }
        if (first_arg == argc) {
            usage(stderr);
            return EXIT_HOST_RUN;
        }
    } else if (strcmp(name, "host-shell") == 0) {
        prefix[n_prefix++] = "/bin/sh";
        prefix[n_prefix++] = "-c";
        prefix[n_prefix++] = host_shell_script;
        prefix[n_prefix++] = "host-shell";
    } else {
        prefix[n_prefix++] = name;
    }

    r = sd_bus_open_user(&st.bus);
    if (r < 0)
        die("cannot connect to the session bus: %s", strerror(-r));
    sd_event_default(&st.event);
    sd_bus_attach_event(st.bus, st.event, SD_EVENT_PRIORITY_NORMAL);

    r = sd_bus_match_signal(st.bus, NULL, FLATPAK_BUS, FLATPAK_PATH, FLATPAK_IFACE,
                            "HostCommandExited", on_exited, NULL);
    if (r < 0)
        die("cannot watch for exit: %s", strerror(-r));

    /* Signals are handled from the event loop; until then they queue up
     * and get forwarded once the command has started. */
    sigemptyset(&mask);
    for (size_t i = 0; i < sizeof(forward_signals) / sizeof(*forward_signals); i++) {
        sigaddset(&mask, forward_signals[i]);
    }
    sigaddset(&mask, SIGWINCH);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    for (size_t i = 0; i < sizeof(forward_signals) / sizeof(*forward_signals); i++) {
        sd_event_add_signal(st.event, NULL, forward_signals[i], on_signal, NULL);
    }

    if (isatty(STDIN_FILENO) && (isatty(STDOUT_FILENO) || isatty(STDERR_FILENO))) {
        slave = setup_pty();
        sd_event_add_signal(st.event, NULL, SIGWINCH, on_signal, NULL);
        sd_event_add_io(st.event, NULL, STDIN_FILENO, EPOLLIN, on_stdin, NULL);
        sd_event_add_io(st.event, NULL, st.pty_master, EPOLLIN, on_pty, NULL);
    }

    /* HostCommand(ay cwd, aay argv, a{uh} fds, a{ss} env, u flags) -> u pid */
    r = sd_bus_message_new_method_call(st.bus, &m, FLATPAK_BUS, FLATPAK_PATH,
                                       FLATPAK_IFACE, "HostCommand");
    if (r < 0)
        die("cannot build message: %s", strerror(-r));

    char *cwd = host_cwd();
    r = sd_bus_message_append_array(m, 'y', cwd, strlen(cwd) + 1);
    free(cwd);

    r = r < 0 ? r : sd_bus_message_open_container(m, 'a', "ay");
    for (int i = 0; r >= 0 && i < n_prefix; i++)
        r = sd_bus_message_append_array(m, 'y', prefix[i], strlen(prefix[i]) + 1);
    for (int i = first_arg; r >= 0 && i < argc; i++)
        r = sd_bus_message_append_array(m, 'y', argv[i], strlen(argv[i]) + 1);
    r = r < 0 ? r : sd_bus_message_close_container(m);

    r = r < 0 ? r : sd_bus_message_open_container(m, 'a', "{uh}");
    for (int fd = 0; r >= 0 && fd <= 2; fd++) {
        if (!fd_is_open(fd))
            continue;
        r = sd_bus_message_append(m, "{uh}", (uint32_t)fd,
                                  slave >= 0 && isatty(fd) ? slave : fd);
    }
    r = r < 0 ? r : sd_bus_message_close_container(m);
    if (r < 0)
        die("cannot build message: %s", strerror(-r));

    sd_bus_message_open_container(m, 'a', "{ss}");
    for (int i = 0; forward_env[i]; i++) {
        const char *value = getenv(forward_env[i]);
        /* Paths inside the app or runtime don't exist on the host. */
        if (!value || strncmp(value, "/app/", 5) == 0 || strncmp(value, "/usr/", 5) == 0)
            continue;
        append_env(m, forward_env[i], value);
    }
    for (int i = 0; i < n_extra_env; i++) {
        char *eq = strchr(extra_env[i], '=');
        if (eq) {
            char *var = strndup(extra_env[i], eq - extra_env[i]);
            append_env(m, var, eq + 1);
            free(var);
        } else if (getenv(extra_env[i])) {
            append_env(m, extra_env[i], getenv(extra_env[i]));
        }
    }
    sd_bus_message_close_container(m);

    if (sd_bus_message_append(m, "u", (uint32_t)HOST_COMMAND_FLAGS_WATCH_BUS) < 0)
        die("cannot build message");

    r = sd_bus_call(st.bus, m, 0, &error, &reply);
    if (r < 0)
        die("cannot run command on host: %s",
            error.message ? error.message : strerror(-r));
    if (sd_bus_message_read(reply, "u", &st.pid) < 0)
        die("unexpected reply from %s", FLATPAK_BUS);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(m);

    /* The host side has its own copy now; holding ours open would stop
     * the pty from hanging up when the command finishes. */
    if (slave >= 0)
        close(slave);

    r = sd_event_loop(st.event);
    restore_terminal();
    if (r < 0 || !st.exited)
        die("lost track of host command: %s", strerror(r < 0 ? -r : EPIPE));

    if (WIFSIGNALED(st.wait_status)) {
        int sig = WTERMSIG(st.wait_status);
        /* Don't dump core here because the host command did. */
        if (sig != SIGINT && sig != SIGTERM && sig != SIGHUP && sig != SIGPIPE && sig != SIGKILL)
            return 128 + sig;
        signal(sig, SIG_DFL);
        sigemptyset(&mask);
        sigaddset(&mask, sig);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        raise(sig);
        return 128 + sig;
    }
    return WEXITSTATUS(st.wait_status);
}
