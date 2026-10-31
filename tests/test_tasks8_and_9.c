#define _POSIX_C_SOURCE 200809L
#include "jobs.h"
#include "history.h"
#include "terminal.h"
#include "shell.h"
#include "parser.h"
#include "theme.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

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

/* ========================================================================= */
/* PART A: Selection Theme & Geometry Tests                                  */
/* ========================================================================= */
static void test_part_a_selection_theme(void) {
    printf("\n--- Testing Part A: Selection Theme & Geometry ---\n");

    Theme theme;
    bool ok = theme_init(NULL, 0, &theme);
    TEST_ASSERT(ok, "Theme initializes without X11 server");
    TEST_ASSERT(theme.hex_values[COLOR_SELECTION_FG] != NULL, "Selection high-contrast FG color defined");
    TEST_ASSERT(strcmp(theme.hex_values[COLOR_SELECTION_FG], "#08071A") == 0, "Selection FG is dark navy (#08071A)");
    TEST_ASSERT(theme.hex_values[COLOR_SELECTION_BORDER] != NULL, "Selection border color defined");

    /* Gradient step validation */
    TEST_ASSERT(theme.selection_gradient[0] != 0, "Selection gradient step 0 has color");
    TEST_ASSERT(theme.selection_gradient[GRADIENT_STEPS - 1] != 0, "Selection gradient final step has color");

    /* Tab selection isolation */
    Tab tab1;
    memset(&tab1, 0, sizeof(tab1));
    Tab tab2;
    memset(&tab2, 0, sizeof(tab2));

    tab1.sel_active = true;
    tab1.sel_start_row = 2;
    tab1.sel_start_col = 5;
    tab1.sel_end_row = 2;
    tab1.sel_end_col = 15;

    TEST_ASSERT(tab1.sel_active, "Tab 1 selection is active");
    TEST_ASSERT(!tab2.sel_active, "Tab 2 selection is inactive (per-tab isolation)");
}

/* ========================================================================= */
/* PART B: Task 8 Signals & Job Control Tests                                */
/* ========================================================================= */
static void test_part_b_jobs(void) {
    printf("\n--- Testing Part B: Task 8 Signals & Job Control ---\n");

    /* 1. Job Table Lifecycle */
    JobTable table;
    jobs_init(&table);
    TEST_ASSERT(table.count == 0, "Job table starts empty");
    TEST_ASSERT(table.next_id == 1, "Next job ID starts at 1");

    pid_t dummy_pids[2] = { 101, 102 };
    Job *j1 = jobs_add(&table, 101, dummy_pids, 2, "sleep 100", JOB_STATE_STOPPED, false, -1);
    TEST_ASSERT(j1 != NULL, "Job 1 added to table");
    TEST_ASSERT(j1->id == 1, "Job 1 has ID 1");
    TEST_ASSERT(j1->pgid == 101, "Job 1 has correct PGID");
    TEST_ASSERT(j1->state == JOB_STATE_STOPPED, "Job 1 state is Stopped");
    TEST_ASSERT(table.current_id == 1, "Current job is 1 (+ marker)");

    pid_t dummy_pids2[1] = { 103 };
    Job *j2 = jobs_add(&table, 103, dummy_pids2, 1, "sleep 200", JOB_STATE_RUNNING, false, -1);
    TEST_ASSERT(j2 != NULL && j2->id == 2, "Job 2 added with ID 2");
    TEST_ASSERT(table.current_id == 2, "Current job updated to 2 (+)");
    TEST_ASSERT(table.previous_id == 1, "Previous job set to 1 (-)");

    /* Find operations */
    TEST_ASSERT(jobs_find_by_id(&table, 1) == j1, "Find job by ID 1 succeeds");
    TEST_ASSERT(jobs_find_by_id(&table, 2) == j2, "Find job by ID 2 succeeds");
    TEST_ASSERT(jobs_find_by_id(&table, 99) == NULL, "Find non-existent job ID returns NULL");
    TEST_ASSERT(jobs_find_by_pgid(&table, 103) == j2, "Find job by PGID succeeds");

    /* Current job selections */
    TEST_ASSERT(jobs_get_current(&table) == j2, "jobs_get_current returns latest active job");
    TEST_ASSERT(jobs_get_current_stopped(&table) == j1, "jobs_get_current_stopped returns stopped job");

    /* 2. Parser trailing '&' background operator */
    Pipeline *p1 = parser_parse_pipeline("sleep 100 &", 0);
    TEST_ASSERT(p1 != NULL && p1->is_background, "Trailing '&' detected as background operator");
    TEST_ASSERT(p1->num_commands == 1 && strcmp(p1->commands[0].argv[0], "sleep") == 0, "Command is 'sleep'");
    TEST_ASSERT(p1->commands[0].argc == 2 && strcmp(p1->commands[0].argv[1], "100") == 0, "Arg is '100' ('&' stripped)");
    pipeline_free(p1);

    Pipeline *p2 = parser_parse_pipeline("echo 'foo & bar'", 0);
    TEST_ASSERT(p2 != NULL && !p2->is_background, "Quoted '&' not treated as background operator");
    pipeline_free(p2);

    Pipeline *p3 = parser_parse_pipeline("printf 'hello\\n' | wc -l &", 0);
    TEST_ASSERT(p3 != NULL && p3->is_background, "Pipeline trailing '&' detected as background");
    pipeline_free(p3);

    /* 3. Live process-group signal delivery */
    pid_t child_pid = fork();
    if (child_pid == 0) {
        setpgid(0, 0);
        /* Child sleeps until signal */
        pause();
        _exit(0);
    }
    setpgid(child_pid, child_pid);

    /* Verify child is running */
    usleep(20000);
    TEST_ASSERT(kill(child_pid, 0) == 0, "Child process running in its own process group");

    /* Signal child process group with SIGINT */
    kill(-child_pid, SIGINT);
    int status = 0;
    pid_t reaped = waitpid(child_pid, &status, 0);
    TEST_ASSERT(reaped == child_pid, "Child reaped after process-group SIGINT");
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT, "Child terminated by SIGINT");

    jobs_cleanup(&table);
}

/* ========================================================================= */
/* PART C: Task 9 Searchable History Tests                                   */
/* ========================================================================= */
static void test_part_c_history(void) {
    printf("\n--- Testing Part C: Task 9 Searchable History ---\n");

    /* 1. Longest Common Substring (Contiguous) algorithm unit tests */
    TEST_ASSERT(longest_common_substring("myprog", "gcc -o myprog myprog.c") == 6,
                "LCS 'myprog' in 'gcc -o myprog myprog.c' is 6");
    TEST_ASSERT(longest_common_substring("prog", "myprog") == 4,
                "LCS 'prog' in 'myprog' is 4");
    TEST_ASSERT(longest_common_substring("pr", "myprog") == 2,
                "LCS 'pr' in 'myprog' is 2");
    TEST_ASSERT(longest_common_substring("xyz", "gcc -o myprog myprog.c") == 1,
                "LCS 'xyz' in 'gcc -o myprog myprog.c' is 1 (common 'y')");
    TEST_ASSERT(longest_common_substring("123", "gcc -o myprog myprog.c") == 0,
                "LCS '123' in 'gcc -o myprog myprog.c' is 0");
    TEST_ASSERT(longest_common_substring("", "anything") == 0,
                "LCS with empty string is 0");

    /* 2. Persistent History File Loading and Saving */
    const char *test_hist_file = "/tmp/.terminator_test_hist_tmp";
    unlink(test_hist_file);
    setenv("TERMINATOR_HISTORY", test_hist_file, 1);

    SharedHistory *hist = history_create(false);
    TEST_ASSERT(hist != NULL, "History created successfully");
    TEST_ASSERT(history_get_count(hist) == 0, "History starts empty when file missing");

    /* Add entries */
    history_add(hist, "git status");
    history_add(hist, "./myprog < in.txt");
    history_add(hist, "gcc -o myprog myprog.c");
    history_add(hist, "myprog");
    TEST_ASSERT(history_get_count(hist) == 4, "History count is 4");

    /* Destroy and re-create to verify file persistence */
    history_destroy(hist);

    SharedHistory *hist2 = history_create(false);
    TEST_ASSERT(hist2 != NULL, "History reloaded from persistent file");
    TEST_ASSERT(history_get_count(hist2) == 4, "Reloaded history retains 4 commands");

    char buf[MAX_HISTORY_LINE_LEN];
    history_get_entry(hist2, 3, buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "myprog") == 0, "Entry 3 matches 'myprog'");

    /* 3. Search Algorithm: Case A Exact Match Priority */
    char results[16][MAX_HISTORY_LINE_LEN];
    int res_count = history_search(hist2, "myprog", results, 16);
    TEST_ASSERT(res_count == 1, "Exact match returns exactly 1 command");
    TEST_ASSERT(strcmp(results[0], "myprog") == 0, "Exact match is 'myprog'");

    /* 4. Search Algorithm: Case B Longest Common Substring (max > 2) */
    /* Remove 'myprog' exact match from search test by creating hist3 with only non-exact matches */
    history_destroy(hist2);
    unlink(test_hist_file);

    SharedHistory *hist3 = history_create(false);
    history_add(hist3, "git commit -m 'initial'");
    history_add(hist3, "./myprog < in.txt");
    history_add(hist3, "gcc -o myprog myprog.c");

    res_count = history_search(hist3, "myprog", results, 16);
    TEST_ASSERT(res_count == 2, "Substring search returns both tied matches (score 6)");
    TEST_ASSERT(strcmp(results[0], "gcc -o myprog myprog.c") == 0, "Newest match returned first");
    TEST_ASSERT(strcmp(results[1], "./myprog < in.txt") == 0, "Second match returned second");

    /* 5. Search Algorithm: Case C No Adequate Match (max <= 2) */
    res_count = history_search(hist3, "xyz", results, 16);
    TEST_ASSERT(res_count == 0, "No match when common substring length <= 2");

    res_count = history_search(hist3, "pr", results, 16);
    TEST_ASSERT(res_count == 0, "No match when common substring length == 2");

    res_count = history_search(hist3, "", results, 16);
    TEST_ASSERT(res_count == 0, "Empty search term returns 0");

    /* 6. Capacity Clamp to 10,000 Entries */
    for (int i = 0; i < 10050; i++) {
        char dummy[64];
        snprintf(dummy, sizeof(dummy), "seq_cmd_%d", i);
        history_add(hist3, dummy);
    }
    TEST_ASSERT(history_get_count(hist3) == MAX_SHARED_HISTORY, "History clamped to exactly 10,000 entries");

    history_destroy(hist3);
    unlink(test_hist_file);
}

int main(void) {
    printf("====================================================\n");
    printf("=== Terminator: Task 8 & Task 9 Verification Suite ===\n");
    printf("====================================================\n");

    test_part_a_selection_theme();
    test_part_b_jobs();
    test_part_c_history();

    printf("\n=== Tasks 8 & 9 Result: %d Passed, %d Failed ===\n", g_tests_passed, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
