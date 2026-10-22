#include "shell.h"
#include "util.h"

Shell *shell_create(const char *initial_dir) {
    Shell *shell = xcalloc(1, sizeof(Shell));
    shell->num_tabs = 1;
    shell->active_tab_idx = 0;
    shell->running = true;

    shell->tabs[0] = terminal_tab_create(1, initial_dir);
    shell->tabs[0]->shell = shell;
    
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

Tab *shell_open_tab(Shell *shell, const char *initial_dir) {
    if (!shell) return NULL;
    if (shell->num_tabs >= MAX_TABS) {
        fprintf(stderr, "terminator: max tab limit reached (%d)\n", MAX_TABS);
        return NULL;
    }

    const char *start_dir = initial_dir;
    if (!start_dir || start_dir[0] == '\0') {
        Tab *active = shell_get_active_tab(shell);
        if (active) {
            start_dir = active->cwd;
        }
    }

    /* Compute unique tab ID */
    int next_id = 1;
    for (int i = 0; i < shell->num_tabs; i++) {
        if (shell->tabs[i] && shell->tabs[i]->id >= next_id) {
            next_id = shell->tabs[i]->id + 1;
        }
    }

    Tab *new_tab = terminal_tab_create(next_id, start_dir);
    new_tab->shell = shell;
    shell->tabs[shell->num_tabs] = new_tab;
    shell->active_tab_idx = shell->num_tabs;
    shell->num_tabs++;

    terminal_show_prompt(new_tab);
    return new_tab;
}

void shell_close_tab(Shell *shell, int tab_idx) {
    if (!shell || tab_idx < 0 || tab_idx >= shell->num_tabs) return;

    if (shell->num_tabs <= 1) {
        shell->running = false;
        return;
    }

    Tab *tab = shell->tabs[tab_idx];
    if (tab) {
        if (tab->running_pid > 0) {
            kill(-tab->running_pid, SIGTERM);
            kill(tab->running_pid, SIGTERM);
        }
        for (int i = 0; i < tab->num_pipeline_pids; i++) {
            if (tab->pipeline_pids[i] > 0) {
                kill(tab->pipeline_pids[i], SIGTERM);
            }
        }
        terminal_tab_destroy(tab);
    }

    /* Shift remaining tabs */
    for (int i = tab_idx; i < shell->num_tabs - 1; i++) {
        shell->tabs[i] = shell->tabs[i + 1];
    }
    shell->tabs[shell->num_tabs - 1] = NULL;
    shell->num_tabs--;

    if (shell->active_tab_idx >= shell->num_tabs) {
        shell->active_tab_idx = shell->num_tabs - 1;
    } else if (shell->active_tab_idx > tab_idx) {
        shell->active_tab_idx--;
    }
}

void shell_switch_tab(Shell *shell, int tab_idx) {
    if (!shell || tab_idx < 0 || tab_idx >= shell->num_tabs) return;
    shell->active_tab_idx = tab_idx;
}
