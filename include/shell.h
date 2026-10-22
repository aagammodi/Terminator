#ifndef TERMINATOR_SHELL_H
#define TERMINATOR_SHELL_H

#include "common.h"
#include "terminal.h"
#include "history.h"

#define MAX_TABS 16

typedef struct Shell {
    Tab *tabs[MAX_TABS];
    int num_tabs;
    int active_tab_idx;
    bool running;
    bool headless;
    bool use_process_mode; /* False = Thread Mode (default), True = Process Mode */
    SharedHistory *history; /* Shared command history across all tabs */
} Shell;

Shell *shell_create(const char *initial_dir, bool use_process_mode);
void shell_destroy(Shell *shell);
Tab *shell_get_active_tab(Shell *shell);
void shell_format_prompt(const Tab *tab, char *out, size_t out_size);

/* Multi-tab management (Task 1, Thread Mode A & Process Mode B) */
Tab *shell_open_tab(Shell *shell, const char *initial_dir);
void shell_close_tab(Shell *shell, int tab_idx);
void shell_switch_tab(Shell *shell, int tab_idx);

#endif /* TERMINATOR_SHELL_H */
