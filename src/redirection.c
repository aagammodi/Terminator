#include "redirection.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int redirection_apply_in_child(const Command *cmd) {
    if (!cmd) return 0;

    /* Handle input redirection < file */
    if (cmd->input_file) {
        int in_fd = open(cmd->input_file, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", cmd->input_file, strerror(errno));
            return -1;
        }
        if (dup2(in_fd, STDIN_FILENO) < 0) {
            perror("terminator: dup2 stdin");
            close(in_fd);
            return -1;
        }
        close(in_fd);
    }

    /* Handle output redirection > file or >> file */
    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->append_output ? O_APPEND : O_TRUNC);
        int out_fd = open(cmd->output_file, flags, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", cmd->output_file, strerror(errno));
            return -1;
        }
        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            perror("terminator: dup2 stdout");
            close(out_fd);
            return -1;
        }
        close(out_fd);
    }

    return 0;
}

int redirection_apply_in_parent(const Command *cmd, int *saved_stdin, int *saved_stdout) {
    if (!cmd || !saved_stdin || !saved_stdout) return 0;

    *saved_stdin = -1;
    *saved_stdout = -1;

    if (cmd->input_file) {
        int in_fd = open(cmd->input_file, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", cmd->input_file, strerror(errno));
            return -1;
        }
        *saved_stdin = dup(STDIN_FILENO);
        if (*saved_stdin < 0 || dup2(in_fd, STDIN_FILENO) < 0) {
            perror("terminator: dup2 stdin parent");
            close(in_fd);
            if (*saved_stdin >= 0) {
                close(*saved_stdin);
                *saved_stdin = -1;
            }
            return -1;
        }
        close(in_fd);
    }

    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->append_output ? O_APPEND : O_TRUNC);
        int out_fd = open(cmd->output_file, flags, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", cmd->output_file, strerror(errno));
            if (*saved_stdin >= 0) {
                dup2(*saved_stdin, STDIN_FILENO);
                close(*saved_stdin);
                *saved_stdin = -1;
            }
            return -1;
        }
        *saved_stdout = dup(STDOUT_FILENO);
        if (*saved_stdout < 0 || dup2(out_fd, STDOUT_FILENO) < 0) {
            perror("terminator: dup2 stdout parent");
            close(out_fd);
            if (*saved_stdout >= 0) {
                close(*saved_stdout);
                *saved_stdout = -1;
            }
            if (*saved_stdin >= 0) {
                dup2(*saved_stdin, STDIN_FILENO);
                close(*saved_stdin);
                *saved_stdin = -1;
            }
            return -1;
        }
        close(out_fd);
    }

    return 0;
}

void redirection_restore_in_parent(int saved_stdin, int saved_stdout) {
    if (saved_stdin >= 0) {
        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);
    }
    if (saved_stdout >= 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
}
