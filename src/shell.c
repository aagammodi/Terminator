#include "shell.h"
#include "util.h"
#include "ipc.h"
#include "executor.h"
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static void run_worker_loop(Tab *tab, int read_fd, int write_fd, bool is_child_process) {
    IPCHeader hdr;
    while (1) {
        if (!ipc_recv_header(read_fd, &hdr)) {
            break; /* Pipe closed or broken */
        }

        if (hdr.type == IPC_MSG_QUIT) {
            break;
        } else if (hdr.type == IPC_MSG_EXEC) {
            char *cmd = xmalloc(hdr.length + 1);
            if (hdr.length > 0) {
                if (!ipc_recv_payload(read_fd, cmd, hdr.length)) {
                    free(cmd);
                    break;
                }
            }
            cmd[hdr.length] = '\0';
            executor_run_line_worker(tab, cmd, write_fd);
            free(cmd);
        } else if (hdr.type == IPC_MSG_INT) {
            if (tab->running_pid > 0) {
                kill(-tab->running_pid, SIGINT);
                kill(tab->running_pid, SIGINT);
            }
            for (int i = 0; i < tab->num_pipeline_pids; i++) {
                if (tab->pipeline_pids[i] > 0) {
                    kill(tab->pipeline_pids[i], SIGINT);
                }
            }
        }
    }

    if (is_child_process) {
        _exit(0);
    }
}

static void *tab_worker_thread_func(void *arg) {
    Tab *tab = (Tab *)arg;
    run_worker_loop(tab, tab->to_worker_pipe[0], tab->from_worker_pipe[1], false);
    return NULL;
}

Shell *shell_create(const char *initial_dir, bool use_process_mode) {
    signal(SIGPIPE, SIG_IGN);

    Shell *shell = xcalloc(1, sizeof(Shell));
    shell->running = true;
    shell->use_process_mode = use_process_mode;
    shell->history = history_create(use_process_mode);

    Tab *tab = shell_open_tab(shell, initial_dir);
    if (!tab) {
        fprintf(stderr, "terminator: failed to initialize initial tab\n");
    }

    return shell;
}

void shell_destroy(Shell *shell) {
    if (!shell) return;
    for (int i = 0; i < shell->num_tabs; i++) {
        Tab *tab = shell->tabs[i];
        if (tab) {
            if (tab->to_worker_pipe[1] >= 0) {
                ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_QUIT, NULL, 0);
            }
            if (tab->thread_created) {
                pthread_join(tab->worker_thread, NULL);
                tab->thread_created = false;
            }
            if (tab->child_pid > 0) {
                if (tab->to_worker_pipe[1] >= 0) {
                    close(tab->to_worker_pipe[1]);
                    tab->to_worker_pipe[1] = -1;
                }
                int status = 0;
                pid_t wp = waitpid(tab->child_pid, &status, WNOHANG);
                if (wp <= 0) {
                    kill(tab->child_pid, SIGTERM);
                    waitpid(tab->child_pid, &status, 0);
                }
                tab->child_pid = -1;
            }
            if (tab->running_pid > 0) {
                kill(-tab->running_pid, SIGTERM);
                kill(tab->running_pid, SIGTERM);
            }
            for (int k = 0; k < tab->num_pipeline_pids; k++) {
                if (tab->pipeline_pids[k] > 0) {
                    kill(tab->pipeline_pids[k], SIGTERM);
                }
            }
            terminal_tab_destroy(tab);
            shell->tabs[i] = NULL;
        }
    }

    if (shell->history) {
        history_destroy(shell->history);
        shell->history = NULL;
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

    Tab *tab = terminal_tab_create(next_id, start_dir, shell);
    if (!tab) return NULL;

    /* Create bidirectional IPC pipes between GUI and Worker */
    if (pipe(tab->to_worker_pipe) < 0 || pipe(tab->from_worker_pipe) < 0) {
        perror("terminator: pipe create failed");
        terminal_tab_destroy(tab);
        return NULL;
    }

    if (shell->use_process_mode) {
        /* Process Mode (Mode B): fork child process */
        pid_t pid = fork();
        if (pid < 0) {
            perror("terminator: fork failed");
            close(tab->to_worker_pipe[0]);
            close(tab->to_worker_pipe[1]);
            close(tab->from_worker_pipe[0]);
            close(tab->from_worker_pipe[1]);
            terminal_tab_destroy(tab);
            return NULL;
        }

        if (pid == 0) {
            /* Child process: close GUI pipe ends */
            close(tab->to_worker_pipe[1]);
            close(tab->from_worker_pipe[0]);
            tab->to_worker_pipe[1] = -1;
            tab->from_worker_pipe[0] = -1;

            /* Close pipe ends inherited from other tabs */
            for (int i = 0; i < shell->num_tabs; i++) {
                if (shell->tabs[i]) {
                    if (shell->tabs[i]->to_worker_pipe[0] >= 0) close(shell->tabs[i]->to_worker_pipe[0]);
                    if (shell->tabs[i]->to_worker_pipe[1] >= 0) close(shell->tabs[i]->to_worker_pipe[1]);
                    if (shell->tabs[i]->from_worker_pipe[0] >= 0) close(shell->tabs[i]->from_worker_pipe[0]);
                    if (shell->tabs[i]->from_worker_pipe[1] >= 0) close(shell->tabs[i]->from_worker_pipe[1]);
                }
            }

            if (tab->cwd[0] != '\0') {
                if (chdir(tab->cwd) != 0) {
                    /* Directory might be inaccessible or changed */
                }
            }

            run_worker_loop(tab, tab->to_worker_pipe[0], tab->from_worker_pipe[1], true);
            _exit(0);
        }

        /* Parent process: close worker pipe ends */
        tab->child_pid = pid;
        close(tab->to_worker_pipe[0]);
        close(tab->from_worker_pipe[1]);
        tab->to_worker_pipe[0] = -1;
        tab->from_worker_pipe[1] = -1;
        tab->worker_running = true;
    } else {
        /* Thread Mode (Mode A): create POSIX thread */
        tab->child_pid = -1;
        if (pthread_create(&tab->worker_thread, NULL, tab_worker_thread_func, tab) != 0) {
            perror("terminator: pthread_create failed");
            close(tab->to_worker_pipe[0]);
            close(tab->to_worker_pipe[1]);
            close(tab->from_worker_pipe[0]);
            close(tab->from_worker_pipe[1]);
            terminal_tab_destroy(tab);
            return NULL;
        }
        tab->thread_created = true;
        tab->worker_running = true;
    }

    shell->tabs[shell->num_tabs] = tab;
    shell->active_tab_idx = shell->num_tabs;
    shell->num_tabs++;

    terminal_show_prompt(tab);
    return tab;
}

void shell_close_tab(Shell *shell, int tab_idx) {
    if (!shell || tab_idx < 0 || tab_idx >= shell->num_tabs) return;

    if (shell->num_tabs <= 1) {
        shell->running = false;
        return;
    }

    Tab *tab = shell->tabs[tab_idx];
    if (tab) {
        if (tab->child_pid > 0) {
            int status = 0;
            pid_t wp = waitpid(tab->child_pid, &status, WNOHANG);
            if (wp == tab->child_pid) {
                tab->child_pid = -1;
            }
        }

        if (tab->to_worker_pipe[1] >= 0) {
            ipc_send_msg(tab->to_worker_pipe[1], IPC_MSG_QUIT, NULL, 0);
            close(tab->to_worker_pipe[1]);
            tab->to_worker_pipe[1] = -1;
        }

        if (tab->thread_created) {
            pthread_join(tab->worker_thread, NULL);
            tab->thread_created = false;
        }

        if (tab->child_pid > 0) {
            int status = 0;
            usleep(10000);
            pid_t wp = waitpid(tab->child_pid, &status, WNOHANG);
            if (wp <= 0) {
                kill(tab->child_pid, SIGTERM);
                waitpid(tab->child_pid, &status, 0);
            }
            tab->child_pid = -1;
        }

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
