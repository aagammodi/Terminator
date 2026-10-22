#ifndef TERMINATOR_PARSER_H
#define TERMINATOR_PARSER_H

#include "common.h"

typedef struct Command {
    char **argv;
    int argc;
    int argv_capacity;
    char *input_file;
    char *output_file;
    bool append_output;
} Command;

typedef struct Pipeline {
    Command *commands;
    int num_commands;
    int capacity;
} Pipeline;

/* Detect if command line has open quotes, trailing backslash, or unclosed substitutions */
bool parser_is_incomplete(const char *cmd_line);

/* Parse a complete command line into a pipeline with redirections and expanded arguments */
Pipeline *parser_parse_pipeline(const char *cmd_line, int last_exit_status);

/* Command and pipeline memory management */
void command_init(Command *cmd);
void command_free(Command *cmd);
void command_add_arg(Command *cmd, const char *arg);
Pipeline *pipeline_create(void);
void pipeline_free(Pipeline *p);

#endif /* TERMINATOR_PARSER_H */
