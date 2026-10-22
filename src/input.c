#include "input.h"
#include "terminal.h"
#include "executor.h"
#include "shell.h"
#include "gui.h"
#include "util.h"
#include "ipc.h"
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

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

    /* Handle Tab Creation and Closing shortcuts */
    if (is_ctrl && is_shift) {
        if (ksym == XK_t || ksym == XK_T) {
            /* Ctrl+Shift+T: Open new tab */
            shell_open_tab(shell, NULL);
            return;
        }
        if (ksym == XK_w || ksym == XK_W) {
            /* Ctrl+Shift+W: Close active tab */
            shell_close_tab(shell, shell->active_tab_idx);
            return;
        }
    }

    /* Handle Ctrl shortcuts */
    if (is_ctrl) {
        if (ksym == XK_Prior) {
            /* Ctrl+PageUp: Switch to previous tab */
            if (shell->num_tabs > 1) {
                int prev = (shell->active_tab_idx - 1 + shell->num_tabs) % shell->num_tabs;
                shell_switch_tab(shell, prev);
            }
            return;
        }
        if (ksym == XK_Next) {
            /* Ctrl+PageDown: Switch to next tab */
            if (shell->num_tabs > 1) {
                int next = (shell->active_tab_idx + 1) % shell->num_tabs;
                shell_switch_tab(shell, next);
            }
            return;
        }
        if (ksym >= XK_1 && ksym <= XK_9) {
            /* Ctrl+1..9: Direct tab switch */
            int target_idx = (int)(ksym - XK_1);
            if (target_idx < shell->num_tabs) {
                shell_switch_tab(shell, target_idx);
            }
            return;
        }
    }

    /* If a foreground command or application is currently running */
    if (tab->state == SHELL_STATE_RUNNING) {
        if (is_ctrl && (ksym == XK_c || ksym == XK_C)) {
            /* Ctrl+C: Send SIGINT to child process group immediately */
            if (tab->to_worker_pipe[1] >= 0) {
                ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_INT, NULL, 0);
            }
            if (tab->foreground_pgid > 0) {
                kill(-tab->foreground_pgid, SIGINT);
            } else if (tab->running_pid > 0) {
                kill(-tab->running_pid, SIGINT);
                kill(tab->running_pid, SIGINT);
            }
            return;
        }
        return;
    }

    /* Handle Ctrl shortcuts for command editing */
    if (is_ctrl) {
        if (ksym == XK_c || ksym == XK_C) {
            /* Display ^C and reset prompt */
            char line_buf[MAX_INPUT_LEN * 2 + 16];
            char prompt[MAX_INPUT_LEN];
            shell_format_prompt(tab, prompt, sizeof(prompt));
            snprintf(line_buf, sizeof(line_buf), "%s%s^C", prompt, tab->input);
            terminal_append_line(tab, line_buf, LINE_TYPE_PROMPT);
            terminal_clear_input(tab);
            tab->accumulated_cmd[0] = '\0';
            tab->in_continuation = false;
            tab->state = SHELL_STATE_READY;
            terminal_show_prompt(tab);
            return;
        }

        if (ksym == XK_l || ksym == XK_L) {
            /* Ctrl+L: Clear screen */
            terminal_clear_screen(tab);
            tab->state = SHELL_STATE_READY;
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
            /* Ctrl+D: Exit shell or close active tab if input line is empty */
            if (tab->input[0] == '\0' && tab->running_pid <= 0) {
                shell_close_tab(shell, shell->active_tab_idx);
                return;
            }
        }
    }

    /* Standard navigation and editing keys */
    switch (ksym) {
        case XK_Return:
        case XK_KP_Enter: {
            /* If a foreground command is still running, ignore enter */
            if (tab->state == SHELL_STATE_RUNNING || tab->out_pipe_fd >= 0) {
                return;
            }

            /* Record line display in scrollback */
            char prompt_buf[MAX_INPUT_LEN];
            shell_format_prompt(tab, prompt_buf, sizeof(prompt_buf));
            char full_line[MAX_INPUT_LEN * 2];
            snprintf(full_line, sizeof(full_line), "%s%s", prompt_buf, tab->input);
            terminal_append_line(tab, full_line, (tab->state == SHELL_STATE_CONTINUATION) ? LINE_TYPE_CONTINUATION : LINE_TYPE_PROMPT);

            /* Check multiline logic */
            size_t input_len = strlen(tab->input);
            bool trailing_backslash = false;
            if (input_len > 0 && tab->input[input_len - 1] == '\\') {
                size_t bs_count = 0;
                while (input_len > bs_count && tab->input[input_len - 1 - bs_count] == '\\') {
                    bs_count++;
                }
                if (bs_count % 2 == 1) {
                    trailing_backslash = true;
                }
            }

            if (tab->state == SHELL_STATE_CONTINUATION) {
                size_t acc_len = strlen(tab->accumulated_cmd);
                if (trailing_backslash) {
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
                    tab->state = SHELL_STATE_CONTINUATION;
                } else {
                    safe_strncpy(tab->accumulated_cmd, tab->input, sizeof(tab->accumulated_cmd));
                }
            }

            /* Check if accumulated command is complete */
            if (trailing_backslash || parser_is_incomplete(tab->accumulated_cmd)) {
                tab->in_continuation = true;
                tab->state = SHELL_STATE_CONTINUATION;
                terminal_clear_input(tab);
                terminal_show_prompt(tab);
            } else {
                /* Command is complete: execute it */
                tab->in_continuation = false;
                tab->state = SHELL_STATE_RUNNING;
                terminal_clear_input(tab);

                char exec_cmd[MAX_CMD_LEN];
                safe_strncpy(exec_cmd, tab->accumulated_cmd, sizeof(exec_cmd));
                tab->accumulated_cmd[0] = '\0';

                if (tab->to_worker_pipe[1] >= 0) {
                    ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_EXEC, exec_cmd, (uint32_t)strlen(exec_cmd));
                } else {
                    executor_run_line(tab, exec_cmd);
                    if (tab->out_pipe_fd < 0) {
                        tab->state = SHELL_STATE_READY;
                        terminal_show_prompt(tab);
                    }
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

void input_handle_button_press(XButtonEvent *event, GUI *gui) {
    if (!gui || !event || !gui->shell) return;
    Tab *tab = shell_get_active_tab(gui->shell);
    if (!tab) return;

    /* Mouse Wheel */
    if (event->button == Button4) {
        terminal_scroll_up(tab, 3);
        gui->needs_redraw = true;
        return;
    } else if (event->button == Button5) {
        terminal_scroll_down(tab, 3);
        gui->needs_redraw = true;
        return;
    }

    if (event->button != Button1) return;

    /* 1. Scrollbar interaction */
    ScrollbarGeom geom;
    if (gui_get_scrollbar_geom(gui, tab, &geom) && geom.max_scroll > 0) {
        if (event->x >= geom.sb_x - 6 && event->x <= geom.sb_x + geom.sb_w + 6 &&
            event->y >= geom.sb_y && event->y <= geom.sb_y + geom.sb_h) {

            if (event->y >= geom.thumb_y && event->y <= geom.thumb_y + geom.thumb_h) {
                /* Clicked thumb: begin drag */
                gui->sb_dragging = true;
                gui->sb_drag_mouse_y = event->y;
                gui->sb_thumb_grab_y = event->y - geom.thumb_y;
                gui->sb_drag_start_offset = tab->scroll_offset;
                gui->needs_redraw = true;
                return;
            } else if (event->y < geom.thumb_y) {
                /* Clicked above thumb: page up */
                int step = (geom.visible_rows > 1) ? geom.visible_rows - 1 : 1;
                terminal_scroll_up(tab, step);
                gui->needs_redraw = true;
                return;
            } else if (event->y > geom.thumb_y + geom.thumb_h) {
                /* Clicked below thumb: page down */
                int step = (geom.visible_rows > 1) ? geom.visible_rows - 1 : 1;
                terminal_scroll_down(tab, step);
                gui->needs_redraw = true;
                return;
            }
        }
    }

    /* 2. Top tab bar interaction */
    int bar_h = gui->theme.top_bar_height;
    int brand_y = 6;
    int brand_h = bar_h - 12;

    if (event->y >= brand_y && event->y <= brand_y + brand_h) {
        int title_len = (int)strlen("TERMINATOR");
        int title_text_w = title_len * gui->theme.font_width;
        int brand_pad_x = 16;
        int brand_w = title_text_w + 24;
        int tab_x = brand_pad_x + brand_w + 12;
        int add_w = 28;
        int add_margin = 8;
        int right_margin = 16;

        int max_tab_w = 180;
        int avail_tab_space = gui->width - tab_x - add_w - add_margin - right_margin;
        int tab_w = (gui->shell->num_tabs > 0) ? (avail_tab_space / gui->shell->num_tabs) - 6 : max_tab_w;
        if (tab_w > max_tab_w) tab_w = max_tab_w;
        if (tab_w < 70) tab_w = 70;

        for (int i = 0; i < gui->shell->num_tabs; i++) {
            int cur_x = tab_x + i * (tab_w + 6);
            if (event->x >= cur_x && event->x <= cur_x + tab_w) {
                if (event->x >= cur_x + tab_w - 20) {
                    /* Clicked 'x' close button */
                    shell_close_tab(gui->shell, i);
                } else {
                    shell_switch_tab(gui->shell, i);
                }
                gui->needs_redraw = true;
                return;
            }
        }

        int add_x = tab_x + gui->shell->num_tabs * (tab_w + 6) + add_margin;
        if (event->x >= add_x && event->x <= add_x + add_w) {
            shell_open_tab(gui->shell, NULL);
            gui->needs_redraw = true;
            return;
        }
    }
}

void input_handle_button_release(XButtonEvent *event, GUI *gui) {
    if (!gui || !event) return;
    if (event->button == Button1) {
        if (gui->sb_dragging) {
            gui->sb_dragging = false;
            gui->needs_redraw = true;
        }
    }
}

void input_handle_motion(XMotionEvent *event, GUI *gui) {
    if (!gui || !event || !gui->shell || !gui->sb_dragging) return;
    Tab *tab = shell_get_active_tab(gui->shell);
    if (!tab) return;

    ScrollbarGeom geom;
    if (!gui_get_scrollbar_geom(gui, tab, &geom) || geom.max_scroll <= 0) return;

    int travel = geom.sb_h - geom.thumb_h;
    if (travel <= 0) return;

    int thumb_y = event->y - gui->sb_thumb_grab_y;
    if (thumb_y < geom.sb_y) thumb_y = geom.sb_y;
    if (thumb_y > geom.sb_y + travel) thumb_y = geom.sb_y + travel;

    float fraction = (float)(thumb_y - geom.sb_y) / (float)travel;
    int new_offset = (int)((1.0f - fraction) * geom.max_scroll + 0.5f);
    if (new_offset < 0) new_offset = 0;
    if (new_offset > geom.max_scroll) new_offset = geom.max_scroll;

    if (new_offset != tab->scroll_offset) {
        tab->scroll_offset = new_offset;
        gui->needs_redraw = true;
    }
}
