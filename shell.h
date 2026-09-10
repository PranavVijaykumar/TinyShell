#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>

#define MAX_LINE   1024
#define MAX_ARGS   64
#define MAX_CMDS   16   /* max commands chained by pipes, e.g. a | b | c */
#define MAX_JOBS   64

/* ---------- Parsing structures ---------- */

/* One command in a pipeline: "grep foo file.txt < in.txt > out.txt" */
typedef struct {
    char *argv[MAX_ARGS];   /* NULL-terminated argv for execvp */
    char *infile;           /* redirect stdin from this file, or NULL */
    char *outfile;          /* redirect stdout to this file, or NULL */
    int   append;           /* 1 if ">>" was used instead of ">" */
} command_t;

/* A full pipeline: cmd1 | cmd2 | ... | cmdN, optionally backgrounded */
typedef struct {
    command_t cmds[MAX_CMDS];
    int       ncmds;
    int       background;   /* 1 if line ended with '&' */
    char      raw[MAX_LINE]; /* original text, stored for `jobs` output */
} pipeline_t;

/* ---------- Job control structures ---------- */

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

typedef struct {
    int         id;         /* small integer job id, like [1] */
    pid_t       pgid;       /* process group id of the whole pipeline */
    job_state_t state;
    int         background; /* was launched with & */
    char        cmdline[MAX_LINE];
} job_t;

/* ---------- parser.c ---------- */
int parse_line(char *line, pipeline_t *pl);

/* ---------- jobs.c ---------- */
void jobs_init(void);
job_t *jobs_add(pid_t pgid, int background, const char *cmdline);
job_t *jobs_find_by_pgid(pid_t pgid);
job_t *jobs_find_by_id(int id);
job_t *jobs_most_recent(job_state_t want_state);
void jobs_remove(job_t *j);
void jobs_print(void);
void jobs_reap(int blocking_wait_for_pgid_or_zero);

/* ---------- builtins.c ---------- */
int builtin_try_exec(pipeline_t *pl); /* returns 1 if it was a builtin (and ran it) */

/* ---------- exec.c ---------- */
void exec_pipeline(pipeline_t *pl);

/* ---------- globals shared for signal handlers ---------- */
extern pid_t shell_pgid;
extern int   shell_terminal;
extern int   shell_is_interactive;
extern pid_t fg_pgid; /* pgid currently in the foreground, 0 if it's the shell itself */

#endif
