#ifndef TERMINATOR_PIPELINE_H
#define TERMINATOR_PIPELINE_H

#include "common.h"
#include "parser.h"
#include "terminal.h"

/* Execute an N-command pipeline (N >= 1) asynchronously.
 * Connects children with N-1 pipes.
 * Connects final stdout and child stderrs to tab->out_pipe_fd.
 * Registers child PIDs in tab->pipeline_pids.
 * Returns 0 on success (execution started), -1 on error. */
int pipeline_execute(Pipeline *pipeline, Tab *tab);

#endif /* TERMINATOR_PIPELINE_H */
