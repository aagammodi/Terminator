#ifndef TERMINATOR_EXPANSION_H
#define TERMINATOR_EXPANSION_H

#include "common.h"

/* Recursively process $(...) and `...` substitutions from innermost to outermost */
char *expansion_process_command_substitutions(const char *input, int last_exit_status);

/* Expand $VAR and $? respecting single vs double quotes */
char *expansion_expand_variables(const char *input, int last_exit_status);

/* Pathname expansion (globbing) for unquoted wildcard tokens */
char **expansion_expand_glob(const char *token, int *out_count);
void expansion_free_glob_list(char **list, int count);

#endif /* TERMINATOR_EXPANSION_H */
