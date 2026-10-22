#include "terminal.h"
#include "util.h"
#include "shell.h"
#include <ctype.h>

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

    tab->line_capacity = INITIAL_LINE_CAPACITY;
    tab->num_lines = 0;
    tab->lines = xcalloc(tab->line_capacity, sizeof(TerminalLine));

    tab->partial_line[0] = '\0';
    tab->partial_len = 0;

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

void terminal_append_line(Tab *tab, const char *text, LineType type) {
    if (!tab) return;

    if (tab->num_lines >= MAX_SCROLLBACK_LINES) {
        free(tab->lines[0].text);
        memmove(&tab->lines[0], &tab->lines[1], (tab->num_lines - 1) * sizeof(TerminalLine));
        tab->num_lines--;
    } else if (tab->num_lines >= tab->line_capacity) {
        tab->line_capacity *= 2;
        tab->lines = xrealloc(tab->lines, tab->line_capacity * sizeof(TerminalLine));
    }

    tab->lines[tab->num_lines].text = xstrdup(text ? text : "");
    tab->lines[tab->num_lines].type = type;
    tab->num_lines++;

    /* Auto-scroll to bottom if user was not scrolled up */
    if (tab->scroll_offset > 0) {
        tab->scroll_offset++;
    }
}

void terminal_append_output_bytes(Tab *tab, const char *bytes, size_t len, bool is_error) {
    if (!tab || !bytes || len == 0) return;

    if (tab->echo_to_stdout) {
        ssize_t written = 0;
        int target_fd = is_error ? STDERR_FILENO : STDOUT_FILENO;
        while (written < (ssize_t)len) {
            ssize_t w = write(target_fd, bytes + written, len - written);
            if (w <= 0) break;
            written += w;
        }
    }

    LineType line_type = is_error ? LINE_TYPE_ERROR : LINE_TYPE_OUTPUT;

    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c == '\n') {
            tab->partial_line[tab->partial_len] = '\0';
            terminal_append_line(tab, tab->partial_line, line_type);
            tab->partial_len = 0;
            tab->partial_line[0] = '\0';
        } else if (c == '\r') {
            /* Ignore bare carriage returns */
            continue;
        } else {
            if ((size_t)tab->partial_len + 1 < sizeof(tab->partial_line)) {
                tab->partial_line[tab->partial_len++] = c;
            } else {
                /* Line buffer full, flush current segment */
                tab->partial_line[tab->partial_len] = '\0';
                terminal_append_line(tab, tab->partial_line, line_type);
                tab->partial_len = 0;
                tab->partial_line[tab->partial_len++] = c;
            }
        }
    }
}

void terminal_flush_partial_line(Tab *tab, bool is_error) {
    if (!tab) return;
    if (tab->partial_len > 0) {
        tab->partial_line[tab->partial_len] = '\0';
        LineType line_type = is_error ? LINE_TYPE_ERROR : LINE_TYPE_OUTPUT;
        terminal_append_line(tab, tab->partial_line, line_type);
        tab->partial_len = 0;
        tab->partial_line[0] = '\0';
    }
}

void terminal_clear_screen(Tab *tab) {
    if (!tab) return;
    for (int i = 0; i < tab->num_lines; i++) {
        free(tab->lines[i].text);
    }
    tab->num_lines = 0;
    tab->scroll_offset = 0;
    tab->partial_len = 0;
    tab->partial_line[0] = '\0';
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
    
    char prompt_buf[MAX_INPUT_LEN];
    shell_format_prompt(tab, prompt_buf, sizeof(prompt_buf));
    terminal_append_line(tab, prompt_buf, LINE_TYPE_PROMPT);
    terminal_scroll_to_bottom(tab);
}
