#include "shell.h"
#include "util.h"

Shell *shell_create(const char *initial_dir) {
    Shell *shell = xcalloc(1, sizeof(Shell));
    shell->num_tabs = 1;
    shell->active_tab_idx = 0;
    shell->running = true;

    shell->tabs[0] = terminal_tab_create(1, initial_dir);
    
    /* Initial prompt for the first tab */
    terminal_show_prompt(shell->tabs[0]);

    return shell;
}

void shell_destroy(Shell *shell) {
    if (!shell) return;
    for (int i = 0; i < shell->num_tabs; i++) {
        if (shell->tabs[i]) {
            terminal_tab_destroy(shell->tabs[i]);
            shell->tabs[i] = NULL;
        }
    }
    free(shell);
}

Tab *shell_get_active_tab(Shell *shell) {
    if (!shell || shell->num_tabs == 0) return NULL;
    if (shell->active_tab_idx < 0 || shell->active_tab_idx >= shell->num_tabs) {
        shell->active_tab_idx = 0;
    }
    return shell->tabs[shell->active_tab_idx];
}

void shell_format_prompt(const Tab *tab, char *out, size_t out_size) {
    if (!out || out_size == 0) return;

    if (tab && tab->in_continuation) {
        safe_strncpy(out, "> ", out_size);
        return;
    }

    const char *user = getenv("USER");
    if (!user || user[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name) {
            user = pw->pw_name;
        } else {
            user = "user";
        }
    }

    snprintf(out, out_size, "%s@terminator> ", user);
}
