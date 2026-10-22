#include "common.h"
#include "shell.h"
#include "gui.h"
#include "executor.h"
#include "util.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <signal.h>

static Tab *g_active_tab = NULL;

static void headless_sigint_handler(int sig) {
    (void)sig;
    if (g_active_tab) {
        if (g_active_tab->state == SHELL_STATE_RUNNING) {
            if (g_active_tab->to_worker_pipe[1] >= 0) {
                ipc_send_msg(g_active_tab->to_worker_pipe[1], IPC_MSG_INT, NULL, 0);
            }
            if (g_active_tab->foreground_pgid > 0) {
                kill(-g_active_tab->foreground_pgid, SIGINT);
            } else if (g_active_tab->running_pid > 0) {
                kill(-g_active_tab->running_pid, SIGINT);
                kill(g_active_tab->running_pid, SIGINT);
            }
        } else {
            /* If waiting at prompt, print newline and redisplay */
            printf("\n");
            char prompt_buf[MAX_INPUT_LEN];
            shell_format_prompt(g_active_tab, prompt_buf, sizeof(prompt_buf));
            printf("%s", prompt_buf);
            fflush(stdout);
        }
    }
}

static void print_usage(const char *prog) {
    printf("Terminator — Custom Terminal Emulator & Shell (Phase 1)\n");
    printf("Usage:\n");
    printf("  %s                      Run X11 GUI terminal (Thread Mode default)\n", prog);
    printf("  %s useProcess=True      Run in Process Mode (per-tab child processes)\n", prog);
    printf("  %s -c <command>         Execute command string and exit\n", prog);
    printf("  %s --test               Run in headless test shell mode\n", prog);
    printf("  %s --help               Display this help message\n", prog);
}

static int run_headless_shell(Shell *shell) {
    Tab *tab = shell_get_active_tab(shell);
    if (!tab) return 1;

    g_active_tab = tab;
    signal(SIGINT, headless_sigint_handler);

    tab->echo_to_stdout = true;
    bool interactive = isatty(STDIN_FILENO);

    char line[MAX_INPUT_LEN];

    while (shell->running) {
        if (interactive) {
            char prompt_buf[MAX_INPUT_LEN];
            shell_format_prompt(tab, prompt_buf, sizeof(prompt_buf));
            printf("%s", prompt_buf);
            fflush(stdout);
        }

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        bool trailing_backslash = false;
        if (len > 0 && line[len - 1] == '\\') {
            size_t bs_count = 0;
            while (len > bs_count && line[len - 1 - bs_count] == '\\') {
                bs_count++;
            }
            if (bs_count % 2 == 1) {
                trailing_backslash = true;
            }
        }

        if (tab->in_continuation) {
            size_t acc_len = strlen(tab->accumulated_cmd);
            if (trailing_backslash) {
                line[len - 1] = '\0';
                snprintf(tab->accumulated_cmd + acc_len, sizeof(tab->accumulated_cmd) - acc_len, "%s", line);
            } else {
                snprintf(tab->accumulated_cmd + acc_len, sizeof(tab->accumulated_cmd) - acc_len, "\n%s", line);
            }
        } else {
            if (trailing_backslash) {
                line[len - 1] = '\0';
                safe_strncpy(tab->accumulated_cmd, line, sizeof(tab->accumulated_cmd));
                tab->in_continuation = true;
            } else {
                safe_strncpy(tab->accumulated_cmd, line, sizeof(tab->accumulated_cmd));
            }
        }

        if (trailing_backslash || parser_is_incomplete(tab->accumulated_cmd)) {
            tab->in_continuation = true;
            continue;
        }

        tab->in_continuation = false;
        tab->state = SHELL_STATE_RUNNING;
        char exec_cmd[MAX_CMD_LEN];
        safe_strncpy(exec_cmd, tab->accumulated_cmd, sizeof(exec_cmd));
        tab->accumulated_cmd[0] = '\0';

        if (tab->to_worker_pipe[1] >= 0) {
            ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_EXEC, exec_cmd, (uint32_t)strlen(exec_cmd));
            IPCHeader hdr;
            while (ipc_recv_header(tab->from_worker_pipe[0], &hdr)) {
                if (hdr.type == IPC_MSG_OUTPUT) {
                    char *buf = xmalloc(hdr.length + 1);
                    if (hdr.length > 0) {
                        ipc_recv_payload(tab->from_worker_pipe[0], buf, hdr.length);
                    }
                    fwrite(buf, 1, hdr.length, stdout);
                    fflush(stdout);
                    free(buf);
                } else if (hdr.type == IPC_MSG_PROMPT) {
                    int status = 0;
                    if (hdr.length >= sizeof(int)) {
                        ipc_recv_payload(tab->from_worker_pipe[0], &status, sizeof(int));
                    }
                    tab->last_exit_status = status;
                    break;
                } else if (hdr.type == IPC_MSG_CWD) {
                    char buf[MAX_PATH_LEN];
                    uint32_t c_len = (hdr.length < sizeof(buf) - 1) ? hdr.length : (sizeof(buf) - 1);
                    if (c_len > 0) ipc_recv_payload(tab->from_worker_pipe[0], buf, c_len);
                    buf[c_len] = '\0';
                    safe_strncpy(tab->cwd, buf, sizeof(tab->cwd));
                } else if (hdr.type == IPC_MSG_EXIT) {
                    tab->request_close = true;
                } else {
                    if (hdr.length > 0) {
                        char *dummy = xmalloc(hdr.length);
                        ipc_recv_payload(tab->from_worker_pipe[0], dummy, hdr.length);
                        free(dummy);
                    }
                }
            }
        } else {
            executor_run_line(tab, exec_cmd);
        }
        tab->state = SHELL_STATE_READY;

        if (tab->request_close) {
            shell_close_tab(shell, shell->active_tab_idx);
            if (!shell->running) break;
            tab = shell_get_active_tab(shell);
            if (!tab) break;
            tab->echo_to_stdout = true;
        }
    }

    return tab->last_exit_status;
}

int main(int argc, char **argv) {
    bool force_headless = false;
    bool use_process_mode = false;
    const char *eval_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "--headless") == 0) {
            force_headless = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            eval_cmd = argv[++i];
        } else if (strncasecmp(argv[i], "useProcess=", 11) == 0) {
            const char *val = argv[i] + 11;
            if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "yes") == 0) {
                use_process_mode = true;
            } else {
                use_process_mode = false;
            }
        } else if (strcmp(argv[i], "--process") == 0) {
            use_process_mode = true;
        }
    }

    /* Initialize Shell with 1 Tab in requested architecture mode */
    Shell *shell = shell_create(NULL, use_process_mode);
    Tab *tab = shell_get_active_tab(shell);

    /* If -c was provided, run command and exit */
    if (eval_cmd) {
        tab->echo_to_stdout = true;
        if (tab->to_worker_pipe[1] >= 0) {
            ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_EXEC, eval_cmd, (uint32_t)strlen(eval_cmd));
            IPCHeader hdr;
            while (ipc_recv_header(tab->from_worker_pipe[0], &hdr)) {
                if (hdr.type == IPC_MSG_OUTPUT) {
                    char *buf = xmalloc(hdr.length + 1);
                    if (hdr.length > 0) {
                        ipc_recv_payload(tab->from_worker_pipe[0], buf, hdr.length);
                    }
                    fwrite(buf, 1, hdr.length, stdout);
                    fflush(stdout);
                    free(buf);
                } else if (hdr.type == IPC_MSG_PROMPT) {
                    int status = 0;
                    if (hdr.length >= sizeof(int)) {
                        ipc_recv_payload(tab->from_worker_pipe[0], &status, sizeof(int));
                    }
                    tab->last_exit_status = status;
                    break;
                } else if (hdr.type == IPC_MSG_CWD) {
                    char buf[MAX_PATH_LEN];
                    uint32_t c_len = (hdr.length < sizeof(buf) - 1) ? hdr.length : (sizeof(buf) - 1);
                    if (c_len > 0) ipc_recv_payload(tab->from_worker_pipe[0], buf, c_len);
                    buf[c_len] = '\0';
                    safe_strncpy(tab->cwd, buf, sizeof(tab->cwd));
                } else {
                    if (hdr.length > 0) {
                        char *dummy = xmalloc(hdr.length);
                        ipc_recv_payload(tab->from_worker_pipe[0], dummy, hdr.length);
                        free(dummy);
                    }
                }
            }
        } else {
            executor_run_line(tab, eval_cmd);
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
        }
        int exit_code = tab->last_exit_status;
        shell_destroy(shell);
        return exit_code;
    }

    /* If headless requested or DISPLAY is unset, run headless shell loop */
    if (force_headless || getenv("DISPLAY") == NULL || !isatty(STDIN_FILENO)) {
        if (getenv("DISPLAY") == NULL && !force_headless) {
            fprintf(stderr, "terminator: note: DISPLAY not set; launching headless shell mode.\n");
        }
        int code = run_headless_shell(shell);
        shell_destroy(shell);
        return code;
    }

    /* Create X11 GUI */
    GUI *gui = gui_create(shell, 960, 640);
    if (!gui) {
        fprintf(stderr, "terminator: failed to open X11 display '%s', falling back to terminal mode\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : ":0");
        int code = run_headless_shell(shell);
        shell_destroy(shell);
        return code;
    }

    /* Run Event Loop */
    gui_run_event_loop(gui);

    /* Clean exit */
    gui_destroy(gui);
    shell_destroy(shell);

    return 0;
}
