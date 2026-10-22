#ifndef TERMINATOR_BUILTINS_H
#define TERMINATOR_BUILTINS_H

#include "common.h"
#include "parser.h"
#include "terminal.h"

/* Returns true if the command name is a built-in */
bool builtin_is_builtin(const char *name);

/* Execute a built-in command in the shell context.
 * Returns the command exit status (0 for success). */
int builtin_execute(const Command *cmd, Tab *tab);

#endif /* TERMINATOR_BUILTINS_H */
