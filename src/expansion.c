#include "expansion.h"
#include "util.h"
#include "executor.h"
#include <glob.h>
#include <ctype.h>

char *expansion_process_command_substitutions(const char *input, int last_exit_status) {
    if (!input) return NULL;

    /* Work with a dynamically growing buffer */
    char *curr = xstrdup(input);

    while (1) {
        size_t len = strlen(curr);
        int sub_start = -1;
        int sub_end = -1;
        bool is_backtick = false;

        /* Scan for innermost substitution:
         * A substitution starting at sub_start that contains NO other substitution start inside it. */
        bool in_single_quote = false;
        bool in_double_quote = false;

        int candidate_start = -1;
        bool candidate_is_backtick = false;

        for (size_t i = 0; i < len; i++) {
            char c = curr[i];

            if (c == '\\' && !in_single_quote && i + 1 < len) {
                i++; /* Skip escaped char */
                continue;
            }

            if (c == '\'' && !in_double_quote) {
                in_single_quote = !in_single_quote;
                continue;
            }

            if (in_single_quote) {
                continue; /* No substitutions inside single quotes */
            }

            if (c == '"') {
                in_double_quote = !in_double_quote;
                continue;
            }

            /* Check for $( */
            if (c == '$' && i + 1 < len && curr[i + 1] == '(') {
                candidate_start = (int)i;
                candidate_is_backtick = false;
                i++; /* skip '(' */
                continue;
            }

            /* Check for backtick */
            if (c == '`') {
                if (candidate_start != -1 && candidate_is_backtick) {
                    /* Found matching closing backtick for innermost candidate */
                    sub_start = candidate_start;
                    sub_end = (int)i;
                    is_backtick = true;
                    break;
                } else {
                    candidate_start = (int)i;
                    candidate_is_backtick = true;
                    continue;
                }
            }

            /* Check for closing ')' */
            if (c == ')' && candidate_start != -1 && !candidate_is_backtick) {
                /* We found closing ')' for our latest candidate_start.
                 * Because candidate_start was updated on any inner $(, this is an innermost $(...) ! */
                sub_start = candidate_start;
                sub_end = (int)i;
                is_backtick = false;
                break;
            }
        }

        if (sub_start == -1 || sub_end == -1) {
            /* No more substitutions found */
            break;
        }

        /* Extract inner command */
        char *inner_cmd = NULL;
        if (is_backtick) {
            size_t inner_len = sub_end - sub_start - 1;
            inner_cmd = xmalloc(inner_len + 1);
            memcpy(inner_cmd, curr + sub_start + 1, inner_len);
            inner_cmd[inner_len] = '\0';
        } else {
            /* $(cmd) starts at sub_start, '(' is at sub_start + 1, ')' is at sub_end */
            size_t inner_len = sub_end - (sub_start + 2);
            inner_cmd = xmalloc(inner_len + 1);
            memcpy(inner_cmd, curr + sub_start + 2, inner_len);
            inner_cmd[inner_len] = '\0';
        }

        /* Execute inner command synchronously and capture output */
        char *sub_output = executor_execute_substitution_sync(inner_cmd, last_exit_status);
        free(inner_cmd);

        if (!sub_output) {
            sub_output = xstrdup("");
        }

        /* Construct new string replacing [sub_start ... sub_end] with sub_output */
        size_t out_len = strlen(sub_output);
        size_t new_len = sub_start + out_len + (len - sub_end - 1);
        char *new_str = xmalloc(new_len + 1);

        memcpy(new_str, curr, sub_start);
        memcpy(new_str + sub_start, sub_output, out_len);
        memcpy(new_str + sub_start + out_len, curr + sub_end + 1, len - sub_end - 1);
        new_str[new_len] = '\0';

        free(sub_output);
        free(curr);
        curr = new_str;
    }

    return curr;
}

char *expansion_expand_variables(const char *input, int last_exit_status) {
    if (!input) return NULL;

    size_t in_len = strlen(input);
    size_t out_cap = in_len * 2 + 64;
    size_t out_len = 0;
    char *out = xmalloc(out_cap);

    bool in_single_quote = false;
    bool in_double_quote = false;

    for (size_t i = 0; i < in_len; i++) {
        char c = input[i];

        /* Single quotes toggle only outside double quotes */
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            if (out_len + 1 >= out_cap) {
                out_cap *= 2;
                out = xrealloc(out, out_cap);
            }
            out[out_len++] = c;
            continue;
        }

        /* Double quotes toggle only outside single quotes */
        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            if (out_len + 1 >= out_cap) {
                out_cap *= 2;
                out = xrealloc(out, out_cap);
            }
            out[out_len++] = c;
            continue;
        }

        /* If inside single quotes, everything is literal! */
        if (in_single_quote) {
            if (out_len + 1 >= out_cap) {
                out_cap *= 2;
                out = xrealloc(out, out_cap);
            }
            out[out_len++] = c;
            continue;
        }

        /* Escaped character */
        if (c == '\\' && i + 1 < in_len) {
            char next = input[i + 1];
            if (in_double_quote) {
                if (next == '$' || next == '`' || next == '"' || next == '\\') {
                    if (out_len + 2 >= out_cap) {
                        out_cap *= 2;
                        out = xrealloc(out, out_cap);
                    }
                    out[out_len++] = c;
                    out[out_len++] = next;
                    i++;
                    continue;
                }
            } else {
                /* Outside quotes, preserve escape for tokenizer */
                if (out_len + 2 >= out_cap) {
                    out_cap *= 2;
                    out = xrealloc(out, out_cap);
                }
                out[out_len++] = c;
                out[out_len++] = next;
                i++;
                continue;
            }
        }

        /* Variable expansion starts with $ */
        if (c == '$' && i + 1 < in_len) {
            char next = input[i + 1];

            /* $? -> last exit status */
            if (next == '?') {
                char status_str[32];
                snprintf(status_str, sizeof(status_str), "%d", last_exit_status);
                size_t slen = strlen(status_str);
                while (out_len + slen >= out_cap) {
                    out_cap *= 2;
                    out = xrealloc(out, out_cap);
                }
                memcpy(out + out_len, status_str, slen);
                out_len += slen;
                i++; /* skip '?' */
                continue;
            }

            /* $$ -> PID */
            if (next == '$') {
                char pid_str[32];
                snprintf(pid_str, sizeof(pid_str), "%d", (int)getpid());
                size_t plen = strlen(pid_str);
                while (out_len + plen >= out_cap) {
                    out_cap *= 2;
                    out = xrealloc(out, out_cap);
                }
                memcpy(out + out_len, pid_str, plen);
                out_len += plen;
                i++; /* skip '$' */
                continue;
            }

            /* ${VAR} syntax */
            if (next == '{') {
                size_t close_pos = i + 2;
                while (close_pos < in_len && input[close_pos] != '}') {
                    close_pos++;
                }
                if (close_pos < in_len && input[close_pos] == '}') {
                    size_t vlen = close_pos - (i + 2);
                    char var_name[256];
                    if (vlen < sizeof(var_name)) {
                        memcpy(var_name, input + i + 2, vlen);
                        var_name[vlen] = '\0';
                        const char *val = getenv(var_name);
                        if (!val) val = "";
                        size_t val_len = strlen(val);
                        while (out_len + val_len >= out_cap) {
                            out_cap *= 2;
                            out = xrealloc(out, out_cap);
                        }
                        memcpy(out + out_len, val, val_len);
                        out_len += val_len;
                        i = close_pos;
                        continue;
                    }
                }
            }

            /* $VAR syntax */
            if (isalpha((unsigned char)next) || next == '_') {
                size_t vstart = i + 1;
                size_t vend = vstart;
                while (vend < in_len && (isalnum((unsigned char)input[vend]) || input[vend] == '_')) {
                    vend++;
                }
                size_t vlen = vend - vstart;
                char var_name[256];
                if (vlen < sizeof(var_name)) {
                    memcpy(var_name, input + vstart, vlen);
                    var_name[vlen] = '\0';
                    const char *val = getenv(var_name);
                    if (!val) val = "";
                    size_t val_len = strlen(val);
                    while (out_len + val_len >= out_cap) {
                        out_cap *= 2;
                        out = xrealloc(out, out_cap);
                    }
                    memcpy(out + out_len, val, val_len);
                    out_len += val_len;
                    i = vend - 1;
                    continue;
                }
            }
        }

        /* Ordinary character */
        if (out_len + 1 >= out_cap) {
            out_cap *= 2;
            out = xrealloc(out, out_cap);
        }
        out[out_len++] = c;
    }

    out[out_len] = '\0';
    return out;
}

char **expansion_expand_glob(const char *token, int *out_count) {
    *out_count = 0;
    if (!token) return NULL;

    glob_t globbuf;
    int flags = GLOB_NOCHECK | GLOB_TILDE;
    int ret = glob(token, flags, NULL, &globbuf);

    if (ret != 0 || globbuf.gl_pathc == 0) {
        char **res = xmalloc(sizeof(char *) * 2);
        res[0] = xstrdup(token);
        res[1] = NULL;
        *out_count = 1;
        globfree(&globbuf);
        return res;
    }

    char **res = xmalloc(sizeof(char *) * (globbuf.gl_pathc + 1));
    for (size_t i = 0; i < globbuf.gl_pathc; i++) {
        res[i] = xstrdup(globbuf.gl_pathv[i]);
    }
    res[globbuf.gl_pathc] = NULL;
    *out_count = (int)globbuf.gl_pathc;

    globfree(&globbuf);
    return res;
}

void expansion_free_glob_list(char **list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}
