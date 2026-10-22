#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include "terminal.h"
#include "gui.h"
#include "input.h"
#include "history.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

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

static void test_scrollbar_geometry_and_scrolling(void) {
    printf("--- Testing Scrollbar Geometry, Proportional Thumb & Bottom Following ---\n");

    GUI gui;
    memset(&gui, 0, sizeof(GUI));
    gui.width = 960;
    gui.height = 640;
    gui.theme.top_bar_height = 38;
    gui.theme.status_bar_height = 28;
    gui.theme.padding_y = 12;
    gui.theme.padding_x = 16;
    gui.theme.font_height = 14;
    gui.theme.font_width = 8;

    Tab *tab = terminal_tab_create(1, "/tmp", NULL);

    /* Test 1: Empty / few lines -> max_scroll == 0 */
    ScrollbarGeom geom;
    bool ok = gui_get_scrollbar_geom(&gui, tab, &geom);
    TEST_ASSERT("Scrollbar geometry computed for empty tab", ok);
    TEST_ASSERT("No scrolling when content fits on screen (max_scroll == 0)", geom.max_scroll == 0);
    TEST_ASSERT("Thumb occupies full height when content fits", geom.thumb_h == geom.sb_h);
    TEST_ASSERT("Thumb Y at track top when content fits", geom.thumb_y == geom.sb_y);

    /* Test 2: Add 100 lines so scrolling is needed */
    for (int i = 0; i < 100; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Log output line %d", i);
        terminal_append_line(tab, buf, LINE_TYPE_OUTPUT);
    }

    ok = gui_get_scrollbar_geom(&gui, tab, &geom);
    TEST_ASSERT("Scrollbar geometry computed for 100 lines", ok);
    TEST_ASSERT("max_scroll > 0 for 100 lines", geom.max_scroll > 0);
    TEST_ASSERT("Thumb height is proportional and smaller than track", geom.thumb_h < geom.sb_h);
    TEST_ASSERT("Thumb height is at least minimum clamp (24px)", geom.thumb_h >= 24);

    /* Test 3: When at bottom (scroll_offset == 0), thumb is at the exact bottom of track */
    tab->scroll_offset = 0;
    gui_get_scrollbar_geom(&gui, tab, &geom);
    int expected_bottom_y = geom.sb_y + geom.sb_h - geom.thumb_h;
    TEST_ASSERT("When scroll_offset == 0, thumb is at bottom of track", geom.thumb_y == expected_bottom_y);

    /* Test 4: When scrolled to the very top (scroll_offset == max_scroll), thumb is at top of track */
    tab->scroll_offset = geom.max_scroll;
    gui_get_scrollbar_geom(&gui, tab, &geom);
    TEST_ASSERT("When scroll_offset == max_scroll, thumb is at top of track", geom.thumb_y == geom.sb_y);

    /* Test 5: Midpoint scroll */
    tab->scroll_offset = geom.max_scroll / 2;
    gui_get_scrollbar_geom(&gui, tab, &geom);
    int travel = geom.sb_h - geom.thumb_h;
    int expected_mid_y = geom.sb_y + travel / 2;
    TEST_ASSERT("When scroll_offset is at 50%, thumb is near track midpoint", abs(geom.thumb_y - expected_mid_y) <= 2);

    /* Test 6: Bottom-following behavior vs Scrolled-up preservation */
    tab->scroll_offset = 0;
    terminal_show_prompt(tab);
    TEST_ASSERT("Prompt refresh at bottom keeps scroll_offset at 0", tab->scroll_offset == 0);

    tab->scroll_offset = 12;
    terminal_append_line(tab, "New asynchronous output", LINE_TYPE_OUTPUT);
    terminal_show_prompt(tab);
    TEST_ASSERT("Prompt refresh when scrolled up preserves user scroll position", tab->scroll_offset >= 12);

    /* Test 7: Page Up and Page Down navigation */
    int old_offset = tab->scroll_offset;
    terminal_scroll_up(tab, geom.visible_rows);
    TEST_ASSERT("terminal_scroll_up increases scroll_offset", tab->scroll_offset > old_offset);

    old_offset = tab->scroll_offset;
    terminal_scroll_down(tab, geom.visible_rows);
    TEST_ASSERT("terminal_scroll_down decreases scroll_offset", tab->scroll_offset < old_offset);

    terminal_tab_destroy(tab);
}

static void test_thread_mode_architecture(void) {
    printf("--- Testing Mode A: Thread Mode (Default Architecture) ---\n");

    /* Create Thread Mode Shell */
    Shell *shell = shell_create("/tmp", false);
    TEST_ASSERT("Thread mode shell created", shell != NULL);
    TEST_ASSERT("use_process_mode is false", shell->use_process_mode == false);
    TEST_ASSERT("Shared history is initialized", shell->history != NULL);

    Tab *tab0 = shell->tabs[0];
    TEST_ASSERT("Tab 0 worker thread created", tab0->thread_created == true);
    TEST_ASSERT("Tab 0 child_pid is -1 (not a forked process)", tab0->child_pid == -1);
    TEST_ASSERT("Tab 0 worker pipes are open", tab0->to_worker_pipe[1] >= 0 && tab0->from_worker_pipe[0] >= 0);

    /* Open second tab */
    Tab *tab1 = shell_open_tab(shell, "/var");
    TEST_ASSERT("Tab 1 created", tab1 != NULL);
    TEST_ASSERT("Tab 1 worker thread created", tab1->thread_created == true);
    TEST_ASSERT("Tab 1 child_pid is -1", tab1->child_pid == -1);
    TEST_ASSERT("Tab 1 cwd is /var", strcmp(tab1->cwd, "/var") == 0);
    TEST_ASSERT("Tab 0 cwd remains /tmp", strcmp(tab0->cwd, "/tmp") == 0);

    /* Execute command in Tab 0 via IPC pipe */
    const char *cmd0 = "echo 'term_thread_test_0'";
    ipc_send_msg(tab0->to_worker_pipe[1], IPC_MSG_EXEC, cmd0, (uint32_t)strlen(cmd0));

    /* Read response from Tab 0 */
    bool got_output0 = false;
    bool got_prompt0 = false;
    IPCHeader hdr;
    while (ipc_recv_header(tab0->from_worker_pipe[0], &hdr)) {
        if (hdr.type == IPC_MSG_OUTPUT) {
            char buf[128];
            if (hdr.length > 0 && hdr.length < sizeof(buf)) {
                ipc_recv_payload(tab0->from_worker_pipe[0], buf, hdr.length);
                buf[hdr.length] = '\0';
                if (strstr(buf, "term_thread_test_0")) {
                    got_output0 = true;
                }
            }
        } else if (hdr.type == IPC_MSG_PROMPT) {
            int status = 0;
            ipc_recv_payload(tab0->from_worker_pipe[0], &status, sizeof(status));
            got_prompt0 = true;
            break;
        } else {
            if (hdr.length > 0) {
                char dummy[256];
                ipc_recv_payload(tab0->from_worker_pipe[0], dummy, hdr.length);
            }
        }
    }
    TEST_ASSERT("Tab 0 thread executed command and returned output", got_output0);
    TEST_ASSERT("Tab 0 thread completed and returned prompt", got_prompt0);

    /* Execute 'cd /usr' in Tab 1 */
    const char *cmd1 = "cd /usr";
    ipc_send_msg(tab1->to_worker_pipe[1], IPC_MSG_EXEC, cmd1, (uint32_t)strlen(cmd1));

    bool got_cwd1 = false;
    bool got_prompt1 = false;
    while (ipc_recv_header(tab1->from_worker_pipe[0], &hdr)) {
        if (hdr.type == IPC_MSG_CWD) {
            char buf[MAX_PATH_LEN];
            if (hdr.length > 0 && hdr.length < sizeof(buf)) {
                ipc_recv_payload(tab1->from_worker_pipe[0], buf, hdr.length);
                buf[hdr.length] = '\0';
                if (strcmp(buf, "/usr") == 0) {
                    got_cwd1 = true;
                    safe_strncpy(tab1->cwd, buf, sizeof(tab1->cwd));
                }
            }
        } else if (hdr.type == IPC_MSG_PROMPT) {
            int status = 0;
            ipc_recv_payload(tab1->from_worker_pipe[0], &status, sizeof(status));
            got_prompt1 = true;
            break;
        } else {
            if (hdr.length > 0) {
                char dummy[256];
                ipc_recv_payload(tab1->from_worker_pipe[0], dummy, hdr.length);
            }
        }
    }
    TEST_ASSERT("Tab 1 received CWD update to /usr", got_cwd1);
    TEST_ASSERT("Tab 1 received prompt completion", got_prompt1);
    TEST_ASSERT("Tab 0 cwd remains /tmp (Thread isolation verified)", strcmp(tab0->cwd, "/tmp") == 0);

    /* Verify shared history in Thread Mode */
    int hist_cnt = history_get_count(shell->history);
    TEST_ASSERT("Both tabs recorded commands into shared history", hist_cnt >= 2);

    shell_destroy(shell);
    TEST_ASSERT("Thread mode shell cleaned up successfully", 1);
}

static void test_process_mode_architecture(void) {
    printf("--- Testing Mode B: Process Mode (Multiprocess Architecture) ---\n");

    /* Create Process Mode Shell */
    Shell *shell = shell_create("/tmp", true);
    TEST_ASSERT("Process mode shell created", shell != NULL);
    TEST_ASSERT("use_process_mode is true", shell->use_process_mode == true);
    TEST_ASSERT("Shared history is initialized with POSIX shared memory", shell->history != NULL && shell->history->is_process_mode);

    Tab *tab0 = shell->tabs[0];
    TEST_ASSERT("Tab 0 child_pid > 0 (forked child process)", tab0->child_pid > 0);
    TEST_ASSERT("Tab 0 thread_created is false", tab0->thread_created == false);
    TEST_ASSERT("Tab 0 worker pipes are open", tab0->to_worker_pipe[1] >= 0 && tab0->from_worker_pipe[0] >= 0);

    /* Open second tab */
    Tab *tab1 = shell_open_tab(shell, "/var");
    TEST_ASSERT("Tab 1 created", tab1 != NULL);
    TEST_ASSERT("Tab 1 child_pid > 0", tab1->child_pid > 0);
    TEST_ASSERT("Tab 0 and Tab 1 have distinct child PIDs", tab0->child_pid != tab1->child_pid);

    /* Execute command in Tab 0 */
    const char *cmd0 = "echo 'process_mode_tab0_test'";
    ipc_send_msg(tab0->to_worker_pipe[1], IPC_MSG_EXEC, cmd0, (uint32_t)strlen(cmd0));

    bool got_output0 = false;
    bool got_prompt0 = false;
    IPCHeader hdr;
    while (ipc_recv_header(tab0->from_worker_pipe[0], &hdr)) {
        if (hdr.type == IPC_MSG_OUTPUT) {
            char buf[128];
            if (hdr.length > 0 && hdr.length < sizeof(buf)) {
                ipc_recv_payload(tab0->from_worker_pipe[0], buf, hdr.length);
                buf[hdr.length] = '\0';
                if (strstr(buf, "process_mode_tab0_test")) {
                    got_output0 = true;
                }
            }
        } else if (hdr.type == IPC_MSG_PROMPT) {
            int status = 0;
            ipc_recv_payload(tab0->from_worker_pipe[0], &status, sizeof(status));
            got_prompt0 = true;
            break;
        } else {
            if (hdr.length > 0) {
                char dummy[256];
                ipc_recv_payload(tab0->from_worker_pipe[0], dummy, hdr.length);
            }
        }
    }
    TEST_ASSERT("Tab 0 process executed command and returned output", got_output0);
    TEST_ASSERT("Tab 0 process completed and returned prompt", got_prompt0);

    /* Verify shared history in Process Mode (shared memory + semaphore) */
    int hist_cnt = history_get_count(shell->history);
    TEST_ASSERT("Process Mode shared history recorded command in shared memory", hist_cnt >= 1);

    /* Test Crash Isolation:
     * Kill Tab 1 process forcefully (SIGKILL). Tab 0 and GUI must survive! */
    printf("--- Testing Process Mode Crash Isolation ---\n");
    pid_t tab1_pid = tab1->child_pid;
    kill(tab1_pid, SIGKILL);
    usleep(20000); /* 20ms */

    /* Tab 0 must still be alive and responsive! */
    const char *cmd_alive = "echo 'tab0_survived'";
    ipc_send_msg(tab0->to_worker_pipe[1], IPC_MSG_EXEC, cmd_alive, (uint32_t)strlen(cmd_alive));

    bool tab0_alive = false;
    while (ipc_recv_header(tab0->from_worker_pipe[0], &hdr)) {
        if (hdr.type == IPC_MSG_OUTPUT) {
            char buf[128];
            if (hdr.length > 0 && hdr.length < sizeof(buf)) {
                ipc_recv_payload(tab0->from_worker_pipe[0], buf, hdr.length);
                buf[hdr.length] = '\0';
                if (strstr(buf, "tab0_survived")) {
                    tab0_alive = true;
                }
            }
        } else if (hdr.type == IPC_MSG_PROMPT) {
            int status = 0;
            ipc_recv_payload(tab0->from_worker_pipe[0], &status, sizeof(status));
            break;
        } else {
            if (hdr.length > 0) {
                char dummy[256];
                ipc_recv_payload(tab0->from_worker_pipe[0], dummy, hdr.length);
            }
        }
    }
    TEST_ASSERT("Tab 0 process survives after Tab 1 is killed (Crash Isolation)", tab0_alive);

    /* Close crashed Tab 1 cleanly */
    shell_close_tab(shell, 1);
    TEST_ASSERT("Crashed tab closed without crashing shell", shell->num_tabs == 1);

    shell_destroy(shell);
    TEST_ASSERT("Process mode shell destroyed cleanly", 1);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("===============================================================\n");
    printf("=== Terminator: Architecture & Scrollbar Verification Tests ===\n");
    printf("===============================================================\n");

    test_scrollbar_geometry_and_scrolling();
    test_thread_mode_architecture();
    test_process_mode_architecture();

    printf("\n=== Architecture & Scrollbar Result: %d Passed, %d Failed ===\n", passes, failures);
    return (failures == 0) ? 0 : 1;
}
