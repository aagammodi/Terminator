#include "history.h"
#include "util.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>

static inline void history_lock(SharedHistory *hist) {
    if (!hist) return;
    if (hist->is_process_mode) {
        while (sem_wait(&hist->sem) < 0) {
            if (errno == EINTR) continue;
            break;
        }
    } else {
        pthread_mutex_lock(&hist->mutex);
    }
}

static inline void history_unlock(SharedHistory *hist) {
    if (!hist) return;
    if (hist->is_process_mode) {
        sem_post(&hist->sem);
    } else {
        pthread_mutex_unlock(&hist->mutex);
    }
}

const char *history_get_file_path(char *buf, size_t len) {
    if (!buf || len == 0) return NULL;
    const char *custom = getenv("TERMINATOR_HISTORY");
    if (custom && custom[0] != '\0') {
        safe_strncpy(buf, custom, len);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || home[0] == '\0') {
        home = "/tmp";
    }
    snprintf(buf, len, "%s/.terminator_history", home);
    return buf;
}

static void escape_cmd_for_file(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size == 0) return;
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 3 < out_size; i++) {
        if (in[i] == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (in[i] == '\\') {
            out[o++] = '\\';
            out[o++] = '\\';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static void unescape_cmd_from_file(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size == 0) return;
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_size; i++) {
        if (in[i] == '\\' && in[i + 1] == 'n') {
            out[o++] = '\n';
            i++;
        } else if (in[i] == '\\' && in[i + 1] == '\\') {
            out[o++] = '\\';
            i++;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

void history_load_file(SharedHistory *hist, const char *path) {
    if (!hist) return;
    char path_buf[MAX_PATH_LEN];
    if (!path || path[0] == '\0') {
        path = history_get_file_path(path_buf, sizeof(path_buf));
    }
    if (!path) return;

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    /* Temporary dynamic buffer to read all entries from file */
    size_t cap = 2048;
    size_t total = 0;
    char (*raw_entries)[MAX_HISTORY_LINE_LEN] = xmalloc(cap * sizeof(*raw_entries));

    char line[MAX_HISTORY_LINE_LEN * 2];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\n') {
            line[l - 1] = '\0';
        }
        if (line[0] == '\0') continue;

        if (total >= cap) {
            cap *= 2;
            raw_entries = xrealloc(raw_entries, cap * sizeof(*raw_entries));
        }
        unescape_cmd_from_file(line, raw_entries[total], MAX_HISTORY_LINE_LEN);
        total++;
    }
    fclose(fp);

    history_lock(hist);
    /* Retain only the most recent MAX_SHARED_HISTORY entries */
    size_t start = (total > MAX_SHARED_HISTORY) ? (total - MAX_SHARED_HISTORY) : 0;
    hist->count = 0;
    for (size_t i = start; i < total && hist->count < MAX_SHARED_HISTORY; i++) {
        safe_strncpy(hist->entries[hist->count++], raw_entries[i], MAX_HISTORY_LINE_LEN);
    }
    history_unlock(hist);

    free(raw_entries);
}

void history_append_to_file(const char *path, const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;
    char path_buf[MAX_PATH_LEN];
    if (!path || path[0] == '\0') {
        path = history_get_file_path(path_buf, sizeof(path_buf));
    }
    if (!path) return;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;

    /* Acquire advisory file lock for multi-process safety */
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_END;
    fcntl(fd, F_SETLK, &fl);

    char esc[MAX_HISTORY_LINE_LEN * 2];
    escape_cmd_for_file(cmd, esc, sizeof(esc));

    ssize_t w1 = write(fd, esc, strlen(esc));
    ssize_t w2 = write(fd, "\n", 1);
    (void)w1;
    (void)w2;

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
}

SharedHistory *history_create(bool process_mode) {
    if (process_mode) {
        char name[64];
        snprintf(name, sizeof(name), "/terminator_shm_hist_%d_%ld", (int)getpid(), (long)time(NULL));

        int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            perror("terminator: shm_open");
            return NULL;
        }

        if (ftruncate(fd, sizeof(SharedHistory)) < 0) {
            perror("terminator: ftruncate shm");
            close(fd);
            shm_unlink(name);
            return NULL;
        }

        SharedHistory *hist = mmap(NULL, sizeof(SharedHistory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (hist == MAP_FAILED) {
            perror("terminator: mmap shm");
            close(fd);
            shm_unlink(name);
            return NULL;
        }

        memset(hist, 0, sizeof(SharedHistory));
        safe_strncpy(hist->shm_name, name, sizeof(hist->shm_name));
        hist->shm_fd = fd;
        hist->is_process_mode = true;
        hist->count = 0;

        /* Initialize process-shared POSIX semaphore with pshared=1 */
        if (sem_init(&hist->sem, 1, 1) < 0) {
            perror("terminator: sem_init (process-shared)");
            munmap(hist, sizeof(SharedHistory));
            close(fd);
            shm_unlink(name);
            return NULL;
        }

        /* Load persistent history */
        history_load_file(hist, NULL);

        return hist;
    } else {
        /* Thread mode: Heap allocated, protected by POSIX mutex */
        SharedHistory *hist = xcalloc(1, sizeof(SharedHistory));
        hist->is_process_mode = false;
        hist->count = 0;
        hist->shm_fd = -1;
        hist->shm_name[0] = '\0';
        pthread_mutex_init(&hist->mutex, NULL);

        /* Load persistent history */
        history_load_file(hist, NULL);

        return hist;
    }
}

void history_destroy(SharedHistory *hist) {
    if (!hist) return;

    if (hist->is_process_mode) {
        sem_destroy(&hist->sem);
        char name[64];
        safe_strncpy(name, hist->shm_name, sizeof(name));
        int fd = hist->shm_fd;

        munmap(hist, sizeof(SharedHistory));
        if (fd >= 0) {
            close(fd);
        }
        if (name[0] != '\0') {
            shm_unlink(name);
        }
    } else {
        pthread_mutex_destroy(&hist->mutex);
        free(hist);
    }
}

void history_add(SharedHistory *hist, const char *cmd) {
    if (!hist || !cmd || cmd[0] == '\0') return;

    history_lock(hist);

    /* Do not duplicate immediate previous command */
    if (hist->count > 0 && strcmp(hist->entries[hist->count - 1], cmd) == 0) {
        history_unlock(hist);
        return;
    }

    if (hist->count < MAX_SHARED_HISTORY) {
        safe_strncpy(hist->entries[hist->count], cmd, MAX_HISTORY_LINE_LEN);
        hist->count++;
    } else {
        /* Shift array to keep latest MAX_SHARED_HISTORY */
        memmove(&hist->entries[0], &hist->entries[1], (MAX_SHARED_HISTORY - 1) * MAX_HISTORY_LINE_LEN);
        safe_strncpy(hist->entries[MAX_SHARED_HISTORY - 1], cmd, MAX_HISTORY_LINE_LEN);
    }

    history_unlock(hist);

    /* Append to persistent storage */
    history_append_to_file(NULL, cmd);
}

int history_get_count(SharedHistory *hist) {
    if (!hist) return 0;
    history_lock(hist);
    int c = hist->count;
    history_unlock(hist);
    return c;
}

bool history_get_entry(SharedHistory *hist, int idx, char *out_buf, size_t out_size) {
    if (!hist || !out_buf || out_size == 0) return false;

    history_lock(hist);
    if (idx < 0 || idx >= hist->count) {
        history_unlock(hist);
        return false;
    }
    safe_strncpy(out_buf, hist->entries[idx], out_size);
    history_unlock(hist);
    return true;
}

void history_print(SharedHistory *hist, FILE *out_fp) {
    if (!hist || !out_fp) return;

    history_lock(hist);
    int total = hist->count;
    int start_idx = (total > 1000) ? (total - 1000) : 0;
    for (int i = start_idx; i < total; i++) {
        fprintf(out_fp, "%5d  %s\n", i + 1, hist->entries[i]);
    }
    history_unlock(hist);
    fflush(out_fp);
}

int longest_common_substring(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int m = (int)strlen(s1);
    int n = (int)strlen(s2);
    if (m == 0 || n == 0) return 0;

    int *prev = xcalloc((size_t)(n + 1), sizeof(int));
    int *curr = xcalloc((size_t)(n + 1), sizeof(int));
    int max_len = 0;

    for (int i = 1; i <= m; i++) {
        char c1 = s1[i - 1];
        for (int j = 1; j <= n; j++) {
            if (c1 == s2[j - 1]) {
                curr[j] = prev[j - 1] + 1;
                if (curr[j] > max_len) {
                    max_len = curr[j];
                }
            } else {
                curr[j] = 0;
            }
        }
        memcpy(prev, curr, (size_t)(n + 1) * sizeof(int));
    }

    free(prev);
    free(curr);
    return max_len;
}

int history_search(SharedHistory *hist, const char *term, char results[][MAX_HISTORY_LINE_LEN], int max_results) {
    if (!hist || !term || term[0] == '\0' || max_results <= 0) return 0;

    history_lock(hist);

    int count = hist->count;
    if (count == 0) {
        history_unlock(hist);
        return 0;
    }

    /* Snapshot commands under lock for non-blocking analysis */
    int num_items = count;
    char **snapshot = xmalloc((size_t)num_items * sizeof(char *));
    for (int i = 0; i < num_items; i++) {
        snapshot[i] = xstrdup(hist->entries[i]);
    }
    history_unlock(hist);

    int result_count = 0;

    /* Case A: Exact match */
    for (int i = num_items - 1; i >= 0; i--) {
        if (strcmp(snapshot[i], term) == 0) {
            safe_strncpy(results[0], snapshot[i], MAX_HISTORY_LINE_LEN);
            result_count = 1;
            goto cleanup;
        }
    }

    /* Case B: Longest Common Contiguous Substring */
    int max_lcs = 0;
    int *scores = xcalloc((size_t)num_items, sizeof(int));
    for (int i = num_items - 1; i >= 0; i--) {
        int lcs = longest_common_substring(term, snapshot[i]);
        scores[i] = lcs;
        if (lcs > max_lcs) {
            max_lcs = lcs;
        }
    }

    if (max_lcs > 2) {
        for (int i = num_items - 1; i >= 0 && result_count < max_results; i--) {
            if (scores[i] == max_lcs) {
                bool is_dup = false;
                for (int d = 0; d < result_count; d++) {
                    if (strcmp(results[d], snapshot[i]) == 0) {
                        is_dup = true;
                        break;
                    }
                }
                if (!is_dup) {
                    safe_strncpy(results[result_count++], snapshot[i], MAX_HISTORY_LINE_LEN);
                }
            }
        }
    }

    free(scores);

cleanup:
    for (int i = 0; i < num_items; i++) {
        free(snapshot[i]);
    }
    free(snapshot);

    return result_count;
}
