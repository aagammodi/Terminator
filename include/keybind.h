#ifndef TERMINATOR_KEYBIND_H
#define TERMINATOR_KEYBIND_H

#include "common.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>

typedef enum {
    ACTION_NONE = 0,
    ACTION_MOVE_LEFT,
    ACTION_MOVE_RIGHT,
    ACTION_BEGINNING_OF_LINE,
    ACTION_END_OF_LINE,
    ACTION_HISTORY_PREV,
    ACTION_HISTORY_NEXT,
    ACTION_DELETE_TO_START,
    ACTION_DELETE_TO_END,
    ACTION_DELETE_WORD,
    ACTION_CLEAR_SCREEN,
    ACTION_CLOSE_TAB,
    ACTION_COPY,
    ACTION_PASTE,
    ACTION_NEW_TAB,
    ACTION_CLOSE_TAB_SHORTCUT,
    ACTION_PREV_TAB,
    ACTION_NEXT_TAB,
    ACTION_HISTORY_SEARCH,
    ACTION_SUSPEND_JOB,
    ACTION_COUNT
} KeyAction;

typedef struct {
    KeyAction action;
    const char *name;
    const char *alt_name;
    KeySym default_ksym;
    unsigned int default_modifiers;
    KeySym custom_ksym;
    unsigned int custom_modifiers;
    bool is_custom;
} KeyBindingEntry;

void keybind_init(void);
KeyAction keybind_match(KeySym ksym, unsigned int state);
bool keybind_parse_combo(const char *str, KeySym *out_sym, unsigned int *out_mod);
const char *keybind_format_combo(KeySym sym, unsigned int mod, char *buf, size_t len);
bool keybind_set(const char *action_name, const char *combo_str);
void keybind_reset_defaults(void);
bool keybind_load_config(const char *path);
bool keybind_save_config(const char *path);
void keybind_list(FILE *out);
const char *keybind_get_config_path(char *buf, size_t len);

#endif /* TERMINATOR_KEYBIND_H */
