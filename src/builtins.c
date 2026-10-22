#include "builtins.h"
#include "util.h"
#include "executor.h"
#include "parser.h"
#include "shell.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>

bool builtin_is_builtin(const char *name) {
    if (!name) return false;
    return (strcmp(name, "cd") == 0 ||
            strcmp(name, "pwd") == 0 ||
            strcmp(name, "clear") == 0 ||
            strcmp(name, "exit") == 0 ||
            strcmp(name, "history") == 0 ||
            strcmp(name, "multiWatch") == 0 ||
            strcmp(name, "jobs") == 0 ||
            strcmp(name, "fg") == 0 ||
            strcmp(name, "bg") == 0 ||
            strcmp(name, "source") == 0 ||
            strcmp(name, ".") == 0);
}

static int builtin_cd(const Command *cmd, Tab *tab) {
    char target[MAX_PATH_LEN];

    if (cmd->argc <= 1 || strcmp(cmd->argv[1], "~") == 0) {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        if (!home) {
            fprintf(stderr, "terminator: cd: HOME not set\n");
            return 1;
        }
        safe_strncpy(target, home, sizeof(target));
    } else {
        path_expand_tilde(cmd->argv[1], target, sizeof(target));
    }

    if (chdir(target) != 0) {
        fprintf(stderr, "terminator: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    if (!getcwd(tab->cwd, sizeof(tab->cwd))) {
        safe_strncpy(tab->cwd, target, sizeof(tab->cwd));
    }

    return 0;
}

static int builtin_pwd(const Command *cmd, Tab *tab) {
    (void)cmd;
    char cwd_buf[MAX_PATH_LEN];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        printf("%s\n", cwd_buf);
        safe_strncpy(tab->cwd, cwd_buf, sizeof(tab->cwd));
    } else {
        printf("%s\n", tab->cwd);
    }
    fflush(stdout);
    return 0;
}

static int builtin_clear(const Command *cmd, Tab *tab) {
    (void)cmd;
    terminal_clear_screen(tab);
    return 0;
}

static int builtin_exit(const Command *cmd, Tab *tab) {
    int code = 0;
    if (cmd->argc > 1) {
        code = atoi(cmd->argv[1]);
    } else {
        code = tab->last_exit_status;
    }
    tab->last_exit_status = code;

    if (tab->shell && tab->shell->num_tabs > 1) {
        tab->request_close = true;
        return code;
    }

    exit(code);
}

static int builtin_history(const Command *cmd, Tab *tab) {
    (void)cmd;
    for (int i = 0; i < tab->history_count; i++) {
        printf("%5d  %s\n", i + 1, tab->history[i]);
    }
    fflush(stdout);
    return 0;
}

static int builtin_source(const Command *cmd, Tab *tab) {
    if (cmd->argc < 2) {
        fprintf(stderr, "terminator: source: filename argument required\n");
        return 1;
    }

    char filepath[MAX_PATH_LEN];
    path_expand_tilde(cmd->argv[1], filepath, sizeof(filepath));

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "terminator: source: %s: %s\n", filepath, strerror(errno));
        return 1;
    }

    char line_buf[MAX_INPUT_LEN];
    char acc_cmd[MAX_CMD_LEN] = {0};
    int status = 0;

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        size_t len = strlen(line_buf);
        if (len > 0 && (line_buf[len - 1] == '\n' || line_buf[len - 1] == '\r')) {
            line_buf[len - 1] = '\0';
            len--;
        }
        if (len > 0 && line_buf[len - 1] == '\r') {
            line_buf[len - 1] = '\0';
            len--;
        }

        /* Check for comments outside multiline accumulation */
        if (acc_cmd[0] == '\0') {
            const char *p = line_buf;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == '#') {
                continue;
            }
        }

        /* Check multiline continuation */
        bool trailing_backslash = false;
        if (len > 0 && line_buf[len - 1] == '\\') {
            size_t bs_count = 0;
            while (len > bs_count && line_buf[len - 1 - bs_count] == '\\') {
                bs_count++;
            }
            if (bs_count % 2 == 1) {
                trailing_backslash = true;
            }
        }

        size_t acc_len = strlen(acc_cmd);
        if (acc_len > 0) {
            if (trailing_backslash) {
                line_buf[len - 1] = '\0';
                snprintf(acc_cmd + acc_len, sizeof(acc_cmd) - acc_len, "%s", line_buf);
            } else {
                snprintf(acc_cmd + acc_len, sizeof(acc_cmd) - acc_len, "\n%s", line_buf);
            }
        } else {
            if (trailing_backslash) {
                line_buf[len - 1] = '\0';
                safe_strncpy(acc_cmd, line_buf, sizeof(acc_cmd));
            } else {
                safe_strncpy(acc_cmd, line_buf, sizeof(acc_cmd));
            }
        }

        if (trailing_backslash || parser_is_incomplete(acc_cmd)) {
            continue;
        }

        /* Complete command: execute sequentially */
        status = executor_run_line(tab, acc_cmd);
        acc_cmd[0] = '\0';

        /* Drain any synchronous child pipeline output */
        if (tab->out_pipe_fd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(tab->out_pipe_fd, buf, sizeof(buf))) > 0) {
                terminal_append_output_bytes(tab, buf, (size_t)n, false);
            }
            close(tab->out_pipe_fd);
            tab->out_pipe_fd = -1;
            executor_reap_children(tab);
        }

        status = tab->last_exit_status;
        if (tab->request_close) {
            break;
        }
    }

    fclose(fp);
    return status;
}

static int builtin_jobs(const Command *cmd, Tab *tab) {
    (void)cmd;
    (void)tab;
    /* Phase 1: No background jobs active */
    return 0;
}

static int builtin_fg(const Command *cmd, Tab *tab) {
    (void)cmd;
    (void)tab;
    fprintf(stderr, "terminator: fg: no current job\n");
    return 1;
}

static int builtin_bg(const Command *cmd, Tab *tab) {
    (void)cmd;
    (void)tab;
    fprintf(stderr, "terminator: bg: no current job\n");
    return 1;
}

static int builtin_multiwatch(const Command *cmd, Tab *tab) {
    (void)cmd;
    (void)tab;
    fprintf(stderr, "terminator: multiWatch: requires Tasks 6-11 (Phase 2)\n");
    return 1;
}

int builtin_execute(const Command *cmd, Tab *tab) {
    if (!cmd || cmd->argc == 0 || !cmd->argv[0]) return 0;

    const char *name = cmd->argv[0];

    if (strcmp(name, "cd") == 0) {
        return builtin_cd(cmd, tab);
    } else if (strcmp(name, "pwd") == 0) {
        return builtin_pwd(cmd, tab);
    } else if (strcmp(name, "clear") == 0) {
        return builtin_clear(cmd, tab);
    } else if (strcmp(name, "exit") == 0) {
        return builtin_exit(cmd, tab);
    } else if (strcmp(name, "history") == 0) {
        return builtin_history(cmd, tab);
    } else if (strcmp(name, "source") == 0 || strcmp(name, ".") == 0) {
        return builtin_source(cmd, tab);
    } else if (strcmp(name, "jobs") == 0) {
        return builtin_jobs(cmd, tab);
    } else if (strcmp(name, "fg") == 0) {
        return builtin_fg(cmd, tab);
    } else if (strcmp(name, "bg") == 0) {
        return builtin_bg(cmd, tab);
    } else if (strcmp(name, "multiWatch") == 0) {
        return builtin_multiwatch(cmd, tab);
    }

    return 127;
}
