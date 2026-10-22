#ifndef TERMINATOR_TERMINAL_H
#define TERMINATOR_TERMINAL_H

#include "common.h"

typedef enum {
    LINE_TYPE_NORMAL = 0,
    LINE_TYPE_PROMPT,
    LINE_TYPE_CONTINUATION,
    LINE_TYPE_OUTPUT,
    LINE_TYPE_ERROR,
    LINE_TYPE_INFO
} LineType;

typedef enum {
    SHELL_STATE_READY = 0,        /* Ready for interactive input */
    SHELL_STATE_CONTINUATION,     /* Collecting multiline continuation */
    SHELL_STATE_RUNNING           /* Command submitted and currently executing */
} ShellState;

typedef enum {
    ANSI_STATE_NORMAL = 0,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
    ANSI_STATE_OSC
} AnsiState;

typedef struct {
    char *text;
    uint8_t *colors; /* Array of ThemeColorIndex for each character, or NULL */
    int length;
    LineType type;
} TerminalLine;

struct Shell;

typedef struct Tab {
    int id;
    char cwd[MAX_PATH_LEN];
    ShellState state;
    struct Shell *shell; /* Parent shell reference */
    bool request_close;  /* Set when tab requests exit */
    
    /* Input buffer and cursor */
    char input[MAX_INPUT_LEN];
    int cursor;
    
    /* Multiline input accumulation */
    char accumulated_cmd[MAX_CMD_LEN];
    bool in_continuation;
    
    /* Output scrollback buffer */
    TerminalLine *lines;
    int num_lines;
    int line_capacity;
    
    /* Partial line buffer for incremental stream reading */
    char partial_line[MAX_INPUT_LEN];
    uint8_t partial_colors[MAX_INPUT_LEN];
    int partial_len;
    
    /* ANSI escape sequence parser state */
    AnsiState ansi_state;
    char csi_buf[64];
    int csi_len;
    uint8_t current_fg_color;
    
    /* View / scrolling */
    int scroll_offset; /* 0 = bottom, >0 = scrolled up into history */
    
    /* Active execution tracking */
    int out_pipe_fd;
    pid_t running_pid;
    pid_t foreground_pgid;
    pid_t pipeline_pids[MAX_PIPELINE_CMDS];
    int num_pipeline_pids;
    int last_exit_status;
    
    /* Worker thread (Mode A) and child process (Mode B) lifecycle */
    pthread_t worker_thread;
    bool thread_created;
    pid_t child_pid;
    int to_worker_pipe[2];   /* GUI -> Worker */
    int from_worker_pipe[2]; /* Worker -> GUI */
    bool worker_running;
    
    /* History navigation index (queries Shell shared history) */
    int history_index; /* For Up/Down navigation (-1 = current new input) */
    char saved_draft[MAX_INPUT_LEN]; /* Saved draft when navigating history */
    
    bool echo_to_stdout; /* For headless/test CLI modes */
} Tab;

/* Terminal lifecycle */
Tab *terminal_tab_create(int id, const char *initial_dir, struct Shell *shell);
void terminal_tab_destroy(Tab *tab);

/* Input buffer operations */
void terminal_insert_char(Tab *tab, char c);
void terminal_insert_string(Tab *tab, const char *str);
void terminal_delete_backspace(Tab *tab);
void terminal_delete_char(Tab *tab);
void terminal_cursor_left(Tab *tab);
void terminal_cursor_right(Tab *tab);
void terminal_cursor_home(Tab *tab);
void terminal_cursor_end(Tab *tab);
void terminal_clear_input(Tab *tab);
void terminal_delete_to_start(Tab *tab);
void terminal_delete_to_end(Tab *tab);
void terminal_delete_word_backward(Tab *tab);

/* History navigation */
void terminal_history_add(Tab *tab, const char *cmd);
void terminal_history_prev(Tab *tab);
void terminal_history_next(Tab *tab);

/* Scrollback and output operations */
void terminal_append_line(Tab *tab, const char *text, LineType type);
void terminal_append_line_with_colors(Tab *tab, const char *text, const uint8_t *colors, int len, LineType type);
void terminal_append_output_bytes(Tab *tab, const char *bytes, size_t len, bool is_error);
void terminal_flush_partial_line(Tab *tab, bool is_error);
void terminal_clear_screen(Tab *tab);
void terminal_scroll_up(Tab *tab, int lines);
void terminal_scroll_down(Tab *tab, int lines);
void terminal_scroll_to_bottom(Tab *tab);

/* Prompt formatting */
void terminal_show_prompt(Tab *tab);

#endif /* TERMINATOR_TERMINAL_H */
