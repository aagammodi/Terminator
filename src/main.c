#include "common.h"
#include "shell.h"
#include "gui.h"
#include "executor.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog) {
    printf("Terminator — Custom Terminal Emulator & Shell (Phase 1)\n");
    printf("Usage:\n");
    printf("  %s                      Run X11 GUI terminal (default)\n", prog);
    printf("  %s useProcess=True      Process mode (architecture flag)\n", prog);
    printf("  %s -c <command>         Execute command string and exit\n", prog);
    printf("  %s --test               Run in headless test shell mode\n", prog);
    printf("  %s --help               Display this help message\n", prog);
}

static int run_headless_shell(Shell *shell) {
    Tab *tab = shell_get_active_tab(shell);
    if (!tab) return 1;

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
        char exec_cmd[MAX_CMD_LEN];
        safe_strncpy(exec_cmd, tab->accumulated_cmd, sizeof(exec_cmd));
        tab->accumulated_cmd[0] = '\0';

        executor_run_line(tab, exec_cmd);

        /* Pump child output synchronously in headless mode */
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

    return tab->last_exit_status;
}

int main(int argc, char **argv) {
    bool force_headless = false;
    const char *eval_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "--headless") == 0) {
            force_headless = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            eval_cmd = argv[++i];
        } else if (strncmp(argv[i], "useProcess=", 11) == 0) {
            /* Handled in architecture configuration */
            term_log("terminator: process mode flag noted: %s\n", argv[i]);
        }
    }

    /* Initialize Shell with 1 Tab */
    Shell *shell = shell_create(NULL);
    Tab *tab = shell_get_active_tab(shell);

    /* If -c was provided, run command directly and exit */
    if (eval_cmd) {
        tab->echo_to_stdout = true;
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
