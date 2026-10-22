#include "history.h"
#include "util.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

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

        return hist;
    } else {
        /* Thread mode: Heap allocated, protected by POSIX mutex */
        SharedHistory *hist = xcalloc(1, sizeof(SharedHistory));
        hist->is_process_mode = false;
        hist->count = 0;
        hist->shm_fd = -1;
        hist->shm_name[0] = '\0';
        pthread_mutex_init(&hist->mutex, NULL);
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
        /* Shift array */
        memmove(&hist->entries[0], &hist->entries[1], (MAX_SHARED_HISTORY - 1) * MAX_HISTORY_LINE_LEN);
        safe_strncpy(hist->entries[MAX_SHARED_HISTORY - 1], cmd, MAX_HISTORY_LINE_LEN);
    }

    history_unlock(hist);
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
    for (int i = 0; i < hist->count; i++) {
        fprintf(out_fp, "%5d  %s\n", i + 1, hist->entries[i]);
    }
    history_unlock(hist);
    fflush(out_fp);
}
