#ifndef TERMINATOR_JOBS_H
#define TERMINATOR_JOBS_H

#include "common.h"
#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
    JOB_STATE_RUNNING = 0,
    JOB_STATE_STOPPED,
    JOB_STATE_DONE
} JobState;

typedef struct Job {
    int id;                           /* 1, 2, 3... */
    pid_t pgid;                       /* Process group ID */
    pid_t pids[MAX_PIPELINE_CMDS];    /* Child PIDs in pipeline */
    int num_pids;
    char cmd_line[MAX_CMD_LEN];       /* Original command string */
    JobState state;
    bool is_foreground;
    int exit_status;
    bool notified;                    /* Whether completion has been reported */
    int out_pipe_fd;                  /* Output pipe if background job captures output */
} Job;

#define MAX_JOBS 64

struct Tab;

typedef struct JobTable {
    Job jobs[MAX_JOBS];
    int count;
    int current_id;   /* '+' marker (most recently stopped or backgrounded job) */
    int previous_id;  /* '-' marker (second most recent) */
    int next_id;      /* Counter for assigning sequential job IDs */
} JobTable;

void jobs_init(JobTable *table);
void jobs_cleanup(JobTable *table);

Job *jobs_add(JobTable *table, pid_t pgid, const pid_t *pids, int num_pids,
              const char *cmd_line, JobState state, bool is_foreground, int out_pipe_fd);

Job *jobs_find_by_id(JobTable *table, int id);
Job *jobs_find_by_pgid(JobTable *table, pid_t pgid);
Job *jobs_get_current(JobTable *table);
Job *jobs_get_current_stopped(JobTable *table);

void jobs_update_status(JobTable *table, struct Tab *tab);
void jobs_list(JobTable *table, FILE *out_fp, struct Tab *tab);
void jobs_remove_done(JobTable *table);
void jobs_remove_by_pgid(JobTable *table, pid_t pgid);
const char *jobs_state_str(JobState state);

#endif /* TERMINATOR_JOBS_H */
