#include "gui.h"
#include "terminal.h"
#include "input.h"
#include "executor.h"
#include "util.h"
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

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
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;

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
    XStoreName(dpy, gui->win, "Terminator — Neon Aurora");

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

    /* Tab 1 Card */
    int tab_x = brand_pad_x + brand_w + 12;
    int add_w = 28;
    int add_margin = 8;
    int right_margin = 16;

    int max_tab_w = 240;
    int avail_tab_space = w - tab_x - add_w - add_margin - right_margin;
    int tab_w = (avail_tab_space < max_tab_w) ? avail_tab_space : max_tab_w;
    if (tab_w < 80) tab_w = 80;

    int tab_y = brand_y;
    int tab_h = brand_h;

    fill_rect(gui, tab_x, tab_y, tab_w, tab_h, theme_color(t, COLOR_SURFACE_ELEV));
    draw_rect(gui, tab_x, tab_y, tab_w, tab_h, theme_color(t, COLOR_BORDER));

    /* Prominent active-tab accent line */
    fill_rect(gui, tab_x, tab_y + tab_h - 2, tab_w, 2, theme_color(t, COLOR_PRIMARY_VIOLET));

    char compact_cwd[MAX_PATH_LEN];
    path_compact_home(tab->cwd, compact_cwd, sizeof(compact_cwd));
    char tab_label[MAX_PATH_LEN + 32];
    snprintf(tab_label, sizeof(tab_label), "1: %s", compact_cwd);

    /* Text truncation with ellipsis */
    int label_pad_x = 12;
    int max_text_chars = (tab_w - (label_pad_x * 2)) / (t->font_width > 0 ? t->font_width : 8);
    if (max_text_chars < 3) max_text_chars = 3;
    char display_label[256];
    safe_strncpy(display_label, tab_label, sizeof(display_label));
    if ((int)strlen(display_label) > max_text_chars) {
        if (max_text_chars > 3) {
            display_label[max_text_chars - 3] = '\0';
            strcat(display_label, "...");
        } else {
            display_label[max_text_chars] = '\0';
        }
    }

    draw_text(gui, tab_x + label_pad_x, text_y, display_label, theme_color(t, COLOR_FG_MAIN));

    /* Add-tab affordance [ + ] in separate, non-overlapping box */
    int add_x = tab_x + tab_w + add_margin;
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
    draw_text(gui, sx, stat_text_y, "[TAB 1]", theme_color(t, COLOR_PRIMARY_VIOLET));
    sx += 8 * t->font_width;

    char cwd_str[MAX_PATH_LEN + 32];
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

    /* Total available rows includes history lines + current input row (if not running) */
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
            /* Historical line */
            TerminalLine *tl = &tab->lines[r];

            if (tl->type == LINE_TYPE_PROMPT) {
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
            /* Active typed command line */
            int curr_x = t->padding_x;

            if (tab->state == SHELL_STATE_CONTINUATION) {
                draw_text(gui, curr_x, render_y, "> ", theme_color(t, COLOR_WARM_ORANGE));
                curr_x += 2 * t->font_width;
            } else {
                /* Vivid Cyan / Purple prompt */
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

            /* Draw Cursor */
            int cursor_x = curr_x + (tab->cursor * t->font_width);
            int cursor_y = render_y - t->font_ascent;
            fill_rect(gui, cursor_x, cursor_y, t->font_width, t->font_height, theme_color(t, COLOR_CURSOR));

            /* Draw character under cursor */
            char under_char = tab->input[tab->cursor];
            if (under_char != '\0') {
                char ch_str[2] = { under_char, '\0' };
                draw_text(gui, cursor_x, render_y, ch_str, theme_color(t, COLOR_BG));
            }
        }
        render_y += line_h;
    }

    /* 5. Scroll Indicator if scrolled */
    if (tab->scroll_offset > 0) {
        int bar_w = 6;
        int bar_x = w - bar_w - 4;
        fill_rect(gui, bar_x, term_top, bar_w, term_height, theme_color(t, COLOR_SURFACE_SEC));
        int thumb_h = (term_height * visible_rows) / (tab->num_lines + 1);
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = term_bottom - thumb_h - (tab->scroll_offset * (term_height - thumb_h)) / (tab->num_lines + 1);
        fill_rect(gui, bar_x, thumb_y, bar_w, thumb_h, theme_color(t, COLOR_PRIMARY_VIOLET));
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
        Tab *tab = shell_get_active_tab(gui->shell);

        /* Process pending X events first */
        while (XPending(gui->dpy)) {
            XEvent ev;
            XNextEvent(gui->dpy, &ev);

            switch (ev.type) {
                case KeyPress:
                    input_handle_key_event(&ev.xkey, gui->shell, &gui->needs_redraw);
                    break;

                case ButtonPress:
                    input_handle_button_event(&ev.xbutton, gui->shell, gui->width, gui->height, &gui->needs_redraw);
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

        /* Check for active child pipe or sleep in poll */
        struct pollfd pfd[2];
        int nfds = 1;
        pfd[0].fd = xfd;
        pfd[0].events = POLLIN;

        if (tab && tab->out_pipe_fd >= 0) {
            pfd[1].fd = tab->out_pipe_fd;
            pfd[1].events = POLLIN | POLLHUP | POLLERR;
            nfds = 2;
        }

        int poll_timeout = 50; /* 50ms responsive timeout */
        int pret = poll(pfd, (nfds_t)nfds, poll_timeout);

        if (pret > 0 && nfds > 1) {
            if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                char read_buf[4096];
                ssize_t n = read(tab->out_pipe_fd, read_buf, sizeof(read_buf));
                if (n > 0) {
                    terminal_append_output_bytes(tab, read_buf, (size_t)n, false);
                    gui->needs_redraw = true;
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    /* EOF reached: child finished */
                    close(tab->out_pipe_fd);
                    tab->out_pipe_fd = -1;
                    executor_reap_children(tab);
                    tab->state = SHELL_STATE_READY;
                    terminal_show_prompt(tab);
                    gui->needs_redraw = true;
                }
            }
        } else if (tab && tab->running_pid > 0 && tab->out_pipe_fd < 0) {
            /* Child exited without pipe */
            executor_reap_children(tab);
            tab->state = SHELL_STATE_READY;
            terminal_show_prompt(tab);
            gui->needs_redraw = true;
        }
    }
}
