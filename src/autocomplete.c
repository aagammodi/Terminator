#include "autocomplete.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static int cmp_str(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

bool autocomplete_handle_tab(Tab *tab) {
    if (!tab || tab->state == SHELL_STATE_RUNNING) return false;

    int cursor_pos = tab->cursor;
    int in_len = (int)strlen(tab->input);
    if (cursor_pos > in_len) cursor_pos = in_len;

    /* 1. Identify word bounds before cursor */
    bool in_single_quote = false;
    bool in_double_quote = false;
    int last_quote_start = -1;
    int last_delimiter = -1;

    for (int i = 0; i < cursor_pos; i++) {
        char c = tab->input[i];
        if (c == '\'' && !in_double_quote) {
            if (!in_single_quote) {
                in_single_quote = true;
                last_quote_start = i;
            } else {
                in_single_quote = false;
                last_quote_start = -1;
            }
        } else if (c == '"' && !in_single_quote) {
            if (!in_double_quote) {
                in_double_quote = true;
                last_quote_start = i;
            } else {
                in_double_quote = false;
                last_quote_start = -1;
            }
        } else if (!in_single_quote && !in_double_quote) {
            if (isspace((unsigned char)c) || c == '|' || c == '>' || c == '<' || c == ';' || c == '&') {
                last_delimiter = i;
            }
        }
    }

    int word_start = 0;
    if (in_single_quote || in_double_quote) {
        word_start = last_quote_start + 1;
    } else if (last_delimiter >= 0) {
        word_start = last_delimiter + 1;
    } else {
        word_start = 0;
    }

    if (word_start > cursor_pos) word_start = cursor_pos;
    int word_len = cursor_pos - word_start;

    char word[MAX_PATH_LEN];
    if (word_len >= (int)sizeof(word)) word_len = (int)sizeof(word) - 1;
    if (word_len > 0) {
        memcpy(word, tab->input + word_start, word_len);
    }
    word[word_len] = '\0';

    /* 2. Separate directory component and file prefix */
    char dir_part[MAX_PATH_LEN] = {0};
    char file_prefix[MAX_PATH_LEN] = {0};

    char *last_slash = strrchr(word, '/');
    if (last_slash) {
        int slash_idx = (int)(last_slash - word);
        memcpy(dir_part, word, slash_idx + 1);
        dir_part[slash_idx + 1] = '\0';
        safe_strncpy(file_prefix, last_slash + 1, sizeof(file_prefix));
    } else {
        dir_part[0] = '\0';
        safe_strncpy(file_prefix, word, sizeof(file_prefix));
    }

    /* 3. Resolve search directory on filesystem */
    char search_dir[MAX_PATH_LEN * 2];
    if (dir_part[0] == '\0') {
        safe_strncpy(search_dir, tab->cwd, sizeof(search_dir));
    } else if (dir_part[0] == '~') {
        path_expand_tilde(dir_part, search_dir, sizeof(search_dir));
    } else if (dir_part[0] == '/') {
        safe_strncpy(search_dir, dir_part, sizeof(search_dir));
    } else {
        snprintf(search_dir, sizeof(search_dir), "%s/%s", tab->cwd, dir_part);
    }

    DIR *dir = opendir(search_dir);
    if (!dir) {
        return false;
    }

    char matches[64][MAX_PATH_LEN];
    int num_matches = 0;
    size_t prefix_len = strlen(file_prefix);

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (de->d_name[0] == '.' && (prefix_len == 0 || file_prefix[0] != '.')) {
            continue;
        }
        if (strncmp(de->d_name, file_prefix, prefix_len) == 0) {
            if (num_matches < 64) {
                safe_strncpy(matches[num_matches++], de->d_name, MAX_PATH_LEN);
            }
        }
    }
    closedir(dir);

    if (num_matches == 0) {
        return false;
    }

    qsort(matches, num_matches, MAX_PATH_LEN, cmp_str);

    /* 4. Handle matching outcomes */
    if (num_matches == 1) {
        /* Single match: complete word directly */
        char completed[MAX_PATH_LEN * 2];
        snprintf(completed, sizeof(completed), "%s%s", dir_part, matches[0]);

        char full_entry_path[MAX_PATH_LEN * 4];
        snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", search_dir, matches[0]);
        struct stat st;
        if (stat(full_entry_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t clen = strlen(completed);
            if (clen > 0 && completed[clen - 1] != '/' && clen + 1 < sizeof(completed)) {
                completed[clen] = '/';
                completed[clen + 1] = '\0';
            }
        }

        char new_input[MAX_INPUT_LEN];
        memcpy(new_input, tab->input, word_start);
        new_input[word_start] = '\0';
        strncat(new_input, completed, sizeof(new_input) - strlen(new_input) - 1);
        size_t new_cursor = strlen(new_input);
        strncat(new_input, tab->input + cursor_pos, sizeof(new_input) - strlen(new_input) - 1);

        safe_strncpy(tab->input, new_input, sizeof(tab->input));
        tab->cursor = (int)new_cursor;
        tab->completion_active = false;
        return true;
    }

    /* Multiple matches: compute Longest Common Prefix */
    char lcp[MAX_PATH_LEN];
    safe_strncpy(lcp, matches[0], sizeof(lcp));
    for (int i = 1; i < num_matches; i++) {
        int j = 0;
        while (lcp[j] && matches[i][j] && lcp[j] == matches[i][j]) {
            j++;
        }
        lcp[j] = '\0';
    }

    if (strlen(lcp) > prefix_len) {
        /* Extend word to common prefix */
        char completed[MAX_PATH_LEN * 2];
        snprintf(completed, sizeof(completed), "%s%s", dir_part, lcp);

        char new_input[MAX_INPUT_LEN];
        memcpy(new_input, tab->input, word_start);
        new_input[word_start] = '\0';
        strncat(new_input, completed, sizeof(new_input) - strlen(new_input) - 1);
        size_t new_cursor = strlen(new_input);
        strncat(new_input, tab->input + cursor_pos, sizeof(new_input) - strlen(new_input) - 1);

        safe_strncpy(tab->input, new_input, sizeof(tab->input));
        tab->cursor = (int)new_cursor;
        tab->completion_active = false;
        return true;
    }

    /* Ambiguous matches with no further common prefix: show numbered list */
    tab->completion_active = true;
    tab->completion_word_start = word_start;
    tab->completion_word_end = cursor_pos;
    tab->completion_num_matches = num_matches;
    safe_strncpy(tab->completion_dir_part, dir_part, sizeof(tab->completion_dir_part));
    for (int i = 0; i < num_matches; i++) {
        safe_strncpy(tab->completion_matches[i], matches[i], sizeof(tab->completion_matches[i]));
    }

    /* Format numbered list: e.g. "1. abc.txt  2. abcd.txt" */
    char disp_buf[MAX_CMD_LEN];
    disp_buf[0] = '\0';
    for (int i = 0; i < num_matches; i++) {
        char item[MAX_PATH_LEN + 16];
        snprintf(item, sizeof(item), "%d. %s  ", i + 1, matches[i]);
        if (strlen(disp_buf) + strlen(item) < sizeof(disp_buf) - 1) {
            strcat(disp_buf, item);
        }
    }

    terminal_append_line(tab, disp_buf, LINE_TYPE_OUTPUT);
    return true;
}

bool autocomplete_handle_choice(Tab *tab, int choice) {
    if (!tab || !tab->completion_active) return false;
    if (choice < 1 || choice > tab->completion_num_matches) return false;

    const char *selected = tab->completion_matches[choice - 1];
    char completed[MAX_PATH_LEN * 2];
    snprintf(completed, sizeof(completed), "%s%s", tab->completion_dir_part, selected);

    /* Check if directory */
    char search_dir[MAX_PATH_LEN * 2];
    if (tab->completion_dir_part[0] == '\0') {
        safe_strncpy(search_dir, tab->cwd, sizeof(search_dir));
    } else if (tab->completion_dir_part[0] == '~') {
        path_expand_tilde(tab->completion_dir_part, search_dir, sizeof(search_dir));
    } else if (tab->completion_dir_part[0] == '/') {
        safe_strncpy(search_dir, tab->completion_dir_part, sizeof(search_dir));
    } else {
        snprintf(search_dir, sizeof(search_dir), "%s/%s", tab->cwd, tab->completion_dir_part);
    }

    char full_entry_path[MAX_PATH_LEN * 4];
    snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", search_dir, selected);
    struct stat st;
    if (stat(full_entry_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t clen = strlen(completed);
        if (clen > 0 && completed[clen - 1] != '/' && clen + 1 < sizeof(completed)) {
            completed[clen] = '/';
            completed[clen + 1] = '\0';
        }
    }

    char new_input[MAX_INPUT_LEN];
    int wstart = tab->completion_word_start;
    int wend = tab->completion_word_end;
    if (wstart < 0) wstart = 0;
    if (wend > (int)strlen(tab->input)) wend = (int)strlen(tab->input);

    memcpy(new_input, tab->input, wstart);
    new_input[wstart] = '\0';
    strncat(new_input, completed, sizeof(new_input) - strlen(new_input) - 1);
    size_t new_cursor = strlen(new_input);
    strncat(new_input, tab->input + wend, sizeof(new_input) - strlen(new_input) - 1);

    safe_strncpy(tab->input, new_input, sizeof(tab->input));
    tab->cursor = (int)new_cursor;
    tab->completion_active = false;
    return true;
}

void autocomplete_cancel(Tab *tab) {
    if (tab) {
        tab->completion_active = false;
    }
}
