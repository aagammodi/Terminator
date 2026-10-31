#include "input.h"
#include "terminal.h"
#include "executor.h"
#include "shell.h"
#include "gui.h"
#include "util.h"
#include "ipc.h"
#include "keybind.h"
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

void input_handle_key_event(XKeyEvent *event, GUI *gui, Shell *shell, bool *needs_redraw) {
    if (!shell && gui) shell = gui->shell;
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
        } else if (is_ctrl && (ksym == XK_z || ksym == XK_Z)) {
            /* Ctrl+Z: Send SIGTSTP to child process group immediately */
            if (tab->to_worker_pipe[1] >= 0) {
                ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_TSTP, NULL, 0);
            }
            if (tab->foreground_pgid > 0) {
                kill(-tab->foreground_pgid, SIGTSTP);
            } else if (tab->running_pid > 0) {
                kill(-tab->running_pid, SIGTSTP);
            }
            return;
        }
        return;
    }

    /* History search mode (Ctrl+R) handling */
    if (tab->in_search_mode) {
        if (ksym == XK_Escape || (is_ctrl && (ksym == XK_c || ksym == XK_C))) {
            tab->in_search_mode = false;
            tab->search_buf[0] = '\0';
            tab->search_cursor = 0;
            terminal_append_line(tab, "^C", LINE_TYPE_OUTPUT);
            terminal_show_prompt(tab);
            return;
        }
        if (ksym == XK_Return || ksym == XK_KP_Enter) {
            char search_term[MAX_INPUT_LEN];
            safe_strncpy(search_term, tab->search_buf, sizeof(search_term));
            tab->in_search_mode = false;
            tab->search_buf[0] = '\0';
            tab->search_cursor = 0;

            char echo_line[MAX_INPUT_LEN + 32];
            snprintf(echo_line, sizeof(echo_line), "Enter search term: %s", search_term);
            terminal_append_line(tab, echo_line, LINE_TYPE_PROMPT);

            char results[16][MAX_HISTORY_LINE_LEN];
            int num_results = 0;
            if (shell && shell->history && search_term[0] != '\0') {
                num_results = history_search(shell->history, search_term, results, 16);
            }

            if (num_results > 0) {
                for (int i = 0; i < num_results; i++) {
                    terminal_append_line(tab, results[i], LINE_TYPE_OUTPUT);
                }
            } else {
                terminal_append_line(tab, "No match for search term in history", LINE_TYPE_OUTPUT);
            }
            terminal_show_prompt(tab);
            return;
        }
        if (ksym == XK_BackSpace) {
            int slen = (int)strlen(tab->search_buf);
            if (slen > 0) {
                tab->search_buf[slen - 1] = '\0';
                tab->search_cursor = slen - 1;
            }
            return;
        }
        if (len > 0 && !is_ctrl && (unsigned char)str[0] >= 32) {
            int slen = (int)strlen(tab->search_buf);
            if (slen + len < (int)sizeof(tab->search_buf) - 1) {
                strcat(tab->search_buf, str);
                tab->search_cursor = (int)strlen(tab->search_buf);
            }
            return;
        }
        return;
    }

    /* Check configured keybindings */
    KeyAction action = keybind_match(ksym, event->state);
    switch (action) {
        case ACTION_NEW_TAB:
            shell_open_tab(shell, NULL);
            return;

        case ACTION_CLOSE_TAB_SHORTCUT:
            shell_close_tab(shell, shell->active_tab_idx);
            return;

        case ACTION_PREV_TAB:
            if (shell->num_tabs > 1) {
                int prev = (shell->active_tab_idx - 1 + shell->num_tabs) % shell->num_tabs;
                shell_switch_tab(shell, prev);
                Tab *nt = shell_get_active_tab(shell);
                if (gui && nt) {
                    gui->sel_active = nt->sel_active;
                    gui->sel_start_row = nt->sel_start_row;
                    gui->sel_start_col = nt->sel_start_col;
                    gui->sel_end_row = nt->sel_end_row;
                    gui->sel_end_col = nt->sel_end_col;
                }
            }
            return;

        case ACTION_NEXT_TAB:
            if (shell->num_tabs > 1) {
                int next = (shell->active_tab_idx + 1) % shell->num_tabs;
                shell_switch_tab(shell, next);
                Tab *nt = shell_get_active_tab(shell);
                if (gui && nt) {
                    gui->sel_active = nt->sel_active;
                    gui->sel_start_row = nt->sel_start_row;
                    gui->sel_start_col = nt->sel_start_col;
                    gui->sel_end_row = nt->sel_end_row;
                    gui->sel_end_col = nt->sel_end_col;
                }
            }
            return;

        case ACTION_BEGINNING_OF_LINE:
            terminal_cursor_home(tab);
            return;

        case ACTION_END_OF_LINE:
            terminal_cursor_end(tab);
            return;

        case ACTION_MOVE_LEFT:
            terminal_cursor_left(tab);
            return;

        case ACTION_MOVE_RIGHT:
            terminal_cursor_right(tab);
            return;

        case ACTION_HISTORY_PREV:
            terminal_history_prev(tab);
            return;

        case ACTION_HISTORY_NEXT:
            terminal_history_next(tab);
            return;

        case ACTION_DELETE_TO_START:
            terminal_delete_to_start(tab);
            return;

        case ACTION_DELETE_TO_END:
            terminal_delete_to_end(tab);
            return;

        case ACTION_DELETE_WORD:
            terminal_delete_word_backward(tab);
            return;

        case ACTION_CLEAR_SCREEN:
            terminal_clear_screen(tab);
            tab->state = SHELL_STATE_READY;
            terminal_show_prompt(tab);
            return;

        case ACTION_CLOSE_TAB:
            /* Ctrl+D: Close tab only if input line is empty; if non-empty, do nothing to protect input */
            if (tab->input[0] == '\0' && tab->running_pid <= 0) {
                shell_close_tab(shell, shell->active_tab_idx);
            }
            return;

        case ACTION_COPY:
            if (gui) {
                gui_copy_selection(gui, tab);
            }
            return;

        case ACTION_PASTE:
            if (gui) {
                gui_request_paste(gui);
            }
            return;

        case ACTION_HISTORY_SEARCH:
            tab->in_search_mode = true;
            tab->search_buf[0] = '\0';
            tab->search_cursor = 0;
            return;

        case ACTION_SUSPEND_JOB:
            if (tab->state == SHELL_STATE_RUNNING) {
                if (tab->to_worker_pipe[1] >= 0) {
                    ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_TSTP, NULL, 0);
                }
                if (tab->foreground_pgid > 0) {
                    kill(-tab->foreground_pgid, SIGTSTP);
                } else if (tab->running_pid > 0) {
                    kill(-tab->running_pid, SIGTSTP);
                }
            }
            return;

        default:
            break;
    }

    /* Direct Ctrl+1..9 tab switching */
    if (is_ctrl && ksym >= XK_1 && ksym <= XK_9) {
        int target_idx = (int)(ksym - XK_1);
        if (target_idx < shell->num_tabs) {
            shell_switch_tab(shell, target_idx);
        }
        return;
    }

    /* Ctrl+C at prompt: cancel current line, display ^C and show clean prompt */
    if (is_ctrl && (ksym == XK_c || ksym == XK_C)) {
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
    } else if (event->button == Button2) {
        /* Middle Click: Paste from PRIMARY selection */
        XConvertSelection(gui->dpy, XA_PRIMARY, gui->utf8_string, gui->sel_prop_atom, gui->win, CurrentTime);
        XFlush(gui->dpy);
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
                    Tab *nt = shell_get_active_tab(gui->shell);
                    if (nt) {
                        gui->sel_active = nt->sel_active;
                        gui->sel_start_row = nt->sel_start_row;
                        gui->sel_start_col = nt->sel_start_col;
                        gui->sel_end_row = nt->sel_end_row;
                        gui->sel_end_col = nt->sel_end_col;
                    } else {
                        gui->sel_active = false;
                    }
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

    /* 3. Text Selection in Terminal Area */
    int term_top = bar_h + gui->theme.padding_y;
    int stat_y = gui->height - gui->theme.status_bar_height;
    int term_bottom = stat_y - gui->theme.padding_y;
    int line_h = gui->theme.font_height + 6;
    if (line_h < 14) line_h = 18;

    if (event->y >= term_top && event->y < term_bottom && event->x < gui->width - 16) {
        int total_rows = tab->num_lines + (tab->state != SHELL_STATE_RUNNING ? 1 : 0);
        int visible_rows = (term_bottom - term_top) / line_h;
        if (visible_rows <= 0) visible_rows = 1;
        int start_row = total_rows - visible_rows - tab->scroll_offset;
        if (start_row < 0) start_row = 0;

        int vis_r = (event->y - term_top) / line_h;
        int vis_c = (event->x - gui->theme.padding_x) / gui->theme.font_width;
        if (vis_c < 0) vis_c = 0;

        gui->sel_start_row = start_row + vis_r;
        gui->sel_start_col = vis_c;
        gui->sel_end_row = gui->sel_start_row;
        gui->sel_end_col = gui->sel_start_col;
        gui->sel_dragging = true;
        gui->sel_active = false;

        tab->sel_start_row = gui->sel_start_row;
        tab->sel_start_col = gui->sel_start_col;
        tab->sel_end_row = gui->sel_end_row;
        tab->sel_end_col = gui->sel_end_col;
        tab->sel_dragging = true;
        tab->sel_active = false;
        gui->needs_redraw = true;
        return;
    }
}

void input_handle_button_release(XButtonEvent *event, GUI *gui) {
    if (!gui || !event) return;
    if (event->button == Button1) {
        if (gui->sb_dragging) {
            gui->sb_dragging = false;
            gui->needs_redraw = true;
        }
        if (gui->sel_dragging) {
            gui->sel_dragging = false;
            if (gui->sel_active) {
                Tab *tab = shell_get_active_tab(gui->shell);
                if (tab) {
                    char *sel_text = gui_get_selected_text(gui, tab);
                    if (sel_text && *sel_text) {
                        free(gui->primary_text);
                        gui->primary_text = sel_text;
                        XSetSelectionOwner(gui->dpy, XA_PRIMARY, gui->win, CurrentTime);
                        XFlush(gui->dpy);
                    } else {
                        free(sel_text);
                    }
                }
            }
            gui->needs_redraw = true;
        }
    }
}

void input_handle_motion(XMotionEvent *event, GUI *gui) {
    if (!gui || !event || !gui->shell) return;
    Tab *tab = shell_get_active_tab(gui->shell);
    if (!tab) return;

    if (gui->sb_dragging) {
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
        return;
    }

    if (gui->sel_dragging) {
        int bar_h = gui->theme.top_bar_height;
        int term_top = bar_h + gui->theme.padding_y;
        int stat_y = gui->height - gui->theme.status_bar_height;
        int term_bottom = stat_y - gui->theme.padding_y;
        int line_h = gui->theme.font_height + 6;
        if (line_h < 14) line_h = 18;

        int total_rows = tab->num_lines + (tab->state != SHELL_STATE_RUNNING ? 1 : 0);
        int visible_rows = (term_bottom - term_top) / line_h;
        if (visible_rows <= 0) visible_rows = 1;
        int start_row = total_rows - visible_rows - tab->scroll_offset;
        if (start_row < 0) start_row = 0;

        int vis_r = (event->y - term_top) / line_h;
        int vis_c = (event->x - gui->theme.padding_x) / gui->theme.font_width;
        if (vis_c < 0) vis_c = 0;

        gui->sel_end_row = start_row + vis_r;
        gui->sel_end_col = vis_c;
        if (gui->sel_start_row != gui->sel_end_row || gui->sel_start_col != gui->sel_end_col) {
            gui->sel_active = true;
        }
        tab->sel_end_row = gui->sel_end_row;
        tab->sel_end_col = gui->sel_end_col;
        tab->sel_active = gui->sel_active;
        gui->needs_redraw = true;
        return;
    }
}
