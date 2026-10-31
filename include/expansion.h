#ifndef TERMINATOR_EXPANSION_H
#define TERMINATOR_EXPANSION_H

#include "common.h"

struct Tab;

typedef struct ScriptVarContext {
    char vars[128][64];   /* variable names */
    char vals[128][512];  /* variable values */
    int num_vars;

    char *argv[64];       /* $0, $1, $2, ... */
    int argc;             /* count of positional parameters ($#) */
} ScriptVarContext;

extern __thread ScriptVarContext *g_active_script_ctx;
extern __thread struct Tab *g_active_expansion_tab;

/* Recursively process $(...) and `...` substitutions from innermost to outermost */
char *expansion_process_command_substitutions(const char *input, int last_exit_status);

/* Expand $VAR, $?, $1..$9, $# respecting single vs double quotes */
char *expansion_expand_variables(const char *input, int last_exit_status);
char *expansion_expand_variables_ex(const char *input, int last_exit_status, const ScriptVarContext *ctx, const struct Tab *tab);

/* Pathname expansion (globbing) for unquoted wildcard tokens */
char **expansion_expand_glob(const char *token, int *out_count);
void expansion_free_glob_list(char **list, int count);

#endif /* TERMINATOR_EXPANSION_H */
