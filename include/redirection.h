#ifndef TERMINATOR_REDIRECTION_H
#define TERMINATOR_REDIRECTION_H

#include "common.h"
#include "parser.h"

/* Set up redirection inside a child process before execvp().
 * Returns 0 on success, -1 on error (with perror/fprintf already printed). */
int redirection_apply_in_child(const Command *cmd);

/* For builtins running in the parent shell process:
 * Save original stdin/stdout and redirect to files.
 * Returns 0 on success, -1 on failure. */
int redirection_apply_in_parent(const Command *cmd, int *saved_stdin, int *saved_stdout);
void redirection_restore_in_parent(int saved_stdin, int saved_stdout);

#endif /* TERMINATOR_REDIRECTION_H */
