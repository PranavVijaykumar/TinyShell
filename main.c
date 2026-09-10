#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/wait.h>
#include "shell.h"

pid_t shell_pgid;
int   shell_terminal;
int   shell_is_interactive;
pid_t fg_pgid = 0;

/*
 * We do NOT reap children directly inside the signal handler, because
 * printf/waitpid-with-side-effects inside a signal handler is fragile
 * (only a small, documented set of functions are async-signal-safe).
 * Instead the handler just flips a flag, and the main loop -- running in
 * normal context -- calls jobs_reap() when it sees the flag set. This is
 * the standard "self-pipe / flag" pattern for making SIGCHLD handling
 * safe and easy to reason about.
 */
static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int sig) {
    (void)sig;
    got_sigchld = 1;
}

/*
 * SIGINT (Ctrl-C) and SIGTSTP (Ctrl-Z) arrive at the shell process too,
 * because the shell is in the foreground process group until it hands
 * the terminal to a child job. We ignore them in the shell itself --
 * if there's a foreground job, the terminal driver already delivered the
 * signal to THAT job's process group as well (since it owns the terminal
 * at that moment), so the shell doesn't need to forward anything manually.
 */
static void sigint_ignore(int sig)  { (void)sig; }
static void sigtstp_ignore(int sig) { (void)sig; }

static void shell_init(void) {
    shell_terminal = STDIN_FILENO;
    shell_is_interactive = isatty(shell_terminal);

    if (shell_is_interactive) {
        /* wait until we're in the foreground */
        while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
            kill(-shell_pgid, SIGTTIN);

        signal(SIGINT, sigint_ignore);
        signal(SIGTSTP, sigtstp_ignore);
        signal(SIGTTOU, SIG_IGN); /* don't stop when we background-write to tty */
        signal(SIGTTIN, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);

        shell_pgid = getpid();
        if (getpgrp() != shell_pgid && setpgid(shell_pgid, shell_pgid) < 0) {
            /* Only a real problem if we weren't already our own group
             * leader. (A session leader -- e.g. when spawned directly
             * under a pty -- is *already* the sole member of its own
             * process group and can't re-setpgid itself; POSIX forbids
             * a session leader from changing its own pgid. That's not
             * an error, just a no-op in that case.) */
            perror("tinyshell: couldn't put shell in its own process group");
        }
        tcsetpgrp(shell_terminal, shell_pgid);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* let interrupted syscalls like read() retry */
    sigaction(SIGCHLD, &sa, NULL);

    jobs_init();
}

static void print_prompt(void) {
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)))
        printf("tinyshell:%s$ ", cwd);
    else
        printf("tinyshell$ ");
    fflush(stdout);
}

int main(void) {
    shell_init();

    char line[MAX_LINE];

    while (1) {
        if (got_sigchld) {
            got_sigchld = 0;
            jobs_reap(0);
        }

        print_prompt();

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF (Ctrl-D) */
            printf("\n");
            break;
        }

        /* fgets can be interrupted by our own SIGCHLD handler returning
         * with SA_RESTART, which handles the retry for us -- but if a
         * signal races in right as we read an empty result, just loop. */
        size_t len = strlen(line);
        if (len == 0) continue;
        if (line[len - 1] == '\n') line[len - 1] = '\0';

        pipeline_t pl;
        int rc = parse_line(line, &pl);
        if (rc <= 0) continue; /* empty line or parse error already reported */

        if (builtin_try_exec(&pl)) continue;

        exec_pipeline(&pl);
    }

    return 0;
}
