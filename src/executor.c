#include "executor.h"
#include "pipeline.h"
#include "builtins.h"
#include "redirection.h"
#include "util.h"
#include "ipc.h"
#include "multiwatch.h"
#include "script.h"
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
            while (waitpid(pid, &status, WUNTRACED) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (i == tab->num_pipeline_pids - 1) {
                if (WIFEXITED(status)) {
                    tab->last_exit_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    tab->last_exit_status = 128 + WTERMSIG(status);
                } else if (WIFSTOPPED(status)) {
                    int stopsig = WSTOPSIG(status);
                    if (stopsig == SIGTTOU || stopsig == SIGTTIN) {
                        kill(-pid, SIGKILL);
                        waitpid(pid, NULL, 0);
                        tab->last_exit_status = 1;
                    } else {
                        tab->last_exit_status = 128 + stopsig;
                    }
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

static const char *get_external_terminal_emulator(void) {
    static const char *candidates[] = {
        "/usr/bin/x-terminal-emulator",
        "/usr/bin/gnome-terminal",
        "/usr/bin/xterm",
        "/usr/bin/xfce4-terminal",
        "/usr/bin/konsole",
        "/usr/bin/alacritty",
        "/usr/bin/kitty"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) {
            return candidates[i];
        }
    }
    return NULL;
}

static bool is_interactive_apt(const Command *cmd, int start_idx) {
    if (!cmd || start_idx >= cmd->argc) return false;
    for (int i = start_idx + 1; i < cmd->argc; i++) {
        const char *arg = cmd->argv[i];
        if (arg[0] == '-') continue; /* skip flags */
        if (strcmp(arg, "install") == 0 ||
            strcmp(arg, "remove") == 0 ||
            strcmp(arg, "purge") == 0 ||
            strcmp(arg, "upgrade") == 0 ||
            strcmp(arg, "dist-upgrade") == 0 ||
            strcmp(arg, "full-upgrade") == 0 ||
            strcmp(arg, "autoremove") == 0 ||
            strcmp(arg, "reinstall") == 0) {
            return true;
        }
        return false;
    }
    return false;
}

static bool command_needs_pause_on_exit(const Command *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) return false;
    const char *base = strrchr(cmd->argv[0], '/');
    base = base ? base + 1 : cmd->argv[0];

    int target_idx = 0;
    if (strcmp(base, "sudo") == 0) {
        for (int i = 1; i < cmd->argc; i++) {
            if (cmd->argv[i][0] != '-') {
                target_idx = i;
                break;
            }
        }
        if (target_idx > 0 && target_idx < cmd->argc) {
            base = strrchr(cmd->argv[target_idx], '/');
            base = base ? base + 1 : cmd->argv[target_idx];
        }
    }

    if (strcmp(base, "apt") == 0 ||
        strcmp(base, "apt-get") == 0 ||
        strcmp(base, "dpkg") == 0 ||
        strcmp(base, "dpkg-reconfigure") == 0 ||
        strcmp(base, "aptitude") == 0) {
        return true;
    }
    return false;
}

static bool has_help_or_version_flag(const Command *cmd, int start_idx) {
    if (!cmd) return false;
    for (int i = start_idx; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], "--help") == 0 ||
            strcmp(cmd->argv[i], "-h") == 0 ||
            strcmp(cmd->argv[i], "--version") == 0 ||
            strcmp(cmd->argv[i], "-V") == 0) {
            return true;
        }
    }
    return false;
}

static bool is_tui_command(const Command *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) return false;
    const char *base = strrchr(cmd->argv[0], '/');
    base = base ? base + 1 : cmd->argv[0];

    int target_idx = 0;
    if (strcmp(base, "sudo") == 0) {
        /* Check if sudo is invoking an interactive shell (-i, -s) */
        for (int i = 1; i < cmd->argc; i++) {
            if (strcmp(cmd->argv[i], "-i") == 0 || strcmp(cmd->argv[i], "-s") == 0) {
                return true;
            }
            if (cmd->argv[i][0] != '-') {
                target_idx = i;
                break;
            }
        }
        if (target_idx == 0 || target_idx >= cmd->argc) {
            return false;
        }
        base = strrchr(cmd->argv[target_idx], '/');
        base = base ? base + 1 : cmd->argv[target_idx];
    }

    /* Help and version queries are non-interactive stdout printers (except forbidden nano) */
    if (strcmp(base, "nano") != 0 && has_help_or_version_flag(cmd, target_idx + 1)) {
        return false;
    }

    if (strcmp(base, "su") == 0) {
        return true;
    }

    /* Interactive package managers */
    if (strcmp(base, "apt") == 0 ||
        strcmp(base, "apt-get") == 0 ||
        strcmp(base, "aptitude") == 0) {
        if (is_interactive_apt(cmd, target_idx)) {
            return true;
        }
    }
    if (strcmp(base, "dpkg") == 0 ||
        strcmp(base, "dpkg-reconfigure") == 0) {
        return true;
    }

    /* Pagers & manual pages */
    if (strcmp(base, "less") == 0 ||
        strcmp(base, "more") == 0 ||
        strcmp(base, "man") == 0 ||
        strcmp(base, "info") == 0 ||
        strcmp(base, "view") == 0) {
        return true;
    }

    /* If 'top' is called with batch mode flag (-b), it's a streaming command */
    if (strcmp(base, "top") == 0) {
        for (int i = (target_idx > 0 ? target_idx + 1 : 1); i < cmd->argc; i++) {
            if (strcmp(cmd->argv[i], "-b") == 0 || strncmp(cmd->argv[i], "-b", 2) == 0) {
                return false; /* top in batch mode streams to stdout */
            }
        }
        return true;
    }

    if (strcmp(base, "htop") == 0 ||
        strcmp(base, "nano") == 0 ||
        strcmp(base, "vim") == 0 ||
        strcmp(base, "vi") == 0 ||
        strcmp(base, "emacs") == 0) {
        return true;
    }

    return false;
}

static void command_wrap_external_terminal(Command *cmd, const char *term_bin) {
    if (!cmd || !term_bin) return;
    int old_argc = cmd->argc;
    char **old_argv = cmd->argv;

    bool is_gnome_direct = (strstr(term_bin, "gnome-terminal") != NULL &&
                            strstr(term_bin, "wrapper") == NULL &&
                            strstr(term_bin, "emulator") == NULL);

    bool needs_pause = command_needs_pause_on_exit(cmd);

    /*
     * If pause is needed on exit (e.g. apt purge / install), wrap in:
     *   sh -c '"$@"; status=$?; printf "\n[Process completed - press Enter to close] "; read -r _; exit $status' terminator_runner <args...>
     */
    int pause_args = needs_pause ? 4 : 0;
    int emulator_args = is_gnome_direct ? 3 : 2; /* gnome-terminal --wait -- vs term -e */

    int new_argc = emulator_args + pause_args + old_argc;
    int new_cap = new_argc + 4;
    char **new_argv = xcalloc((size_t)new_cap, sizeof(char *));

    int idx = 0;
    new_argv[idx++] = xstrdup(term_bin);
    if (is_gnome_direct) {
        new_argv[idx++] = xstrdup("--wait");
        new_argv[idx++] = xstrdup("--");
    } else {
        new_argv[idx++] = xstrdup("-e");
    }

    if (needs_pause) {
        new_argv[idx++] = xstrdup("sh");
        new_argv[idx++] = xstrdup("-c");
        new_argv[idx++] = xstrdup("\"$@\"; status=$?; printf \"\\n[Process completed - press Enter to close] \"; read -r _; exit $status");
        new_argv[idx++] = xstrdup("terminator_runner");
    }

    for (int i = 0; i < old_argc; i++) {
        new_argv[idx++] = old_argv[i];
    }
    new_argv[new_argc] = NULL;

    free(old_argv);
    cmd->argv = new_argv;
    cmd->argc = new_argc;
    cmd->argv_capacity = new_cap;
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

    int ret = 0;

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

    /* Check for full-screen interactive TUI commands (top, htop, nano, vim, vi) */
    if (pipeline->num_commands == 1 && pipeline->commands[0].argc > 0 &&
        is_tui_command(&pipeline->commands[0])) {
        const char *term_bin = get_external_terminal_emulator();
        bool is_headless = (tab->echo_to_stdout || getenv("TERMINATOR_HEADLESS") != NULL || getenv("DISPLAY") == NULL);
        bool can_spawn_gui = (term_bin != NULL && !is_headless);

        if (can_spawn_gui) {
            char note[256];
            snprintf(note, sizeof(note), "[Terminator: Running '%s' in dedicated terminal window]\n",
                     pipeline->commands[0].argv[0]);
            terminal_append_output_bytes(tab, note, strlen(note), false);
            command_wrap_external_terminal(&pipeline->commands[0], term_bin);
            goto execute_external_pipeline;
        } else {
            /* Headless / test / no-display fallback */
            const char *cmd_name = pipeline->commands[0].argv[0];
            const char *base = strrchr(cmd_name, '/');
            base = base ? base + 1 : cmd_name;
            char err_msg[256];
            if (strcmp(base, "nano") == 0) {
                snprintf(err_msg, sizeof(err_msg), "terminator: nano: command forbidden\n");
            } else if (strcmp(base, "top") == 0 || strcmp(base, "htop") == 0) {
                snprintf(err_msg, sizeof(err_msg), "terminator: %s: interactive TUI requires graphical display (use '%s -b -n 1' for batch output)\n", base, base);
            } else {
                snprintf(err_msg, sizeof(err_msg), "terminator: %s: interactive TUI command requires graphical display\n", base);
            }
            terminal_append_output_bytes(tab, err_msg, strlen(err_msg), true);
            tab->last_exit_status = 1;
            pipeline_free(pipeline);
            return -1;
        }
    }

    /* Check for script invocation (./script.sh, bash script.sh, source script.sh) */
    char script_path[MAX_PATH_LEN * 2];
    bool is_source = false;
    if (pipeline->num_commands == 1 && pipeline->commands[0].argc > 0 &&
        script_is_script_invocation(&pipeline->commands[0], tab->cwd, script_path, sizeof(script_path), &is_source)) {
        Command *cmd = &pipeline->commands[0];
        int saved_stdin = -1, saved_stdout = -1;
        if (cmd->output_file) {
            redirection_apply_in_parent(cmd, &saved_stdin, &saved_stdout);
        }
        ret = script_execute_file(tab, script_path, cmd->argc, cmd->argv, is_source, -1, -1);
        if (cmd->output_file) {
            redirection_restore_in_parent(saved_stdin, saved_stdout);
        }
        tab->last_exit_status = ret;
        pipeline_free(pipeline);
        return ret;
    }

    /* Check for single built-in command */
    if (pipeline->num_commands == 1 &&
        pipeline->commands[0].argc > 0 &&
        builtin_is_builtin(pipeline->commands[0].argv[0])) {

        if (strcmp(pipeline->commands[0].argv[0], "multiWatch") == 0) {
            int ret = multiwatch_run(tab, cmd_line, -1, -1);
            pipeline_free(pipeline);
            return ret;
        }

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
                return 1;
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

        if (strcmp(cmd->argv[0], "fg") == 0 && tab->last_exit_status == 0 && tab->foreground_pgid > 0) {
            pipeline_free(pipeline);
            ret = 0;
            goto sync_monitor_job;
        }

        pipeline_free(pipeline);
        return tab->last_exit_status;
    }

execute_external_pipeline:;
    /* External command or pipeline: execute */
    bool is_bg = pipeline->is_background;
    ret = pipeline_execute(pipeline, tab);
    pipeline_free(pipeline);

    if (is_bg) {
        Job *j = jobs_add(&tab->jobs, tab->foreground_pgid, tab->pipeline_pids, tab->num_pipeline_pids, cmd_line, JOB_STATE_RUNNING, false, tab->out_pipe_fd);
        char bg_msg[128];
        snprintf(bg_msg, sizeof(bg_msg), "[%d] %d\n", j ? j->id : 1, (int)tab->foreground_pgid);
        terminal_append_output_bytes(tab, bg_msg, strlen(bg_msg), false);
        tab->foreground_pgid = -1;
        tab->out_pipe_fd = -1;
        tab->num_pipeline_pids = 0;
        return 0;
    }

sync_monitor_job:
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
    if (ret == 0) {
        ret = tab->last_exit_status;
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

    int ret = 0;

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

    /* Check for full-screen interactive TUI commands (top, htop, nano, vim, vi) */
    if (pipeline->num_commands == 1 && pipeline->commands[0].argc > 0 &&
        is_tui_command(&pipeline->commands[0])) {
        const char *term_bin = get_external_terminal_emulator();
        bool is_headless = (tab->echo_to_stdout || getenv("TERMINATOR_HEADLESS") != NULL || getenv("DISPLAY") == NULL);
        bool can_spawn_gui = (term_bin != NULL && !is_headless);

        if (can_spawn_gui) {
            char note[256];
            snprintf(note, sizeof(note), "[Terminator: Running '%s' in dedicated terminal window]\n",
                     pipeline->commands[0].argv[0]);
            ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, note, (uint32_t)strlen(note));
            command_wrap_external_terminal(&pipeline->commands[0], term_bin);
            goto execute_external_worker_pipeline;
        } else {
            /* Headless / test / no-display fallback */
            const char *cmd_name = pipeline->commands[0].argv[0];
            const char *base = strrchr(cmd_name, '/');
            base = base ? base + 1 : cmd_name;
            char err_msg[256];
            if (strcmp(base, "nano") == 0) {
                snprintf(err_msg, sizeof(err_msg), "terminator: nano: command forbidden\n");
            } else if (strcmp(base, "top") == 0 || strcmp(base, "htop") == 0) {
                snprintf(err_msg, sizeof(err_msg), "terminator: %s: interactive TUI requires graphical display (use '%s -b -n 1' for batch output)\n", base, base);
            } else {
                snprintf(err_msg, sizeof(err_msg), "terminator: %s: interactive TUI command requires graphical display\n", base);
            }
            ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, err_msg, (uint32_t)strlen(err_msg));
            tab->last_exit_status = 1;
            pipeline_free(pipeline);
            int status = 1;
            ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
            return -1;
        }
    }

    /* Check for script invocation (./script.sh, bash script.sh, source script.sh) */
    char script_path[MAX_PATH_LEN * 2];
    bool is_source = false;
    if (pipeline->num_commands == 1 && pipeline->commands[0].argc > 0 &&
        script_is_script_invocation(&pipeline->commands[0], tab->cwd, script_path, sizeof(script_path), &is_source)) {
        Command *cmd = &pipeline->commands[0];
        int saved_stdin = -1, saved_stdout = -1;
        if (cmd->output_file) {
            redirection_apply_in_parent(cmd, &saved_stdin, &saved_stdout);
        }
        ret = script_execute_file(tab, script_path, cmd->argc, cmd->argv, is_source, read_ipc_fd, write_ipc_fd);
        if (cmd->output_file) {
            redirection_restore_in_parent(saved_stdin, saved_stdout);
        }
        tab->last_exit_status = ret;
        pipeline_free(pipeline);
        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return ret;
    }

    /* Check for single built-in command */
    if (pipeline->num_commands == 1 &&
        pipeline->commands[0].argc > 0 &&
        builtin_is_builtin(pipeline->commands[0].argv[0])) {

        if (strcmp(pipeline->commands[0].argv[0], "multiWatch") == 0) {
            int ret = multiwatch_run(tab, cmd_line, read_ipc_fd, write_ipc_fd);
            pipeline_free(pipeline);
            return ret;
        }

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

        if (strcmp(cmd->argv[0], "fg") == 0 && tab->last_exit_status == 0 && tab->foreground_pgid > 0) {
            pipeline_free(pipeline);
            goto worker_monitor_job;
        }

        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));

        pipeline_free(pipeline);
        return status;
    }

execute_external_worker_pipeline:;
    /* External command or pipeline */
    bool is_bg = pipeline->is_background;
    ret = pipeline_execute(pipeline, tab);
    pipeline_free(pipeline);

    if (ret != 0) {
        int status = tab->last_exit_status;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return ret;
    }

    if (is_bg) {
        Job *j = jobs_add(&tab->jobs, tab->foreground_pgid, tab->pipeline_pids, tab->num_pipeline_pids, cmd_line, JOB_STATE_RUNNING, false, tab->out_pipe_fd);
        char bg_msg[128];
        snprintf(bg_msg, sizeof(bg_msg), "[%d] %d\n", j ? j->id : 1, (int)tab->foreground_pgid);
        ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, bg_msg, (uint32_t)strlen(bg_msg));

        tab->foreground_pgid = -1;
        tab->out_pipe_fd = -1;
        tab->num_pipeline_pids = 0;

        int status = 0;
        ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &status, sizeof(status));
        return 0;
    }

worker_monitor_job:
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
                    } else if (hdr.type == IPC_MSG_TSTP) {
                        /* Ctrl+Z from user: deliver SIGTSTP to foreground process group */
                        if (tab->foreground_pgid > 0) {
                            kill(-tab->foreground_pgid, SIGTSTP);
                        } else if (tab->running_pid > 0) {
                            kill(-tab->running_pid, SIGTSTP);
                        }
                        for (int i = 0; i < tab->num_pipeline_pids; i++) {
                            if (tab->pipeline_pids[i] > 0) {
                                kill(tab->pipeline_pids[i], SIGTSTP);
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

        /* Check if child processes have finished or stopped */
        bool any_alive = false;
        bool any_stopped = false;
        for (int i = 0; i < tab->num_pipeline_pids; i++) {
            pid_t pid = tab->pipeline_pids[i];
            if (pid > 0) {
                int status = 0;
                pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
                if (r == pid) {
                    if (WIFSTOPPED(status)) {
                        int stopsig = WSTOPSIG(status);
                        if (stopsig == SIGTTOU || stopsig == SIGTTIN) {
                            kill(-pid, SIGKILL);
                            waitpid(pid, NULL, 0);
                            tab->pipeline_pids[i] = 0;
                            tab->last_exit_status = 1;
                        } else {
                            any_stopped = true;
                        }
                    } else if (WIFEXITED(status)) {
                        if (i == tab->num_pipeline_pids - 1) {
                            tab->last_exit_status = WEXITSTATUS(status);
                        }
                        tab->pipeline_pids[i] = 0;
                    } else if (WIFSIGNALED(status)) {
                        if (i == tab->num_pipeline_pids - 1) {
                            tab->last_exit_status = 128 + WTERMSIG(status);
                        }
                        tab->pipeline_pids[i] = 0;
                    }
                } else if (r == 0) {
                    any_alive = true;
                } else if (r == -1 && errno == ECHILD) {
                    tab->pipeline_pids[i] = 0;
                } else {
                    any_alive = true;
                }
            }
        }

        if (any_stopped) {
            /* Child process group was suspended (Ctrl+Z) */
            Job *j = jobs_find_by_pgid(&tab->jobs, tab->foreground_pgid);
            if (!j) {
                j = jobs_add(&tab->jobs, tab->foreground_pgid, tab->pipeline_pids, tab->num_pipeline_pids, cmd_line, JOB_STATE_STOPPED, false, tab->out_pipe_fd);
            } else {
                j->state = JOB_STATE_STOPPED;
                j->is_foreground = false;
                if (tab->out_pipe_fd >= 0) j->out_pipe_fd = tab->out_pipe_fd;
            }
            if (j) {
                if (tab->jobs.current_id != j->id) {
                    tab->jobs.previous_id = tab->jobs.current_id;
                    tab->jobs.current_id = j->id;
                }
                char stop_msg[MAX_CMD_LEN + 128];
                snprintf(stop_msg, sizeof(stop_msg), "\n[%d]+ Stopped %s\n", j->id, j->cmd_line);
                ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, stop_msg, (uint32_t)strlen(stop_msg));
            }
            tab->foreground_pgid = 0;
            tab->out_pipe_fd = -1;
            tab->num_pipeline_pids = 0;
            tab->running_pid = -1;
            tab->last_exit_status = 148; /* 128 + SIGTSTP */
            break;
        }

        if (!any_alive) {
            if (tab->foreground_pgid > 0) {
                Job *j = jobs_find_by_pgid(&tab->jobs, tab->foreground_pgid);
                if (j) {
                    j->state = JOB_STATE_DONE;
                    j->is_foreground = false;
                    j->exit_status = tab->last_exit_status;
                }
            }
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
