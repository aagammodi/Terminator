#define _POSIX_C_SOURCE 200809L
#include "autocomplete.h"
#include "script.h"
#include "gui.h"
#include "terminal.h"
#include "shell.h"
#include "parser.h"
#include "theme.h"
#include "expansion.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        g_tests_failed++; \
    } \
} while(0)

/* Helper to create a file with given contents */
static void write_test_file(const char *path, const char *content, mode_t mode) {
    FILE *f = fopen(path, "w");
    if (f) {
        if (content) fputs(content, f);
        fclose(f);
        if (mode) chmod(path, mode);
    }
}

/* Helper to read file contents */
static char *read_test_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (buf) {
        size_t n = fread(buf, 1, (size_t)len, f);
        buf[n] = '\0';
    }
    fclose(f);
    return buf;
}

/* ========================================================================= */
/* PART A: Native Blinking Cursor & Focus / Selection Behavior Tests         */
/* ========================================================================= */
static void test_part_a_cursor(void) {
    printf("\n--- Testing Part A: Native Blinking Cursor & Focus Behavior ---\n");

    /* 1. Blink Timer and Visibility Reset */
    GUI dummy_gui;
    memset(&dummy_gui, 0, sizeof(dummy_gui));
    dummy_gui.cursor_visible = false;
    dummy_gui.last_cursor_blink_time = 0;
    dummy_gui.has_focus = true;

    gui_cursor_reset_blink(&dummy_gui);
    TEST_ASSERT(dummy_gui.cursor_visible == true, "gui_cursor_reset_blink makes cursor immediately visible");
    TEST_ASSERT(dummy_gui.last_cursor_blink_time > 0, "gui_cursor_reset_blink updates blink timestamp");

    /* 2. Focus In / Focus Out */
    dummy_gui.has_focus = false;
    TEST_ASSERT(dummy_gui.has_focus == false, "GUI focus can be suspended (FocusOut)");
    dummy_gui.has_focus = true;
    TEST_ASSERT(dummy_gui.has_focus == true, "GUI focus restored (FocusIn)");

    /* 3. Tab State Cursor Visibility Constraints */
    Tab tab;
    memset(&tab, 0, sizeof(tab));
    tab.state = SHELL_STATE_READY;
    tab.sel_active = false;
    tab.cursor = 5;
    strcpy(tab.input, "hello world");

    bool should_draw = (dummy_gui.has_focus && dummy_gui.cursor_visible &&
                        !tab.sel_active && tab.state != SHELL_STATE_RUNNING);
    TEST_ASSERT(should_draw == true, "Cursor drawn when focused, visible, not selecting, and idle");

    /* When tab is running command, cursor must not be drawn */
    tab.state = SHELL_STATE_RUNNING;
    should_draw = (dummy_gui.has_focus && dummy_gui.cursor_visible &&
                   !tab.sel_active && tab.state != SHELL_STATE_RUNNING);
    TEST_ASSERT(should_draw == false, "Cursor hidden while tab is executing a command");
    tab.state = SHELL_STATE_READY;

    /* When selection is active, cursor must not be drawn */
    tab.sel_active = true;
    should_draw = (dummy_gui.has_focus && dummy_gui.cursor_visible &&
                   !tab.sel_active && tab.state != SHELL_STATE_RUNNING);
    TEST_ASSERT(should_draw == false, "Cursor hidden while mouse selection highlight is active");
    tab.sel_active = false;

    /* When window lost focus, cursor must not be drawn */
    dummy_gui.has_focus = false;
    should_draw = (dummy_gui.has_focus && dummy_gui.cursor_visible &&
                   !tab.sel_active && tab.state != SHELL_STATE_RUNNING);
    TEST_ASSERT(should_draw == false, "Cursor hidden when Terminator window loses focus");
    dummy_gui.has_focus = true;

    /* 4. Input Buffer Integrity: caret is never inserted into input buffer */
    TEST_ASSERT(strcmp(tab.input, "hello world") == 0, "Input buffer text is clean and unmodified");
    TEST_ASSERT(strchr(tab.input, 0x1b) == NULL, "Input buffer contains no escape sequences");
    TEST_ASSERT(strchr(tab.input, '_') == NULL && strchr(tab.input, '|') == NULL, "Input buffer contains no glyph markers");

    /* 5. Per-tab Cursor Isolation */
    Tab tab2;
    memset(&tab2, 0, sizeof(tab2));
    tab.cursor = 3;
    tab2.cursor = 10;
    TEST_ASSERT(tab.cursor == 3 && tab2.cursor == 10, "Per-tab cursor position isolation maintained");
}

/* ========================================================================= */
/* PART B: Task 10 — File-name Auto-completion Tests                         */
/* ========================================================================= */
static void test_part_b_autocomplete(void) {
    printf("\n--- Testing Part B: Task 10 File-name Auto-completion ---\n");

    /* Create temporary test sandbox directory */
    char test_dir[] = "/tmp/terminator_ac_test_XXXXXX";
    char *dres = mkdtemp(test_dir);
    assert(dres != NULL);

    /* Populate test directory structure:
     * - alpha_test.c
     * - beta_one.txt
     * - beta_two.txt
     * - gamma_unique.log
     * - my_folder/ (directory)
     * - .hidden_config
     * - .hidden_dir/ (directory)
     */
    char p[MAX_PATH_LEN];
    snprintf(p, sizeof(p), "%s/alpha_test.c", test_dir);
    write_test_file(p, "/* alpha */", 0644);

    snprintf(p, sizeof(p), "%s/beta_one.txt", test_dir);
    write_test_file(p, "beta 1", 0644);

    snprintf(p, sizeof(p), "%s/beta_two.txt", test_dir);
    write_test_file(p, "beta 2", 0644);

    snprintf(p, sizeof(p), "%s/gamma_unique.log", test_dir);
    write_test_file(p, "log", 0644);

    snprintf(p, sizeof(p), "%s/my_folder", test_dir);
    mkdir(p, 0755);

    snprintf(p, sizeof(p), "%s/my_folder/sub_file.txt", test_dir);
    write_test_file(p, "sub", 0644);

    snprintf(p, sizeof(p), "%s/.hidden_config", test_dir);
    write_test_file(p, "hidden", 0644);

    snprintf(p, sizeof(p), "%s/.hidden_dir", test_dir);
    mkdir(p, 0755);

    Tab *tab = terminal_tab_create(0, test_dir, NULL);
    assert(tab != NULL);

    /* Test 1: Single unique file match */
    safe_strncpy(tab->input, "cat gam", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    bool ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Autocomplete handles single match");
    TEST_ASSERT(strcmp(tab->input, "cat gamma_unique.log") == 0, "Single file match completed directly");
    TEST_ASSERT(tab->cursor == (int)strlen("cat gamma_unique.log"), "Cursor placed at end of completed word");

    /* Test 2: Single directory match appends trailing slash '/' */
    safe_strncpy(tab->input, "cd my_fol", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Autocomplete handles directory match");
    TEST_ASSERT(strcmp(tab->input, "cd my_folder/") == 0, "Directory completion appends trailing slash '/'");
    TEST_ASSERT(tab->cursor == (int)strlen("cd my_folder/"), "Cursor placed after trailing slash");

    /* Test 3: Multiple matches with common prefix extends to LCP */
    safe_strncpy(tab->input, "cat bet", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Autocomplete handles multiple matches with longer common prefix");
    TEST_ASSERT(strcmp(tab->input, "cat beta_") == 0, "Extended word to longest common prefix 'beta_'");
    TEST_ASSERT(tab->cursor == (int)strlen("cat beta_"), "Cursor positioned after common prefix");
    TEST_ASSERT(tab->completion_active == false, "Completion active is false when LCP extension was made");

    /* Test 4: Ambiguous matches with no further common prefix displays numbered list */
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Tab on ambiguous prefix displays numbered choices");
    TEST_ASSERT(tab->completion_active == true, "Completion state set to active for choice selection");
    TEST_ASSERT(tab->completion_num_matches == 2, "Completion detected exactly 2 ambiguous matches");
    TEST_ASSERT(strcmp(tab->completion_matches[0], "beta_one.txt") == 0, "First match is beta_one.txt (alphabetical)");
    TEST_ASSERT(strcmp(tab->completion_matches[1], "beta_two.txt") == 0, "Second match is beta_two.txt (alphabetical)");

    /* Verify terminal output line was added with choices */
    TEST_ASSERT(tab->num_lines > 0, "Numbered choice list output to terminal");
    TEST_ASSERT(strstr(tab->lines[tab->num_lines - 1].text, "1. beta_one.txt") != NULL, "Choice 1 formatted correctly");
    TEST_ASSERT(strstr(tab->lines[tab->num_lines - 1].text, "2. beta_two.txt") != NULL, "Choice 2 formatted correctly");

    /* Test 5: Selecting choice 1 completes the word */
    ok = autocomplete_handle_choice(tab, 1);
    TEST_ASSERT(ok == true, "Selecting choice 1 succeeded");
    TEST_ASSERT(strcmp(tab->input, "cat beta_one.txt") == 0, "Word completed to choice 1 (beta_one.txt)");
    TEST_ASSERT(tab->cursor == (int)strlen("cat beta_one.txt"), "Cursor positioned after choice completion");
    TEST_ASSERT(tab->completion_active == false, "Completion active reset after choice selected");

    /* Test 6: Selecting choice 2 when ambiguous */
    safe_strncpy(tab->input, "cat bet", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    autocomplete_handle_tab(tab); /* Extends to beta_ */
    autocomplete_handle_tab(tab); /* Shows choices */
    ok = autocomplete_handle_choice(tab, 2);
    TEST_ASSERT(ok == true, "Selecting choice 2 succeeded");
    TEST_ASSERT(strcmp(tab->input, "cat beta_two.txt") == 0, "Word completed to choice 2 (beta_two.txt)");

    /* Test 7: Preserves suffix after cursor (mid-line completion) */
    safe_strncpy(tab->input, "cat gam | grep foo", sizeof(tab->input));
    tab->cursor = 7; /* Immediately after "gam" */
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Mid-line completion succeeds");
    TEST_ASSERT(strcmp(tab->input, "cat gamma_unique.log | grep foo") == 0, "Trailing suffix ' | grep foo' preserved intact");
    TEST_ASSERT(tab->cursor == (int)strlen("cat gamma_unique.log"), "Cursor moved to end of inserted completion");

    /* Test 8: Subdirectory path completion */
    safe_strncpy(tab->input, "cat my_folder/sub", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Subdirectory path completion succeeds");
    TEST_ASSERT(strcmp(tab->input, "cat my_folder/sub_file.txt") == 0, "Completed file inside subdirectory path");

    /* Test 9: Hidden file completion */
    safe_strncpy(tab->input, "cat .hidden_c", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Hidden file completion succeeds");
    TEST_ASSERT(strcmp(tab->input, "cat .hidden_config") == 0, "Completed hidden file starting with '.'");

    /* Test 10: Hidden directory completion appends '/' */
    safe_strncpy(tab->input, "cd .hidden_d", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == true, "Hidden directory completion succeeds");
    TEST_ASSERT(strcmp(tab->input, "cd .hidden_dir/") == 0, "Completed hidden directory with trailing '/'");

    /* Test 11: Non-existent prefix does nothing */
    safe_strncpy(tab->input, "cat non_existent_prefix", sizeof(tab->input));
    tab->cursor = (int)strlen(tab->input);
    ok = autocomplete_handle_tab(tab);
    TEST_ASSERT(ok == false, "No match returns false");
    TEST_ASSERT(strcmp(tab->input, "cat non_existent_prefix") == 0, "Input unchanged when no match");

    /* Test 12: Cancel completion */
    tab->completion_active = true;
    autocomplete_cancel(tab);
    TEST_ASSERT(tab->completion_active == false, "autocomplete_cancel resets completion state");

    terminal_tab_destroy(tab);

    /* Cleanup temporary test sandbox */
    unlink(p);
    snprintf(p, sizeof(p), "%s/alpha_test.c", test_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/beta_one.txt", test_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/beta_two.txt", test_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/gamma_unique.log", test_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/my_folder/sub_file.txt", test_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/my_folder", test_dir); rmdir(p);
    snprintf(p, sizeof(p), "%s/.hidden_config", test_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/.hidden_dir", test_dir); rmdir(p);
    rmdir(test_dir);
}

/* ========================================================================= */
/* PART C: Task 11 — Bash Script Interpreter Tests                           */
/* ========================================================================= */
static void test_part_c_script(void) {
    printf("\n--- Testing Part C: Task 11 Bash Script Interpreter ---\n");

    /* 1. Invocation form detection */
    Command cmd;
    char path_buf[MAX_PATH_LEN];
    bool is_source = false;

    /* Form A: ./test.sh */
    command_init(&cmd);
    command_add_arg(&cmd, "./tests/dummy_script.sh");
    write_test_file("./tests/dummy_script.sh", "#!/bin/bash\necho hi\n", 0755);
    bool is_scr = script_is_script_invocation(&cmd, ".", path_buf, sizeof(path_buf), &is_source);
    TEST_ASSERT(is_scr == true, "Form A './script.sh' recognized as script invocation");
    TEST_ASSERT(is_source == false, "Form A is not source mode");
    command_free(&cmd);

    /* Form B: bash test.sh */
    command_init(&cmd);
    command_add_arg(&cmd, "bash");
    command_add_arg(&cmd, "tests/dummy_script.sh");
    is_scr = script_is_script_invocation(&cmd, ".", path_buf, sizeof(path_buf), &is_source);
    TEST_ASSERT(is_scr == true, "Form B 'bash script.sh' recognized as script invocation");
    TEST_ASSERT(is_source == false, "Form B is not source mode");
    command_free(&cmd);

    /* Form C: source test.sh */
    command_init(&cmd);
    command_add_arg(&cmd, "source");
    command_add_arg(&cmd, "tests/dummy_script.sh");
    is_scr = script_is_script_invocation(&cmd, ".", path_buf, sizeof(path_buf), &is_source);
    TEST_ASSERT(is_scr == true, "Form C 'source script.sh' recognized as script invocation");
    TEST_ASSERT(is_source == true, "Form C identified as source mode");
    command_free(&cmd);

    /* Compiled ELF Binary should NOT be detected as script */
    command_init(&cmd);
    command_add_arg(&cmd, "./terminator");
    is_scr = script_is_script_invocation(&cmd, ".", path_buf, sizeof(path_buf), &is_source);
    TEST_ASSERT(is_scr == false, "Compiled ELF binary './terminator' is NOT classified as script");
    command_free(&cmd);

    /* Regular command should NOT be detected as script */
    command_init(&cmd);
    command_add_arg(&cmd, "echo");
    command_add_arg(&cmd, "dummy_script.sh");
    is_scr = script_is_script_invocation(&cmd, ".", path_buf, sizeof(path_buf), &is_source);
    TEST_ASSERT(is_scr == false, "Standard command 'echo script.sh' is NOT classified as script");
    command_free(&cmd);

    unlink("./tests/dummy_script.sh");

    /* 2. Mandatory Phase 1 Syntax Validation & Marker File Test */
    printf("\n  [MANDATORY VERIFICATION] Phase 1 Syntax Validation & Marker File Test\n");
    const char *marker_path = "/tmp/terminator_syntax_marker.tmp";
    unlink(marker_path);

    const char *bad_script_path = "/tmp/terminator_syntax_error.sh";
    /* Script has a command on line 1 that MUST NOT run if there is a syntax error later */
    const char *bad_script_content =
        "touch /tmp/terminator_syntax_marker.tmp\n"
        "echo line 2\n"
        "if [ 1 -eq 1 ]; then\n"
        "    echo line 4 unclosed if\n";
        /* missing fi */

    write_test_file(bad_script_path, bad_script_content, 0755);

    char err_buf[MAX_CMD_LEN];
    int err_line = -1;
    int vres = script_validate_file(bad_script_path, err_buf, sizeof(err_buf), &err_line);
    TEST_ASSERT(vres != 0, "Phase 1 detects missing 'fi' syntax error");
    TEST_ASSERT(err_line > 0, "Phase 1 reports error line number");

    Tab *tab = terminal_tab_create(0, "/tmp", NULL);
    assert(tab != NULL);

    int exec_rc = script_execute_file(tab, bad_script_path, 0, NULL, false, -1, -1);
    TEST_ASSERT(exec_rc != 0, "script_execute_file rejects script with syntax error");

    /* CRITICAL ASSERTION: The marker file must NOT exist! */
    int marker_exists = (access(marker_path, F_OK) == 0);
    TEST_ASSERT(!marker_exists, "CRITICAL: Marker file does NOT exist! Line 1 was NEVER executed!");

    unlink(bad_script_path);
    unlink(marker_path);

    /* 3. Further Phase 1 Syntax Validation Tests */
    /* Unmatched quote */
    const char *quote_err_script = "/tmp/term_quote_err.sh";
    write_test_file(quote_err_script, "echo \"unterminated string\n", 0755);
    vres = script_validate_file(quote_err_script, err_buf, sizeof(err_buf), &err_line);
    TEST_ASSERT(vres != 0, "Phase 1 rejects unterminated double quote");
    unlink(quote_err_script);

    /* Extra fi without if */
    const char *extra_fi_script = "/tmp/term_extra_fi.sh";
    write_test_file(extra_fi_script, "echo hi\nfi\n", 0755);
    vres = script_validate_file(extra_fi_script, err_buf, sizeof(err_buf), &err_line);
    TEST_ASSERT(vres != 0, "Phase 1 rejects extraneous 'fi' without matching 'if'");
    unlink(extra_fi_script);

    /* Malformed test syntax */
    const char *bad_test_script = "/tmp/term_bad_test.sh";
    write_test_file(bad_test_script, "if [ -f ]; then\n    echo hi\nfi\n", 0755);
    vres = script_validate_file(bad_test_script, err_buf, sizeof(err_buf), &err_line);
    TEST_ASSERT(vres != 0, "Phase 1 rejects malformed test expression '[ -f ]' (missing operand)");
    unlink(bad_test_script);

    /* 4. Phase 2 Execution Tests */
    /* Test Variables and Positional Parameters */
    const char *vars_script = "/tmp/term_test_vars.sh";
    const char *vars_out = "/tmp/term_vars_out.txt";
    unlink(vars_out);

    const char *vars_content =
        "#!/bin/bash\n"
        "# Comment test\n"
        "GREET=\"Hello\"\n"
        "TARGET=$1\n"
        "COUNT=$#\n"
        "echo \"$GREET $TARGET (args: $COUNT)\" > /tmp/term_vars_out.txt\n";
    write_test_file(vars_script, vars_content, 0755);

    char *argv_call[] = { "TerminatorUser", "extra_arg" };
    exec_rc = script_execute_file(tab, vars_script, 2, argv_call, false, -1, -1);
    TEST_ASSERT(exec_rc == 0, "Variables and positional arguments script executed with exit code 0");

    char *out_txt = read_test_file(vars_out);
    TEST_ASSERT(out_txt != NULL, "Script output file created");
    if (out_txt) {
        TEST_ASSERT(strstr(out_txt, "Hello TerminatorUser (args: 2)") != NULL,
                    "Variable expansion and positional parameters $1, $# expanded correctly");
        free(out_txt);
    }
    unlink(vars_script);
    unlink(vars_out);

    /* Test Conditionals: if, elif, else, fi, and operators (-eq, -gt, =, -f, -d) */
    const char *cond_script = "/tmp/term_test_cond.sh";
    const char *cond_out = "/tmp/term_cond_out.txt";
    unlink(cond_out);

    const char *cond_content =
        "NUM=10\n"
        "STR=\"terminator\"\n"
        "if [ $NUM -gt 20 ]; then\n"
        "    echo \"gt20\" > /tmp/term_cond_out.txt\n"
        "elif [ $NUM -eq 10 ]; then\n"
        "    if [ \"$STR\" = \"terminator\" ]; then\n"
        "        if [ -d /tmp ]; then\n"
        "            echo \"condition_success\" > /tmp/term_cond_out.txt\n"
        "        fi\n"
        "    fi\n"
        "else\n"
        "    echo \"else_branch\" > /tmp/term_cond_out.txt\n"
        "fi\n";
    write_test_file(cond_script, cond_content, 0755);

    exec_rc = script_execute_file(tab, cond_script, 0, NULL, false, -1, -1);
    TEST_ASSERT(exec_rc == 0, "Conditionals script executed successfully");

    out_txt = read_test_file(cond_out);
    TEST_ASSERT(out_txt != NULL && strstr(out_txt, "condition_success") != NULL,
                "Conditionals (-gt, -eq, =, -d, nested if, elif) evaluated correctly");
    free(out_txt);
    unlink(cond_script);
    unlink(cond_out);

    /* Test Pipelines and Redirection inside Script */
    const char *pipe_script = "/tmp/term_test_pipe.sh";
    const char *pipe_out = "/tmp/term_pipe_out.txt";
    unlink(pipe_out);

    const char *pipe_content =
        "printf 'banana\\napple\\ncherry\\n' | sort > /tmp/term_pipe_out.txt\n";
    write_test_file(pipe_script, pipe_content, 0755);

    exec_rc = script_execute_file(tab, pipe_script, 0, NULL, false, -1, -1);
    TEST_ASSERT(exec_rc == 0, "Pipeline and redirection inside script executed successfully");

    out_txt = read_test_file(pipe_out);
    TEST_ASSERT(out_txt != NULL && strncmp(out_txt, "apple", 5) == 0,
                "Pipeline stdout passed to sort and redirected to output file");
    free(out_txt);
    unlink(pipe_script);
    unlink(pipe_out);

    /* Test exit [n] statement */
    const char *exit_script = "/tmp/term_test_exit.sh";
    const char *exit_content =
        "echo before exit\n"
        "exit 42\n"
        "echo after exit\n";
    write_test_file(exit_script, exit_content, 0755);

    exec_rc = script_execute_file(tab, exit_script, 0, NULL, false, -1, -1);
    TEST_ASSERT(exec_rc == 42, "exit [n] returns explicit status 42 and terminates script");
    unlink(exit_script);

    /* Test Source Mode Context Persistence */
    const char *source_script = "/tmp/term_test_source.sh";
    const char *source_content =
        "TAB_CUSTOM_VAR=\"persisted_value\"\n";
    write_test_file(source_script, source_content, 0755);

    /* 1) In non-source mode (./script.sh), tab vars should not be modified */
    exec_rc = script_execute_file(tab, source_script, 0, NULL, false, -1, -1);
    TEST_ASSERT(exec_rc == 0, "Non-source execution succeeds");
    TEST_ASSERT(terminal_get_var(tab, "TAB_CUSTOM_VAR") == NULL,
                "Non-source execution does not leak script variable into tab context");

    /* 2) In source mode (source script.sh), tab vars MUST persist in tab */
    exec_rc = script_execute_file(tab, source_script, 0, NULL, true, -1, -1);
    TEST_ASSERT(exec_rc == 0, "Source mode execution succeeds");
    const char *persisted = terminal_get_var(tab, "TAB_CUSTOM_VAR");
    TEST_ASSERT(persisted != NULL && strcmp(persisted, "persisted_value") == 0,
                "Source mode persists variables directly into Tab environment");

    unlink(source_script);

    /* Test Builtin Exit Status & Branching in Scripts (Issue 2) */
    const char *status_script = "/tmp/term_test_status.sh";
    const char *status_out = "/tmp/term_status_out.txt";
    unlink(status_out);
    const char *status_content =
        "cd /nonexistent_dir_for_test\n"
        "if [ $? -eq 0 ]; then\n"
        "    echo \"BUG\" > /tmp/term_status_out.txt\n"
        "else\n"
        "    echo \"CORRECT\" > /tmp/term_status_out.txt\n"
        "fi\n";
    write_test_file(status_script, status_content, 0755);

    exec_rc = script_execute_file(tab, status_script, 0, NULL, false, -1, -1);
    out_txt = read_test_file(status_out);
    TEST_ASSERT(out_txt != NULL && strstr(out_txt, "CORRECT") != NULL,
                "Failed builtin updates $? and allows script conditional to branch to else");
    free(out_txt);
    unlink(status_script);
    unlink(status_out);

    terminal_tab_destroy(tab);
}

int main(void) {
    printf("====================================================\n");
    printf("=== Terminator: Task 10 & Task 11 Verification Suite ===\n");
    printf("====================================================\n");

    test_part_a_cursor();
    test_part_b_autocomplete();
    test_part_c_script();

    printf("\n=== Tasks 10 & 11 Result: %d Passed, %d Failed ===\n", g_tests_passed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
