#include "builtins.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

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
            strcmp(name, "source") == 0);
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
    (void)cmd;
    (void)tab;
    exit(0);
}

static int builtin_history(const Command *cmd, Tab *tab) {
    (void)cmd;
    for (int i = 0; i < tab->history_count; i++) {
        printf("%5d  %s\n", i + 1, tab->history[i]);
    }
    fflush(stdout);
    return 0;
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
    } else if (strcmp(name, "multiWatch") == 0 ||
               strcmp(name, "jobs") == 0 ||
               strcmp(name, "fg") == 0 ||
               strcmp(name, "bg") == 0 ||
               strcmp(name, "source") == 0) {
        fprintf(stderr, "terminator: %s: built-in deferred to Tasks 6-11 (Phase 2)\n", name);
        return 0;
    }

    return 127;
}
