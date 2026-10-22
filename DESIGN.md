# Terminator Architecture & Systems Design

This document details the engineering architecture, data structures, POSIX system-call workflows, and execution models of **Terminator** (Phase 1, Tasks 1–5).

---

## 1. Overall System Architecture

Terminator is composed of two primary subsystems operating in harmony:
1. **The X11 Terminal Presentation Engine**: Responsible for window creation, font metrics, event handling, double-buffered rendering, input line editing, and scrollback visualization.
2. **The Shell & Process Execution Engine**: Responsible for lexical tokenization, quoting rules, variable expansion, nested command substitutions, globbing, I/O redirection, pipeline construction, child process lifecycle management, and output streaming.

```
+--------------------------------------------------------------------+
|                         Terminator Process                         |
|                                                                    |
|  +--------------------+     X11 Events      +-------------------+  |
|  |  Xlib Window & GC  | <=================> |  poll() Event     |  |
|  |  (Double-buffered) |                     |  Loop             |  |
|  +--------------------+                     +---------+---------+  |
|            ^                                          |            |
|            | redraws from                             | reads pipe |
|            v                                          v            |
|  +--------------------+                     +---------+---------+  |
|  |  Tab Data Model    |                     | Capture Pipe Read |  |
|  |  (lines, input,    |                     | fd (tab->out_fd)  |  |
|  |   cwd, history)    |                     +---------^---------+  |
|  +---------^----------+                               |            |
|            |                                          |            |
|            | runs parsed command                      |            |
|            v                                          |            |
|  +--------------------+       pipe() + fork()         |            |
|  | Shell Engine       | --------------------------+   |            |
|  | (parser/expansion) |                           |   |            |
|  +--------------------+                           |   |            |
+---------------------------------------------------|---|------------+
                                                    |   |
                                                    v   |
                       +--------------------------------+--+
                       | Child Process / Pipeline          |
                       |  - dup2() redirection onto fds    |
                       |  - close() unused descriptors     |
                       |  - execvp("cmd", argv)            |
                       |  - stdout / stderr -> capture pipe|
                       +-----------------------------------+
```

---

## 2. X11 GUI & Event Loop Design

### Window Creation & Graphic Context
- Window initialization connects to the X server via `XOpenDisplay(NULL)`.
- The main window is created via `XCreateWindow` using visual and colormap attributes with `CWBackPixel | CWEventMask`.
- Event masks selected: `ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask`.
- Window manager protocol `WM_DELETE_WINDOW` is registered via `XInternAtom` and `XSetWMProtocols` to ensure safe window closure without orphan background processes.

### Double-Buffering Mechanism
Direct-to-window drawing often causes severe flickering during fast scrolling or typing. Terminator implements strict double-buffering:
1. An off-screen `Pixmap` matching the current window dimensions is created via `XCreatePixmap`.
2. All drawing operations (background fills, top bar rendering, text strings, selection pills, borders, status bars, and cursors) draw exclusively onto the back-buffer Pixmap.
3. Once rendering is complete, `XCopyArea` transfers the entire back-buffer Pixmap to the visible X11 window in one atomic screen refresh, followed by `XFlush`.
4. When the window is resized (`ConfigureNotify`), the old Pixmap is freed and a new Pixmap is reallocated to match the updated geometry.

### Responsive Non-Blocking Event Loop (poll)
The heart of Terminator is an event-driven multiplexer that never busy-waits:

```c
struct pollfd fds[2];
fds[0].fd = ConnectionNumber(dpy); /* X11 connection */
fds[0].events = POLLIN;

if (tab->out_pipe_fd >= 0) {
    fds[1].fd = tab->out_pipe_fd;   /* Active command capture pipe */
    fds[1].events = POLLIN | POLLHUP | POLLERR;
    nfds = 2;
}

int pret = poll(fds, nfds, 50); /* 50ms responsive tick */
```

- When the user presses a key or resizes the window, `fds[0]` wakes up `poll()`. Pending events are drained using `while (XPending(dpy)) { XNextEvent(...); }`.
- When a running child process produces output, `fds[1]` wakes up `poll()`. `read()` reads incoming byte chunks and appends them to the tab's output model.
- When the child process finishes and closes its write descriptors, `read()` returns 0 (EOF). Terminator detects this, closes the read-end descriptor, reaps all child processes via `waitpid()`, records `$?`, prints the return prompt, and requests a redraw.
- If no events or pipes are active, `poll()` puts the process to sleep in the kernel (near 0% CPU usage).

---

## 3. Data Structures & Memory Management

### Tab Structure (`struct Tab`)
The tab data structure represents an isolated shell and terminal instance:

```c
typedef struct Tab {
    int id;                             /* Tab identifier (1, 2, ...) */
    char cwd[MAX_PATH_LEN];             /* Current working directory */
    
    /* Input line and cursor */
    char input[MAX_INPUT_LEN];          /* Active line being edited */
    int cursor;                         /* Cursor position (0 .. strlen) */
    
    /* Multiline state */
    char accumulated_cmd[MAX_CMD_LEN];  /* Accumulated multiline buffer */
    bool in_continuation;               /* True when waiting for continuation */
    
    /* Scrollback buffer */
    TerminalLine *lines;                /* Dynamic array of terminal lines */
    int num_lines;                      /* Number of lines stored */
    int line_capacity;                  /* Capacity of line buffer */
    
    /* Incremental stream parser */
    char partial_line[MAX_INPUT_LEN];   /* Partial line received from pipe */
    int partial_len;                    /* Current bytes in partial line */
    
    /* View / Scrolling */
    int scroll_offset;                  /* Lines scrolled back from bottom */
    
    /* Execution state */
    int out_pipe_fd;                    /* Read end of output capture pipe */
    pid_t running_pid;                  /* Active child or group leader */
    pid_t pipeline_pids[MAX_PIPELINE_CMDS]; /* All child PIDs in pipeline */
    int num_pipeline_pids;              /* Count of child processes */
    int last_exit_status;               /* Exit status for $? expansion */
    
    /* In-memory history */
    char **history;                     /* Array of historical command strings */
    int history_count;                  /* History count */
    int history_capacity;               /* History capacity */
    int history_index;                  /* Current history browsing index */
    char saved_draft[MAX_INPUT_LEN];    /* Draft saved during history navigation */
    
    bool echo_to_stdout;                /* Set for headless / test modes */
} Tab;
```

### Command & Pipeline AST
```c
typedef struct Command {
    char **argv;                        /* NULL-terminated argument vector */
    int argc;                           /* Argument count */
    int argv_capacity;                  /* Capacity of argv buffer */
    char *input_file;                   /* Path for '<' redirection (or NULL) */
    char *output_file;                  /* Path for '>' / '>>' (or NULL) */
    bool append_output;                 /* True for '>>', False for '>' */
} Command;

typedef struct Pipeline {
    Command *commands;                  /* Array of pipeline stages */
    int num_commands;                   /* Number of pipeline commands */
    int capacity;                       /* Allocated capacity */
} Pipeline;
```

---

## 4. Parser & Expansion Architecture

The shell parser does not rely on naive `strtok()` splitting. It implements a multi-stage lexer/parser state machine:

### Stage 1: Incomplete Command Detection
Before attempting execution, the lexer scans the raw command string tracking quote states:
- Unquoted state
- Single-quote state (`'...'`)
- Double-quote state (`"..."`)
- Escape state (`\` outside single quotes)
- Command substitution nesting (`$(...)` parenthesis depth and `` `...` ``)

If the line ends with an unclosed single quote, unclosed double quote, unclosed substitution, or an unescaped trailing `\`, `parser_is_incomplete()` returns `true`. The terminal preserves the accumulated text, displays the continuation prompt `> `, and waits for further user input.

### Stage 2: Recursive Innermost Command Substitution
Substitutions (`$(cmd)` and `` `cmd` ``) are resolved recursively:
1. The lexer scans for the innermost substitution candidate (a substitution containing no nested `$(` or `` ` `` before its termination).
2. The inner command text is extracted and passed to `executor_execute_substitution_sync()`.
3. The inner command is parsed and executed through Terminator's own pipeline engine with stdout directed to an internal pipe.
4. The output is collected until EOF, trailing newlines are stripped, and the output string replaces the `$(...)` expression.
5. The scan repeats until all substitutions are resolved.

### Stage 3: Variable Expansion
The substituted string is expanded for `$VAR`, `${VAR}`, `$?`, and `$$`:
- Single-quoted segments are strictly untouched: `'literal: $HOME'` remains `'literal: $HOME'`.
- Double-quoted and unquoted segments expand variables: `"expanded: $HOME"` expands `$HOME` to `/home/user`.
- Escaped dollars (`\$`) produce literal `$`.

### Stage 4: Pipeline Splitting & Word Tokenization
1. The expanded command line is split into pipeline stages by unquoted pipe characters (`|`).
2. Each stage is scanned for redirection operators (`<`, `>`, `>>`). Redirection filenames are extracted.
3. Arguments are tokenized with quote-removal:
   - Outer quotes are stripped while preserving the content and internal spaces intact.
   - Tokens containing unquoted wildcards (`*`, `?`, `[...]`) are expanded via POSIX `glob()`.
   - Quoted wildcards (e.g. `"*.txt"`) are protected from globbing.
   - The resulting `argv` vector is constructed and null-terminated.

---

## 5. File Descriptors, Redirection & Pipelines

### Single Command Redirection
For a single command with `< in.txt > out.txt`:

```
Parent Shell:
   - Creates capture_pipe[2]
   - fork()
   - In Child:
       in_fd  = open("in.txt", O_RDONLY)
       out_fd = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644)
       dup2(in_fd, STDIN_FILENO);   close(in_fd)
       dup2(out_fd, STDOUT_FILENO); close(out_fd)
       dup2(capture_pipe[1], STDERR_FILENO)
       close(capture_pipe[0]); close(capture_pipe[1])
       execvp(argv[0], argv)
   - In Parent:
       close(capture_pipe[1])
       tab->out_pipe_fd = capture_pipe[0]
       waitpid(pid, &status, WNOHANG) via event loop
```

### Pipeline Descriptor Management ($N$ Commands)
For a pipeline `cmd1 | cmd2 | ... | cmdN`:
1. Create $N-1$ pipes: `int pipes[N-1][2]`.
2. Create terminal output capture pipe: `int capture_pipe[2]`.
3. Fork $N$ children:
   - **Child $0$**:
     - STDIN: Redirected from file if `< in.txt` was given.
     - STDOUT: `dup2(pipes[0][1], STDOUT_FILENO)`.
     - STDERR: `dup2(capture_pipe[1], STDERR_FILENO)`.
   - **Child $i$ ($0 < i < N-1$)**:
     - STDIN: `dup2(pipes[i-1][0], STDIN_FILENO)`.
     - STDOUT: `dup2(pipes[i][1], STDOUT_FILENO)`.
     - STDERR: `dup2(capture_pipe[1], STDERR_FILENO)`.
   - **Child $N-1$ (Final Command)**:
     - STDIN: `dup2(pipes[N-2][0], STDIN_FILENO)`.
     - STDOUT: If `> out.txt` given, redirected to file; otherwise `dup2(capture_pipe[1], STDOUT_FILENO)`.
     - STDERR: `dup2(capture_pipe[1], STDERR_FILENO)`.
4. **Child Descriptor Cleanup**:
   Every child closes all ends of all $N-1$ pipeline pipes and both ends of `capture_pipe`.
5. **Parent Descriptor Cleanup (Deadlock Prevention)**:
   Immediately after all $N$ children are forked, the parent closes all $2 \times (N-1)$ pipe descriptors and closes `capture_pipe[1]`.
   *Critical principle*: If the parent forgets to close any write end, the downstream reader will never receive EOF, causing an eternal hang. Terminator guarantees immediate parent cleanup.
6. **Parent Child Reaping**:
   Parent monitors `capture_pipe[0]` in `poll()`. Upon EOF, parent iterates through all recorded PIDs with `waitpid(pid, &status, 0)` and records the exit status of child $N-1$ into `tab->last_exit_status`.

---

## 6. Execution Model Evolution (Roadmap to Phase 2)

Phase 1 establishes the single-tab foundation with full isolation of the tab state data structure. Here is how the architecture is designed to evolve into Tasks 6–11:

### Current Single-Tab Foundation (Phase 1)
- The application owns the X11 connection and event loop.
- The single tab's shell engine runs within the application process.
- All external commands run as forked child processes.
- The tab structure encapsulates working directory, input buffer, scrollback history, and running child PIDs.

### Thread Mode (Task 6+ Architecture)
```
+-------------------------------------------------------------+
|                     Terminator Application                  |
|                                                             |
|  [ GUI Thread ]                                             |
|    - Owns X11 Display, Window, and Event Loop               |
|    - Double-buffers rendering from tab buffers              |
|                                                             |
|  [ Tab 1 Thread ]               [ Tab 2 Thread ]            |
|    - Shell engine tab 1           - Shell engine tab 2      |
|    - Own working directory        - Own working directory   |
|    - fork() + exec child jobs     - fork() + exec child jobs|
|                                                             |
|  [ Shared State ]                                           |
|    - In-memory global history protected by pthread_mutex_t  |
|    - Thread-safe clipboard                                  |
+-------------------------------------------------------------+
```

### Process Mode (`useProcess=True`) (Task 6+ Architecture)
```
+-------------------------------------------------------------+
|                      Parent GUI Process                     |
|    - Owns X11 Display and poll() loop                       |
|    - Reads tab output pipes                                 |
+------------------------------+------------------------------+
                               | IPC pipes
        +----------------------+----------------------+
        v                                             v
+-------------------------------+   +-------------------------------+
|     Tab 1 Process (Child)     |   |     Tab 2 Process (Child)     |
|   - Own shell engine          |   |   - Own shell engine          |
|   - Private CWD, private fds  |   |   - Private CWD, private fds  |
|   - fork() + exec commands    |   |   - fork() + exec commands    |
+---------------+---------------+   +---------------+---------------+
                |                                   |
                +-----------------+-----------------+
                                  v
                    +---------------------------+
                    | Shared Memory (POSIX shm) |
                    | - Global history buffer   |
                    | - Process-shared sem_t    |
                    +---------------------------+
```
Because `Tab` state is completely decoupled from global variables and Xlib calls are kept strictly in the GUI presentation layer, transitioning from single-tab to multi-tab Thread or Process mode in Phase 2 requires no rewrite of the parser, expansion, redirection, or pipeline execution engine.
