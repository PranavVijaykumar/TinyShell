#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "shell.h"

static job_t job_table[MAX_JOBS];
static int   next_job_id = 1;

void jobs_init(void) {
    memset(job_table, 0, sizeof(job_table));
}

job_t *jobs_add(pid_t pgid, int background, const char *cmdline) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].pgid == 0) {
            job_table[i].id = next_job_id++;
            job_table[i].pgid = pgid;
            job_table[i].state = JOB_RUNNING;
            job_table[i].background = background;
            strncpy(job_table[i].cmdline, cmdline, MAX_LINE - 1);
            return &job_table[i];
        }
    }
    fprintf(stderr, "tinyshell: job table full\n");
    return NULL;
}

job_t *jobs_find_by_pgid(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (job_table[i].pgid == pgid) return &job_table[i];
    return NULL;
}

job_t *jobs_find_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (job_table[i].pgid != 0 && job_table[i].id == id) return &job_table[i];
    return NULL;
}

job_t *jobs_most_recent(job_state_t want_state) {
    job_t *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].pgid != 0 && job_table[i].state == want_state) {
            if (!best || job_table[i].id > best->id) best = &job_table[i];
        }
    }
    return best;
}

void jobs_remove(job_t *j) {
    if (j) memset(j, 0, sizeof(*j));
}

void jobs_print(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t *j = &job_table[i];
        if (j->pgid == 0) continue;
        const char *state =
            j->state == JOB_RUNNING ? "Running" :
            j->state == JOB_STOPPED ? "Stopped" : "Done";
        printf("[%d]  %-8s pgid=%-6d %s\n",
               j->id, state, (int)j->pgid, j->cmdline);
    }
}

/*
 * Reap terminated/stopped children without blocking the shell.
 * WNOHANG   -> don't block if nothing has changed state
 * WUNTRACED -> also report children stopped by SIGTSTP (Ctrl-Z)
 * WCONTINUED-> also report children resumed by SIGCONT (bg)
 *
 * This is called from the SIGCHLD handler (async-signal-context-safe:
 * only calls waitpid/kill/write-safe operations) and also from the
 * main loop when we need to synchronously wait on a specific foreground
 * process group.
 */
void jobs_reap(int wait_for_pgid) {
    int status;
    pid_t pid;

    while (1) {
        pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break;

        /* Find which job this pid's process group belongs to. We stored
         * pgid = pid of the group leader, so first check direct match,
         * then fall back to getpgid. */
        job_t *j = jobs_find_by_pgid(pid);
        if (!j) {
            pid_t pgid = getpgid(pid);
            if (pgid > 0) j = jobs_find_by_pgid(pgid);
        }
        if (!j) continue;

        if (WIFSTOPPED(status)) {
            j->state = JOB_STOPPED;
            if (!j->background)
                printf("\n[%d]  Stopped\t\t%s\n", j->id, j->cmdline);
        } else if (WIFCONTINUED(status)) {
            j->state = JOB_RUNNING;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (j->background)
                printf("[%d]  Done\t\t%s\n", j->id, j->cmdline);
            jobs_remove(j);
        }
    }
    (void)wait_for_pgid; /* reserved for future targeted blocking wait */
}
