#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "shell.h"

/*
 * Move a job to the foreground: give it the controlling terminal, send
 * SIGCONT if it was stopped, then block the shell until it's done or
 * stopped again. This is the crux of "job control" -- the reason a shell
 * needs process groups at all is so Ctrl-C / Ctrl-Z affect the whole
 * pipeline, and so the kernel knows who is allowed to read the terminal.
 */
static void put_job_in_foreground(job_t *j, int send_cont) {
    fg_pgid = j->pgid;
    tcsetpgrp(shell_terminal, j->pgid);

    if (send_cont) {
        j->state = JOB_RUNNING;
        kill(-j->pgid, SIGCONT);
    }

    int status;
    pid_t pid;
    while ((pid = waitpid(-j->pgid, &status, WUNTRACED)) > 0) {
        if (WIFSTOPPED(status)) {
            j->state = JOB_STOPPED;
            j->background = 1; /* leave it behind, user can `fg`/`bg` later */
            printf("\n[%d]  Stopped\t\t%s\n", j->id, j->cmdline);
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            /* one process in the pipeline finished; keep waiting for the
             * rest of the group until waitpid returns -1 (no more children
             * in that pgid) */
            continue;
        }
    }

    /* give the terminal back to the shell itself */
    fg_pgid = 0;
    tcsetpgrp(shell_terminal, shell_pgid);

    if (pid == -1) {
        jobs_remove(j); /* whole group finished */
    }
}

static void bi_cd(char **argv) {
    const char *target = argv[1];
    if (!target) target = getenv("HOME");
    if (chdir(target) != 0) perror("cd");
}

static void bi_jobs(void) {
    jobs_print();
}

static int parse_job_arg(char *arg, job_t **out) {
    /* accepts "%3", "3", or nothing (most recent stopped/running job) */
    if (!arg) {
        *out = jobs_most_recent(JOB_STOPPED);
        if (!*out) *out = jobs_most_recent(JOB_RUNNING);
        if (!*out) { fprintf(stderr, "tinyshell: no current job\n"); return -1; }
        return 0;
    }
    int id = atoi(arg[0] == '%' ? arg + 1 : arg);
    *out = jobs_find_by_id(id);
    if (!*out) { fprintf(stderr, "tinyshell: no such job %s\n", arg); return -1; }
    return 0;
}

static void bi_fg(char **argv) {
    job_t *j;
    if (parse_job_arg(argv[1], &j) != 0) return;
    printf("%s\n", j->cmdline);
    put_job_in_foreground(j, j->state == JOB_STOPPED);
}

static void bi_bg(char **argv) {
    job_t *j;
    if (parse_job_arg(argv[1], &j) != 0) return;
    j->state = JOB_RUNNING;
    j->background = 1;
    kill(-j->pgid, SIGCONT);
    printf("[%d]  %s &\n", j->id, j->cmdline);
}

static void bi_kill(char **argv) {
    if (!argv[1]) { fprintf(stderr, "usage: kill %%jobid | pid\n"); return; }
    int sig = SIGTERM;
    int idx = 1;
    if (argv[1][0] == '-') { sig = atoi(argv[1] + 1); idx = 2; }
    if (!argv[idx]) { fprintf(stderr, "usage: kill [-sig] %%jobid | pid\n"); return; }

    if (argv[idx][0] == '%') {
        job_t *j = jobs_find_by_id(atoi(argv[idx] + 1));
        if (!j) { fprintf(stderr, "tinyshell: no such job\n"); return; }
        kill(-j->pgid, sig);
    } else {
        kill((pid_t)atoi(argv[idx]), sig);
    }
}

static void bi_help(void) {
    printf(
        "tinyshell -- built-in commands:\n"
        "  cd [dir]         change directory (default: $HOME)\n"
        "  jobs             list background/stopped jobs\n"
        "  fg [%%job]        resume a job in the foreground\n"
        "  bg [%%job]        resume a stopped job in the background\n"
        "  kill [-sig] %%job|pid   send a signal (default SIGTERM)\n"
        "  exit             quit the shell\n"
        "Pipelines: cmd1 | cmd2 | cmd3\n"
        "Redirection: cmd < infile, cmd > outfile, cmd >> outfile\n"
        "Background: end a line with &\n"
        "Ctrl-C interrupts the foreground job, Ctrl-Z suspends it.\n"
    );
}

/* Returns 1 and executes the builtin if pl->cmds[0] is a builtin name.
 * Builtins must NOT be forked -- e.g. `cd` has to change the shell's own
 * working directory, which is meaningless in a child process. */
int builtin_try_exec(pipeline_t *pl) {
    if (pl->ncmds != 1) return 0; /* builtins don't participate in pipes here */
    char **argv = pl->cmds[0].argv;
    if (!argv[0]) return 1; /* blank command, nothing to do */

    if (strcmp(argv[0], "cd") == 0)   { bi_cd(argv);   return 1; }
    if (strcmp(argv[0], "exit") == 0) { exit(argv[1] ? atoi(argv[1]) : 0); }
    if (strcmp(argv[0], "jobs") == 0) { bi_jobs();      return 1; }
    if (strcmp(argv[0], "fg") == 0)   { bi_fg(argv);    return 1; }
    if (strcmp(argv[0], "bg") == 0)   { bi_bg(argv);    return 1; }
    if (strcmp(argv[0], "kill") == 0) { bi_kill(argv);  return 1; }
    if (strcmp(argv[0], "help") == 0) { bi_help();      return 1; }

    return 0;
}
