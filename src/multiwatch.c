#include "multiwatch.h"
#include "parser.h"
#include "pipeline.h"
#include "builtins.h"
#include "executor.h"
#include "ipc.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <errno.h>

static volatile sig_atomic_t g_mw_interrupted = 0;

static void mw_sigint_handler(int sig) {
    (void)sig;
    g_mw_interrupted = 1;
}

void multiwatch_free_args(MultiWatchArgs *args) {
    if (!args) return;
    if (args->commands) {
        for (int i = 0; i < args->count; i++) {
            free(args->commands[i]);
        }
        free(args->commands);
        args->commands = NULL;
    }
    args->count = 0;
}

/*
 * Parse the multiWatch command argument list: multiWatch ["cmd1", "cmd2", ...]
 * Validates opening/closing brackets, comma delimiters, and string quoting.
 */
bool multiwatch_parse_args(const char *arg_str, MultiWatchArgs *args, char *err_msg, size_t err_len) {
    if (!args) return false;
    args->commands = NULL;
    args->count = 0;

    if (!arg_str) {
        snprintf(err_msg, err_len, "missing argument: syntax: multiWatch [\"cmd1\", \"cmd2\", ...]");
        return false;
    }

    const char *p = arg_str;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Skip optional "multiWatch" prefix if passed full command line */
    if (strncmp(p, "multiWatch", 10) == 0) {
        p += 10;
        while (*p && isspace((unsigned char)*p)) p++;
    }

    if (*p == '\0') {
        snprintf(err_msg, err_len, "missing argument: syntax: multiWatch [\"cmd1\", \"cmd2\", ...]");
        return false;
    }

    if (*p != '[') {
        snprintf(err_msg, err_len, "syntax error: missing opening bracket '['");
        return false;
    }
    p++; /* Skip '[' */

    int capacity = 4;
    args->commands = xmalloc(capacity * sizeof(char *));
    bool closed_bracket = false;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) {
            snprintf(err_msg, err_len, "syntax error: missing closing bracket ']'");
            multiwatch_free_args(args);
            return false;
        }

        if (*p == ']') {
            closed_bracket = true;
            p++;
            break;
        }

        if (args->count > 0) {
            if (*p != ',') {
                snprintf(err_msg, err_len, "syntax error: expected ',' between commands");
                multiwatch_free_args(args);
                return false;
            }
            p++; /* Skip ',' */
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == ']') {
                snprintf(err_msg, err_len, "syntax error: trailing comma before ']'");
                multiwatch_free_args(args);
                return false;
            }
        }

        if (*p != '"' && *p != '\'') {
            snprintf(err_msg, err_len, "syntax error: command must be enclosed in quotes");
            multiwatch_free_args(args);
            return false;
        }

        char quote = *p++;
        size_t cmd_cap = 64;
        size_t cmd_len = 0;
        char *cmd_buf = xmalloc(cmd_cap);
        bool closed = false;

        while (*p) {
            if (*p == '\\' && quote == '"' && *(p + 1) != '\0') {
                p++;
                char esc = *p++;
                if (cmd_len + 1 >= cmd_cap) {
                    cmd_cap *= 2;
                    cmd_buf = xrealloc(cmd_buf, cmd_cap);
                }
                cmd_buf[cmd_len++] = esc;
                continue;
            }

            if (*p == quote) {
                closed = true;
                p++;
                break;
            }

            if (cmd_len + 1 >= cmd_cap) {
                cmd_cap *= 2;
                cmd_buf = xrealloc(cmd_buf, cmd_cap);
            }
            cmd_buf[cmd_len++] = *p++;
        }

        if (!closed) {
            snprintf(err_msg, err_len, "syntax error: unmatched quote in command");
            free(cmd_buf);
            multiwatch_free_args(args);
            return false;
        }

        cmd_buf[cmd_len] = '\0';

        if (cmd_len == 0) {
            snprintf(err_msg, err_len, "empty command entry");
            free(cmd_buf);
            multiwatch_free_args(args);
            return false;
        }

        if (args->count >= capacity) {
            capacity *= 2;
            args->commands = xrealloc(args->commands, capacity * sizeof(char *));
        }
        args->commands[args->count++] = cmd_buf;
    }

    if (!closed_bracket) {
        snprintf(err_msg, err_len, "syntax error: missing closing bracket ']'");
        multiwatch_free_args(args);
        return false;
    }

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0') {
        snprintf(err_msg, err_len, "syntax error: unexpected tokens '%s' after ']'", p);
        multiwatch_free_args(args);
        return false;
    }

    if (args->count == 0) {
        snprintf(err_msg, err_len, "empty command list");
        multiwatch_free_args(args);
        return false;
    }

    return true;
}

typedef struct {
    char *cmd_str;
    pid_t pid;
    int pipe_fd;
    time_t exec_timestamp;
    char *output_buf;
    size_t output_len;
    size_t output_cap;
    bool is_running;
    struct timespec next_run_time;
} MonitoredCmd;

static bool start_monitored_cmd(MonitoredCmd *m, const Tab *tab) {
    int pfd[2];
    if (pipe(pfd) < 0) {
        return false;
    }

    m->exec_timestamp = time(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }

    if (pid == 0) {
        /* In child: put in own process group */
        setpgid(0, 0);

        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);

        /* Reset signals */
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        /* Parse command using Terminator shell parser */
        Pipeline *p = parser_parse_pipeline(m->cmd_str, 0);
        if (!p || p->num_commands == 0) {
            _exit(1);
        }

        if (p->num_commands == 1 && builtin_is_builtin(p->commands[0].argv[0])) {
            Tab temp_tab;
            memset(&temp_tab, 0, sizeof(temp_tab));
            if (tab && tab->cwd[0] != '\0') {
                safe_strncpy(temp_tab.cwd, tab->cwd, sizeof(temp_tab.cwd));
            } else if (!getcwd(temp_tab.cwd, sizeof(temp_tab.cwd))) {
                safe_strncpy(temp_tab.cwd, "/tmp", sizeof(temp_tab.cwd));
            }
            int status = builtin_execute(&p->commands[0], &temp_tab);
            fflush(stdout);
            fflush(stderr);
            _exit(status);
        }

        Tab sub_tab;
        memset(&sub_tab, 0, sizeof(sub_tab));
        if (tab && tab->cwd[0] != '\0') {
            safe_strncpy(sub_tab.cwd, tab->cwd, sizeof(sub_tab.cwd));
        } else if (!getcwd(sub_tab.cwd, sizeof(sub_tab.cwd))) {
            safe_strncpy(sub_tab.cwd, "/tmp", sizeof(sub_tab.cwd));
        }

        pipeline_execute(p, &sub_tab);

        if (sub_tab.out_pipe_fd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(sub_tab.out_pipe_fd, buf, sizeof(buf))) > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(STDOUT_FILENO, buf + written, (size_t)(n - written));
                    if (w <= 0) break;
                    written += w;
                }
            }
            close(sub_tab.out_pipe_fd);
        }
        executor_reap_children(&sub_tab);
        _exit(sub_tab.last_exit_status);
    }

    /* Parent */
    close(pfd[1]);
    m->pid = pid;
    m->pipe_fd = pfd[0];
    m->is_running = true;
    m->output_len = 0;
    return true;
}

static void emit_output_chunk(Tab *tab, int write_ipc_fd, const char *data, size_t len) {
    if (!data || len == 0) return;
    if (write_ipc_fd >= 0) {
        ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, data, (uint32_t)len);
    } else {
        if (tab) {
            terminal_append_output_bytes(tab, data, len, false);
        } else {
            ssize_t w = write(STDOUT_FILENO, data, len);
            (void)w;
        }
    }
}

/*
 * Execute multiWatch: concurrently runs multiple commands, multiplexes their output via poll(),
 * formats updates with timestamp headers, and supports clean Ctrl+C interruption.
 */
int multiwatch_run(Tab *tab, const char *raw_cmd, int read_ipc_fd, int write_ipc_fd) {
    MultiWatchArgs args;
    char err_buf[256];

    if (!multiwatch_parse_args(raw_cmd, &args, err_buf, sizeof(err_buf))) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "terminator: multiWatch: %s\n", err_buf);
        emit_output_chunk(tab, write_ipc_fd, err_msg, strlen(err_msg));
        if (tab) tab->last_exit_status = 1;
        if (write_ipc_fd >= 0) {
            int st = 1;
            ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &st, sizeof(st));
        }
        return 1;
    }

    MonitoredCmd *cmds = xcalloc((size_t)args.count, sizeof(MonitoredCmd));
    for (int i = 0; i < args.count; i++) {
        cmds[i].cmd_str = xstrdup(args.commands[i]);
        cmds[i].output_cap = 4096;
        cmds[i].output_buf = xmalloc(cmds[i].output_cap);
        cmds[i].output_len = 0;
        cmds[i].pipe_fd = -1;
        cmds[i].pid = -1;
        cmds[i].is_running = false;
    }

    /* Setup SIGINT handling for cancellation */
    g_mw_interrupted = 0;
    struct sigaction sa_int, old_sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = mw_sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, &old_sa_int);

    /* Launch all commands initially */
    for (int i = 0; i < args.count; i++) {
        start_monitored_cmd(&cmds[i], tab);
    }

    bool cancelled = false;

    while (!cancelled && !g_mw_interrupted) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* Check if any finished command is ready for 2-second re-run */
        for (int i = 0; i < args.count; i++) {
            if (!cmds[i].is_running) {
                if (now.tv_sec > cmds[i].next_run_time.tv_sec ||
                    (now.tv_sec == cmds[i].next_run_time.tv_sec && now.tv_nsec >= cmds[i].next_run_time.tv_nsec)) {
                    start_monitored_cmd(&cmds[i], tab);
                }
            }
        }

        /* Build pollfd set */
        struct pollfd pfds[1 + 64];
        int cmd_map[1 + 64];
        int nfds = 0;

        /* pfds[0]: GUI IPC channel */
        if (read_ipc_fd >= 0) {
            pfds[nfds].fd = read_ipc_fd;
            pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
            cmd_map[nfds] = -1;
            nfds++;
        }

        /* Running commands output pipes */
        for (int i = 0; i < args.count && nfds < 64; i++) {
            if (cmds[i].is_running && cmds[i].pipe_fd >= 0) {
                pfds[nfds].fd = cmds[i].pipe_fd;
                pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
                cmd_map[nfds] = i;
                nfds++;
            }
        }

        /* Compute timeout: minimum time until next scheduled re-run */
        long min_delay_ms = 50; /* Check process state every 50ms at most */
        for (int i = 0; i < args.count; i++) {
            if (!cmds[i].is_running) {
                long diff_ms = (long)((cmds[i].next_run_time.tv_sec - now.tv_sec) * 1000 +
                               (cmds[i].next_run_time.tv_nsec - now.tv_nsec) / 1000000);
                if (diff_ms < 0) diff_ms = 0;
                if (diff_ms < min_delay_ms) min_delay_ms = diff_ms;
            }
        }
        if (min_delay_ms < 5) min_delay_ms = 5;

        int pret = poll(pfds, (nfds_t)nfds, (int)min_delay_ms);

        if (g_mw_interrupted) {
            cancelled = true;
            break;
        }

        if (pret > 0) {
            /* 1. Check GUI IPC */
            if (read_ipc_fd >= 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                IPCHeader hdr;
                if (!ipc_recv_header(read_ipc_fd, &hdr)) {
                    cancelled = true;
                    break;
                }
                while (1) {
                    if (hdr.type == IPC_MSG_INT) {
                        cancelled = true;
                        break;
                    } else if (hdr.type == IPC_MSG_QUIT) {
                        cancelled = true;
                        break;
                    } else if (hdr.length > 0) {
                        char dummy[256];
                        uint32_t to_read = hdr.length;
                        while (to_read > 0) {
                            uint32_t chunk = to_read > sizeof(dummy) ? (uint32_t)sizeof(dummy) : to_read;
                            if (!ipc_recv_payload(read_ipc_fd, dummy, chunk)) break;
                            to_read -= chunk;
                        }
                    }

                    struct pollfd chk;
                    chk.fd = read_ipc_fd;
                    chk.events = POLLIN;
                    if (poll(&chk, 1, 0) <= 0 || !(chk.revents & POLLIN)) break;
                    if (!ipc_recv_header(read_ipc_fd, &hdr)) break;
                }
                if (cancelled) break;
            }

            /* 2. Check Command Output Pipes */
            int start_idx = (read_ipc_fd >= 0) ? 1 : 0;
            for (int k = start_idx; k < nfds; k++) {
                int c_idx = cmd_map[k];
                if (c_idx < 0 || c_idx >= args.count) continue;
                MonitoredCmd *m = &cmds[c_idx];

                if (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                    char buf[4096];
                    ssize_t n = read(m->pipe_fd, buf, sizeof(buf));
                    if (n > 0) {
                        while (m->output_len + (size_t)n + 1 >= m->output_cap) {
                            m->output_cap *= 2;
                            m->output_buf = xrealloc(m->output_buf, m->output_cap);
                        }
                        memcpy(m->output_buf + m->output_len, buf, (size_t)n);
                        m->output_len += (size_t)n;
                    } else {
                        /* EOF reached */
                        close(m->pipe_fd);
                        m->pipe_fd = -1;
                    }
                }
            }
        }

        /* Check for finished child processes */
        for (int i = 0; i < args.count; i++) {
            MonitoredCmd *m = &cmds[i];
            if (m->is_running) {
                int status = 0;
                pid_t r = waitpid(m->pid, &status, WNOHANG);
                if (r == m->pid || (m->pipe_fd < 0)) {
                    /* If pipe still has remaining unread output, drain it */
                    if (m->pipe_fd >= 0) {
                        char buf[4096];
                        ssize_t n;
                        while ((n = read(m->pipe_fd, buf, sizeof(buf))) > 0) {
                            while (m->output_len + (size_t)n + 1 >= m->output_cap) {
                                m->output_cap *= 2;
                                m->output_buf = xrealloc(m->output_buf, m->output_cap);
                            }
                            memcpy(m->output_buf + m->output_len, buf, (size_t)n);
                            m->output_len += (size_t)n;
                        }
                        close(m->pipe_fd);
                        m->pipe_fd = -1;
                    }

                    if (r != m->pid) {
                        waitpid(m->pid, &status, 0);
                    }

                    m->output_buf[m->output_len] = '\0';

                    /* Format required output block:
                     * "<command>", <unix_timestamp>:
                     * ----------------------------------------------------
                     * <output>
                     * ----------------------------------------------------
                     */
                    char header[512];
                    snprintf(header, sizeof(header), "\"%s\", %ld:\n----------------------------------------------------\n",
                             m->cmd_str, (long)m->exec_timestamp);
                    emit_output_chunk(tab, write_ipc_fd, header, strlen(header));

                    if (m->output_len > 0) {
                        emit_output_chunk(tab, write_ipc_fd, m->output_buf, m->output_len);
                        if (m->output_buf[m->output_len - 1] != '\n') {
                            emit_output_chunk(tab, write_ipc_fd, "\n", 1);
                        }
                    }

                    const char *footer = "----------------------------------------------------\n";
                    emit_output_chunk(tab, write_ipc_fd, footer, strlen(footer));

                    /* Mark not running and schedule rerun in 2 seconds */
                    m->is_running = false;
                    m->pid = -1;
                    m->output_len = 0;

                    struct timespec finish_time;
                    clock_gettime(CLOCK_MONOTONIC, &finish_time);
                    m->next_run_time.tv_sec = finish_time.tv_sec + 2;
                    m->next_run_time.tv_nsec = finish_time.tv_nsec;
                }
            }
        }
    }

    /* Cleanup: Terminate and reap all running children */
    for (int i = 0; i < args.count; i++) {
        if (cmds[i].is_running && cmds[i].pid > 0) {
            kill(-cmds[i].pid, SIGINT);
            kill(cmds[i].pid, SIGINT);
        }
        if (cmds[i].pipe_fd >= 0) {
            close(cmds[i].pipe_fd);
            cmds[i].pipe_fd = -1;
        }
    }

    usleep(20000); /* 20ms grace period */
    for (int i = 0; i < args.count; i++) {
        if (cmds[i].is_running && cmds[i].pid > 0) {
            kill(-cmds[i].pid, SIGKILL);
            kill(cmds[i].pid, SIGKILL);
            waitpid(cmds[i].pid, NULL, 0);
            cmds[i].is_running = false;
        }
        free(cmds[i].output_buf);
        free(cmds[i].cmd_str);
    }
    free(cmds);
    multiwatch_free_args(&args);

    sigaction(SIGINT, &old_sa_int, NULL);

    /* Display ^C indication and restore prompt */
    if (cancelled || g_mw_interrupted) {
        emit_output_chunk(tab, write_ipc_fd, "^C\n", 3);
        if (tab) tab->last_exit_status = 130;
        if (write_ipc_fd >= 0) {
            int st = 130;
            ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &st, sizeof(st));
        }
    } else {
        if (write_ipc_fd >= 0) {
            int st = tab ? tab->last_exit_status : 0;
            ipc_send_msg(write_ipc_fd, IPC_MSG_PROMPT, &st, sizeof(st));
        }
    }

    return 0;
}
