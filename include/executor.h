#ifndef TERMINATOR_EXECUTOR_H
#define TERMINATOR_EXECUTOR_H

#include "common.h"
#include "parser.h"
#include "terminal.h"

/* High-level execution entry point for a submitted command line.
 * Handles built-ins directly or launches pipelines/external commands. */
int executor_run_line(Tab *tab, const char *cmd_line);

/* Worker execution entry point (runs in tab thread or tab child process)
 * Streams output, CWD updates, and prompt completions via IPC pipe. */
int executor_run_line_worker(Tab *tab, const char *cmd_line, int write_ipc_fd);

/* Synchronous execution used specifically by command substitution $(cmd) and `cmd`.
 * Runs cmd through our own shell parser/pipeline and captures output into a string.
 * Strips trailing newline and returns allocated buffer. */
char *executor_execute_substitution_sync(const char *sub_cmd, int last_exit_status);

/* Non-blocking child reaping called by event loop */
void executor_reap_children(Tab *tab);

#endif /* TERMINATOR_EXECUTOR_H */
