#include "pipeline.h"
#include "redirection.h"
#include "util.h"
#include "script.h"
#include "builtins.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

static void prepare_command_sudo(Command *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) return;
    const char *base = strrchr(cmd->argv[0], '/');
    base = base ? base + 1 : cmd->argv[0];
    if (strcmp(base, "sudo") != 0) return;

    /* Don't insert -A in headless/test mode */
    bool is_headless = (getenv("TERMINATOR_HEADLESS") != NULL || getenv("DISPLAY") == NULL);
    if (is_headless) return;

    /* Check if already has -A or -S or help/version/invalidation flags */
    for (int i = 1; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], "-A") == 0 ||
            strcmp(cmd->argv[i], "-S") == 0 ||
            strcmp(cmd->argv[i], "-h") == 0 ||
            strcmp(cmd->argv[i], "--help") == 0 ||
            strcmp(cmd->argv[i], "-V") == 0 ||
            strcmp(cmd->argv[i], "--version") == 0 ||
            strcmp(cmd->argv[i], "-k") == 0 ||
            strcmp(cmd->argv[i], "-K") == 0) {
            return;
        }
    }

    ensure_askpass_helper();

    /* Insert -A at index 1 */
    int old_argc = cmd->argc;
    char **old_argv = cmd->argv;
    int new_argc = old_argc + 1;
    int new_cap = new_argc + 2;
    char **new_argv = xcalloc((size_t)new_cap, sizeof(char *));
    new_argv[0] = old_argv[0];
    new_argv[1] = xstrdup("-A");
    for (int i = 1; i < old_argc; i++) {
        new_argv[i + 1] = old_argv[i];
    }
    new_argv[new_argc] = NULL;
    free(old_argv);

    cmd->argv = new_argv;
    cmd->argc = new_argc;
    cmd->argv_capacity = new_cap;
}

int pipeline_execute(Pipeline *pipeline, Tab *tab) {
    if (!pipeline || pipeline->num_commands == 0 || !tab) {
        return -1;
    }

    int n = pipeline->num_commands;

    if (n > MAX_PIPELINE_CMDS) {
        fprintf(stderr, "terminator: pipeline exceeds maximum stages (%d)\n", MAX_PIPELINE_CMDS);
        tab->last_exit_status = 1;
        return -1;
    }

    /* Allocate pipeline pipes: n - 1 pipes */
    int pipes[MAX_PIPELINE_CMDS][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("terminator: pipe creation failed");
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return -1;
        }
    }

    /* Create the capture pipe for the terminal tab to receive output/errors */
    int capture_pipe[2];
    if (pipe(capture_pipe) < 0) {
        perror("terminator: capture pipe failed");
        for (int i = 0; i < n - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        return -1;
    }

    /* Ensure close-on-exec for capture_pipe read-end in parent */
    fcntl(capture_pipe[0], F_SETFD, FD_CLOEXEC);

    tab->num_pipeline_pids = 0;
    pid_t first_pgid = 0;

    for (int i = 0; i < n; i++) {
        Command *cmd = &pipeline->commands[i];
        prepare_command_sudo(cmd);

        pid_t pid = fork();
        if (pid < 0) {
            perror("terminator: fork failed");
            for (int k = 0; k < i; k++) {
                kill(tab->pipeline_pids[k], SIGTERM);
                waitpid(tab->pipeline_pids[k], NULL, 0);
            }
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);
            tab->num_pipeline_pids = 0;
            return -1;
        }

        if (pid == 0) {
            /* Child process i */
            if (first_pgid == 0) {
                setpgid(0, 0);
            } else {
                setpgid(0, first_pgid);
            }

            if (tab->cwd[0] != '\0') {
                if (chdir(tab->cwd) != 0) {
                    /* Directory might have been removed, continue anyway */
                }
            }

            /* 1. STDIN setup */
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                    perror("terminator: dup2 prev pipe to stdin");
                    _exit(1);
                }
            }

            /* Explicit input redirection overrides previous pipe if specified */
            if (cmd->input_file) {
                int in_fd = open(cmd->input_file, O_RDONLY);
                if (in_fd < 0) {
                    fprintf(stderr, "terminator: %s: %s\n", cmd->input_file, strerror(errno));
                    _exit(1);
                }
                if (dup2(in_fd, STDIN_FILENO) < 0) {
                    perror("terminator: dup2 file to stdin");
                    _exit(1);
                }
                close(in_fd);
            }

            /* 2. STDOUT setup */
            if (i < n - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                    perror("terminator: dup2 next pipe to stdout");
                    _exit(1);
                }
            } else {
                /* Last command in pipeline */
                if (cmd->output_file) {
                    int flags = O_WRONLY | O_CREAT | (cmd->append_output ? O_APPEND : O_TRUNC);
                    int out_fd = open(cmd->output_file, flags, 0644);
                    if (out_fd < 0) {
                        fprintf(stderr, "terminator: %s: %s\n", cmd->output_file, strerror(errno));
                        _exit(1);
                    }
                    if (dup2(out_fd, STDOUT_FILENO) < 0) {
                        perror("terminator: dup2 file to stdout");
                        _exit(1);
                    }
                    close(out_fd);
                } else {
                    /* Connect stdout to tab capture pipe */
                    if (dup2(capture_pipe[1], STDOUT_FILENO) < 0) {
                        perror("terminator: dup2 capture to stdout");
                        _exit(1);
                    }
                }
            }

            /* 3. STDERR setup: send to tab capture pipe */
            if (dup2(capture_pipe[1], STDERR_FILENO) < 0) {
                perror("terminator: dup2 capture to stderr");
                _exit(1);
            }

            /* 4. Close all unused descriptors in child */
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);

            /* 5. Execute command */
            if (cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) {
                _exit(0);
            }

            char script_path[MAX_PATH_LEN * 2];
            bool is_source = false;
            if (script_is_script_invocation(cmd, tab->cwd, script_path, sizeof(script_path), &is_source)) {
                _exit(script_execute_file(tab, script_path, cmd->argc, cmd->argv, is_source, -1, -1));
            }

            if (builtin_is_builtin(cmd->argv[0])) {
                _exit(builtin_execute(cmd, tab));
            }

            execvp(cmd->argv[0], cmd->argv);

            if (errno == ENOENT) {
                fprintf(stderr, "terminator: %s: command not found\n", cmd->argv[0]);
                _exit(127);
            } else {
                fprintf(stderr, "terminator: %s: %s\n", cmd->argv[0], strerror(errno));
                _exit(126);
            }
        }

        /* Parent process: track child PID and set process group */
        if (i == 0) {
            first_pgid = pid;
            setpgid(pid, first_pgid);
            tab->foreground_pgid = first_pgid;
        } else {
            setpgid(pid, first_pgid);
        }

        tab->pipeline_pids[i] = pid;
        tab->num_pipeline_pids++;
    }

    /* Parent closes all pipeline pipes */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Parent closes write end of capture pipe so EOF is generated when all children exit */
    close(capture_pipe[1]);

    tab->out_pipe_fd = capture_pipe[0];
    tab->running_pid = tab->pipeline_pids[n - 1];

    return 0;
}
