#ifndef TERMINATOR_SHELL_H
#define TERMINATOR_SHELL_H

#include "common.h"
#include "terminal.h"

#define MAX_TABS 16

typedef struct Shell {
    Tab *tabs[MAX_TABS];
    int num_tabs;
    int active_tab_idx;
    bool running;
    bool headless;
} Shell;

Shell *shell_create(const char *initial_dir);
void shell_destroy(Shell *shell);
Tab *shell_get_active_tab(Shell *shell);
void shell_format_prompt(const Tab *tab, char *out, size_t out_size);

#endif /* TERMINATOR_SHELL_H */
