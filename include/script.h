#ifndef TERMINATOR_SCRIPT_H
#define TERMINATOR_SCRIPT_H

#include "common.h"
#include "terminal.h"
#include "parser.h"

/* Determine whether a parsed command represents a script invocation:
 * 1. Form A: "./script.sh [args...]" or "path/to/script.sh" (or executable text file)
 * 2. Form B: "bash script.sh [args...]"
 * 3. Form C: "source script.sh [args...]" or ". script.sh [args...]"
 *
 * If true, resolves absolute/relative path into out_filepath,
 * sets *out_is_source, and returns true. */
bool script_is_script_invocation(const Command *cmd, const char *cwd,
                                 char *out_filepath, size_t out_size,
                                 bool *out_is_source);

/* Phase 1: Parse and validate entire script syntax before running any commands.
 * Checks control-flow nesting (if/elif/else/fi), test syntax ([ ... ]),
 * matching brackets and quotes.
 * Returns 0 on success, or non-zero on syntax error (with error description and line). */
int script_validate_file(const char *filepath, char *err_buf, size_t err_size, int *err_line);

/* Phase 2: Interpret and execute script internally (never delegates to /bin/bash).
 * Supports variables, positional parameters ($1..$9, $#), exit codes ($?),
 * conditionals (if/elif/else/fi), test operators (-eq, -gt, =, -f, -d),
 * pipelines, redirections, substitutions, and exit [n].
 * If is_source is true, variables and directory changes persist in tab.
 * Returns script exit status. */
int script_execute_file(Tab *tab, const char *filepath, int argc, char **argv,
                        bool is_source, int read_ipc_fd, int write_ipc_fd);

#endif /* TERMINATOR_SCRIPT_H */
