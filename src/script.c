#include "script.h"
#include "util.h"
#include "expansion.h"
#include "executor.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

typedef enum {
    STMT_CMD,
    STMT_ASSIGN,
    STMT_IF,
    STMT_EXIT,
} StmtType;

typedef struct Stmt Stmt;

typedef struct IfBranch {
    char condition[MAX_CMD_LEN];
    int cond_line;
    Stmt **body;
    int body_count;
    int body_cap;
} IfBranch;

typedef struct IfStmt {
    IfBranch *branches;
    int branch_count;
    int branch_cap;
    int start_line;
} IfStmt;

struct Stmt {
    StmtType type;
    int line_num;
    union {
        char cmd[MAX_CMD_LEN];
        struct {
            char name[64];
            char val[512];
        } assign;
        int exit_code;
        IfStmt if_stmt;
    } u;
};

typedef struct LogicalLine {
    int line_num;
    char text[MAX_CMD_LEN];
} LogicalLine;

typedef struct ParseContext {
    const char *filename;
    LogicalLine *lines;
    int num_lines;
    int current_line_idx;
    char err_buf[512];
    int err_line;
} ParseContext;

static void free_stmts(Stmt **stmts, int count);

static void free_if_stmt(IfStmt *if_stmt) {
    if (!if_stmt || !if_stmt->branches) return;
    for (int i = 0; i < if_stmt->branch_count; i++) {
        free_stmts(if_stmt->branches[i].body, if_stmt->branches[i].body_count);
    }
    free(if_stmt->branches);
    if_stmt->branches = NULL;
    if_stmt->branch_count = 0;
}

static void free_stmts(Stmt **stmts, int count) {
    if (!stmts) return;
    for (int i = 0; i < count; i++) {
        if (stmts[i]) {
            if (stmts[i]->type == STMT_IF) {
                free_if_stmt(&stmts[i]->u.if_stmt);
            }
            free(stmts[i]);
        }
    }
    free(stmts);
}

static void add_stmt(Stmt ***stmts, int *count, int *cap, Stmt *stmt) {
    if (*count >= *cap) {
        *cap = (*cap == 0) ? 16 : (*cap * 2);
        *stmts = xrealloc(*stmts, (size_t)*cap * sizeof(Stmt *));
    }
    (*stmts)[(*count)++] = stmt;
}

static void add_branch(IfStmt *if_stmt, const char *cond, int cond_line) {
    if (if_stmt->branch_count >= if_stmt->branch_cap) {
        if_stmt->branch_cap = (if_stmt->branch_cap == 0) ? 4 : (if_stmt->branch_cap * 2);
        if_stmt->branches = xrealloc(if_stmt->branches, (size_t)if_stmt->branch_cap * sizeof(IfBranch));
    }
    IfBranch *b = &if_stmt->branches[if_stmt->branch_count++];
    memset(b, 0, sizeof(IfBranch));
    safe_strncpy(b->condition, cond ? cond : "", sizeof(b->condition));
    b->cond_line = cond_line;
    b->body = NULL;
    b->body_count = 0;
    b->body_cap = 0;
}

static void resolve_path(const char *path, const char *cwd, char *out, size_t out_size) {
    if (!path || !out) return;
    char expanded[MAX_PATH_LEN];
    path_expand_tilde(path, expanded, sizeof(expanded));
    if (expanded[0] == '/') {
        safe_strncpy(out, expanded, out_size);
    } else if (cwd && cwd[0]) {
        snprintf(out, out_size, "%s/%s", cwd, expanded);
    } else {
        safe_strncpy(out, expanded, out_size);
    }
}

static bool is_bash_shebang(const char *header, size_t len) {
    if (len < 2 || header[0] != '#' || header[1] != '!') {
        return false;
    }
    char line[256];
    size_t copy_len = (len < sizeof(line) - 1) ? len : sizeof(line) - 1;
    memcpy(line, header, copy_len);
    line[copy_len] = '\0';
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    if (strstr(line, "bash") != NULL) return true;
    if (strstr(line, "dash") != NULL) return true;

    /* Check for /sh or env sh with boundary check */
    const char *p = strstr(line, "/sh");
    if (p) {
        char next = p[3];
        if (next == '\0' || next == ' ' || next == '\t' || next == '\r') {
            return true;
        }
    }
    p = strstr(line, " sh");
    if (p) {
        char next = p[3];
        if (next == '\0' || next == ' ' || next == '\t' || next == '\r') {
            return true;
        }
    }
    return false;
}

bool script_is_script_invocation(const Command *cmd, const char *cwd,
                                 char *out_filepath, size_t out_size,
                                 bool *out_is_source) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) return false;
    const char *prog = cmd->argv[0];
    if (out_is_source) *out_is_source = false;

    /* Form B: "bash script.sh [args...]" */
    if (strcmp(prog, "bash") == 0) {
        if (cmd->argc >= 2) {
            resolve_path(cmd->argv[1], cwd, out_filepath, out_size);
            return true;
        }
        return false;
    }

    /* Form C: "source script.sh [args...]" or ". script.sh [args...]" */
    if (strcmp(prog, "source") == 0 || strcmp(prog, ".") == 0) {
        if (cmd->argc >= 2) {
            resolve_path(cmd->argv[1], cwd, out_filepath, out_size);
            if (out_is_source) *out_is_source = true;
            return true;
        }
        return false;
    }

    /* Form A: "./script.sh", "path/to/script.sh", or executable script file */
    size_t prog_len = strlen(prog);
    bool has_sh_ext = (prog_len >= 3 && strcmp(prog + prog_len - 3, ".sh") == 0);
    bool has_slash = (strchr(prog, '/') != NULL);

    if (has_sh_ext || has_slash) {
        char resolved[MAX_PATH_LEN * 2];
        resolve_path(prog, cwd, resolved, sizeof(resolved));

        struct stat st;
        if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
            FILE *fp = fopen(resolved, "rb");
            if (fp) {
                char header[256] = {0};
                size_t read_bytes = fread(header, 1, sizeof(header) - 1, fp);
                fclose(fp);

                /* Compiled ELF binary: execute through execvp */
                if (read_bytes >= 4 && memcmp(header, "\x7f\x45\x4c\x46", 4) == 0) {
                    return false;
                }

                if (has_sh_ext) {
                    /* Ends in .sh and is not an ELF binary: execute via script interpreter */
                    safe_strncpy(out_filepath, resolved, out_size);
                    return true;
                }

                /* If does not end in .sh, only treat as script if shebang indicates bash/sh */
                if (is_bash_shebang(header, read_bytes)) {
                    safe_strncpy(out_filepath, resolved, out_size);
                    return true;
                }

                /* Non-bash shebang (e.g. perl, python) or binary without .sh: execute via execvp */
                return false;
            }
        }
    }

    return false;
}

static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (*str && isspace((unsigned char)*str)) str++;
    if (!*str) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

static bool tokenize_condition(const char *cond_str, char tokens[16][MAX_PATH_LEN], int *num_tokens) {
    *num_tokens = 0;
    const char *p = cond_str;
    while (*p && isspace((unsigned char)*p)) p++;

    while (*p && *num_tokens < 16) {
        char tok[MAX_PATH_LEN];
        int tlen = 0;
        char quote = '\0';

        if (*p == '\'' || *p == '"') {
            quote = *p++;
            while (*p && *p != quote && tlen < (int)sizeof(tok) - 1) {
                if (*p == '\\' && quote == '"' && *(p + 1)) {
                    p++;
                }
                tok[tlen++] = *p++;
            }
            if (*p == quote) p++;
        } else {
            while (*p && !isspace((unsigned char)*p) && tlen < (int)sizeof(tok) - 1) {
                tok[tlen++] = *p++;
            }
        }
        tok[tlen] = '\0';
        safe_strncpy(tokens[(*num_tokens)++], tok, MAX_PATH_LEN);
        while (*p && isspace((unsigned char)*p)) p++;
    }
    return true;
}

static bool validate_test_condition(const char *raw_cond, char *err_buf, size_t err_size) {
    char cond_copy[MAX_CMD_LEN];
    safe_strncpy(cond_copy, raw_cond, sizeof(cond_copy));
    char *trimmed = trim_whitespace(cond_copy);

    if (trimmed[0] != '[') {
        snprintf(err_buf, err_size, "expected '[' in conditional expression");
        return false;
    }
    size_t len = strlen(trimmed);
    if (len < 2 || trimmed[len - 1] != ']') {
        snprintf(err_buf, err_size, "missing closing ']' in conditional expression");
        return false;
    }

    /* Strip outer brackets */
    trimmed[len - 1] = '\0';
    char *inner = trim_whitespace(trimmed + 1);

    char tokens[16][MAX_PATH_LEN];
    int ntokens = 0;
    tokenize_condition(inner, tokens, &ntokens);

    if (ntokens == 0) {
        snprintf(err_buf, err_size, "empty test expression inside [ ]");
        return false;
    }

    if (strcmp(tokens[0], "-f") == 0 || strcmp(tokens[0], "-d") == 0) {
        if (ntokens != 2) {
            snprintf(err_buf, err_size, "%.32s operator requires exactly 1 operand", tokens[0]);
            return false;
        }
        return true;
    }

    if (ntokens == 3) {
        if (strcmp(tokens[1], "-eq") == 0 || strcmp(tokens[1], "-gt") == 0 || strcmp(tokens[1], "=") == 0) {
            return true;
        }
    }

    snprintf(err_buf, err_size, "unsupported or malformed test expression '%.100s'", inner);
    return false;
}

static bool parse_stmts(ParseContext *ctx, Stmt ***out_stmts, int *out_count, const char *stop_tokens);

static bool parse_if_stmt(ParseContext *ctx, Stmt **out_stmt) {
    LogicalLine *cur = &ctx->lines[ctx->current_line_idx];
    char line_buf[MAX_CMD_LEN];
    safe_strncpy(line_buf, cur->text, sizeof(line_buf));
    char *trimmed = trim_whitespace(line_buf);

    int start_line = cur->line_num;
    ctx->current_line_idx++;

    /* Extract condition from "if [ ... ]; then" or "if [ ... ]" */
    char *open_br = strchr(trimmed, '[');
    char *close_br = strrchr(trimmed, ']');
    if (!open_br || !close_br || close_br <= open_br) {
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "malformed 'if' condition");
        ctx->err_line = start_line;
        return false;
    }

    char raw_cond[MAX_CMD_LEN];
    int cond_len = (int)(close_br - open_br + 1);
    memcpy(raw_cond, open_br, (size_t)cond_len);
    raw_cond[cond_len] = '\0';

    char test_err[256];
    if (!validate_test_condition(raw_cond, test_err, sizeof(test_err))) {
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "%s", test_err);
        ctx->err_line = start_line;
        return false;
    }

    /* Check 'then' */
    bool has_then = (strstr(close_br, "then") != NULL);
    if (!has_then) {
        if (ctx->current_line_idx < ctx->num_lines) {
            char next_buf[MAX_CMD_LEN];
            safe_strncpy(next_buf, ctx->lines[ctx->current_line_idx].text, sizeof(next_buf));
            char *next_trimmed = trim_whitespace(next_buf);
            if (strcmp(next_trimmed, "then") == 0) {
                ctx->current_line_idx++;
                has_then = true;
            }
        }
    }
    if (!has_then) {
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "missing 'then' after 'if'");
        ctx->err_line = start_line;
        return false;
    }

    IfStmt if_stmt;
    memset(&if_stmt, 0, sizeof(IfStmt));
    if_stmt.start_line = start_line;
    add_branch(&if_stmt, raw_cond, start_line);

    /* Parse branch 0 body */
    IfBranch *b0 = &if_stmt.branches[0];
    if (!parse_stmts(ctx, &b0->body, &b0->body_count, "elif|else|fi")) {
        free_if_stmt(&if_stmt);
        return false;
    }

    /* Handle elif / else / fi chains */
    while (ctx->current_line_idx < ctx->num_lines) {
        LogicalLine *next_line = &ctx->lines[ctx->current_line_idx];
        char nbuf[MAX_CMD_LEN];
        safe_strncpy(nbuf, next_line->text, sizeof(nbuf));
        char *ntrim = trim_whitespace(nbuf);

        if (strncmp(ntrim, "elif", 4) == 0 && (isspace((unsigned char)ntrim[4]) || ntrim[4] == '[')) {
            int elif_line = next_line->line_num;
            ctx->current_line_idx++;

            char *e_open = strchr(ntrim, '[');
            char *e_close = strrchr(ntrim, ']');
            if (!e_open || !e_close || e_close <= e_open) {
                snprintf(ctx->err_buf, sizeof(ctx->err_buf), "malformed 'elif' condition");
                ctx->err_line = elif_line;
                free_if_stmt(&if_stmt);
                return false;
            }
            int e_cond_len = (int)(e_close - e_open + 1);
            char elif_cond[MAX_CMD_LEN];
            memcpy(elif_cond, e_open, (size_t)e_cond_len);
            elif_cond[e_cond_len] = '\0';

            if (!validate_test_condition(elif_cond, test_err, sizeof(test_err))) {
                snprintf(ctx->err_buf, sizeof(ctx->err_buf), "%s", test_err);
                ctx->err_line = elif_line;
                free_if_stmt(&if_stmt);
                return false;
            }

            bool elif_has_then = (strstr(e_close, "then") != NULL);
            if (!elif_has_then && ctx->current_line_idx < ctx->num_lines) {
                char tbuf[MAX_CMD_LEN];
                safe_strncpy(tbuf, ctx->lines[ctx->current_line_idx].text, sizeof(tbuf));
                char *ttrim = trim_whitespace(tbuf);
                if (strcmp(ttrim, "then") == 0) {
                    ctx->current_line_idx++;
                    elif_has_then = true;
                }
            }
            if (!elif_has_then) {
                snprintf(ctx->err_buf, sizeof(ctx->err_buf), "missing 'then' after 'elif'");
                ctx->err_line = elif_line;
                free_if_stmt(&if_stmt);
                return false;
            }

            add_branch(&if_stmt, elif_cond, elif_line);
            IfBranch *b_elif = &if_stmt.branches[if_stmt.branch_count - 1];
            if (!parse_stmts(ctx, &b_elif->body, &b_elif->body_count, "elif|else|fi")) {
                free_if_stmt(&if_stmt);
                return false;
            }
            continue;
        }

        if (strcmp(ntrim, "else") == 0) {
            int else_line = next_line->line_num;
            ctx->current_line_idx++;

            add_branch(&if_stmt, "", else_line);
            IfBranch *b_else = &if_stmt.branches[if_stmt.branch_count - 1];
            if (!parse_stmts(ctx, &b_else->body, &b_else->body_count, "fi")) {
                free_if_stmt(&if_stmt);
                return false;
            }
            continue;
        }

        if (strcmp(ntrim, "fi") == 0) {
            ctx->current_line_idx++;
            /* Successfully closed if block */
            Stmt *st = xcalloc(1, sizeof(Stmt));
            st->type = STMT_IF;
            st->line_num = start_line;
            st->u.if_stmt = if_stmt;
            *out_stmt = st;
            return true;
        }

        /* Unexpected token */
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "unexpected statement '%s' inside 'if' block", ntrim);
        ctx->err_line = next_line->line_num;
        free_if_stmt(&if_stmt);
        return false;
    }

    snprintf(ctx->err_buf, sizeof(ctx->err_buf), "unexpected end of file, expecting 'fi'");
    ctx->err_line = start_line;
    free_if_stmt(&if_stmt);
    return false;
}

static bool parse_stmts(ParseContext *ctx, Stmt ***out_stmts, int *out_count, const char *stop_tokens) {
    int cap = 16;
    *out_count = 0;
    *out_stmts = xcalloc((size_t)cap, sizeof(Stmt *));

    while (ctx->current_line_idx < ctx->num_lines) {
        LogicalLine *cur = &ctx->lines[ctx->current_line_idx];
        char line_buf[MAX_CMD_LEN];
        safe_strncpy(line_buf, cur->text, sizeof(line_buf));
        char *trimmed = trim_whitespace(line_buf);

        if (*trimmed == '\0') {
            ctx->current_line_idx++;
            continue;
        }

        /* Check stop tokens */
        if (stop_tokens) {
            if (strstr(stop_tokens, "fi") && strcmp(trimmed, "fi") == 0) {
                return true;
            }
            if (strstr(stop_tokens, "else") && strcmp(trimmed, "else") == 0) {
                return true;
            }
            if (strstr(stop_tokens, "elif") && strncmp(trimmed, "elif", 4) == 0 &&
                (isspace((unsigned char)trimmed[4]) || trimmed[4] == '[')) {
                return true;
            }
        }

        /* If encounters stray fi, else, or elif */
        if (strcmp(trimmed, "fi") == 0) {
            snprintf(ctx->err_buf, sizeof(ctx->err_buf), "unexpected 'fi' without matching 'if'");
            ctx->err_line = cur->line_num;
            return false;
        }
        if (strcmp(trimmed, "else") == 0) {
            snprintf(ctx->err_buf, sizeof(ctx->err_buf), "unexpected 'else' without matching 'if'");
            ctx->err_line = cur->line_num;
            return false;
        }
        if (strncmp(trimmed, "elif", 4) == 0 && (isspace((unsigned char)trimmed[4]) || trimmed[4] == '[')) {
            snprintf(ctx->err_buf, sizeof(ctx->err_buf), "unexpected 'elif' without matching 'if'");
            ctx->err_line = cur->line_num;
            return false;
        }

        /* 1. If statement */
        if (strncmp(trimmed, "if", 2) == 0 && (isspace((unsigned char)trimmed[2]) || trimmed[2] == '[')) {
            Stmt *if_stmt_node = NULL;
            if (!parse_if_stmt(ctx, &if_stmt_node)) {
                return false;
            }
            add_stmt(out_stmts, out_count, &cap, if_stmt_node);
            continue;
        }

        /* 2. Exit statement */
        if (strcmp(trimmed, "exit") == 0 || (strncmp(trimmed, "exit", 4) == 0 && isspace((unsigned char)trimmed[4]))) {
            Stmt *st = xcalloc(1, sizeof(Stmt));
            st->type = STMT_EXIT;
            st->line_num = cur->line_num;
            char *arg = trim_whitespace(trimmed + 4);
            if (*arg) {
                st->u.exit_code = atoi(arg);
            } else {
                st->u.exit_code = -1;
            }
            add_stmt(out_stmts, out_count, &cap, st);
            ctx->current_line_idx++;
            continue;
        }

        /* 3. Variable assignment: NAME=val */
        char *eq = strchr(trimmed, '=');
        if (eq && eq > trimmed) {
            bool valid_ident = (isalpha((unsigned char)trimmed[0]) || trimmed[0] == '_');
            for (char *c = trimmed + 1; c < eq && valid_ident; c++) {
                if (!isalnum((unsigned char)*c) && *c != '_') valid_ident = false;
            }
            if (valid_ident) {
                Stmt *st = xcalloc(1, sizeof(Stmt));
                st->type = STMT_ASSIGN;
                st->line_num = cur->line_num;

                size_t nlen = (size_t)(eq - trimmed);
                if (nlen >= sizeof(st->u.assign.name)) nlen = sizeof(st->u.assign.name) - 1;
                memcpy(st->u.assign.name, trimmed, nlen);
                st->u.assign.name[nlen] = '\0';

                safe_strncpy(st->u.assign.val, eq + 1, sizeof(st->u.assign.val));
                add_stmt(out_stmts, out_count, &cap, st);
                ctx->current_line_idx++;
                continue;
            }
        }

        /* 4. Ordinary command */
        Stmt *st = xcalloc(1, sizeof(Stmt));
        st->type = STMT_CMD;
        st->line_num = cur->line_num;
        safe_strncpy(st->u.cmd, trimmed, sizeof(st->u.cmd));
        add_stmt(out_stmts, out_count, &cap, st);
        ctx->current_line_idx++;
    }

    if (stop_tokens) {
        snprintf(ctx->err_buf, sizeof(ctx->err_buf), "unexpected end of file, expecting '%s'", stop_tokens);
        ctx->err_line = (ctx->num_lines > 0) ? ctx->lines[ctx->num_lines - 1].line_num : 1;
        return false;
    }

    return true;
}

static bool load_script_lines(const char *filepath, LogicalLine **out_lines, int *out_count, char *err_buf, size_t err_size, int *err_line) {
    if (err_line) *err_line = 1;
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        snprintf(err_buf, err_size, "%s: %s", filepath, strerror(errno));
        return false;
    }

    int cap = 32;
    *out_count = 0;
    *out_lines = xcalloc((size_t)cap, sizeof(LogicalLine));

    char raw_buf[MAX_INPUT_LEN];
    int cur_line = 0;
    char acc_buf[MAX_CMD_LEN] = {0};
    int acc_start_line = 0;
    bool in_sq = false;
    bool in_dq = false;
    bool trailing_bs = false;

    while (fgets(raw_buf, sizeof(raw_buf), fp)) {
        cur_line++;
        size_t rlen = strlen(raw_buf);
        while (rlen > 0 && (raw_buf[rlen - 1] == '\n' || raw_buf[rlen - 1] == '\r')) {
            raw_buf[--rlen] = '\0';
        }

        /* Skip pure comment lines when not accumulating multiline */
        if (acc_buf[0] == '\0') {
            const char *p = raw_buf;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == '#') {
                continue;
            }
            acc_start_line = cur_line;
        }

        /* Check trailing backslash continuation */
        trailing_bs = false;
        if (rlen > 0 && raw_buf[rlen - 1] == '\\') {
            size_t bc = 0;
            while (rlen > bc && raw_buf[rlen - 1 - bc] == '\\') bc++;
            if (bc % 2 == 1) {
                trailing_bs = true;
                raw_buf[rlen - 1] = '\0';
            }
        }

        if (acc_buf[0] != '\0') {
            strncat(acc_buf, " ", sizeof(acc_buf) - strlen(acc_buf) - 1);
            strncat(acc_buf, raw_buf, sizeof(acc_buf) - strlen(acc_buf) - 1);
        } else {
            safe_strncpy(acc_buf, raw_buf, sizeof(acc_buf));
        }

        /* Check if multiline quotes are open */
        in_sq = false;
        in_dq = false;
        size_t acc_len = strlen(acc_buf);
        for (size_t i = 0; i < acc_len; i++) {
            if (acc_buf[i] == '\'' && !in_dq) in_sq = !in_sq;
            else if (acc_buf[i] == '"' && !in_sq) in_dq = !in_dq;
        }

        if (trailing_bs || in_sq || in_dq) {
            continue;
        }

        if (*out_count >= cap) {
            cap *= 2;
            *out_lines = xrealloc(*out_lines, (size_t)cap * sizeof(LogicalLine));
        }
        (*out_lines)[*out_count].line_num = acc_start_line;
        safe_strncpy((*out_lines)[*out_count].text, acc_buf, MAX_CMD_LEN);
        (*out_count)++;

        acc_buf[0] = '\0';
    }

    fclose(fp);

    if (in_sq || in_dq || trailing_bs) {
        if (err_line) *err_line = acc_start_line;
        snprintf(err_buf, err_size, "line %d: syntax error: unexpected end of file while looking for matching quote or continuation", acc_start_line);
        free(*out_lines);
        *out_lines = NULL;
        *out_count = 0;
        return false;
    }

    if (acc_buf[0] != '\0') {
        if (*out_count >= cap) {
            cap *= 2;
            *out_lines = xrealloc(*out_lines, (size_t)cap * sizeof(LogicalLine));
        }
        (*out_lines)[*out_count].line_num = acc_start_line;
        safe_strncpy((*out_lines)[*out_count].text, acc_buf, MAX_CMD_LEN);
        (*out_count)++;
    }

    return true;
}

int script_validate_file(const char *filepath, char *err_buf, size_t err_size, int *err_line) {
    if (err_line) *err_line = 1;
    LogicalLine *lines = NULL;
    int num_lines = 0;
    if (!load_script_lines(filepath, &lines, &num_lines, err_buf, err_size, err_line)) {
        return 1;
    }

    ParseContext ctx;
    memset(&ctx, 0, sizeof(ParseContext));
    ctx.filename = filepath;
    ctx.lines = lines;
    ctx.num_lines = num_lines;
    ctx.current_line_idx = 0;

    Stmt **stmts = NULL;
    int stmt_count = 0;
    bool ok = parse_stmts(&ctx, &stmts, &stmt_count, NULL);

    if (!ok) {
        safe_strncpy(err_buf, ctx.err_buf, err_size);
        if (err_line) *err_line = ctx.err_line;
    }

    free_stmts(stmts, stmt_count);
    free(lines);

    return ok ? 0 : 2;
}

static bool evaluate_test_condition(const char *raw_cond, Tab *tab, const ScriptVarContext *ctx) {
    char cond_buf[MAX_CMD_LEN];
    safe_strncpy(cond_buf, raw_cond, sizeof(cond_buf));
    char *trimmed = trim_whitespace(cond_buf);

    size_t len = strlen(trimmed);
    if (trimmed[0] == '[' && trimmed[len - 1] == ']') {
        trimmed[len - 1] = '\0';
        trimmed++;
    }
    trimmed = trim_whitespace(trimmed);

    /* Expand variables inside condition */
    char *expanded = expansion_expand_variables_ex(trimmed, tab->last_exit_status, ctx, tab);
    if (!expanded) return false;

    char tokens[16][MAX_PATH_LEN];
    int ntokens = 0;
    tokenize_condition(expanded, tokens, &ntokens);
    free(expanded);

    if (ntokens == 2) {
        char resolved[MAX_PATH_LEN * 2];
        resolve_path(tokens[1], tab->cwd, resolved, sizeof(resolved));
        struct stat st;
        int stat_res = stat(resolved, &st);

        if (strcmp(tokens[0], "-f") == 0) {
            return (stat_res == 0 && S_ISREG(st.st_mode));
        } else if (strcmp(tokens[0], "-d") == 0) {
            return (stat_res == 0 && S_ISDIR(st.st_mode));
        }
    } else if (ntokens == 3) {
        if (strcmp(tokens[1], "-eq") == 0) {
            long a = atol(tokens[0]);
            long b = atol(tokens[2]);
            return (a == b);
        } else if (strcmp(tokens[1], "-gt") == 0) {
            long a = atol(tokens[0]);
            long b = atol(tokens[2]);
            return (a > b);
        } else if (strcmp(tokens[1], "=") == 0) {
            return (strcmp(tokens[0], tokens[2]) == 0);
        }
    }

    return false;
}

static void execute_stmt_list(Tab *tab, Stmt **stmts, int count, ScriptVarContext *ctx,
                              bool is_source, int read_ipc_fd, int write_ipc_fd,
                              bool *should_exit, int *exit_code) {
    (void)read_ipc_fd;
    for (int i = 0; i < count && !*should_exit; i++) {
        Stmt *st = stmts[i];
        if (!st) continue;

        if (st->type == STMT_ASSIGN) {
            char *expanded_val = expansion_expand_variables_ex(st->u.assign.val, tab->last_exit_status, ctx, tab);
            const char *val_to_store = expanded_val ? expanded_val : "";

            /* Strip enclosing quotes if present */
            size_t vlen = strlen(val_to_store);
            char clean_val[512];
            if (vlen >= 2 && ((val_to_store[0] == '"' && val_to_store[vlen - 1] == '"') ||
                             (val_to_store[0] == '\'' && val_to_store[vlen - 1] == '\''))) {
                memcpy(clean_val, val_to_store + 1, vlen - 2);
                clean_val[vlen - 2] = '\0';
            } else {
                safe_strncpy(clean_val, val_to_store, sizeof(clean_val));
            }

            /* Update script context variables */
            bool updated = false;
            for (int j = 0; j < ctx->num_vars; j++) {
                if (strcmp(ctx->vars[j], st->u.assign.name) == 0) {
                    safe_strncpy(ctx->vals[j], clean_val, sizeof(ctx->vals[j]));
                    updated = true;
                    break;
                }
            }
            if (!updated && ctx->num_vars < 128) {
                safe_strncpy(ctx->vars[ctx->num_vars], st->u.assign.name, sizeof(ctx->vars[ctx->num_vars]));
                safe_strncpy(ctx->vals[ctx->num_vars], clean_val, sizeof(ctx->vals[ctx->num_vars]));
                ctx->num_vars++;
            }

            if (is_source) {
                terminal_set_var(tab, st->u.assign.name, clean_val);
            }
            free(expanded_val);
            tab->last_exit_status = 0;
            continue;
        }

        if (st->type == STMT_EXIT) {
            *should_exit = true;
            *exit_code = (st->u.exit_code >= 0) ? st->u.exit_code : tab->last_exit_status;
            tab->last_exit_status = *exit_code;
            break;
        }

        if (st->type == STMT_CMD) {
            int lines_before = tab->num_lines;
            int ret = executor_run_line(tab, st->u.cmd);
            if (write_ipc_fd >= 0) {
                for (int l = lines_before; l < tab->num_lines; l++) {
                    const char *t = tab->lines[l].text;
                    size_t tlen = strlen(t);
                    if (tlen > 0) {
                        ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, t, (uint32_t)tlen);
                    }
                    ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, "\n", 1);
                }
            }
            tab->last_exit_status = ret;
            continue;
        }

        if (st->type == STMT_IF) {
            IfStmt *ifs = &st->u.if_stmt;
            bool branch_executed = false;

            for (int b = 0; b < ifs->branch_count && !branch_executed && !*should_exit; b++) {
                IfBranch *branch = &ifs->branches[b];
                if (branch->condition[0] != '\0') {
                    if (evaluate_test_condition(branch->condition, tab, ctx)) {
                        branch_executed = true;
                        execute_stmt_list(tab, branch->body, branch->body_count, ctx, is_source, read_ipc_fd, write_ipc_fd, should_exit, exit_code);
                    }
                } else {
                    /* 'else' branch */
                    branch_executed = true;
                    execute_stmt_list(tab, branch->body, branch->body_count, ctx, is_source, read_ipc_fd, write_ipc_fd, should_exit, exit_code);
                }
            }
        }
    }
}

int script_execute_file(Tab *tab, const char *filepath, int argc, char **argv,
                        bool is_source, int read_ipc_fd, int write_ipc_fd) {
    if (!tab || !filepath) return 1;

    /* Phase 1: Validate entire script syntax before executing any command */
    char err_buf[512] = {0};
    int err_line = 1;
    int val_ret = script_validate_file(filepath, err_buf, sizeof(err_buf), &err_line);
    if (val_ret != 0) {
        char full_err[MAX_CMD_LEN];
        snprintf(full_err, sizeof(full_err), "terminator: %s: line %d: syntax error: %s\n",
                 filepath, err_line, err_buf[0] ? err_buf : "syntax error");
        if (write_ipc_fd >= 0) {
            ipc_send_msg(write_ipc_fd, IPC_MSG_OUTPUT, full_err, (uint32_t)strlen(full_err));
        } else {
            fprintf(stderr, "%s", full_err);
            fflush(stderr);
        }
        tab->last_exit_status = 2;
        return 2;
    }

    /* Phase 2: Parse validated script into AST and execute */
    LogicalLine *lines = NULL;
    int num_lines = 0;
    if (!load_script_lines(filepath, &lines, &num_lines, err_buf, sizeof(err_buf), NULL)) {
        fprintf(stderr, "terminator: %s: %s\n", filepath, err_buf);
        tab->last_exit_status = 1;
        return 1;
    }

    ParseContext pctx;
    memset(&pctx, 0, sizeof(ParseContext));
    pctx.filename = filepath;
    pctx.lines = lines;
    pctx.num_lines = num_lines;
    pctx.current_line_idx = 0;

    Stmt **stmts = NULL;
    int stmt_count = 0;
    parse_stmts(&pctx, &stmts, &stmt_count, NULL);

    /* Setup script variable and positional arguments context */
    ScriptVarContext ctx;
    memset(&ctx, 0, sizeof(ScriptVarContext));

    int start_arg = 0;
    if (argc > 0 && argv && argv[0]) {
        if (strcmp(argv[0], "bash") == 0 || strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
            start_arg = (argc >= 2) ? 2 : argc;
        } else if (strcmp(argv[0], filepath) == 0 || strstr(argv[0], filepath) != NULL ||
                   strstr(filepath, argv[0]) != NULL ||
                   (strlen(argv[0]) >= 3 && strcmp(argv[0] + strlen(argv[0]) - 3, ".sh") == 0)) {
            start_arg = 1;
        } else {
            start_arg = 0;
        }
    }

    ctx.argv[0] = (char *)filepath;
    ctx.argc = 0;
    for (int i = start_arg; i < argc && ctx.argc < 63; i++) {
        ctx.argc++;
        ctx.argv[ctx.argc] = argv[i];
    }

    ScriptVarContext *prev_ctx = g_active_script_ctx;
    Tab *prev_tab = g_active_expansion_tab;
    g_active_script_ctx = &ctx;
    g_active_expansion_tab = tab;

    bool saved_echo = tab->echo_to_stdout;
    if (write_ipc_fd >= 0) {
        tab->echo_to_stdout = false;
    }

    bool should_exit = false;
    int exit_code = 0;

    execute_stmt_list(tab, stmts, stmt_count, &ctx, is_source, read_ipc_fd, write_ipc_fd, &should_exit, &exit_code);

    tab->echo_to_stdout = saved_echo;

    g_active_script_ctx = prev_ctx;
    g_active_expansion_tab = prev_tab;

    free_stmts(stmts, stmt_count);
    free(lines);

    return should_exit ? exit_code : tab->last_exit_status;
}
