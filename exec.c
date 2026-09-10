#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include "shell.h"

/*
 * Launch one pipeline (possibly multiple commands joined by |).
 *
 * The key systems-programming ideas demonstrated here:
 *
 * 1. Process groups: every process in the pipeline is put into ONE new
 *    process group, led by the first child (pgid == first child's pid).
 *    This is what lets Ctrl-C/Ctrl-Z (which the terminal driver sends to
 *    the whole foreground process group) stop/kill an entire "cmd1 | cmd2"
 *    pipeline at once, not just one half of it.
 *
 * 2. The "double setpgid" race: both the parent and each child call
 *    setpgid() on the child right after fork(). Whichever runs first wins,
 *    but both must do it, because we can't guarantee the child gets
 *    scheduled before the parent tries to tcsetpgrp() that pgid to the
 *    foreground. This is the textbook race fix from APUE / the classic
 *    "Implement Your Own Shell" assignments.
 *
 * 3. Terminal ownership: tcsetpgrp() tells the kernel which process group
 *    is allowed to read/write the controlling terminal and receive
 *    terminal-generated signals. The shell must hand it over to the child
 *    pipeline while it runs in the foreground, then take it back.
 *
 * 4. Child signal dispositions: interactive shells ignore SIGINT/SIGTSTP/
 *    SIGTTOU/SIGTTIN themselves (see main.c), but children must restore
 *    the DEFAULT disposition, or Ctrl-C would do nothing to them either.
 */
void exec_pipeline(pipeline_t *pl) {
    int ncmds = pl->ncmds;
    int pipes[MAX_CMDS - 1][2];

    for (int i = 0; i < ncmds - 1; i++) {
        if (pipe(pipes[i]) < 0) { perror("pipe"); return; }
    }

    pid_t pgid = 0;

    for (int i = 0; i < ncmds; i++) {
        command_t *c = &pl->cmds[i];

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return; }

        if (pid == 0) {
            /* ---------- child ---------- */
            pid_t mypid = getpid();
            pid_t target_pgid = (pgid == 0) ? mypid : pgid;
            setpgid(mypid, target_pgid);

            if (!pl->background) {
                /* only the foreground pipeline should own the terminal */
                /* (parent also does this; see race note above) */
            }

            /* restore default signal behaviour for job-control signals */
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            /* wire up stdin from previous pipe stage */
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            /* wire up stdout to next pipe stage */
            if (i < ncmds - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            /* close all pipe fds in the child; the dup2'd ones are already
             * copied onto stdin/stdout, so the originals must go away or
             * later stages' readers never see EOF. */
            for (int k = 0; k < ncmds - 1; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }

            /* explicit file redirection overrides pipe wiring */
            if (c->infile) {
                int fd = open(c->infile, O_RDONLY);
                if (fd < 0) { perror(c->infile); _exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (c->outfile) {
                int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
                int fd = open(c->outfile, flags, 0644);
                if (fd < 0) { perror(c->outfile); _exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            execvp(c->argv[0], c->argv);
            /* execvp only returns on failure */
            fprintf(stderr, "tinyshell: %s: command not found\n", c->argv[0]);
            _exit(127);
        }

        /* ---------- parent ---------- */
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid); /* race-free: both sides set it */
    }

    /* parent closes its copies of all pipe fds -- otherwise readers never
     * see EOF because the write end stays open in the shell too */
    for (int i = 0; i < ncmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    job_t *j = jobs_add(pgid, pl->background, pl->raw);
    if (!j) return;

    if (pl->background) {
        printf("[%d] %d\n", j->id, (int)pgid);
        return; /* don't wait; main loop's SIGCHLD handler will reap it */
    }

    /* foreground: hand over the terminal and block until done/stopped */
    fg_pgid = pgid;
    tcsetpgrp(shell_terminal, pgid);

    int status;
    pid_t w;
    while ((w = waitpid(-pgid, &status, WUNTRACED)) > 0) {
        if (WIFSTOPPED(status)) {
            j->state = JOB_STOPPED;
            j->background = 1;
            printf("\n[%d]  Stopped\t\t%s\n", j->id, j->cmdline);
            break;
        }
    }
    if (w == -1) jobs_remove(j); /* whole pipeline finished normally */

    fg_pgid = 0;
    tcsetpgrp(shell_terminal, shell_pgid);
}
