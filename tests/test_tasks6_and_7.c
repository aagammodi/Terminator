#define _POSIX_C_SOURCE 200809L
#include "multiwatch.h"
#include "keybind.h"
#include "terminal.h"
#include "shell.h"
#include "history.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passes = 0;
static int failures = 0;

#define TEST_ASSERT(desc, cond) do { \
    if (cond) { \
        printf("  [PASS] %s\n", desc); \
        passes++; \
    } else { \
        printf("  [FAIL] %s\n", desc); \
        failures++; \
    } \
} while(0)

static void test_multiwatch_parsing(void) {
    printf("--- Testing Task 6: multiWatch Parsing ---\n");

    MultiWatchArgs args;
    char err[256];

    /* 1. Valid single command */
    bool ok = multiwatch_parse_args("multiWatch [\"date +%T\"]", &args, err, sizeof(err));
    TEST_ASSERT("Valid single command parsed", ok && args.count == 1);
    TEST_ASSERT("Single command string exact match", args.count == 1 && strcmp(args.commands[0], "date +%T") == 0);
    multiwatch_free_args(&args);

    /* 2. Valid multiple commands */
    ok = multiwatch_parse_args("multiWatch [\"date +%T\", \"ls | wc -l\"]", &args, err, sizeof(err));
    TEST_ASSERT("Valid multiple commands parsed", ok && args.count == 2);
    TEST_ASSERT("First command matches", args.count == 2 && strcmp(args.commands[0], "date +%T") == 0);
    TEST_ASSERT("Second command (with pipe) matches", args.count == 2 && strcmp(args.commands[1], "ls | wc -l") == 0);
    multiwatch_free_args(&args);

    /* 3. Commas inside quoted strings preserved without splitting */
    ok = multiwatch_parse_args("multiWatch [\"awk -F, '{print $1}' file\", \"grep -v foo\"]", &args, err, sizeof(err));
    TEST_ASSERT("Comma inside quoted command not split", ok && args.count == 2);
    TEST_ASSERT("Command with internal comma preserved", args.count == 2 && strcmp(args.commands[0], "awk -F, '{print $1}' file") == 0);
    multiwatch_free_args(&args);

    /* 4. Single quotes around commands supported */
    ok = multiwatch_parse_args("multiWatch ['uptime', 'ps aux | wc -l']", &args, err, sizeof(err));
    TEST_ASSERT("Single quoted commands supported", ok && args.count == 2);
    multiwatch_free_args(&args);

    /* 5. Spaces around brackets and commas handled */
    ok = multiwatch_parse_args("multiWatch  [   \"echo 1\"   ,   \"echo 2\"   ]  ", &args, err, sizeof(err));
    TEST_ASSERT("Tolerant of whitespace around tokens", ok && args.count == 2);
    multiwatch_free_args(&args);

    /* 6. Missing brackets rejected */
    ok = multiwatch_parse_args("multiWatch \"echo 1\"", &args, err, sizeof(err));
    TEST_ASSERT("Missing opening bracket rejected", !ok && strstr(err, "opening bracket") != NULL);

    ok = multiwatch_parse_args("multiWatch [\"echo 1\"", &args, err, sizeof(err));
    TEST_ASSERT("Missing closing bracket rejected", !ok && strstr(err, "closing bracket") != NULL);

    /* 7. Empty command list rejected */
    ok = multiwatch_parse_args("multiWatch []", &args, err, sizeof(err));
    TEST_ASSERT("Empty command list [] rejected", !ok && strstr(err, "empty command list") != NULL);

    ok = multiwatch_parse_args("multiWatch [   ]", &args, err, sizeof(err));
    TEST_ASSERT("Empty whitespace command list [  ] rejected", !ok && strstr(err, "empty command list") != NULL);

    /* 8. Empty command entry rejected */
    ok = multiwatch_parse_args("multiWatch [\"\"]", &args, err, sizeof(err));
    TEST_ASSERT("Empty quoted command entry rejected", !ok && strstr(err, "empty command entry") != NULL);

    ok = multiwatch_parse_args("multiWatch [\"date\", \"\"]", &args, err, sizeof(err));
    TEST_ASSERT("Second entry empty rejected", !ok && strstr(err, "empty command entry") != NULL);

    /* 9. Unquoted command rejected */
    ok = multiwatch_parse_args("multiWatch [date]", &args, err, sizeof(err));
    TEST_ASSERT("Unquoted command [date] rejected", !ok && strstr(err, "must be enclosed in quotes") != NULL);

    /* 10. Missing comma between commands rejected */
    ok = multiwatch_parse_args("multiWatch [\"date\" \"uptime\"]", &args, err, sizeof(err));
    TEST_ASSERT("Missing comma between commands rejected", !ok && strstr(err, "expected ','") != NULL);

    /* 11. Trailing garbage after ']' rejected */
    ok = multiwatch_parse_args("multiWatch [\"date\"] trailing_garbage", &args, err, sizeof(err));
    TEST_ASSERT("Trailing characters after ']' rejected", !ok && strstr(err, "unexpected tokens") != NULL);
}

static void test_keybindings_and_config(void) {
    printf("--- Testing Task 7: Configurable Key Bindings & Persistence ---\n");

    keybind_reset_defaults();

    /* 1. Test default bindings */
    TEST_ASSERT("Default Left Arrow matches ACTION_MOVE_LEFT", keybind_match(XK_Left, 0) == ACTION_MOVE_LEFT);
    TEST_ASSERT("Default Right Arrow matches ACTION_MOVE_RIGHT", keybind_match(XK_Right, 0) == ACTION_MOVE_RIGHT);
    TEST_ASSERT("Default Ctrl+A matches ACTION_BEGINNING_OF_LINE", keybind_match(XK_a, ControlMask) == ACTION_BEGINNING_OF_LINE);
    TEST_ASSERT("Default Ctrl+E matches ACTION_END_OF_LINE", keybind_match(XK_e, ControlMask) == ACTION_END_OF_LINE);
    TEST_ASSERT("Default Up Arrow matches ACTION_HISTORY_PREV", keybind_match(XK_Up, 0) == ACTION_HISTORY_PREV);
    TEST_ASSERT("Default Down Arrow matches ACTION_HISTORY_NEXT", keybind_match(XK_Down, 0) == ACTION_HISTORY_NEXT);
    TEST_ASSERT("Default Ctrl+U matches ACTION_DELETE_TO_START", keybind_match(XK_u, ControlMask) == ACTION_DELETE_TO_START);
    TEST_ASSERT("Default Ctrl+K matches ACTION_DELETE_TO_END", keybind_match(XK_k, ControlMask) == ACTION_DELETE_TO_END);
    TEST_ASSERT("Default Ctrl+W matches ACTION_DELETE_WORD", keybind_match(XK_w, ControlMask) == ACTION_DELETE_WORD);
    TEST_ASSERT("Default Ctrl+L matches ACTION_CLEAR_SCREEN", keybind_match(XK_l, ControlMask) == ACTION_CLEAR_SCREEN);
    TEST_ASSERT("Default Ctrl+D matches ACTION_CLOSE_TAB", keybind_match(XK_d, ControlMask) == ACTION_CLOSE_TAB);
    TEST_ASSERT("Default Ctrl+Shift+C matches ACTION_COPY", keybind_match(XK_c, ControlMask | ShiftMask) == ACTION_COPY);
    TEST_ASSERT("Default Ctrl+Shift+V matches ACTION_PASTE", keybind_match(XK_v, ControlMask | ShiftMask) == ACTION_PASTE);

    /* 2. Combo Parser */
    KeySym sym = NoSymbol;
    unsigned int mod = 0;
    bool ok = keybind_parse_combo("Ctrl+Shift+L", &sym, &mod);
    TEST_ASSERT("Combo 'Ctrl+Shift+L' parsed", ok && sym == XK_l && mod == (ControlMask | ShiftMask));

    ok = keybind_parse_combo("Alt+Backspace", &sym, &mod);
    TEST_ASSERT("Combo 'Alt+Backspace' parsed", ok && sym == XK_BackSpace && mod == Mod1Mask);

    /* 3. Custom binding set */
    ok = keybind_set("clear-screen", "Ctrl+Shift+L");
    TEST_ASSERT("Binding 'clear-screen' to 'Ctrl+Shift+L' succeeds", ok);
    TEST_ASSERT("Ctrl+Shift+L now triggers ACTION_CLEAR_SCREEN", keybind_match(XK_l, ControlMask | ShiftMask) == ACTION_CLEAR_SCREEN);
    TEST_ASSERT("Old default Ctrl+L no longer triggers ACTION_CLEAR_SCREEN", keybind_match(XK_l, ControlMask) == ACTION_NONE);

    /* 4. Conflict detection */
    /* Attempt to bind delete-word to Ctrl+A which is already beginning-of-line */
    ok = keybind_set("delete-word", "Ctrl+A");
    TEST_ASSERT("Conflicting binding rejected safely", !ok);
    TEST_ASSERT("Existing binding for Ctrl+A preserved", keybind_match(XK_a, ControlMask) == ACTION_BEGINNING_OF_LINE);

    /* 5. Persistence across save and load */
    char tmp_path[] = "/tmp/terminator_test_rc_XXXXXX";
    int fd = mkstemp(tmp_path);
    close(fd);

    ok = keybind_save_config(tmp_path);
    TEST_ASSERT("Config saved to temporary file", ok);

    /* Reset to defaults */
    keybind_reset_defaults();
    TEST_ASSERT("Defaults restored: Ctrl+L triggers ACTION_CLEAR_SCREEN", keybind_match(XK_l, ControlMask) == ACTION_CLEAR_SCREEN);

    /* Load from saved config */
    ok = keybind_load_config(tmp_path);
    TEST_ASSERT("Config loaded successfully", ok);
    TEST_ASSERT("Saved custom binding Ctrl+Shift+L re-applied", keybind_match(XK_l, ControlMask | ShiftMask) == ACTION_CLEAR_SCREEN);

    unlink(tmp_path);
    keybind_reset_defaults();
}

static void test_cursor_and_editing(void) {
    printf("--- Testing Task 7: Cursor-Aware Editing & History Navigation ---\n");

    Tab *tab = terminal_tab_create(1, "/tmp", NULL);

    /* 1. Left and Right Arrow cursor clamping */
    safe_strncpy(tab->input, "hello", sizeof(tab->input));
    tab->cursor = 5;

    terminal_cursor_right(tab);
    TEST_ASSERT("Cursor clamped to end of line on Right Arrow", tab->cursor == 5);

    terminal_cursor_left(tab);
    terminal_cursor_left(tab);
    TEST_ASSERT("Cursor moved left by 2 positions", tab->cursor == 3);

    terminal_cursor_home(tab);
    TEST_ASSERT("Cursor at beginning on Ctrl+A", tab->cursor == 0);

    terminal_cursor_left(tab);
    TEST_ASSERT("Cursor clamped at start of line on Left Arrow", tab->cursor == 0);

    terminal_cursor_end(tab);
    TEST_ASSERT("Cursor at end of line on Ctrl+E", tab->cursor == 5);

    /* 2. Ctrl+U: Delete to beginning of line (cursor in middle) */
    safe_strncpy(tab->input, "first second third", sizeof(tab->input));
    tab->cursor = 6; /* Cursor at start of "second" */
    terminal_delete_to_start(tab);
    TEST_ASSERT("Ctrl+U preserves text after cursor", strcmp(tab->input, "second third") == 0);
    TEST_ASSERT("Ctrl+U repositions cursor to 0", tab->cursor == 0);

    /* 3. Ctrl+K: Delete to end of line (cursor in middle) */
    safe_strncpy(tab->input, "first second third", sizeof(tab->input));
    tab->cursor = 6; /* Cursor after "first " */
    terminal_delete_to_end(tab);
    TEST_ASSERT("Ctrl+K preserves text before cursor", strcmp(tab->input, "first ") == 0);
    TEST_ASSERT("Ctrl+K leaves cursor at deletion point", tab->cursor == 6);

    /* 4. Ctrl+W: Delete word backward (cursor in middle) */
    safe_strncpy(tab->input, "alpha beta gamma delta", sizeof(tab->input));
    tab->cursor = 10; /* Cursor right after "beta" */
    terminal_delete_word_backward(tab);
    TEST_ASSERT("Ctrl+W deletes word backward and preserves trailing text", strcmp(tab->input, "alpha  gamma delta") == 0);
    TEST_ASSERT("Ctrl+W updates cursor position", tab->cursor == 6);

    /* 5. Ctrl+L: Clear screen preserves input line and cursor */
    safe_strncpy(tab->input, "my_pending_command", sizeof(tab->input));
    tab->cursor = 5;
    terminal_append_line(tab, "Old scrollback line 1", LINE_TYPE_OUTPUT);
    terminal_append_line(tab, "Old scrollback line 2", LINE_TYPE_OUTPUT);
    TEST_ASSERT("Tab has lines before clear", tab->num_lines == 2);

    terminal_clear_screen(tab);
    TEST_ASSERT("Screen lines cleared to 0", tab->num_lines == 0);
    TEST_ASSERT("Input string preserved after Ctrl+L", strcmp(tab->input, "my_pending_command") == 0);
    TEST_ASSERT("Cursor position preserved after Ctrl+L", tab->cursor == 5);

    /* 6. Pasted text insertion */
    safe_strncpy(tab->input, "start end", sizeof(tab->input));
    tab->cursor = 6; /* Between "start " and "end" */
    terminal_insert_pasted_text(tab, "MIDDLE ");
    TEST_ASSERT("Pasted text inserted at cursor and preserves following text", strcmp(tab->input, "start MIDDLE end") == 0);
    TEST_ASSERT("Cursor advanced past pasted text", tab->cursor == 13);

    /* 7. History Navigation with Draft Command Preservation */
    SharedHistory *hist = history_create(false);
    tab->shell = xcalloc(1, sizeof(Shell));
    tab->shell->history = hist;

    history_add(hist, "command_1");
    history_add(hist, "command_2");
    history_add(hist, "command_3");

    safe_strncpy(tab->input, "my_unsubmitted_draft", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);

    /* Up Arrow */
    terminal_history_prev(tab);
    TEST_ASSERT("Up Arrow displays newest command", strcmp(tab->input, "command_3") == 0);

    terminal_history_prev(tab);
    TEST_ASSERT("Second Up Arrow displays command_2", strcmp(tab->input, "command_2") == 0);

    terminal_history_next(tab);
    TEST_ASSERT("Down Arrow moves forward to command_3", strcmp(tab->input, "command_3") == 0);

    terminal_history_next(tab);
    TEST_ASSERT("Down Arrow past newest restores user draft command", strcmp(tab->input, "my_unsubmitted_draft") == 0);
    TEST_ASSERT("History index reset to -1 after restoring draft", tab->history_index == -1);

    history_destroy(hist);
    free(tab->shell);
    terminal_tab_destroy(tab);
}

int main(void) {
    printf("====================================================\n");
    printf("=== Terminator: Task 6 & Task 7 Verification Suite ===\n");
    printf("====================================================\n");

    test_multiwatch_parsing();
    test_keybindings_and_config();
    test_cursor_and_editing();

    printf("\n=== Tasks 6 & 7 Result: %d Passed, %d Failed ===\n", passes, failures);
    return (failures == 0) ? 0 : 1;
}
