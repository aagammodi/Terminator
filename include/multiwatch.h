#ifndef TERMINATOR_MULTIWATCH_H
#define TERMINATOR_MULTIWATCH_H

#include "common.h"
#include "terminal.h"

typedef struct {
    char **commands;
    int count;
} MultiWatchArgs;

bool multiwatch_parse_args(const char *arg_str, MultiWatchArgs *args, char *err_msg, size_t err_len);
void multiwatch_free_args(MultiWatchArgs *args);

int multiwatch_run(Tab *tab, const char *raw_cmd, int read_ipc_fd, int write_ipc_fd);

#endif /* TERMINATOR_MULTIWATCH_H */
