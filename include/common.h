#ifndef TERMINATOR_COMMON_H
#define TERMINATOR_COMMON_H

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <limits.h>
#include <pwd.h>

#define MAX_INPUT_LEN           4096
#define MAX_CMD_LEN             16384
#define MAX_ARGS                256
#define MAX_PIPELINE_CMDS       64
#define MAX_SCROLLBACK_LINES    10000
#define INITIAL_LINE_CAPACITY   512
#define MAX_PATH_LEN            1024
#define MAX_HISTORY_ENTRIES     1000

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Safe string copy that always null-terminates without truncation warnings */
static inline void safe_strncpy(char *dest, const char *src, size_t n) {
    if (!dest || n == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < n && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

#endif /* TERMINATOR_COMMON_H */
