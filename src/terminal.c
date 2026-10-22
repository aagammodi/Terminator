#include "terminal.h"
#include "util.h"
#include "shell.h"
#include "theme.h"
#include <ctype.h>

static ThemeColorIndex ansi_to_theme_color(int code, bool bright) {
    (void)bright;
    switch (code) {
        case 30: return COLOR_SURFACE_ELEV;
        case 31: return COLOR_ERROR_RED;
        case 32: return COLOR_SOFT_GREEN;
        case 33: return COLOR_WARM_ORANGE;
        case 34: return COLOR_BLUE_ACCENT;
        case 35: return COLOR_NEON_PINK;
        case 36: return COLOR_ELECTRIC_CYAN;
        case 37: return COLOR_FG_MAIN;
        case 90: return COLOR_FG_SEC;
        case 91: return COLOR_ERROR_RED;
        case 92: return COLOR_SOFT_GREEN;
        case 93: return COLOR_WARM_ORANGE;
        case 94: return COLOR_BLUE_ACCENT;
        case 95: return COLOR_NEON_PINK;
        case 96: return COLOR_ELECTRIC_CYAN;
        case 97: return COLOR_FG_MAIN;
        default: return COLOR_FG_MAIN;
    }
}

Tab *terminal_tab_create(int id, const char *initial_dir) {
    Tab *tab = xcalloc(1, sizeof(Tab));
    tab->id = id;

    if (initial_dir && initial_dir[0] != '\0') {
        path_expand_tilde(initial_dir, tab->cwd, sizeof(tab->cwd));
    } else {
        if (!getcwd(tab->cwd, sizeof(tab->cwd))) {
            const char *home = getenv("HOME");
            safe_strncpy(tab->cwd, home ? home : "/tmp", sizeof(tab->cwd));
        }
    }

    tab->input[0] = '\0';
    tab->cursor = 0;
    tab->accumulated_cmd[0] = '\0';
    tab->in_continuation = false;
    tab->shell = NULL;
    tab->request_close = false;

    tab->line_capacity = INITIAL_LINE_CAPACITY;
    tab->num_lines = 0;
    tab->lines = xcalloc(tab->line_capacity, sizeof(TerminalLine));

    tab->partial_line[0] = '\0';
    tab->partial_len = 0;
    tab->current_fg_color = COLOR_FG_MAIN;

    tab->scroll_offset = 0;

    tab->out_pipe_fd = -1;
    tab->running_pid = -1;
    tab->num_pipeline_pids = 0;
    tab->last_exit_status = 0;

    tab->history_capacity = 64;
    tab->history_count = 0;
    tab->history = xcalloc(tab->history_capacity, sizeof(char *));
    tab->history_index = -1;
    tab->saved_draft[0] = '\0';
    tab->echo_to_stdout = false;
    tab->state = SHELL_STATE_READY;
    tab->ansi_state = ANSI_STATE_NORMAL;
    tab->csi_len = 0;
    tab->csi_buf[0] = '\0';

    return tab;
}

void terminal_tab_destroy(Tab *tab) {
    if (!tab) return;

    if (tab->out_pipe_fd >= 0) {
        close(tab->out_pipe_fd);
        tab->out_pipe_fd = -1;
    }

    for (int i = 0; i < tab->num_lines; i++) {
        free(tab->lines[i].text);
        free(tab->lines[i].colors);
    }
    free(tab->lines);

    for (int i = 0; i < tab->history_count; i++) {
        free(tab->history[i]);
    }
    free(tab->history);

    free(tab);
}

void terminal_insert_char(Tab *tab, char c) {
    if (!tab) return;
    size_t len = strlen(tab->input);
    if (len + 1 >= sizeof(tab->input)) return;
    if (tab->cursor < 0) tab->cursor = 0;
    if ((size_t)tab->cursor > len) tab->cursor = (int)len;

    memmove(&tab->input[tab->cursor + 1], &tab->input[tab->cursor], len - tab->cursor + 1);
    tab->input[tab->cursor] = c;
    tab->cursor++;
}

void terminal_insert_string(Tab *tab, const char *str) {
    if (!tab || !str) return;
    while (*str) {
        terminal_insert_char(tab, *str++);
    }
}

void terminal_delete_backspace(Tab *tab) {
    if (!tab || tab->cursor <= 0) return;
    size_t len = strlen(tab->input);
    if (tab->cursor > (int)len) tab->cursor = (int)len;

    memmove(&tab->input[tab->cursor - 1], &tab->input[tab->cursor], len - tab->cursor + 1);
    tab->cursor--;
}

void terminal_delete_char(Tab *tab) {
    if (!tab) return;
    size_t len = strlen(tab->input);
    if (tab->cursor < 0 || tab->cursor >= (int)len) return;

    memmove(&tab->input[tab->cursor], &tab->input[tab->cursor + 1], len - tab->cursor);
}

void terminal_cursor_left(Tab *tab) {
    if (!tab) return;
    if (tab->cursor > 0) {
        tab->cursor--;
    }
}

void terminal_cursor_right(Tab *tab) {
    if (!tab) return;
    size_t len = strlen(tab->input);
    if (tab->cursor < (int)len) {
        tab->cursor++;
    }
}

void terminal_cursor_home(Tab *tab) {
    if (!tab) return;
    tab->cursor = 0;
}

void terminal_cursor_end(Tab *tab) {
    if (!tab) return;
    tab->cursor = (int)strlen(tab->input);
}

void terminal_clear_input(Tab *tab) {
    if (!tab) return;
    tab->input[0] = '\0';
    tab->cursor = 0;
    tab->history_index = -1;
}

void terminal_delete_to_start(Tab *tab) {
    if (!tab || tab->cursor <= 0) return;
    size_t len = strlen(tab->input);
    memmove(&tab->input[0], &tab->input[tab->cursor], len - tab->cursor + 1);
    tab->cursor = 0;
}

void terminal_delete_to_end(Tab *tab) {
    if (!tab || tab->cursor < 0) return;
    size_t len = strlen(tab->input);
    if (tab->cursor < (int)len) {
        tab->input[tab->cursor] = '\0';
    }
}

void terminal_delete_word_backward(Tab *tab) {
    if (!tab || tab->cursor <= 0) return;
    int idx = tab->cursor;
    while (idx > 0 && isspace((unsigned char)tab->input[idx - 1])) {
        idx--;
    }
    while (idx > 0 && !isspace((unsigned char)tab->input[idx - 1])) {
        idx--;
    }
    int del_count = tab->cursor - idx;
    size_t len = strlen(tab->input);
    memmove(&tab->input[idx], &tab->input[tab->cursor], len - tab->cursor + 1);
    tab->cursor = idx;
    (void)del_count;
}

void terminal_history_add(Tab *tab, const char *cmd) {
    if (!tab || !cmd || cmd[0] == '\0') return;

    /* Don't duplicate immediate previous entry */
    if (tab->history_count > 0 && strcmp(tab->history[tab->history_count - 1], cmd) == 0) {
        tab->history_index = -1;
        return;
    }

    if (tab->history_count >= tab->history_capacity) {
        tab->history_capacity *= 2;
        tab->history = xrealloc(tab->history, tab->history_capacity * sizeof(char *));
    }

    tab->history[tab->history_count++] = xstrdup(cmd);
    tab->history_index = -1;
}

void terminal_history_prev(Tab *tab) {
    if (!tab || tab->history_count == 0) return;

    if (tab->history_index == -1) {
        safe_strncpy(tab->saved_draft, tab->input, sizeof(tab->saved_draft));
        tab->history_index = tab->history_count - 1;
    } else if (tab->history_index > 0) {
        tab->history_index--;
    } else {
        return; /* Already at oldest command */
    }

    safe_strncpy(tab->input, tab->history[tab->history_index], sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
}

void terminal_history_next(Tab *tab) {
    if (!tab || tab->history_index == -1) return;

    if (tab->history_index < tab->history_count - 1) {
        tab->history_index++;
        safe_strncpy(tab->input, tab->history[tab->history_index], sizeof(tab->input));
    } else {
        /* Restored saved draft */
        tab->history_index = -1;
        safe_strncpy(tab->input, tab->saved_draft, sizeof(tab->input));
    }
    tab->cursor = (int)strlen(tab->input);
}

void terminal_append_line_with_colors(Tab *tab, const char *text, const uint8_t *colors, int len, LineType type) {
    if (!tab) return;

    if (tab->num_lines >= MAX_SCROLLBACK_LINES) {
        free(tab->lines[0].text);
        free(tab->lines[0].colors);
        memmove(&tab->lines[0], &tab->lines[1], (tab->num_lines - 1) * sizeof(TerminalLine));
        tab->num_lines--;
    } else if (tab->num_lines >= tab->line_capacity) {
        tab->line_capacity *= 2;
        tab->lines = xrealloc(tab->lines, tab->line_capacity * sizeof(TerminalLine));
    }

    tab->lines[tab->num_lines].text = xstrdup(text ? text : "");
    if (colors && len > 0) {
        tab->lines[tab->num_lines].colors = xmalloc((size_t)len);
        memcpy(tab->lines[tab->num_lines].colors, colors, (size_t)len);
        tab->lines[tab->num_lines].length = len;
    } else {
        tab->lines[tab->num_lines].colors = NULL;
        tab->lines[tab->num_lines].length = (int)strlen(tab->lines[tab->num_lines].text);
    }
    tab->lines[tab->num_lines].type = type;
    tab->num_lines++;

    /* Auto-scroll to bottom if user was not scrolled up */
    if (tab->scroll_offset > 0) {
        tab->scroll_offset++;
    }
}

void terminal_append_line(Tab *tab, const char *text, LineType type) {
    terminal_append_line_with_colors(tab, text, NULL, 0, type);
}

static inline void safe_write_bytes(int fd, const char *buf, size_t len) {
    ssize_t written = 0;
    while (written < (ssize_t)len) {
        ssize_t w = write(fd, buf + written, len - written);
        if (w <= 0) break;
        written += w;
    }
}

void terminal_append_output_bytes(Tab *tab, const char *bytes, size_t len, bool is_error) {
    if (!tab || !bytes || len == 0) return;

    LineType line_type = is_error ? LINE_TYPE_ERROR : LINE_TYPE_OUTPUT;
    int target_fd = is_error ? STDERR_FILENO : STDOUT_FILENO;

    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];

        switch (tab->ansi_state) {
            case ANSI_STATE_NORMAL:
                if (c == 0x1B) {
                    tab->ansi_state = ANSI_STATE_ESC;
                } else if (c == '\n') {
                    tab->partial_line[tab->partial_len] = '\0';
                    terminal_append_line_with_colors(tab, tab->partial_line, tab->partial_colors, tab->partial_len, line_type);
                    if (tab->echo_to_stdout) {
                        safe_write_bytes(target_fd, "\n", 1);
                    }
                    tab->partial_len = 0;
                    tab->partial_line[0] = '\0';
                } else if (c == '\r') {
                    /* Ignore carriage returns */
                    continue;
                } else if (c == '\b') {
                    if (tab->partial_len > 0) {
                        tab->partial_len--;
                        tab->partial_line[tab->partial_len] = '\0';
                    }
                } else if ((unsigned char)c >= 32 || c == '\t') {
                    if (tab->echo_to_stdout) {
                        safe_write_bytes(target_fd, &c, 1);
                    }
                    if ((size_t)tab->partial_len + 1 < sizeof(tab->partial_line)) {
                        tab->partial_colors[tab->partial_len] = tab->current_fg_color;
                        tab->partial_line[tab->partial_len++] = c;
                    } else {
                        /* Line buffer full, flush current segment */
                        tab->partial_line[tab->partial_len] = '\0';
                        terminal_append_line_with_colors(tab, tab->partial_line, tab->partial_colors, tab->partial_len, line_type);
                        tab->partial_len = 0;
                        tab->partial_colors[tab->partial_len] = tab->current_fg_color;
                        tab->partial_line[tab->partial_len++] = c;
                    }
                }
                break;

            case ANSI_STATE_ESC:
                if (c == '[') {
                    tab->ansi_state = ANSI_STATE_CSI;
                    tab->csi_len = 0;
                    tab->csi_buf[0] = '\0';
                } else if (c == ']') {
                    tab->ansi_state = ANSI_STATE_OSC;
                } else {
                    tab->ansi_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_CSI:
                if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == ' ') {
                    if (tab->csi_len < (int)sizeof(tab->csi_buf) - 1) {
                        tab->csi_buf[tab->csi_len++] = c;
                        tab->csi_buf[tab->csi_len] = '\0';
                    }
                } else if (c >= 0x40 && c <= 0x7E) {
                    /* Final character of CSI sequence */
                    if (c == 'm') {
                        /* SGR Color/Attribute code */
                        if (tab->csi_len == 0) {
                            tab->current_fg_color = COLOR_FG_MAIN;
                        } else {
                            char tmp[64];
                            safe_strncpy(tmp, tab->csi_buf, sizeof(tmp));
                            char *saveptr = NULL;
                            char *tok = strtok_r(tmp, ";", &saveptr);
                            bool bright = false;
                            while (tok) {
                                int val = atoi(tok);
                                if (val == 0) {
                                    tab->current_fg_color = COLOR_FG_MAIN;
                                    bright = false;
                                } else if (val == 1) {
                                    bright = true;
                                } else if (val >= 30 && val <= 37) {
                                    tab->current_fg_color = (uint8_t)ansi_to_theme_color(val, bright);
                                } else if (val == 39) {
                                    tab->current_fg_color = COLOR_FG_MAIN;
                                } else if (val >= 90 && val <= 97) {
                                    tab->current_fg_color = (uint8_t)ansi_to_theme_color(val, true);
                                }
                                tok = strtok_r(NULL, ";", &saveptr);
                            }
                        }
                    } else if (c == 'J') {
                        /* Erase in Display: 2J (screen), 3J (scrollback) */
                        if (strstr(tab->csi_buf, "2") || strstr(tab->csi_buf, "3") || tab->csi_len == 0) {
                            terminal_clear_screen(tab);
                        }
                    } else if (c == 'H' || c == 'f') {
                        /* Cursor Home */
                        tab->scroll_offset = 0;
                    } else if (c == 'K') {
                        /* Erase in Line */
                        tab->partial_len = 0;
                        tab->partial_line[0] = '\0';
                    }
                    /* End of CSI sequence */
                    tab->ansi_state = ANSI_STATE_NORMAL;
                } else {
                    tab->ansi_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_OSC:
                if (c == 0x07 || c == 0x1B) {
                    tab->ansi_state = ANSI_STATE_NORMAL;
                }
                break;
        }
    }
}

void terminal_flush_partial_line(Tab *tab, bool is_error) {
    if (!tab) return;
    if (tab->partial_len > 0) {
        tab->partial_line[tab->partial_len] = '\0';
        LineType line_type = is_error ? LINE_TYPE_ERROR : LINE_TYPE_OUTPUT;
        terminal_append_line_with_colors(tab, tab->partial_line, tab->partial_colors, tab->partial_len, line_type);
        tab->partial_len = 0;
        tab->partial_line[0] = '\0';
    }
}

void terminal_clear_screen(Tab *tab) {
    if (!tab) return;
    for (int i = 0; i < tab->num_lines; i++) {
        free(tab->lines[i].text);
        free(tab->lines[i].colors);
    }
    tab->num_lines = 0;
    tab->scroll_offset = 0;
    tab->partial_len = 0;
    tab->partial_line[0] = '\0';
    tab->ansi_state = ANSI_STATE_NORMAL;
    tab->csi_len = 0;
    tab->csi_buf[0] = '\0';
    tab->current_fg_color = COLOR_FG_MAIN;
}

void terminal_scroll_up(Tab *tab, int lines) {
    if (!tab) return;
    tab->scroll_offset += lines;
    if (tab->scroll_offset > tab->num_lines) {
        tab->scroll_offset = tab->num_lines;
    }
}

void terminal_scroll_down(Tab *tab, int lines) {
    if (!tab) return;
    tab->scroll_offset -= lines;
    if (tab->scroll_offset < 0) {
        tab->scroll_offset = 0;
    }
}

void terminal_scroll_to_bottom(Tab *tab) {
    if (!tab) return;
    tab->scroll_offset = 0;
}

void terminal_show_prompt(Tab *tab) {
    if (!tab) return;
    terminal_flush_partial_line(tab, false);
    terminal_scroll_to_bottom(tab);
}
