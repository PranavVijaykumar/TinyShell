#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"

/*
 * Tokenizer:
 *  - splits on whitespace
 *  - treats |, <, >, >>, & as their own tokens even without surrounding spaces
 *    (e.g. "ls|wc" and "ls | wc" both work)
 * This keeps the parser simple: no quoting/escaping support, which is a
 * reasonable and clearly-stated scope cut for a CV project (mention it in
 * interviews as a "known limitation / next step").
 */
static int tokenize(char *line, char *tokens[], int max_tokens) {
    int n = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (n >= max_tokens - 1) break;

        if (*p == '|' || *p == '<' || *p == '&') {
            /* single-char operator tokens */
            tokens[n] = malloc(2);
            tokens[n][0] = *p; tokens[n][1] = '\0';
            n++; p++;
            continue;
        }
        if (*p == '>') {
            if (*(p + 1) == '>') {
                tokens[n] = strdup(">>");
                n++; p += 2;
            } else {
                tokens[n] = strdup(">");
                n++; p += 1;
            }
            continue;
        }

        /* ordinary word */
        char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' &&
               *p != '<' && *p != '>' && *p != '&') {
            p++;
        }
        int len = p - start;
        tokens[n] = malloc(len + 1);
        memcpy(tokens[n], start, len);
        tokens[n][len] = '\0';
        n++;
    }
    return n;
}

/*
 * Turns a token stream into a pipeline_t: a sequence of commands separated
 * by "|", each of which may have "<infile", ">outfile" / ">>outfile", and
 * the whole line may end in "&" to run in the background.
 */
int parse_line(char *line, pipeline_t *pl) {
    memset(pl, 0, sizeof(*pl));
    strncpy(pl->raw, line, MAX_LINE - 1);

    char *tokens[MAX_ARGS * MAX_CMDS];
    int ntok = tokenize(line, tokens, MAX_ARGS * MAX_CMDS);
    if (ntok == 0) return 0; /* empty line */

    command_t *cur = &pl->cmds[0];
    int argc = 0;
    pl->ncmds = 1;

    for (int i = 0; i < ntok; i++) {
        char *tok = tokens[i];

        if (strcmp(tok, "|") == 0) {
            cur->argv[argc] = NULL;
            pl->ncmds++;
            if (pl->ncmds > MAX_CMDS) {
                fprintf(stderr, "tinyshell: too many piped commands\n");
                return -1;
            }
            cur = &pl->cmds[pl->ncmds - 1];
            argc = 0;
        } else if (strcmp(tok, "<") == 0) {
            if (i + 1 >= ntok) { fprintf(stderr, "tinyshell: expected filename after <\n"); return -1; }
            cur->infile = strdup(tokens[++i]);
        } else if (strcmp(tok, ">") == 0) {
            if (i + 1 >= ntok) { fprintf(stderr, "tinyshell: expected filename after >\n"); return -1; }
            cur->outfile = strdup(tokens[++i]);
            cur->append = 0;
        } else if (strcmp(tok, ">>") == 0) {
            if (i + 1 >= ntok) { fprintf(stderr, "tinyshell: expected filename after >>\n"); return -1; }
            cur->outfile = strdup(tokens[++i]);
            cur->append = 1;
        } else if (strcmp(tok, "&") == 0) {
            pl->background = 1;
        } else {
            if (argc < MAX_ARGS - 1) {
                cur->argv[argc++] = strdup(tok);
            }
        }
    }
    cur->argv[argc] = NULL;

    for (int i = 0; i < ntok; i++) free(tokens[i]);
    return pl->ncmds;
}
