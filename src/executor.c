#include "executor.h"
#include "pipeline.h"
#include "builtins.h"
#include "redirection.h"
#include "util.h"
#include "ipc.h"
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

void executor_reap_children(Tab *tab) {
    if (!tab || tab->num_pipeline_pids == 0) return;

    for (int i = 0; i < tab->num_pipeline_pids; i++) {
        pid_t pid = tab->pipeline_pids[i];
        if (pid > 0) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (i == tab->num_pipeline_pids - 1) {
                if (WIFEXITED(status)) {
                    tab->last_exit_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    tab->last_exit_status = 128 + WTERMSIG(status);
                } else {
                    tab->last_exit_status = 1;
                }
            }
        }
    }

    tab->num_pipeline_pids = 0;
    tab->running_pid = -1;
    tab->foreground_pgid = 0;
}

static bool is_forbidden_command(const char *cmd) {
    if (!cmd) return false;
    const char *base = strrchr(cmd, '/');
    base = base ? base + 1 : cmd;
    return (strcmp(base, "nano") == 0);
}

int executor_run_line(Tab *tab, const char *cmd_line) {
    if (!tab || !cmd_line) return 0;

    /* Skip blank lines */
    const char *p = cmd_line;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    if (!*p) {
        return 0;
    }

    /* Record command in history */
    terminal_history_add(tab, cmd_line);

    /* Parse pipeline */
    Pipeline *pipeline = parser_parse_pipeline(cmd_line, tab->last_exit_status);
    if (!pipeline) {
        tab->last_exit_status = 1;
        return -1;
    }
    if (pipeline->num_commands == 0) {
        pipeline_free(pipeline);
        return 0;
    }

    /* Check for forbidden command (nano) */
    for (int i = 0; i < pipeline->num_commands; i++) {
        if (pipeline->commands[i].argc > 0 && is_forbidden_command(pipeline->commands[i].argv[0])) {
            const char *msg = "terminator: nano: command forbidden\n";
            terminal_append_output_bytes(tab, msg, strlen(msg), true);
            tab->last_exit_status = 1;
            pipeline_free(pipeline);
            return -1;
        }
    }

    /* Check for single built-in command */
    if (pipeline->num_commands == 1 &&
        pipeline->commands[0].argc > 0 &&
        builtin_is_builtin(pipeline->commands[0].argv[0])) {

        Command *cmd = &pipeline->commands[0];

        /* If output redirection is specified, apply file redirection */
        int saved_stdin = -1;
        int saved_stdout = -1;
        int saved_stderr = -1;

        int capture_pipe[2] = {-1, -1};

        if (cmd->output_file) {
            if (redirection_apply_in_parent(cmd, &saved_stdin, &saved_stdout) < 0) {
                tab->last_exit_status = 1;
                pipeline_free(pipeline);
                return -1;
            }
        } else {
            /* Capture stdout/stderr via pipe to display in terminal tab */
            if (pipe(capture_pipe) == 0) {
                saved_stdout = dup(STDOUT_FILENO);
                saved_stderr = dup(STDERR_FILENO);
                dup2(capture_pipe[1], STDOUT_FILENO);
                dup2(capture_pipe[1], STDERR_FILENO);
                close(capture_pipe[1]);
            }
        }

        /* Execute the built-in directly in this shell process */
        tab->last_exit_status = builtin_execute(cmd, tab);

        fflush(stdout);
        fflush(stderr);

        if (cmd->output_file) {
            redirection_restore_in_parent(saved_stdin, saved_stdout);
        } else if (capture_pipe[0] >= 0) {
            /* Restore stdout/stderr */
            if (saved_stdout >= 0) {
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);
            }
            if (saved_stderr >= 0) {
                dup2(saved_stderr, STDERR_FILENO);
                close(saved_stderr);
            }

            /* Read captured built-in output and append to tab */
            char buf[4096];
            ssize_t n;
            while ((n = read(capture_pipe[0], buf, sizeof(buf))) > 0) {
                terminal_append_output_bytes(tab, buf, (size_t)n, false);
            }
            close(capture_pipe[0]);
        }

        pipeline_free(pipeline);
        return 0;
    }

    /* External command or pipeline: execute asynchronously */
    int ret = pipeline_execute(pipeline, tab);
    pipeline_free(pipeline);

    if (ret == 0 && tab->out_pipe_fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(tab->out_pipe_fd, buf, sizeof(buf))) > 0) {
            terminal_append_output_bytes(tab, buf, (size_t)n, false);
        }
        close(tab->out_pipe_fd);
        tab->out_pipe_fd = -1;
        executor_reap_children(tab);
    }
    return ret;
}

int executor_run_line_worker(Tab *tab, const char *cmd_line, int read_ipc_fd, int write_ipc_fd) {
    if (!tab || !cmd_line) return 0;

    /* Skip blank lines */
    const char *p = cmd_line;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    if (!*p) {
        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return 0;
    }

    /* Record command in shared history */
    terminal_history_add(tab, cmd_line);

    /* Parse pipeline */
    Pipeline *pipeline = parser_parse_pipeline(cmd_line, tab->last_exit_status);
    if (!pipeline) {
        tab->last_exit_status = 1;
        int status = 1;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return -1;
    }
    if (pipeline->num_commands == 0) {
        pipeline_free(pipeline);
        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return 0;
    }

    /* Check for forbidden command (nano) */
    for (int i = 0; i < pipeline->num_commands; i++) {
        if (pipeline->commands[i].argc > 0 && is_forbidden_command(pipeline->commands[i].argv[0])) {
            const char *msg = "terminator: nano: command forbidden\n";
            ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, msg, (uint32_t)strlen(msg));
            tab->last_exit_status = 1;
            int status = 1;
            ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
            pipeline_free(pipeline);
            return -1;
        }
    }

    /* Check for single built-in command */
    if (pipeline->num_commands == 1 &&
        pipeline->commands[0].argc > 0 &&
        builtin_is_builtin(pipeline->commands[0].argv[0])) {

        Command *cmd = &pipeline->commands[0];

        int saved_stdin = -1;
        int saved_stdout = -1;
        int saved_stderr = -1;
        int capture_pipe[2] = {-1, -1};

        if (cmd->output_file) {
            if (redirection_apply_in_parent(cmd, &saved_stdin, &saved_stdout) < 0) {
                tab->last_exit_status = 1;
                pipeline_free(pipeline);
                int status = 1;
                ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
                return -1;
            }
        } else {
            if (pipe(capture_pipe) == 0) {
                saved_stdout = dup(STDOUT_FILENO);
                saved_stderr = dup(STDERR_FILENO);
                dup2(capture_pipe[1], STDOUT_FILENO);
                dup2(capture_pipe[1], STDERR_FILENO);
                close(capture_pipe[1]);
            }
        }

        tab->last_exit_status = builtin_execute(cmd, tab);

        fflush(stdout);
        fflush(stderr);

        if (cmd->output_file) {
            redirection_restore_in_parent(saved_stdin, saved_stdout);
        } else if (capture_pipe[0] >= 0) {
            if (saved_stdout >= 0) {
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);
            }
            if (saved_stderr >= 0) {
                dup2(saved_stderr, STDERR_FILENO);
                close(saved_stderr);
            }

            char buf[4096];
            ssize_t n;
            while ((n = read(capture_pipe[0], buf, sizeof(buf))) > 0) {
                ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, buf, (uint32_t)n);
            }
            close(capture_pipe[0]);
        }

        if (strcmp(cmd->argv[0], "cd") == 0) {
            ipc_send_msg(write_ipc_fd, IPC_MSG_CWD, tab->cwd, (uint32_t)strlen(tab->cwd));
        } else if (strcmp(cmd->argv[0], "clear") == 0) {
            ipc_send_msg(write_ipc_fd, IPC_MSG_CLEAR, NULL, 0);
        }

        if (tab->request_close) {
            ipc_send_msg(write_ipc_fd, IPC_MSG_EXIT, NULL, 0);
        }

        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));

        pipeline_free(pipeline);
        return 0;
    }

    /* External command or pipeline */
    int ret = pipeline_execute(pipeline, tab);
    pipeline_free(pipeline);

    if (ret != 0) {
        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return ret;
    }

    /* Multiplex child output and IPC commands from GUI */
    while (1) {
        struct pollfd pfds[2];
        int nfds = 0;

        /* pfds[0]: messages from GUI (input, Ctrl+C, quit, winch) */
        if (read_ipc_fd >= 0) {
            pfds[nfds].fd = read_ipc_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        /* pfds[1]: output from child process / PTY master */
        int out_pfd_idx = -1;
        if (tab->out_pipe_fd >= 0) {
            out_pfd_idx = nfds;
            pfds[nfds].fd = tab->out_pipe_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        /* Poll with 20ms timeout so we can check child process status */
        int poll_ret = poll(pfds, nfds, 20);

        if (poll_ret > 0) {
            /* 1. Check messages from GUI */
            if (read_ipc_fd >= 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                IPCHeader hdr;
                if (!ipc_recv_header(read_ipc_fd, &hdr)) {
                    /* GUI pipe closed: terminate child and exit */
                    if (tab->foreground_pgid > 0) {
                        kill(-tab->foreground_pgid, SIGTERM);
                        kill(-tab->foreground_pgid, SIGKILL);
                    }
                    goto worker_done;
                }

                while (1) {
                    if (hdr.type == IPC_MSG_INT) {
                        /* Ctrl+C from user: deliver SIGINT to foreground process group */
                        if (tab->foreground_pgid > 0) {
                            kill(-tab->foreground_pgid, SIGINT);
                        } else if (tab->running_pid > 0) {
                            kill(-tab->running_pid, SIGINT);
                            kill(tab->running_pid, SIGINT);
                        }
                        for (int i = 0; i < tab->num_pipeline_pids; i++) {
                            if (tab->pipeline_pids[i] > 0) {
                                kill(tab->pipeline_pids[i], SIGINT);
                            }
                        }
                    } else if (hdr.type == IPC_MSG_QUIT) {
                        /* Application or tab terminating: kill child immediately */
                        if (tab->foreground_pgid > 0) {
                            kill(-tab->foreground_pgid, SIGTERM);
                            kill(-tab->foreground_pgid, SIGKILL);
                        }
                        for (int i = 0; i < tab->num_pipeline_pids; i++) {
                            if (tab->pipeline_pids[i] > 0) {
                                kill(tab->pipeline_pids[i], SIGKILL);
                            }
                        }
                        goto worker_done;
                    } else if (hdr.length > 0) {
                        /* Drain any unhandled payload bytes */
                        char dummy[256];
                        uint32_t to_read = hdr.length;
                        while (to_read > 0) {
                            uint32_t chunk = to_read > sizeof(dummy) ? (uint32_t)sizeof(dummy) : to_read;
                            if (!ipc_recv_payload(read_ipc_fd, dummy, chunk)) break;
                            to_read -= chunk;
                        }
                    }

                    /* Check if more IPC messages are ready without blocking */
                    struct pollfd check_pfd;
                    check_pfd.fd = read_ipc_fd;
                    check_pfd.events = POLLIN;
                    check_pfd.revents = 0;
                    if (poll(&check_pfd, 1, 0) <= 0 || !(check_pfd.revents & POLLIN)) {
                        break;
                    }
                    if (!ipc_recv_header(read_ipc_fd, &hdr)) {
                        break;
                    }
                }
            }

            /* 2. Check output from child */
            if (out_pfd_idx >= 0 && (pfds[out_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                char buf[4096];
                ssize_t n = read(tab->out_pipe_fd, buf, sizeof(buf));
                if (n > 0) {
                    ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, buf, (uint32_t)n);
                } else {
                    /* EOF or error */
                    close(tab->out_pipe_fd);
                    tab->out_pipe_fd = -1;
                }
            }
        }

        /* Check if child processes have finished */
        bool any_alive = false;
        for (int i = 0; i < tab->num_pipeline_pids; i++) {
            pid_t pid = tab->pipeline_pids[i];
            if (pid > 0) {
                int status = 0;
                pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid) {
                    if (i == tab->num_pipeline_pids - 1) {
                        if (WIFEXITED(status)) {
                            tab->last_exit_status = WEXITSTATUS(status);
                        } else if (WIFSIGNALED(status)) {
                            tab->last_exit_status = 128 + WTERMSIG(status);
                        } else {
                            tab->last_exit_status = 1;
                        }
                    }
                    tab->pipeline_pids[i] = 0;
                } else if (r == -1 && errno == ECHILD) {
                    tab->pipeline_pids[i] = 0;
                } else {
                    any_alive = true;
                }
            }
        }

        if (!any_alive) {
            /* If pipe is still open, drain any remaining output before finishing */
            if (tab->out_pipe_fd >= 0) {
                char buf[4096];
                ssize_t n;
                while ((n = read(tab->out_pipe_fd, buf, sizeof(buf))) > 0) {
                    ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, buf, (uint32_t)n);
                }
                close(tab->out_pipe_fd);
                tab->out_pipe_fd = -1;
            }
            break;
        }
    }

worker_done:
    executor_reap_children(tab);

    int status = tab->last_exit_status;
    ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
    return ret;
}

char *executor_execute_substitution_sync(const char *sub_cmd, int last_exit_status) {
    if (!sub_cmd) return xstrdup("");

    /* Parse inner command */
    Pipeline *pipeline = parser_parse_pipeline(sub_cmd, last_exit_status);
    if (!pipeline || pipeline->num_commands == 0) {
        pipeline_free(pipeline);
        return xstrdup("");
    }

    int sub_pipe[2];
    if (pipe(sub_pipe) < 0) {
        perror("terminator: substitution pipe");
        pipeline_free(pipeline);
        return xstrdup("");
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("terminator: substitution fork");
        close(sub_pipe[0]);
        close(sub_pipe[1]);
        pipeline_free(pipeline);
        return xstrdup("");
    }

    if (pid == 0) {
        /* Child subshell */
        dup2(sub_pipe[1], STDOUT_FILENO);
        close(sub_pipe[0]);
        close(sub_pipe[1]);

        /* If single built-in */
        if (pipeline->num_commands == 1 &&
            pipeline->commands[0].argc > 0 &&
            builtin_is_builtin(pipeline->commands[0].argv[0])) {

            Tab temp_tab;
            memset(&temp_tab, 0, sizeof(temp_tab));
            if (!getcwd(temp_tab.cwd, sizeof(temp_tab.cwd))) {
                safe_strncpy(temp_tab.cwd, "/tmp", sizeof(temp_tab.cwd));
            }
            int status = builtin_execute(&pipeline->commands[0], &temp_tab);
            fflush(stdout);
            _exit(status);
        }

        /* For pipeline: execute */
        Tab sub_tab;
        memset(&sub_tab, 0, sizeof(sub_tab));
        pipeline_execute(pipeline, &sub_tab);

        /* In subshell child, pump pipeline output into our stdout */
        if (sub_tab.out_pipe_fd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(sub_tab.out_pipe_fd, buf, sizeof(buf))) > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(STDOUT_FILENO, buf + written, n - written);
                    if (w <= 0) break;
                    written += w;
                }
            }
            close(sub_tab.out_pipe_fd);
        }
        executor_reap_children(&sub_tab);
        _exit(sub_tab.last_exit_status);
    }

    /* Parent: read from sub_pipe[0] until EOF */
    close(sub_pipe[1]);

    size_t cap = 256;
    size_t total = 0;
    char *result = xmalloc(cap);

    char buf[1024];
    ssize_t n;
    while ((n = read(sub_pipe[0], buf, sizeof(buf))) > 0) {
        while (total + (size_t)n + 1 >= cap) {
            cap *= 2;
            result = xrealloc(result, cap);
        }
        memcpy(result + total, buf, n);
        total += n;
    }
    result[total] = '\0';

    close(sub_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }

    pipeline_free(pipeline);

    /* Strip trailing newline(s) as per shell command substitution standard */
    str_trim_trailing_newlines(result);
    return result;
}
