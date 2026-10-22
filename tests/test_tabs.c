#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include "terminal.h"
#include "executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

int main(void) {
    printf("=== Running Task 1 Multi-Tab State & Lifecycle Tests ===\n");

    /* 1. Shell creation has 1 tab */
    Shell *shell = shell_create("/tmp");
    TEST_ASSERT("Initial shell has exactly 1 tab", shell->num_tabs == 1);
    TEST_ASSERT("Initial active tab index is 0", shell->active_tab_idx == 0);
    TEST_ASSERT("Initial tab cwd is /tmp", strcmp(shell->tabs[0]->cwd, "/tmp") == 0);

    /* 2. Open new tab with independent directory */
    Tab *t2 = shell_open_tab(shell, "/var");
    TEST_ASSERT("Opening tab increases num_tabs to 2", shell->num_tabs == 2);
    TEST_ASSERT("Opening tab activates the new tab (idx 1)", shell->active_tab_idx == 1);
    TEST_ASSERT("New tab pointer is valid", t2 != NULL && shell->tabs[1] == t2);
    TEST_ASSERT("New tab cwd is /var", strcmp(t2->cwd, "/var") == 0);
    TEST_ASSERT("Tab 0 cwd remains /tmp (directory isolation)", strcmp(shell->tabs[0]->cwd, "/tmp") == 0);

    /* 3. Modify directory in tab 1, verify tab 0 is unaffected */
    executor_run_line(t2, "cd /usr");
    TEST_ASSERT("Tab 1 cwd changes to /usr", strcmp(t2->cwd, "/usr") == 0);
    TEST_ASSERT("Tab 0 cwd strictly remains /tmp", strcmp(shell->tabs[0]->cwd, "/tmp") == 0);

    /* 4. Tab switching */
    shell_switch_tab(shell, 0);
    TEST_ASSERT("Switch to tab 0 succeeds", shell->active_tab_idx == 0);
    shell_switch_tab(shell, 1);
    TEST_ASSERT("Switch back to tab 1 succeeds", shell->active_tab_idx == 1);

    /* 5. Open tabs up to MAX_TABS */
    for (int i = 2; i < MAX_TABS; i++) {
        Tab *t = shell_open_tab(shell, "/tmp");
        if (!t) break;
    }
    TEST_ASSERT("Opened tabs up to MAX_TABS (16)", shell->num_tabs == MAX_TABS);

    /* 6. Opening past MAX_TABS returns NULL safely without crash */
    Tab *overflow = shell_open_tab(shell, "/tmp");
    TEST_ASSERT("Opening tab past MAX_TABS returns NULL safely", overflow == NULL);
    TEST_ASSERT("num_tabs remains at MAX_TABS", shell->num_tabs == MAX_TABS);

    /* 7. Close middle tab */
    int old_count = shell->num_tabs;
    shell_close_tab(shell, 2);
    TEST_ASSERT("Closing tab decreases num_tabs by 1", shell->num_tabs == old_count - 1);

    /* 8. Close remaining tabs until 1 tab remains */
    while (shell->num_tabs > 1) {
        shell_close_tab(shell, shell->num_tabs - 1);
    }
    TEST_ASSERT("Closed down to 1 active tab", shell->num_tabs == 1 && shell->running == true);

    /* 9. Closing the final tab signals shell to exit */
    shell_close_tab(shell, 0);
    TEST_ASSERT("Closing final tab sets shell->running to false", shell->running == false);

    /* Clean destruction */
    shell_destroy(shell);
    TEST_ASSERT("shell_destroy completes without errors", 1);

    printf("=== Multi-Tab Result: %d Passed, %d Failed ===\n", passes, failures);
    return (failures == 0) ? 0 : 1;
}
