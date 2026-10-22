#include "gui.h"
#include "terminal.h"
#include "input.h"
#include "executor.h"
#include "util.h"
#include "ipc.h"
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>

static void draw_text(GUI *gui, int x, int y, const char *text, unsigned long color) {
    if (!gui || !text || text[0] == '\0') return;
    XSetForeground(gui->dpy, gui->gc, color);
    XDrawString(gui->dpy, gui->back_buffer, gui->gc, x, y, text, (int)strlen(text));
}

static void fill_rect(GUI *gui, int x, int y, int w, int h, unsigned long color) {
    if (!gui || w <= 0 || h <= 0) return;
    XSetForeground(gui->dpy, gui->gc, color);
    XFillRectangle(gui->dpy, gui->back_buffer, gui->gc, x, y, (unsigned int)w, (unsigned int)h);
}

static void draw_rect(GUI *gui, int x, int y, int w, int h, unsigned long color) {
    if (!gui || w <= 0 || h <= 0) return;
    XSetForeground(gui->dpy, gui->gc, color);
    XDrawRectangle(gui->dpy, gui->back_buffer, gui->gc, x, y, (unsigned int)w, (unsigned int)h);
}

static void draw_line(GUI *gui, int x1, int y1, int x2, int y2, unsigned long color) {
    if (!gui) return;
    XSetForeground(gui->dpy, gui->gc, color);
    XDrawLine(gui->dpy, gui->back_buffer, gui->gc, x1, y1, x2, y2);
}

bool gui_get_scrollbar_geom(const GUI *gui, const Tab *tab, ScrollbarGeom *geom) {
    if (!gui || !tab || !geom) return false;

    int bar_h = gui->theme.top_bar_height;
    int stat_h = gui->theme.status_bar_height;
    int term_top = bar_h + gui->theme.padding_y;
    int term_bottom = gui->height - stat_h - gui->theme.padding_y;
    int term_height = term_bottom - term_top;
    if (term_height <= 0) return false;

    int line_h = gui->theme.font_height + 6;
    if (line_h < 14) line_h = 18;

    int visible_rows = term_height / line_h;
    if (visible_rows <= 0) visible_rows = 1;

    int total_rows = tab->num_lines;
    if (tab->state != SHELL_STATE_RUNNING) {
        total_rows++;
    }

    int max_scroll = (total_rows > visible_rows) ? (total_rows - visible_rows) : 0;

    int sb_w = 8;
    int sb_x = gui->width - sb_w - 4;
    int sb_y = term_top;
    int sb_h = term_height;

    int thumb_h = sb_h;
    int thumb_y = sb_y;

    if (max_scroll > 0 && total_rows > 0) {
        thumb_h = (sb_h * visible_rows) / total_rows;
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > sb_h) thumb_h = sb_h;

        int travel = sb_h - thumb_h;
        int clamped_offset = tab->scroll_offset;
        if (clamped_offset < 0) clamped_offset = 0;
        if (clamped_offset > max_scroll) clamped_offset = max_scroll;

        thumb_y = sb_y + (int)(((float)(max_scroll - clamped_offset) / (float)max_scroll) * travel + 0.5f);
    }

    geom->sb_x = sb_x;
    geom->sb_y = sb_y;
    geom->sb_w = sb_w;
    geom->sb_h = sb_h;
    geom->thumb_y = thumb_y;
    geom->thumb_h = thumb_h;
    geom->max_scroll = max_scroll;
    geom->visible_rows = visible_rows;
    geom->total_rows = total_rows;

    return true;
}

GUI *gui_create(Shell *shell, int width, int height) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        return NULL;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    GUI *gui = xcalloc(1, sizeof(GUI));
    gui->dpy = dpy;
    gui->screen = screen;
    gui->width = (width > 400) ? width : 960;
    gui->height = (height > 300) ? height : 640;
    gui->shell = shell;
    gui->needs_redraw = true;

    /* Initialize Theme */
    theme_init(dpy, screen, &gui->theme);

    /* Create Window */
    XSetWindowAttributes swa;
    swa.background_pixel = theme_color(&gui->theme, COLOR_BG);
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask | StructureNotifyMask;

    gui->win = XCreateWindow(
        dpy, root,
        100, 100,
        (unsigned int)gui->width, (unsigned int)gui->height,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWBackPixel | CWEventMask,
        &swa
    );

    /* Window Title */
    XStoreName(dpy, gui->win, "Terminator");
    XSetIconName(dpy, gui->win, "Terminator");
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom net_wm_icon_name = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    XChangeProperty(dpy, gui->win, net_wm_name, utf8_string, 8, PropModeReplace, (const unsigned char *)"Terminator", 10);
    XChangeProperty(dpy, gui->win, net_wm_icon_name, utf8_string, 8, PropModeReplace, (const unsigned char *)"Terminator", 10);

    /* Handle WM_DELETE_WINDOW cleanly */
    gui->wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, gui->win, &gui->wm_delete_window, 1);

    /* Graphics Context */
    XGCValues gcv;
    gcv.font = gui->theme.font ? gui->theme.font->fid : 0;
    gui->gc = XCreateGC(dpy, gui->win, (gui->theme.font ? GCFont : 0), &gcv);

    /* Off-screen Pixmap for smooth double-buffered rendering */
    gui->back_buffer = XCreatePixmap(
        dpy, gui->win,
        (unsigned int)gui->width, (unsigned int)gui->height,
        (unsigned int)DefaultDepth(dpy, screen)
    );

    XMapWindow(dpy, gui->win);
    XFlush(dpy);

    return gui;
}

void gui_destroy(GUI *gui) {
    if (!gui) return;

    if (gui->dpy) {
        if (gui->back_buffer) {
            XFreePixmap(gui->dpy, gui->back_buffer);
        }
        if (gui->gc) {
            XFreeGC(gui->dpy, gui->gc);
        }
        theme_cleanup(gui->dpy, &gui->theme);
        if (gui->win) {
            XDestroyWindow(gui->dpy, gui->win);
        }
        XCloseDisplay(gui->dpy);
    }
    free(gui);
}

void gui_redraw(GUI *gui) {
    if (!gui || !gui->dpy || !gui->back_buffer) return;

    Tab *tab = shell_get_active_tab(gui->shell);
    if (!tab) return;

    Theme *t = &gui->theme;
    int w = gui->width;
    int h = gui->height;

    /* 1. Clear background */
    fill_rect(gui, 0, 0, w, h, theme_color(t, COLOR_BG));

    /* 2. Render Top Bar */
    int bar_h = t->top_bar_height;
    fill_rect(gui, 0, 0, w, bar_h, theme_color(t, COLOR_SURFACE_SEC));
    draw_line(gui, 0, bar_h - 1, w, bar_h - 1, theme_color(t, COLOR_BORDER));

    /* Logo / Identity in dedicated region */
    const char *title = "TERMINATOR";
    int title_len = (int)strlen(title);
    int title_text_w = title_len * t->font_width;
    int brand_pad_x = 16;
    int brand_y = 6;
    int brand_h = bar_h - 12;
    int brand_w = title_text_w + 24;

    fill_rect(gui, brand_pad_x, brand_y, brand_w, brand_h, theme_color(t, COLOR_SURFACE_ELEV));
    draw_rect(gui, brand_pad_x, brand_y, brand_w, brand_h, theme_color(t, COLOR_BORDER));
    int text_y = brand_y + (brand_h / 2) + (t->font_ascent / 2) - 1;
    draw_text(gui, brand_pad_x + 12, text_y, title, theme_color(t, COLOR_ELECTRIC_CYAN));

    /* Tabs Bar (Multi-tab support) */
    int tab_x = brand_pad_x + brand_w + 12;
    int add_w = 28;
    int add_margin = 8;
    int right_margin = 16;

    int max_tab_w = 180;
    int avail_tab_space = w - tab_x - add_w - add_margin - right_margin;
    int tab_w = (gui->shell->num_tabs > 0) ? (avail_tab_space / gui->shell->num_tabs) - 6 : max_tab_w;
    if (tab_w > max_tab_w) tab_w = max_tab_w;
    if (tab_w < 70) tab_w = 70;

    int tab_y = brand_y;
    int tab_h = brand_h;

    for (int i = 0; i < gui->shell->num_tabs; i++) {
        Tab *t_item = gui->shell->tabs[i];
        if (!t_item) continue;

        int cur_x = tab_x + i * (tab_w + 6);
        bool is_active = (i == gui->shell->active_tab_idx);

        unsigned long bg_col = is_active ? theme_color(t, COLOR_SURFACE_ELEV) : theme_color(t, COLOR_SURFACE_SEC);
        fill_rect(gui, cur_x, tab_y, tab_w, tab_h, bg_col);
        draw_rect(gui, cur_x, tab_y, tab_w, tab_h, theme_color(t, COLOR_BORDER));

        if (is_active) {
            /* Active tab violet accent underline */
            fill_rect(gui, cur_x, tab_y + tab_h - 2, tab_w, 2, theme_color(t, COLOR_PRIMARY_VIOLET));
        }

        char compact_dir[MAX_PATH_LEN];
        path_compact_home(t_item->cwd, compact_dir, sizeof(compact_dir));
        char tab_label[MAX_PATH_LEN + 32];
        snprintf(tab_label, sizeof(tab_label), "%d: %s", t_item->id, compact_dir);

        int label_pad_x = 10;
        int close_btn_w = 16;
        int max_chars = (tab_w - label_pad_x - close_btn_w) / (t->font_width > 0 ? t->font_width : 8);
        if (max_chars < 3) max_chars = 3;

        char display_label[256];
        safe_strncpy(display_label, tab_label, sizeof(display_label));
        if ((int)strlen(display_label) > max_chars) {
            if (max_chars > 3) {
                display_label[max_chars - 3] = '\0';
                strcat(display_label, "...");
            } else {
                display_label[max_chars] = '\0';
            }
        }

        unsigned long text_col = is_active ? theme_color(t, COLOR_FG_MAIN) : theme_color(t, COLOR_FG_SEC);
        draw_text(gui, cur_x + label_pad_x, text_y, display_label, text_col);

        /* Tab close affordance 'x' on right edge */
        int close_x = cur_x + tab_w - 14;
        unsigned long close_col = is_active ? theme_color(t, COLOR_NEON_PINK) : theme_color(t, COLOR_FG_SEC);
        draw_text(gui, close_x, text_y, "x", close_col);
    }

    /* Add-tab affordance [ + ] in separate, non-overlapping box */
    int add_x = tab_x + gui->shell->num_tabs * (tab_w + 6) + add_margin;
    fill_rect(gui, add_x, tab_y, add_w, tab_h, theme_color(t, COLOR_SURFACE_ELEV));
    draw_rect(gui, add_x, tab_y, add_w, tab_h, theme_color(t, COLOR_BORDER));
    int plus_x = add_x + (add_w - t->font_width) / 2;
    draw_text(gui, plus_x, text_y, "+", theme_color(t, COLOR_ELECTRIC_CYAN));

    /* 3. Render Status Bar */
    int stat_h = t->status_bar_height;
    int stat_y = h - stat_h;
    fill_rect(gui, 0, stat_y, w, stat_h, theme_color(t, COLOR_SURFACE_SEC));
    draw_line(gui, 0, stat_y, w, stat_y, theme_color(t, COLOR_BORDER));

    int stat_text_y = stat_y + (stat_h / 2) + (t->font_ascent / 2) - 1;
    int sx = 16;
    char tab_stat[32];
    snprintf(tab_stat, sizeof(tab_stat), "[TAB %d/%d]", gui->shell->active_tab_idx + 1, gui->shell->num_tabs);
    draw_text(gui, sx, stat_text_y, tab_stat, theme_color(t, COLOR_PRIMARY_VIOLET));
    sx += ((int)strlen(tab_stat) + 1) * t->font_width;

    char cwd_str[MAX_PATH_LEN + 32];
    char compact_cwd[MAX_PATH_LEN];
    path_compact_home(tab->cwd, compact_cwd, sizeof(compact_cwd));
    snprintf(cwd_str, sizeof(cwd_str), "CWD: %s", compact_cwd);
    draw_text(gui, sx, stat_text_y, cwd_str, theme_color(t, COLOR_BLUE_ACCENT));
    sx += ((int)strlen(cwd_str) + 2) * t->font_width;

    char exit_str[32];
    snprintf(exit_str, sizeof(exit_str), "EXIT: %d", tab->last_exit_status);
    unsigned long exit_color = (tab->last_exit_status == 0) ? theme_color(t, COLOR_SOFT_GREEN) : theme_color(t, COLOR_ERROR_RED);
    draw_text(gui, sx, stat_text_y, exit_str, exit_color);
    sx += ((int)strlen(exit_str) + 2) * t->font_width;

    char info_str[64];
    snprintf(info_str, sizeof(info_str), "LINES: %d", tab->num_lines);
    draw_text(gui, sx, stat_text_y, info_str, theme_color(t, COLOR_FG_SEC));

    /* 4. Render Terminal Scrollback & Input */
    int term_top = bar_h + t->padding_y;
    int term_bottom = stat_y - t->padding_y;
    int term_height = term_bottom - term_top;
    int line_h = t->font_height + 6;
    if (line_h < 14) line_h = 18;
    int visible_rows = term_height / line_h;
    if (visible_rows <= 0) visible_rows = 1;

    int total_rows = tab->num_lines;
    bool show_input_row = (tab->state != SHELL_STATE_RUNNING);
    if (show_input_row) {
        total_rows++;
    }

    int start_row = total_rows - visible_rows - tab->scroll_offset;
    if (start_row < 0) start_row = 0;

    int render_y = term_top + t->font_ascent;

    for (int r = start_row; r < start_row + visible_rows && r < total_rows; r++) {
        if (r < tab->num_lines) {
            TerminalLine *tl = &tab->lines[r];

            if (tl->colors != NULL && tl->length > 0) {
                int span_start = 0;
                int span_x = t->padding_x;
                while (span_start < tl->length) {
                    uint8_t c_idx = tl->colors[span_start];
                    int span_end = span_start + 1;
                    while (span_end < tl->length && tl->colors[span_end] == c_idx) {
                        span_end++;
                    }
                    int span_len = span_end - span_start;
                    char span_text[MAX_INPUT_LEN];
                    if (span_len >= (int)sizeof(span_text)) span_len = (int)sizeof(span_text) - 1;
                    memcpy(span_text, tl->text + span_start, (size_t)span_len);
                    span_text[span_len] = '\0';

                    unsigned long col = theme_color(t, (ThemeColorIndex)c_idx);
                    draw_text(gui, span_x, render_y, span_text, col);
                    span_x += span_len * t->font_width;
                    span_start = span_end;
                }
            } else if (tl->type == LINE_TYPE_PROMPT) {
                char *arrow = strstr(tl->text, "> ");
                if (arrow) {
                    size_t pfx_len = (arrow - tl->text) + 2;
                    char pfx[256];
                    if (pfx_len < sizeof(pfx)) {
                        memcpy(pfx, tl->text, pfx_len);
                        pfx[pfx_len] = '\0';
                        draw_text(gui, t->padding_x, render_y, pfx, theme_color(t, COLOR_ELECTRIC_CYAN));
                        int cmd_x = t->padding_x + ((int)pfx_len * t->font_width);
                        draw_text(gui, cmd_x, render_y, tl->text + pfx_len, theme_color(t, COLOR_FG_MAIN));
                    } else {
                        draw_text(gui, t->padding_x, render_y, tl->text, theme_color(t, COLOR_ELECTRIC_CYAN));
                    }
                } else {
                    draw_text(gui, t->padding_x, render_y, tl->text, theme_color(t, COLOR_ELECTRIC_CYAN));
                }
            } else if (tl->type == LINE_TYPE_CONTINUATION) {
                draw_text(gui, t->padding_x, render_y, "> ", theme_color(t, COLOR_WARM_ORANGE));
                int cmd_x = t->padding_x + (2 * t->font_width);
                draw_text(gui, cmd_x, render_y, tl->text + (strncmp(tl->text, "> ", 2) == 0 ? 2 : 0), theme_color(t, COLOR_FG_MAIN));
            } else if (tl->type == LINE_TYPE_ERROR) {
                draw_text(gui, t->padding_x, render_y, tl->text, theme_color(t, COLOR_ERROR_RED));
            } else {
                draw_text(gui, t->padding_x, render_y, tl->text, theme_color(t, COLOR_FG_MAIN));
            }
        } else if (show_input_row) {
            int curr_x = t->padding_x;

            if (tab->state == SHELL_STATE_CONTINUATION) {
                draw_text(gui, curr_x, render_y, "> ", theme_color(t, COLOR_WARM_ORANGE));
                curr_x += 2 * t->font_width;
            } else {
                const char *user = getenv("USER");
                if (!user || user[0] == '\0') user = "user";
                draw_text(gui, curr_x, render_y, user, theme_color(t, COLOR_NEON_PINK));
                curr_x += (int)strlen(user) * t->font_width;

                draw_text(gui, curr_x, render_y, "@terminator", theme_color(t, COLOR_PRIMARY_VIOLET));
                curr_x += 11 * t->font_width;

                draw_text(gui, curr_x, render_y, "> ", theme_color(t, COLOR_ELECTRIC_CYAN));
                curr_x += 2 * t->font_width;
            }

            draw_text(gui, curr_x, render_y, tab->input, theme_color(t, COLOR_FG_MAIN));

            int cursor_x = curr_x + (tab->cursor * t->font_width);
            int cursor_y = render_y - t->font_ascent;
            fill_rect(gui, cursor_x, cursor_y, t->font_width, t->font_height, theme_color(t, COLOR_CURSOR));

            char under_char = tab->input[tab->cursor];
            if (under_char != '\0') {
                char ch_str[2] = { under_char, '\0' };
                draw_text(gui, cursor_x, render_y, ch_str, theme_color(t, COLOR_BG));
            }
        }
        render_y += line_h;
    }

    /* 5. Scrollbar */
    ScrollbarGeom sb;
    if (gui_get_scrollbar_geom(gui, tab, &sb)) {
        if (sb.max_scroll > 0) {
            /* Draw Track */
            fill_rect(gui, sb.sb_x, sb.sb_y, sb.sb_w, sb.sb_h, theme_color(t, COLOR_SURFACE_SEC));
            draw_rect(gui, sb.sb_x, sb.sb_y, sb.sb_w, sb.sb_h, theme_color(t, COLOR_BORDER));

            /* Draw Thumb */
            unsigned long thumb_col = gui->sb_dragging ? theme_color(t, COLOR_ELECTRIC_CYAN) : theme_color(t, COLOR_PRIMARY_VIOLET);
            fill_rect(gui, sb.sb_x, sb.thumb_y, sb.sb_w, sb.thumb_h, thumb_col);
        }
    }

    /* 6. Copy back_buffer to window */
    XCopyArea(gui->dpy, gui->back_buffer, gui->win, gui->gc, 0, 0, (unsigned int)w, (unsigned int)h, 0, 0);
    XFlush(gui->dpy);

    gui->needs_redraw = false;
}

void gui_run_event_loop(GUI *gui) {
    if (!gui || !gui->dpy || !gui->shell) return;

    int xfd = ConnectionNumber(gui->dpy);

    while (gui->shell->running) {
        /* Process pending X events first */
        while (XPending(gui->dpy)) {
            XEvent ev;
            XNextEvent(gui->dpy, &ev);

            switch (ev.type) {
                case KeyPress:
                    input_handle_key_event(&ev.xkey, gui->shell, &gui->needs_redraw);
                    break;

                case ButtonPress:
                    input_handle_button_press(&ev.xbutton, gui);
                    break;

                case ButtonRelease:
                    input_handle_button_release(&ev.xbutton, gui);
                    break;

                case MotionNotify:
                    input_handle_motion(&ev.xmotion, gui);
                    break;

                case Expose:
                    gui->needs_redraw = true;
                    break;

                case ConfigureNotify: {
                    if (ev.xconfigure.width != gui->width || ev.xconfigure.height != gui->height) {
                        gui->width = ev.xconfigure.width;
                        gui->height = ev.xconfigure.height;
                        if (gui->back_buffer) {
                            XFreePixmap(gui->dpy, gui->back_buffer);
                        }
                        gui->back_buffer = XCreatePixmap(
                            gui->dpy, gui->win,
                            (unsigned int)gui->width, (unsigned int)gui->height,
                            (unsigned int)DefaultDepth(gui->dpy, gui->screen)
                        );

                        gui->needs_redraw = true;
                    }
                    break;
                }

                case ClientMessage:
                    if ((Atom)ev.xclient.data.l[0] == gui->wm_delete_window) {
                        gui->shell->running = false;
                    }
                    break;

                default:
                    break;
            }
        }

        if (gui->needs_redraw) {
            gui_redraw(gui);
        }

        /* Check for active worker pipes across all tabs and poll */
        struct pollfd pfd[1 + MAX_TABS];
        int tab_map[1 + MAX_TABS];
        int nfds = 1;
        pfd[0].fd = xfd;
        pfd[0].events = POLLIN;

        for (int i = 0; i < gui->shell->num_tabs; i++) {
            Tab *t = gui->shell->tabs[i];
            if (t && t->from_worker_pipe[0] >= 0) {
                pfd[nfds].fd = t->from_worker_pipe[0];
                pfd[nfds].events = POLLIN | POLLHUP | POLLERR;
                tab_map[nfds] = i;
                nfds++;
            }
        }

        int poll_timeout = 20; /* 20ms timeout for responsive UI */
        int pret = poll(pfd, (nfds_t)nfds, poll_timeout);

        if (pret > 0 && nfds > 1) {
            for (int i = 1; i < nfds; i++) {
                if (pfd[i].revents & POLLIN) {
                    int t_idx = tab_map[i];
                    Tab *t = gui->shell->tabs[t_idx];
                    if (!t) continue;

                    IPCHeader hdr;
                    while (ipc_recv_header(t->from_worker_pipe[0], &hdr)) {
                        if (hdr.type == IPC_MSG_OUTPUT) {
                            char *buf = xmalloc(hdr.length + 1);
                            if (hdr.length > 0) {
                                if (!ipc_recv_payload(t->from_worker_pipe[0], buf, hdr.length)) {
                                    free(buf);
                                    break;
                                }
                            }
                            buf[hdr.length] = '\0';
                            terminal_append_output_bytes(t, buf, hdr.length, false);
                            free(buf);
                            if (t_idx == gui->shell->active_tab_idx) {
                                gui->needs_redraw = true;
                            }
                        } else if (hdr.type == IPC_MSG_CWD) {
                            char buf[MAX_PATH_LEN];
                            uint32_t len = (hdr.length < sizeof(buf) - 1) ? hdr.length : (sizeof(buf) - 1);
                            if (len > 0) {
                                ipc_recv_payload(t->from_worker_pipe[0], buf, len);
                            }
                            buf[len] = '\0';
                            safe_strncpy(t->cwd, buf, sizeof(t->cwd));
                            if (t_idx == gui->shell->active_tab_idx) {
                                gui->needs_redraw = true;
                            }
                        } else if (hdr.type == IPC_MSG_CLEAR) {
                            terminal_clear_screen(t);
                            if (t_idx == gui->shell->active_tab_idx) {
                                gui->needs_redraw = true;
                            }
                        } else if (hdr.type == IPC_MSG_EXIT) {
                            t->request_close = true;
                        } else if (hdr.type == IPC_MSG_PROMPT) {
                            int status = 0;
                            if (hdr.length >= sizeof(int)) {
                                ipc_recv_payload(t->from_worker_pipe[0], &status, sizeof(int));
                            }
                            t->last_exit_status = status;
                            t->state = SHELL_STATE_READY;
                            terminal_show_prompt(t);
                            if (t_idx == gui->shell->active_tab_idx) {
                                gui->needs_redraw = true;
                            }
                        }

                        /* Check if more messages ready without blocking */
                        struct pollfd check_pfd;
                        check_pfd.fd = t->from_worker_pipe[0];
                        check_pfd.events = POLLIN;
                        if (poll(&check_pfd, 1, 0) <= 0 || !(check_pfd.revents & POLLIN)) {
                            break;
                        }
                    }
                }

                if (pfd[i].revents & (POLLHUP | POLLERR)) {
                    int t_idx = tab_map[i];
                    Tab *t = gui->shell->tabs[t_idx];
                    if (t) {
                        if (gui->shell->use_process_mode && t->child_pid > 0) {
                            int st = 0;
                            waitpid(t->child_pid, &st, WNOHANG);
                            t->child_pid = -1;
                        }
                        close(t->from_worker_pipe[0]);
                        t->from_worker_pipe[0] = -1;
                        if (t->to_worker_pipe[1] >= 0) {
                            close(t->to_worker_pipe[1]);
                            t->to_worker_pipe[1] = -1;
                        }
                        t->worker_running = false;
                        if (!t->request_close) {
                            terminal_append_line(t, "[Tab disconnected]", LINE_TYPE_ERROR);
                            t->state = SHELL_STATE_READY;
                            terminal_show_prompt(t);
                            if (t_idx == gui->shell->active_tab_idx) {
                                gui->needs_redraw = true;
                            }
                        }
                    }
                }
            }
        }

        /* Check tab close requests across all tabs */
        for (int i = 0; i < gui->shell->num_tabs; i++) {
            Tab *t = gui->shell->tabs[i];
            if (t && t->request_close) {
                shell_close_tab(gui->shell, i);
                gui->needs_redraw = true;
                break;
            }
        }
    }
}
