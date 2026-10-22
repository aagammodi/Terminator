#include "input.h"
#include "terminal.h"
#include "executor.h"
#include "util.h"
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <signal.h>

void input_handle_key_event(XKeyEvent *event, Shell *shell, bool *needs_redraw) {
    if (!shell || !event) return;
    Tab *tab = shell_get_active_tab(shell);
    if (!tab) return;

    *needs_redraw = true;

    char str[64];
    KeySym ksym = NoSymbol;
    int len = XLookupString(event, str, sizeof(str) - 1, &ksym, NULL);
    if (len >= 0) {
        str[len] = '\0';
    }

    bool is_ctrl = (event->state & ControlMask) != 0;
    bool is_shift = (event->state & ShiftMask) != 0;
    (void)is_shift;

    /* Handle Ctrl shortcuts */
    if (is_ctrl) {
        if (ksym == XK_c || ksym == XK_C) {
            /* Ctrl+C: Cancel current line or interrupt active running child */
            if (tab->running_pid > 0) {
                kill(-tab->running_pid, SIGINT);
                kill(tab->running_pid, SIGINT);
            } else {
                /* Display ^C and reset prompt */
                char line_buf[MAX_INPUT_LEN * 2 + 16];
                char prompt[MAX_INPUT_LEN];
                shell_format_prompt(tab, prompt, sizeof(prompt));
                snprintf(line_buf, sizeof(line_buf), "%s%s^C", prompt, tab->input);
                terminal_append_line(tab, line_buf, LINE_TYPE_PROMPT);
                terminal_clear_input(tab);
                tab->accumulated_cmd[0] = '\0';
                tab->in_continuation = false;
                terminal_show_prompt(tab);
            }
            return;
        }

        if (ksym == XK_l || ksym == XK_L) {
            /* Ctrl+L: Clear screen */
            terminal_clear_screen(tab);
            terminal_show_prompt(tab);
            return;
        }

        if (ksym == XK_a || ksym == XK_A) {
            /* Ctrl+A: Beginning of line */
            terminal_cursor_home(tab);
            return;
        }

        if (ksym == XK_e || ksym == XK_E) {
            /* Ctrl+E: End of line */
            terminal_cursor_end(tab);
            return;
        }

        if (ksym == XK_u || ksym == XK_U) {
            /* Ctrl+U: Delete to beginning of line */
            terminal_delete_to_start(tab);
            return;
        }

        if (ksym == XK_k || ksym == XK_K) {
            /* Ctrl+K: Delete to end of line */
            terminal_delete_to_end(tab);
            return;
        }

        if (ksym == XK_w || ksym == XK_W) {
            /* Ctrl+W: Delete word backward */
            terminal_delete_word_backward(tab);
            return;
        }

        if (ksym == XK_d || ksym == XK_D) {
            /* Ctrl+D: Exit shell if line is empty */
            if (tab->input[0] == '\0' && tab->running_pid <= 0) {
                shell->running = false;
                return;
            }
        }
    }

    /* Standard navigation and editing keys */
    switch (ksym) {
        case XK_Return:
        case XK_KP_Enter: {
            /* If a foreground command is still running, ignore enter or buffer it */
            if (tab->out_pipe_fd >= 0) {
                return;
            }

            /* Record line display in scrollback */
            char prompt_buf[MAX_INPUT_LEN];
            shell_format_prompt(tab, prompt_buf, sizeof(prompt_buf));
            char full_line[MAX_INPUT_LEN * 2];
            snprintf(full_line, sizeof(full_line), "%s%s", prompt_buf, tab->input);
            terminal_append_line(tab, full_line, tab->in_continuation ? LINE_TYPE_CONTINUATION : LINE_TYPE_PROMPT);

            /* Check multiline logic */
            size_t input_len = strlen(tab->input);
            bool trailing_backslash = false;
            if (input_len > 0 && tab->input[input_len - 1] == '\\') {
                /* Count trailing backslashes to see if it is escaped */
                size_t bs_count = 0;
                while (input_len > bs_count && tab->input[input_len - 1 - bs_count] == '\\') {
                    bs_count++;
                }
                if (bs_count % 2 == 1) {
                    trailing_backslash = true;
                }
            }

            if (tab->in_continuation) {
                /* Append to accumulated command */
                size_t acc_len = strlen(tab->accumulated_cmd);
                if (trailing_backslash) {
                    /* Strip trailing backslash and append without newline */
                    tab->input[input_len - 1] = '\0';
                    snprintf(tab->accumulated_cmd + acc_len, sizeof(tab->accumulated_cmd) - acc_len, "%s", tab->input);
                } else {
                    snprintf(tab->accumulated_cmd + acc_len, sizeof(tab->accumulated_cmd) - acc_len, "\n%s", tab->input);
                }
            } else {
                if (trailing_backslash) {
                    tab->input[input_len - 1] = '\0';
                    safe_strncpy(tab->accumulated_cmd, tab->input, sizeof(tab->accumulated_cmd));
                    tab->in_continuation = true;
                } else {
                    safe_strncpy(tab->accumulated_cmd, tab->input, sizeof(tab->accumulated_cmd));
                }
            }

            /* Check if accumulated command is complete */
            if (trailing_backslash || parser_is_incomplete(tab->accumulated_cmd)) {
                tab->in_continuation = true;
                terminal_clear_input(tab);
                terminal_show_prompt(tab);
            } else {
                /* Command is complete: execute it */
                tab->in_continuation = false;
                terminal_clear_input(tab);

                char exec_cmd[MAX_CMD_LEN];
                safe_strncpy(exec_cmd, tab->accumulated_cmd, sizeof(exec_cmd));
                tab->accumulated_cmd[0] = '\0';

                executor_run_line(tab, exec_cmd);

                /* If built-in completed synchronously, show prompt immediately */
                if (tab->out_pipe_fd < 0) {
                    terminal_show_prompt(tab);
                }
            }
            break;
        }

        case XK_BackSpace:
            terminal_delete_backspace(tab);
            break;

        case XK_Delete:
            terminal_delete_char(tab);
            break;

        case XK_Left:
            terminal_cursor_left(tab);
            break;

        case XK_Right:
            terminal_cursor_right(tab);
            break;

        case XK_Home:
            terminal_cursor_home(tab);
            break;

        case XK_End:
            terminal_cursor_end(tab);
            break;

        case XK_Up:
            terminal_history_prev(tab);
            break;

        case XK_Down:
            terminal_history_next(tab);
            break;

        case XK_Prior: /* Page Up */
            terminal_scroll_up(tab, 10);
            break;

        case XK_Next: /* Page Down */
            terminal_scroll_down(tab, 10);
            break;

        default:
            /* Printable characters */
            if (len > 0 && (unsigned char)str[0] >= 32 && (unsigned char)str[0] != 127) {
                terminal_insert_string(tab, str);
            }
            break;
    }
}

void input_handle_button_event(XButtonEvent *event, Shell *shell, int win_width, int win_height, bool *needs_redraw) {
    if (!shell || !event) return;
    Tab *tab = shell_get_active_tab(shell);
    if (!tab) return;

    (void)win_width;
    (void)win_height;

    /* Mouse Wheel */
    if (event->button == Button4) {
        /* Wheel Up */
        terminal_scroll_up(tab, 3);
        *needs_redraw = true;
    } else if (event->button == Button5) {
        /* Wheel Down */
        terminal_scroll_down(tab, 3);
        *needs_redraw = true;
    }
}
