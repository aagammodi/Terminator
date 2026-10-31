#include "jobs.h"
#include "terminal.h"
#include "util.h"
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

void jobs_init(JobTable *table) {
    if (!table) return;
    memset(table, 0, sizeof(JobTable));
    table->next_id = 1;
}

void jobs_cleanup(JobTable *table) {
    if (!table) return;
    for (int i = 0; i < table->count; i++) {
        if (table->jobs[i].out_pipe_fd >= 0) {
            close(table->jobs[i].out_pipe_fd);
            table->jobs[i].out_pipe_fd = -1;
        }
    }
    table->count = 0;
}

const char *jobs_state_str(JobState state) {
    switch (state) {
        case JOB_STATE_RUNNING: return "Running";
        case JOB_STATE_STOPPED: return "Stopped";
        case JOB_STATE_DONE:    return "Done";
        default:                return "Unknown";
    }
}

Job *jobs_add(JobTable *table, pid_t pgid, const pid_t *pids, int num_pids,
              const char *cmd_line, JobState state, bool is_foreground, int out_pipe_fd) {
    if (!table || table->count >= MAX_JOBS) return NULL;

    /* Check if already present by pgid */
    Job *existing = jobs_find_by_pgid(table, pgid);
    if (existing) {
        existing->state = state;
        existing->is_foreground = is_foreground;
        if (out_pipe_fd >= 0 && existing->out_pipe_fd < 0) {
            existing->out_pipe_fd = out_pipe_fd;
        }
        return existing;
    }

    Job *j = &table->jobs[table->count];
    memset(j, 0, sizeof(Job));

    j->id = table->next_id++;
    j->pgid = pgid;
    j->num_pids = (num_pids <= MAX_PIPELINE_CMDS) ? num_pids : MAX_PIPELINE_CMDS;
    if (pids && j->num_pids > 0) {
        memcpy(j->pids, pids, (size_t)j->num_pids * sizeof(pid_t));
    }
    safe_strncpy(j->cmd_line, cmd_line ? cmd_line : "", sizeof(j->cmd_line));
    j->state = state;
    j->is_foreground = is_foreground;
    j->exit_status = 0;
    j->notified = false;
    j->out_pipe_fd = out_pipe_fd;

    table->count++;

    /* Update current and previous markers */
    if (table->current_id != j->id) {
        table->previous_id = table->current_id;
        table->current_id = j->id;
    }

    return j;
}

Job *jobs_find_by_id(JobTable *table, int id) {
    if (!table || id <= 0) return NULL;
    for (int i = 0; i < table->count; i++) {
        if (table->jobs[i].id == id) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_find_by_pgid(JobTable *table, pid_t pgid) {
    if (!table || pgid <= 0) return NULL;
    for (int i = 0; i < table->count; i++) {
        if (table->jobs[i].pgid == pgid) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_get_current(JobTable *table) {
    if (!table || table->count == 0) return NULL;
    Job *j = jobs_find_by_id(table, table->current_id);
    if (j && j->state != JOB_STATE_DONE) return j;

    /* Otherwise, pick the latest non-done job */
    for (int i = table->count - 1; i >= 0; i--) {
        if (table->jobs[i].state != JOB_STATE_DONE) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_get_current_stopped(JobTable *table) {
    if (!table || table->count == 0) return NULL;
    /* First check current_id */
    Job *j = jobs_find_by_id(table, table->current_id);
    if (j && j->state == JOB_STATE_STOPPED) return j;

    /* Next check previous_id */
    j = jobs_find_by_id(table, table->previous_id);
    if (j && j->state == JOB_STATE_STOPPED) return j;

    /* Otherwise, most recent stopped job */
    for (int i = table->count - 1; i >= 0; i--) {
        if (table->jobs[i].state == JOB_STATE_STOPPED) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

void jobs_update_status(JobTable *table, struct Tab *tab) {
    if (!table) return;

    for (int i = 0; i < table->count; i++) {
        Job *j = &table->jobs[i];
        if (j->state == JOB_STATE_DONE) continue;

        /* Drain background output if available */
        if (j->out_pipe_fd >= 0) {
            char buf[4096];
            int flags = fcntl(j->out_pipe_fd, F_GETFL, 0);
            if (!(flags & O_NONBLOCK)) {
                fcntl(j->out_pipe_fd, F_SETFL, flags | O_NONBLOCK);
            }
            ssize_t n;
            while ((n = read(j->out_pipe_fd, buf, sizeof(buf))) > 0) {
                if (tab) {
                    terminal_append_output_bytes(tab, buf, (size_t)n, false);
                }
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(j->out_pipe_fd);
                j->out_pipe_fd = -1;
            }
        }

        /* Check child process statuses */
        int alive_count = 0;
        for (int p = 0; p < j->num_pids; p++) {
            pid_t pid = j->pids[p];
            if (pid <= 0) continue;

            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (r == pid) {
                if (WIFSTOPPED(status)) {
                    j->state = JOB_STATE_STOPPED;
                    j->is_foreground = false;
                    if (table->current_id != j->id) {
                        table->previous_id = table->current_id;
                        table->current_id = j->id;
                    }
                    alive_count++;
                } else if (WIFCONTINUED(status)) {
                    j->state = JOB_STATE_RUNNING;
                    alive_count++;
                } else if (WIFEXITED(status)) {
                    if (p == j->num_pids - 1) {
                        j->exit_status = WEXITSTATUS(status);
                    }
                    j->pids[p] = 0;
                } else if (WIFSIGNALED(status)) {
                    if (p == j->num_pids - 1) {
                        j->exit_status = 128 + WTERMSIG(status);
                    }
                    j->pids[p] = 0;
                }
            } else if (r == 0) {
                alive_count++;
            } else if (r < 0 && errno == ECHILD) {
                j->pids[p] = 0;
            } else {
                alive_count++;
            }
        }

        if (alive_count == 0 && j->state != JOB_STATE_STOPPED) {
            j->state = JOB_STATE_DONE;
            /* Drain any final output */
            if (j->out_pipe_fd >= 0) {
                char buf[4096];
                ssize_t n;
                while ((n = read(j->out_pipe_fd, buf, sizeof(buf))) > 0) {
                    if (tab) {
                        terminal_append_output_bytes(tab, buf, (size_t)n, false);
                    }
                }
                close(j->out_pipe_fd);
                j->out_pipe_fd = -1;
            }
        }
    }
}

void jobs_list(JobTable *table, FILE *out_fp, struct Tab *tab) {
    if (!table || !out_fp) return;

    jobs_update_status(table, tab);

    for (int i = 0; i < table->count; i++) {
        Job *j = &table->jobs[i];
        char marker = ' ';
        if (j->id == table->current_id) {
            marker = '+';
        } else if (j->id == table->previous_id) {
            marker = '-';
        }

        const char *state_str = jobs_state_str(j->state);
        bool has_amp = (strstr(j->cmd_line, "&") != NULL);

        if (j->state == JOB_STATE_RUNNING && !has_amp) {
            fprintf(out_fp, "[%d]%c %-20s %s &\n", j->id, marker, state_str, j->cmd_line);
        } else {
            fprintf(out_fp, "[%d]%c %-20s %s\n", j->id, marker, state_str, j->cmd_line);
        }
        j->notified = true;
    }
    fflush(out_fp);

    jobs_remove_done(table);
}

void jobs_remove_done(JobTable *table) {
    if (!table) return;
    int write_idx = 0;
    for (int i = 0; i < table->count; i++) {
        if (table->jobs[i].state == JOB_STATE_DONE && table->jobs[i].notified) {
            if (table->jobs[i].out_pipe_fd >= 0) {
                close(table->jobs[i].out_pipe_fd);
                table->jobs[i].out_pipe_fd = -1;
            }
            continue;
        }
        if (write_idx != i) {
            table->jobs[write_idx] = table->jobs[i];
        }
        write_idx++;
    }
    table->count = write_idx;
}

void jobs_remove_by_pgid(JobTable *table, pid_t pgid) {
    if (!table || pgid <= 0) return;
    int write_idx = 0;
    for (int i = 0; i < table->count; i++) {
        if (table->jobs[i].pgid == pgid) {
            if (table->jobs[i].out_pipe_fd >= 0) {
                close(table->jobs[i].out_pipe_fd);
                table->jobs[i].out_pipe_fd = -1;
            }
            continue;
        }
        if (write_idx != i) {
            table->jobs[write_idx] = table->jobs[i];
        }
        write_idx++;
    }
    table->count = write_idx;
}
