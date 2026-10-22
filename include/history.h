#ifndef TERMINATOR_HISTORY_H
#define TERMINATOR_HISTORY_H

#include "common.h"
#include <semaphore.h>
#include <pthread.h>

#define MAX_SHARED_HISTORY 2048
#define MAX_HISTORY_LINE_LEN 1024

typedef struct SharedHistory {
    sem_t sem;                         /* POSIX process-shared semaphore (Mode B) */
    pthread_mutex_t mutex;             /* Mutex for thread mode (Mode A) */
    bool is_process_mode;              /* True if running in process mode */
    int count;                         /* Number of commands in history */
    char entries[MAX_SHARED_HISTORY][MAX_HISTORY_LINE_LEN];
    char shm_name[64];
    int shm_fd;
} SharedHistory;

SharedHistory *history_create(bool process_mode);
void history_destroy(SharedHistory *hist);
void history_add(SharedHistory *hist, const char *cmd);
int history_get_count(SharedHistory *hist);
bool history_get_entry(SharedHistory *hist, int idx, char *out_buf, size_t out_size);
void history_print(SharedHistory *hist, FILE *out_fp);

#endif /* TERMINATOR_HISTORY_H */
