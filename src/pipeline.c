#include "pipeline.h"
#include "redirection.h"
#include "util.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

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

            execvp(cmd->argv[0], cmd->argv);

            fprintf(stderr, "terminator: %s: command not found\n", cmd->argv[0]);
            _exit(127);
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
