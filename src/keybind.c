#include "keybind.h"
#include "util.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>

/*
 * Master table of configurable keyboard actions and default X11 KeySym/modifier bindings.
 * Supports runtime remapping via 'bind <action> <combo>' and persistent ~/.terminatorrc storage.
 */
static KeyBindingEntry g_bindings[] = {
    { ACTION_MOVE_LEFT,          "move-left",          "left",              XK_Left,      0,                          0, 0, false },
    { ACTION_MOVE_RIGHT,         "move-right",         "right",             XK_Right,     0,                          0, 0, false },
    { ACTION_BEGINNING_OF_LINE,  "beginning-of-line",  "start-of-line",     XK_a,         ControlMask,                0, 0, false },
    { ACTION_END_OF_LINE,        "end-of-line",        "move-end",          XK_e,         ControlMask,                0, 0, false },
    { ACTION_HISTORY_PREV,       "history-prev",       "history-up",        XK_Up,        0,                          0, 0, false },
    { ACTION_HISTORY_NEXT,       "history-next",       "history-down",      XK_Down,      0,                          0, 0, false },
    { ACTION_DELETE_TO_START,    "delete-to-start",    "delete-start",      XK_u,         ControlMask,                0, 0, false },
    { ACTION_DELETE_TO_END,      "delete-to-end",      "delete-end",        XK_k,         ControlMask,                0, 0, false },
    { ACTION_DELETE_WORD,        "delete-word",        "delete-prev-word",  XK_w,         ControlMask,                0, 0, false },
    { ACTION_CLEAR_SCREEN,       "clear-screen",       "clear",             XK_l,         ControlMask,                0, 0, false },
    { ACTION_CLOSE_TAB,          "close-tab",          "exit-tab",          XK_d,         ControlMask,                0, 0, false },
    { ACTION_COPY,               "copy",               "copy-selection",    XK_c,         ControlMask | ShiftMask,    0, 0, false },
    { ACTION_PASTE,              "paste",              "paste-selection",   XK_v,         ControlMask | ShiftMask,    0, 0, false },
    { ACTION_NEW_TAB,            "new-tab",            "tab-new",           XK_t,         ControlMask | ShiftMask,    0, 0, false },
    { ACTION_CLOSE_TAB_SHORTCUT, "close-tab-shortcut", "tab-close",         XK_w,         ControlMask | ShiftMask,    0, 0, false },
    { ACTION_PREV_TAB,           "prev-tab",           "tab-prev",          XK_Prior,     ControlMask,                0, 0, false },
    { ACTION_NEXT_TAB,           "next-tab",           "tab-next",          XK_Next,      ControlMask,                0, 0, false },
    { ACTION_HISTORY_SEARCH,     "history-search",     "search-history",    XK_r,         ControlMask,                0, 0, false },
    { ACTION_SUSPEND_JOB,        "suspend-job",        "stop-job",          XK_z,         ControlMask,                0, 0, false },
    { ACTION_NONE,               NULL,                 NULL,                0,            0,                          0, 0, false }
};

static KeySym normalize_ksym(KeySym ksym) {
    if (ksym >= XK_A && ksym <= XK_Z) {
        return XK_a + (ksym - XK_A);
    }
    return ksym;
}

const char *keybind_get_config_path(char *buf, size_t len) {
    if (!buf || len == 0) return NULL;
    const char *custom = getenv("TERMINATORRC");
    if (custom && custom[0] != '\0') {
        safe_strncpy(buf, custom, len);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || home[0] == '\0') {
        home = "/tmp";
    }
    snprintf(buf, len, "%s/.terminatorrc", home);
    return buf;
}

/*
 * Parse a human-readable key combination string (e.g., "Ctrl+Shift+T") into an X11 KeySym and modifier bitmask.
 */
bool keybind_parse_combo(const char *str, KeySym *out_sym, unsigned int *out_mod) {
    if (!str || !out_sym || !out_mod) return false;

    char copy[128];
    safe_strncpy(copy, str, sizeof(copy));

    /* Trim whitespace */
    char *s = copy;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        end--;
        *end = '\0';
    }

    if (!*s) return false;

    unsigned int mod = 0;
    KeySym ksym = NoSymbol;

    char *token = strtok(s, "+-");
    while (token) {
        /* Trim token */
        while (*token && isspace((unsigned char)*token)) token++;
        char *t_end = token + strlen(token);
        while (t_end > token && isspace((unsigned char)*(t_end - 1))) {
            t_end--;
            *t_end = '\0';
        }

        char *next = strtok(NULL, "+-");
        if (next != NULL) {
            /* This token must be a modifier */
            if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
                mod |= ControlMask;
            } else if (strcasecmp(token, "Shift") == 0) {
                mod |= ShiftMask;
            } else if (strcasecmp(token, "Alt") == 0 || strcasecmp(token, "Mod1") == 0) {
                mod |= Mod1Mask;
            } else {
                return false; /* Unknown modifier */
            }
        } else {
            /* Last token: the main key */
            if (strcasecmp(token, "Left") == 0) {
                ksym = XK_Left;
            } else if (strcasecmp(token, "Right") == 0) {
                ksym = XK_Right;
            } else if (strcasecmp(token, "Up") == 0) {
                ksym = XK_Up;
            } else if (strcasecmp(token, "Down") == 0) {
                ksym = XK_Down;
            } else if (strcasecmp(token, "Home") == 0) {
                ksym = XK_Home;
            } else if (strcasecmp(token, "End") == 0) {
                ksym = XK_End;
            } else if (strcasecmp(token, "PageUp") == 0 || strcasecmp(token, "Prior") == 0) {
                ksym = XK_Prior;
            } else if (strcasecmp(token, "PageDown") == 0 || strcasecmp(token, "Next") == 0) {
                ksym = XK_Next;
            } else if (strcasecmp(token, "Backspace") == 0 || strcasecmp(token, "BackSpace") == 0) {
                ksym = XK_BackSpace;
            } else if (strcasecmp(token, "Delete") == 0) {
                ksym = XK_Delete;
            } else if (strcasecmp(token, "Enter") == 0 || strcasecmp(token, "Return") == 0) {
                ksym = XK_Return;
            } else if (strcasecmp(token, "Tab") == 0) {
                ksym = XK_Tab;
            } else if (strcasecmp(token, "Escape") == 0 || strcasecmp(token, "Esc") == 0) {
                ksym = XK_Escape;
            } else if (strlen(token) == 1) {
                char c = token[0];
                if (c >= 'A' && c <= 'Z') {
                    ksym = XK_a + (c - 'A');
                } else if (c >= 'a' && c <= 'z') {
                    ksym = XK_a + (c - 'a');
                } else if (c >= '0' && c <= '9') {
                    ksym = XK_0 + (c - '0');
                } else {
                    ksym = (KeySym)c;
                }
            } else {
                KeySym parsed = XStringToKeysym(token);
                if (parsed != NoSymbol) {
                    ksym = normalize_ksym(parsed);
                } else {
                    return false;
                }
            }
        }
        token = next;
    }

    if (ksym == NoSymbol) return false;

    *out_sym = ksym;
    *out_mod = mod;
    return true;
}

const char *keybind_format_combo(KeySym sym, unsigned int mod, char *buf, size_t len) {
    if (!buf || len == 0) return "";
    buf[0] = '\0';

    if (mod & ControlMask) {
        strncat(buf, "Ctrl+", len - strlen(buf) - 1);
    }
    if (mod & Mod1Mask) {
        strncat(buf, "Alt+", len - strlen(buf) - 1);
    }
    if (mod & ShiftMask) {
        strncat(buf, "Shift+", len - strlen(buf) - 1);
    }

    const char *name = NULL;
    switch (sym) {
        case XK_Left:      name = "Left"; break;
        case XK_Right:     name = "Right"; break;
        case XK_Up:        name = "Up"; break;
        case XK_Down:      name = "Down"; break;
        case XK_Home:      name = "Home"; break;
        case XK_End:       name = "End"; break;
        case XK_Prior:     name = "PageUp"; break;
        case XK_Next:      name = "PageDown"; break;
        case XK_BackSpace: name = "Backspace"; break;
        case XK_Delete:    name = "Delete"; break;
        case XK_Return:    name = "Enter"; break;
        case XK_Tab:       name = "Tab"; break;
        case XK_Escape:    name = "Escape"; break;
        default:
            if (sym >= XK_a && sym <= XK_z) {
                char single[2] = { (char)toupper((unsigned char)('a' + (sym - XK_a))), '\0' };
                strncat(buf, single, len - strlen(buf) - 1);
                return buf;
            } else if (sym >= XK_0 && sym <= XK_9) {
                char single[2] = { (char)('0' + (sym - XK_0)), '\0' };
                strncat(buf, single, len - strlen(buf) - 1);
                return buf;
            }
            name = XKeysymToString(sym);
            break;
    }

    if (name) {
        strncat(buf, name, len - strlen(buf) - 1);
    } else {
        strncat(buf, "Unknown", len - strlen(buf) - 1);
    }

    return buf;
}

static KeyBindingEntry *find_entry(const char *name) {
    if (!name) return NULL;
    char norm[64];
    safe_strncpy(norm, name, sizeof(norm));
    for (int j = 0; norm[j]; j++) {
        if (norm[j] == '_') norm[j] = '-';
    }

    for (int i = 0; g_bindings[i].action != ACTION_NONE; i++) {
        if (strcasecmp(g_bindings[i].name, name) == 0 ||
            strcasecmp(g_bindings[i].name, norm) == 0 ||
            (g_bindings[i].alt_name && (strcasecmp(g_bindings[i].alt_name, name) == 0 || strcasecmp(g_bindings[i].alt_name, norm) == 0))) {
            return &g_bindings[i];
        }
    }

    /* Common aliases */
    if (strcasecmp(norm, "start-of-line") == 0 || strcasecmp(norm, "cursor-start") == 0) {
        return find_entry("beginning-of-line");
    }
    if (strcasecmp(norm, "move-end") == 0 || strcasecmp(norm, "cursor-end") == 0) {
        return find_entry("end-of-line");
    }
    if (strcasecmp(norm, "cursor-left") == 0) {
        return find_entry("move-left");
    }
    if (strcasecmp(norm, "cursor-right") == 0) {
        return find_entry("move-right");
    }

    return NULL;
}

KeyAction keybind_match(KeySym ksym, unsigned int state) {
    KeySym norm_sym = normalize_ksym(ksym);
    unsigned int active_mods = state & (ControlMask | ShiftMask | Mod1Mask);

    for (int i = 0; g_bindings[i].action != ACTION_NONE; i++) {
        KeySym target_sym = g_bindings[i].is_custom ? g_bindings[i].custom_ksym : g_bindings[i].default_ksym;
        unsigned int target_mod = g_bindings[i].is_custom ? g_bindings[i].custom_modifiers : g_bindings[i].default_modifiers;

        if (norm_sym == normalize_ksym(target_sym) && active_mods == target_mod) {
            return g_bindings[i].action;
        }
    }

    return ACTION_NONE;
}

bool keybind_set(const char *action_name, const char *combo_str) {
    if (!action_name || !combo_str) return false;

    KeyBindingEntry *target = find_entry(action_name);
    if (!target) {
        fprintf(stderr, "terminator: config: invalid action '%s'\n", action_name);
        return false;
    }

    KeySym sym = NoSymbol;
    unsigned int mod = 0;
    if (!keybind_parse_combo(combo_str, &sym, &mod)) {
        fprintf(stderr, "terminator: config: invalid key combo '%s' for action '%s'\n", combo_str, action_name);
        return false;
    }

    KeySym norm_sym = normalize_ksym(sym);

    /* Check for conflicts with other actions */
    for (int i = 0; g_bindings[i].action != ACTION_NONE; i++) {
        if (&g_bindings[i] == target) continue;

        KeySym other_sym = g_bindings[i].is_custom ? g_bindings[i].custom_ksym : g_bindings[i].default_ksym;
        unsigned int other_mod = g_bindings[i].is_custom ? g_bindings[i].custom_modifiers : g_bindings[i].default_modifiers;

        if (norm_sym == normalize_ksym(other_sym) && mod == other_mod) {
            fprintf(stderr, "terminator: config: conflict: '%s' is already bound to '%s', keeping existing binding\n",
                    combo_str, g_bindings[i].name);
            return false;
        }
    }

    target->custom_ksym = sym;
    target->custom_modifiers = mod;
    target->is_custom = true;
    return true;
}

void keybind_reset_defaults(void) {
    for (int i = 0; g_bindings[i].action != ACTION_NONE; i++) {
        g_bindings[i].custom_ksym = 0;
        g_bindings[i].custom_modifiers = 0;
        g_bindings[i].is_custom = false;
    }
}

bool keybind_load_config(const char *path) {
    char default_path[MAX_PATH_LEN];
    if (!path || path[0] == '\0') {
        path = keybind_get_config_path(default_path, sizeof(default_path));
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* Config file does not exist; silent default operation */
        return true;
    }

    char line[256];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        /* Expected syntax: bind <action> <combo> */
        char cmd[32] = {0};
        char action[64] = {0};
        char combo[64] = {0};

        if (sscanf(p, "%31s %63s %63s", cmd, action, combo) == 3) {
            if (strcmp(cmd, "bind") == 0) {
                keybind_set(action, combo);
            } else {
                fprintf(stderr, "terminator: config: %s:line %d: unknown command '%s'\n", path, line_num, cmd);
            }
        } else {
            fprintf(stderr, "terminator: config: %s:line %d: malformed line: '%s'\n", path, line_num, p);
        }
    }

    fclose(fp);
    return true;
}

bool keybind_save_config(const char *path) {
    char default_path[MAX_PATH_LEN];
    if (!path || path[0] == '\0') {
        path = keybind_get_config_path(default_path, sizeof(default_path));
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "terminator: config: unable to write to '%s'\n", path);
        return false;
    }

    fprintf(fp, "# ~/.terminatorrc - Terminator Key Bindings Configuration\n");
    fprintf(fp, "# Format: bind <action> <key_combo>\n\n");

    for (int i = 0; g_bindings[i].action != ACTION_NONE; i++) {
        KeySym sym = g_bindings[i].is_custom ? g_bindings[i].custom_ksym : g_bindings[i].default_ksym;
        unsigned int mod = g_bindings[i].is_custom ? g_bindings[i].custom_modifiers : g_bindings[i].default_modifiers;
        char combo[64];
        keybind_format_combo(sym, mod, combo, sizeof(combo));
        fprintf(fp, "bind %-20s %s\n", g_bindings[i].name, combo);
    }

    fclose(fp);
    return true;
}

void keybind_list(FILE *out) {
    if (!out) out = stdout;
    for (int i = 0; g_bindings[i].action != ACTION_NONE; i++) {
        KeySym sym = g_bindings[i].is_custom ? g_bindings[i].custom_ksym : g_bindings[i].default_ksym;
        unsigned int mod = g_bindings[i].is_custom ? g_bindings[i].custom_modifiers : g_bindings[i].default_modifiers;
        char combo[64];
        keybind_format_combo(sym, mod, combo, sizeof(combo));
        fprintf(out, "bind %-20s %s%s\n", g_bindings[i].name, combo, g_bindings[i].is_custom ? " (custom)" : "");
    }
}

void keybind_init(void) {
    keybind_reset_defaults();
    keybind_load_config(NULL);
}
