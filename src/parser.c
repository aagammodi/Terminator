#include "parser.h"
#include "expansion.h"
#include "util.h"
#include <ctype.h>

void command_init(Command *cmd) {
    if (!cmd) return;
    cmd->argv = NULL;
    cmd->argc = 0;
    cmd->argv_capacity = 0;
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->append_output = false;
}

void command_free(Command *cmd) {
    if (!cmd) return;
    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; i++) {
            free(cmd->argv[i]);
        }
        free(cmd->argv);
        cmd->argv = NULL;
    }
    free(cmd->input_file);
    cmd->input_file = NULL;
    free(cmd->output_file);
    cmd->output_file = NULL;
    cmd->argc = 0;
    cmd->argv_capacity = 0;
}

void command_add_arg(Command *cmd, const char *arg) {
    if (!cmd || !arg) return;
    if (cmd->argc + 2 > cmd->argv_capacity) {
        cmd->argv_capacity = (cmd->argv_capacity == 0) ? 16 : cmd->argv_capacity * 2;
        cmd->argv = xrealloc(cmd->argv, cmd->argv_capacity * sizeof(char *));
    }
    cmd->argv[cmd->argc++] = xstrdup(arg);
    cmd->argv[cmd->argc] = NULL;
}

Pipeline *pipeline_create(void) {
    Pipeline *p = xcalloc(1, sizeof(Pipeline));
    p->capacity = 8;
    p->num_commands = 0;
    p->commands = xcalloc(p->capacity, sizeof(Command));
    return p;
}

void pipeline_free(Pipeline *p) {
    if (!p) return;
    for (int i = 0; i < p->num_commands; i++) {
        command_free(&p->commands[i]);
    }
    free(p->commands);
    free(p);
}

bool parser_is_incomplete(const char *cmd_line) {
    if (!cmd_line) return false;

    size_t len = strlen(cmd_line);
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    int sub_paren_depth = 0;
    bool in_backticks = false;

    for (size_t i = 0; i < len; i++) {
        char c = cmd_line[i];

        if (escaped) {
            escaped = false;
            continue;
        }

        if (in_single_quote) {
            if (c == '\'') {
                in_single_quote = false;
            }
            continue;
        }

        if (in_double_quote) {
            if (c == '\\') {
                if (i + 1 < len) {
                    char next = cmd_line[i + 1];
                    if (next == '"' || next == '\\' || next == '$' || next == '`' || next == '\n') {
                        escaped = true;
                    }
                } else {
                    /* Trailing backslash in double quotes */
                    return true;
                }
            } else if (c == '"') {
                in_double_quote = false;
            } else if (c == '`') {
                in_backticks = !in_backticks;
            } else if (c == '$' && i + 1 < len && cmd_line[i + 1] == '(') {
                sub_paren_depth++;
                i++;
            } else if (c == ')' && sub_paren_depth > 0) {
                sub_paren_depth--;
            }
            continue;
        }

        /* Outside quotes */
        if (c == '\\') {
            if (i + 1 == len) {
                /* Trailing backslash at end of input */
                return true;
            }
            escaped = true;
        } else if (c == '\'') {
            in_single_quote = true;
        } else if (c == '"') {
            in_double_quote = true;
        } else if (c == '`') {
            in_backticks = !in_backticks;
        } else if (c == '$' && i + 1 < len && cmd_line[i + 1] == '(') {
            sub_paren_depth++;
            i++;
        } else if (c == ')' && sub_paren_depth > 0) {
            sub_paren_depth--;
        }
    }

    if (escaped) return true;
    if (in_single_quote) return true;
    if (in_double_quote) return true;
    if (sub_paren_depth > 0) return true;
    if (in_backticks) return true;

    return false;
}

/* Helper to read next token or filename, with quote stripping */
static char *parse_next_word(const char **cursor, bool *has_wildcard) {
    const char *p = *cursor;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return NULL;
    }

    /* Redirection operators are handled separately */
    if (*p == '<' || *p == '>' || *p == '|') {
        *cursor = p;
        return NULL;
    }

    size_t cap = 64;
    size_t len = 0;
    char *word = xmalloc(cap);

    bool in_single_quote = false;
    bool in_double_quote = false;
    if (has_wildcard) *has_wildcard = false;

    while (*p) {
        char c = *p;

        if (!in_single_quote && !in_double_quote && (isspace((unsigned char)c) || c == '<' || c == '>' || c == '|')) {
            break;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            p++;
            continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            p++;
            continue;
        }

        if (c == '\\' && !in_single_quote) {
            p++;
            if (*p) {
                c = *p++;
            } else {
                break;
            }
        } else {
            p++;
            if (!in_single_quote && !in_double_quote) {
                if (c == '*' || c == '?' || c == '[') {
                    if (has_wildcard) *has_wildcard = true;
                }
            }
        }

        if (len + 1 >= cap) {
            cap *= 2;
            word = xrealloc(word, cap);
        }
        word[len++] = c;
    }

    word[len] = '\0';
    *cursor = p;
    return word;
}

Pipeline *parser_parse_pipeline(const char *cmd_line, int last_exit_status) {
    if (!cmd_line) return NULL;

    /* 1. Command substitution $(...) and `...` */
    char *subbed = expansion_process_command_substitutions(cmd_line, last_exit_status);
    if (!subbed) return NULL;

    /* 2. Variable expansion $VAR and $? */
    char *expanded = expansion_expand_variables(subbed, last_exit_status);
    free(subbed);
    if (!expanded) return NULL;

    Pipeline *pipeline = pipeline_create();

    /* 3. Split by unquoted pipes | */
    const char *p = expanded;
    bool in_single = false;
    bool in_double = false;
    const char *stage_start = p;

    while (1) {
        char c = *p;
        bool is_pipe = false;

        if (c == '\0') {
            is_pipe = true;
        } else if (c == '\\' && !in_single && *(p + 1) != '\0') {
            p += 2;
            continue;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == '|' && !in_single && !in_double) {
            is_pipe = true;
        }

        if (is_pipe) {
            /* Extract stage substring */
            size_t stage_len = p - stage_start;
            char *stage_str = xmalloc(stage_len + 1);
            memcpy(stage_str, stage_start, stage_len);
            stage_str[stage_len] = '\0';

            /* Parse stage into Command */
            if (pipeline->num_commands >= MAX_PIPELINE_CMDS) {
                fprintf(stderr, "terminator: pipeline exceeds maximum stages (%d)\n", MAX_PIPELINE_CMDS);
                free(stage_str);
                pipeline_free(pipeline);
                free(expanded);
                return NULL;
            }

            if (pipeline->num_commands >= pipeline->capacity) {
                pipeline->capacity *= 2;
                pipeline->commands = xrealloc(pipeline->commands, pipeline->capacity * sizeof(Command));
            }
            Command *cmd = &pipeline->commands[pipeline->num_commands];
            command_init(cmd);

            const char *sp = stage_str;
            while (*sp) {
                while (*sp && isspace((unsigned char)*sp)) sp++;
                if (!*sp) break;

                if (*sp == '<') {
                    sp++;
                    while (*sp && isspace((unsigned char)*sp)) sp++;
                    bool wild = false;
                    char *infile = parse_next_word(&sp, &wild);
                    if (infile) {
                        free(cmd->input_file);
                        cmd->input_file = infile;
                    }
                } else if (*sp == '>') {
                    sp++;
                    if (*sp == '>') {
                        sp++;
                        cmd->append_output = true;
                    } else {
                        cmd->append_output = false;
                    }
                    while (*sp && isspace((unsigned char)*sp)) sp++;
                    bool wild = false;
                    char *outfile = parse_next_word(&sp, &wild);
                    if (outfile) {
                        free(cmd->output_file);
                        cmd->output_file = outfile;
                    }
                } else {
                    bool wild = false;
                    char *arg = parse_next_word(&sp, &wild);
                    if (arg) {
                        if (wild) {
                            int gcount = 0;
                            char **glob_matches = expansion_expand_glob(arg, &gcount);
                            for (int gi = 0; gi < gcount; gi++) {
                                command_add_arg(cmd, glob_matches[gi]);
                            }
                            expansion_free_glob_list(glob_matches, gcount);
                            free(arg);
                        } else {
                            command_add_arg(cmd, arg);
                            free(arg);
                        }
                    }
                }
            }

            free(stage_str);

            if (cmd->argc > 0 || cmd->input_file || cmd->output_file) {
                pipeline->num_commands++;
            } else {
                command_free(cmd);
            }

            if (*p == '\0') {
                break;
            }
            p++; /* skip '|' */
            stage_start = p;
        } else {
            p++;
        }
    }

    free(expanded);
    return pipeline;
}
