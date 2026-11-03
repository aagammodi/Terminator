# Terminator Architecture & Systems Design

This document details the engineering architecture, data structures, POSIX system-call workflows, and execution models of **Terminator**.

---

## Section A: Overall Architecture

Terminator is composed of two primary subsystems operating in harmony:
1. **The X11 Terminal Presentation Engine**: Responsible for window creation, font metrics, event handling, double-buffered rendering, input line editing, proportional scrollbar interaction, and scrollback visualization.
2. **The Shell & Process Execution Engine**: Responsible for lexical tokenization, quoting rules, variable expansion, nested command substitutions, globbing, I/O redirection, pipeline construction, child process lifecycle management, and output streaming.

Terminator provides two distinct, assignment-compliant concurrency architectures:
- **Mode A (Thread Mode — Default)**: The GUI runs in the main thread; each tab runs an independent POSIX thread (`pthread_create`). Shared history is synchronized via `pthread_mutex_t`.
- **Mode B (Process Mode — `useProcess=True`)**: The GUI runs in the parent process; each tab runs in an independent child process (`fork()`). Shared history resides in POSIX shared memory (`shm_open`, `mmap`) protected by a **process-shared semaphore** (`sem_init(..., 1, 1)`).

Both modes share a symmetric, framed IPC protocol across unidirectional pipe pairs (`to_worker_pipe` and `from_worker_pipe`), ensuring consistent behavior, clean crash isolation, and identical rendering.

```
=============================================================================
MODE A — THREAD MODE (Default: ./terminator)
=============================================================================
+---------------------------------------------------------------------------+
| Terminator Application Process                                            |
|                                                                           |
|  +--------------------+     X11 Events      +--------------------------+  |
|  |  Xlib Window & GC  | <=================> |  GUI Main Thread (poll)  |  |
|  |  (Double-buffered) |                     +------------+-------------+  |
|  +--------------------+                                  |                |
|                                     IPC Pipes (to/from)  |                |
|               +------------------------------------------+                |
|               |                                          |                |
|               v                                          v                |
|  +--------------------------+              +--------------------------+   |
|  | Tab 1 POSIX Thread       |              | Tab 2 POSIX Thread       |   |
|  |  - Own shell worker loop |              |  - Own shell worker loop |   |
|  |  - Private tab->cwd      |              |  - Private tab->cwd      |   |
|  |  - fork()+execvp() child |              |  - fork()+execvp() child |   |
|  +------------+-------------+              +------------+-------------+   |
|               |                                         |                 |
|               +--------------------+--------------------+                 |
|                                    v                                      |
|                     +-----------------------------+                       |
|                     | Shared In-Memory History    |                       |
|                     | - Synchronized via Mutex    |                       |
|                     +-----------------------------+                       |
+---------------------------------------------------------------------------+

=============================================================================
MODE B — PROCESS MODE (Flag: ./terminator useProcess=True)
=============================================================================
+---------------------------------------------------------------------------+
| Parent GUI Process                                                        |
|  - Owns X11 Display, Window, double-buffer Pixmap, and poll() event loop  |
|  - Manages tab bar, scrollbar, user input, and terminal rendering         |
+-------------------------------------+-------------------------------------+
                                      | Framed IPC Pipes
            +-------------------------+-------------------------+
            v                                                   v
+-------------------------------+                   +-------------------------------+
| Tab 1 Child Process           |                   | Tab 2 Child Process           |
|  - Own shell worker loop      |                   |  - Own shell worker loop      |
|  - Private CWD (chdir isolated|                   |  - Private CWD (chdir isolated|
|  - fork()+execvp() pipelines  |                   |  - fork()+execvp() pipelines  |
+---------------+---------------+                   +---------------+---------------+
                |                                                   |
                +-------------------------+-------------------------+
                                          v
                            +---------------------------+
                            | POSIX Shared Memory (shm) |
                            |  - shm_open() + mmap()    |
                            |  - Process-Shared sem_t   |
                            |    (sem_init(&sem, 1, 1)) |
                            +---------------------------+
```

---

## Section B: Data Structures and Component Responsibilities

The Terminator architecture organizes state into distinct, modular C data structures defined in `include/`:

1. **`Shell` (`include/shell.h`)**:
   - Represents global application state.
   - Holds array of open tabs (`Tab *tabs[MAX_TABS]`), active tab index, running flag, and execution mode (`use_process_mode`).
   - Owns the pointer to `SharedHistory`.
2. **`Tab` (`include/terminal.h`)**:
   - Complete encapsulation of per-tab state.
   - Working directory (`cwd`), input buffer (`input`), cursor index (`cursor`), multiline accumulation buffer (`accumulated_cmd`).
   - Terminal scrollback ring buffer (`TerminalLine *lines`, `num_lines`, `line_capacity`, `scroll_offset`).
   - Job control table (`JobTable jobs`), foreground process group (`foreground_pgid`), pipeline PIDs (`pipeline_pids`).
   - IPC descriptors: `to_worker_pipe[2]` and `from_worker_pipe[2]`.
   - Concurrency handle: `pthread_t worker_thread` (Mode A) or `pid_t child_pid` (Mode B).
   - Autocomplete and selection state: `completion_active`, selection row/col anchors.
3. **`Command` & `Pipeline` (`include/parser.h`)**:
   - `Command`: Represents an individual stage in a pipeline. Stores argument array (`char **argv`, `argc`), input redirection file (`input_file`), output redirection file (`output_file`), and append flag (`append_output`).
   - `Pipeline`: Dynamic array of `Command` structures (`num_commands`, `capacity`) and background execution flag (`is_background`).
4. **`JobTable` & `Job` (`include/jobs.h`)**:
   - `Job`: Individual job entry containing job ID (`id`), process group ID (`pgid`), member PIDs (`pids[]`), command string (`cmd_line`), state (`Running`, `Stopped`, `Done`), foreground flag, and output pipe descriptor.
   - `JobTable`: Per-tab table tracking active jobs, next ID generator, current (`+`) and previous (`-`) job IDs.
5. **`TerminalLine` (`include/terminal.h`)**:
   - Dynamically allocated line string (`text`), per-character ANSI/theme color index array (`colors`), text length, and line type (prompt, output, error).
6. **`KeyBindingEntry` (`include/keybind.h`)**:
   - Configurable action mapping table storing action enum (`KeyAction`), action name, alias, X11 KeySym, modifier bitmask (`ControlMask`, `ShiftMask`, `Mod1Mask`), and custom override flag.
7. **`ScriptVarContext` (`include/script.h`)**:
   - Local variable and positional argument execution context for the script interpreter (`vars[][]`, `vals[][]`, `argv[]`, `argc`).
8. **`SharedHistory` (`include/history.h`)**:
   - Fixed-size circular command buffer (`entries[10000][1024]`, `count`).
   - Protected by `pthread_mutex_t mutex` in Mode A or `sem_t sem` in POSIX shared memory in Mode B.

---

## Section C: Concurrency, Threading, and Multiprocess Models

### Mode A: POSIX Thread Model (Default)
- The main thread creates the X11 GUI window, configures graphics contexts, and runs the central `poll()` event loop.
- When a tab is opened (`shell_open_tab`):
  1. Creates bidirectional anonymous pipe pairs (`to_worker_pipe`, `from_worker_pipe`).
  2. Spawns a dedicated POSIX thread via `pthread_create(&tab->worker_thread, NULL, tab_worker_thread_func, tab)`.
  3. The worker thread runs `run_worker_loop()` reading framed IPC commands and executing them via `executor_run_line_worker()`.
- **Thread CWD Isolation**: Because Linux `chdir()` modifies process-global directory state, worker threads never invoke `chdir()`. Instead, each tab stores its directory in `tab->cwd`. External command child processes call `chdir(tab->cwd)` immediately after `fork()`, guaranteeing complete working directory isolation across tabs.

### Mode B: Multiprocess Model (`useProcess=True`)
- When launched with `useProcess=True`, Terminator executes in multiprocess mode:
  1. Parent process owns the X11 Display connection and window.
  2. Opening a tab invokes `fork()`.
  3. Child process closes GUI ends of the pipes, sets up signal traps, and runs `run_worker_loop()`.
  4. Parent records `tab->child_pid = pid` and closes worker pipe ends.
  5. Child processes have completely independent address spaces, file descriptor tables, and OS-level current working directories (`chdir()`).
- **Crash Isolation**: If a tab child process crashes (e.g. from `SIGSEGV` or `SIGKILL`), the parent's `poll()` loop receives `POLLHUP` on the worker pipe, gracefully reaps the child via `waitpid(..., WNOHANG)`, displays a termination message, and allows all other tabs to continue running undisturbed.

---

## Section D: Inter-Process Communication and Synchronization

### 1. Framed IPC Protocol (Common to Modes A & B)
To guarantee that neither mode experiences partial reads, pipe truncation, or corrupted message frames, all communication between the GUI and tab workers utilizes a framed binary protocol defined in `include/ipc.h`:

```c
typedef enum {
    IPC_MSG_EXEC   = 1, /* GUI -> Worker: execute command string */
    IPC_MSG_INT    = 2, /* GUI -> Worker: SIGINT interrupt active foreground job */
    IPC_MSG_QUIT   = 3, /* GUI -> Worker: terminate worker thread/process */
    IPC_MSG_OUTPUT = 4, /* Worker -> GUI: output bytes from stdout/stderr */
    IPC_MSG_PROMPT = 5, /* Worker -> GUI: execution done, payload: int exit_status */
    IPC_MSG_CWD    = 6, /* Worker -> GUI: working directory changed, payload: path */
    IPC_MSG_CLEAR  = 7, /* Worker -> GUI: clear screen requested */
    IPC_MSG_EXIT   = 8, /* Worker -> GUI: exit command requested */
    IPC_MSG_TSTP   = 9, /* GUI -> Worker: SIGTSTP suspend foreground job */
    IPC_MSG_CONT   = 10 /* GUI -> Worker: SIGCONT resume job */
} IPCMsgType;

typedef struct {
    uint32_t type;
    uint32_t length;
} IPCHeader;
```

Each message consists of an 8-byte `IPCHeader` followed immediately by `length` bytes of payload. Robust I/O helpers `write_all()` and `read_all()` loop over `EINTR` and `EAGAIN` to guarantee atomic delivery.

### 2. Shared History Architecture
The course specification dictates that command history must be shared across all tabs:

- **Thread Mode (Mode A)**:
  - Stored on the heap as `SharedHistory`.
  - Synchronized using a standard POSIX `pthread_mutex_t`.
  - When any tab executes a command, `history_add()` acquires the mutex, appends the line into circular history entries, and releases the mutex.

- **Process Mode (Mode B)**:
  - Allocated in POSIX shared memory using:
    ```c
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    ftruncate(shm_fd, sizeof(SharedHistory));
    SharedHistory *hist = mmap(NULL, sizeof(SharedHistory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    ```
  - Protected by a **process-shared POSIX semaphore**:
    ```c
    sem_init(&hist->sem, 1, 1); /* pshared = 1: shared between processes */
    ```
  - Because child processes are created via `fork()`, they inherit the `MAP_SHARED` mapping and the semaphore.
  - Operations on history invoke `sem_wait(&hist->sem)` before accessing entries and `sem_post(&hist->sem)` upon completion, guaranteeing mutual exclusion across distinct process address spaces without race conditions or memory corruption.

---

## Section E: Command Execution, Pipelines, and Redirection

### 1. Pipeline Execution Workflow (`cmd1 | cmd2 | ... | cmdN`)
1. Create $N-1$ pipes via `pipe()`.
2. For each command $0 \le i < N$:
   - `fork()` child process.
   - Child connects stdin from pipe $i-1$ (or `< file` if $i=0$).
   - Child connects stdout to pipe $i$ (or `> file` or `capture_pipe` if $i=N-1$).
   - **Crucial step**: Child closes all unused pipe ends (prevents hang waiting for EOF).
   - Child executes `chdir(tab->cwd)`.
   - Child invokes `execvp()`.
3. **Crucial step**: Parent closes all $2 \times (N-1)$ pipe descriptors immediately so EOF propagates properly when writers finish.
4. Output is streamed incrementally through `capture_pipe` to prevent kernel pipe buffer deadlock on large outputs (verified with 1MB+ streams).
5. `waitpid()` reaps all child processes and records the exit status of command $N-1$ into `tab->last_exit_status`.

### 2. Redirection Management (`redirection.c`)
- **Child Redirection**: Uses `dup2()` to map open file descriptors directly to `STDIN_FILENO` (`<`) or `STDOUT_FILENO` (`>`, `>>`) before `execvp()`.
- **Parent Redirection (Built-ins)**: When built-ins (like `pwd > out.txt`) redirect, `redirection_apply_in_parent()` duplicates original stdin/stdout via `dup()`, applies redirection via `dup2()`, executes the built-in, and restores original streams with `redirection_restore_in_parent()`.

### 3. Command Substitution & Variable Expansion (`expansion.c`)
- Command substitutions `$(cmd)` and `` `cmd` `` are evaluated iteratively from innermost to outermost using `expansion_process_command_substitutions()`. Inner commands run synchronously via `executor_execute_substitution_sync()`, capturing output into dynamically resized buffers.
- Variable expansion handles `$VAR`, `${VAR}`, `$?` (exit status of preceding command), and `$$` (PID). Single-quoted text `'...'` is strictly preserved literally without expansion.

### 4. Forbidden Command Policy (`nano`)
Full-screen curses/terminal-raw editor commands (`nano`, `/bin/nano`, etc.) are explicitly forbidden for Phase 1. Attempting to execute `nano` outputs `terminator: nano: command forbidden`, sets exit status `1`, and safely returns to the prompt without blanking, freezing, or corrupting terminal state.

### 5. multiWatch Concurrency & Periodic Execution Architecture (Task 6)
- **Syntax**: `multiWatch ["cmd1", "cmd2", ...]`
- Parses bracketed JSON-style lists with `multiwatch_parse_args()`, allowing internal commas, pipes, and flags.
- Every monitored command runs in its own child process group (`setpgid(0, 0)`), multiplexing output streams using `poll()`.
- Commands run independently on a 2-second monotonic cycle (`CLOCK_MONOTONIC`) starting after individual execution completes. Fast commands do not starve slower commands.
- Atomic blocks matching course specification with 52 dashes:
  ```text
  "<cmd>", <unix_timestamp>:
  ----------------------------------------------------
  <output>
  ----------------------------------------------------
  ```
- Pressing `Ctrl+C` immediately broadcasts `SIGINT` to all monitored process groups, reaps children, and restores prompt.

### 6. Internal Bash Script Interpreter (Task 11)
- Implemented natively in C without delegating to `/bin/bash` or `popen()`.
- **Phase 1 (Static Pre-Execution Validation)**: Pre-parses script syntax before running any command. Any syntax error (unclosed quotes, mismatched `if`/`fi`, malformed test bracket) halts execution immediately with exit code 2. **Zero commands preceding the error are executed.**
- **Phase 2 (AST Execution)**: Traverses Abstract Syntax Tree nodes (`STMT_CMD`, `STMT_ASSIGN`, `STMT_IF`, `STMT_EXIT`).
- **Scoping**: Sourcing (`source script.sh`) executes in calling tab's context and modifies `tab->vars` and working directory; direct execution (`./script.sh`, `bash script.sh`) runs in an isolated subshell context.

---

## Section F: Signal Handling, Job Control, and Process Groups

### 1. Process Group Management (`setpgid`)
Every launched command or pipeline stage is assigned its own process group:
```c
if (i == 0) {
    first_pgid = pid;
    setpgid(pid, first_pgid);
} else {
    setpgid(pid, first_pgid);
}
```
This isolates the process group completely from Terminator's main GUI and worker processes, ensuring signals targeted at child processes do not inadvertently terminate the terminal emulator.

### 2. Job Table State Machine (`include/jobs.h`, `src/jobs.c`)
Each tab owns an independent `JobTable` tracking up to `MAX_JOBS` (32) concurrent background and suspended jobs:
- `JobState`: `JOB_STATE_RUNNING`, `JOB_STATE_STOPPED`, `JOB_STATE_DONE`.
- Job Indicators:
  - `+` marks `table->current_id` (most recently suspended or placed in background).
  - `-` marks `table->previous_id`.
- Non-Blocking State Updates:
  - `jobs_update_status()` queries all tracked processes with `waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED)`.
  - Non-blocking output draining: background jobs with open pipes are read with `O_NONBLOCK` into the tab's scrollback without corrupting the active input prompt.

### 3. Signal Handling & Routing
- **`Ctrl+C` (SIGINT)**:
  - If a foreground command is active: delivered directly to `tab->foreground_pgid` via `kill(-pgid, SIGINT)`.
  - In Thread Mode and Process Mode: GUI sends `IPC_MSG_INT` to worker pipe.
  - Monitored process groups terminate cleanly, `^C\n` is printed, and the prompt is restored immediately.
- **`Ctrl+Z` (SIGTSTP)**:
  - Intercepted by GUI key handler or headless signal trap.
  - GUI delivers `SIGTSTP` to `tab->foreground_pgid` and sends `IPC_MSG_TSTP` to worker pipe.
  - The worker loop detects `WIFSTOPPED(status)` via `waitpid(..., WUNTRACED)`.
  - The job is recorded as `JOB_STATE_STOPPED` in `tab->jobs`.
  - Prints `\n[<id>]+ Stopped <cmd>\n`, clears `foreground_pgid`, and returns prompt immediately.
- **Background Execution (`command &`)**:
  - Trailing unquoted `&` operator is parsed into `pipeline->is_background = true`.
  - Command launches in its own process group, is registered in `JobTable` as `JOB_STATE_RUNNING`, prints `[<id>] <pgid>`, and immediately returns prompt.
- **Built-in `jobs`**:
  - Calls `jobs_list()` to reap terminated jobs, updates `+`/`-` markers, and formats output.
- **Built-in `fg [%id]`**:
  - Brings job to foreground. If stopped, resumes via `SIGCONT`. Enters foreground monitoring loop until exit or subsequent stop.
- **Built-in `bg [%id]`**:
  - Resumes stopped job via `kill(-pgid, SIGCONT)`, updates state to `JOB_STATE_RUNNING`, prints `[<id>]+ <cmd> &`, and returns prompt immediately.

### 4. Hang-Free Application Shutdown
When closing a tab (`shell_close_tab`) or destroying the window (`shell_destroy`):
1. Delivers `SIGTERM` and `SIGKILL` to `tab->foreground_pgid` and all pipeline PIDs **before** joining worker threads.
2. Sends `IPC_MSG_QUIT` and closes worker pipes.
3. Joins worker threads or reaps worker child processes (`pthread_join` / `waitpid`).
Because running child processes are terminated first, worker threads unblock immediately, preventing deadlocks or application freezes during exit.

---

## Section G: Terminal Emulation, Rendering, and Scrollbar Mechanics

### 1. Functional Vertical Scrollbar Design & Mathematics
Terminator features a fully functional vertical scrollbar built directly in Xlib:

#### Geometry & Proportional Sizing
Given:
- `term_top`: Top margin below the tab bar (`bar_h + padding_y`)
- `term_bottom`: Bottom margin above status bar (`h - stat_h - padding_y`)
- `term_height`: Usable height `term_bottom - term_top`
- `line_h`: Line spacing (`font_height + 6`)
- `visible_rows`: `term_height / line_h`
- `total_rows`: `tab->num_lines + (show_input_row ? 1 : 0)`
- `max_scroll`: `total_rows > visible_rows ? total_rows - visible_rows : 0`

Scrollbar dimensions:
- Track: `sb_x = w - sb_w - 4`, `sb_y = term_top`, `sb_w = 8`, `sb_h = term_height`
- Thumb Height:
  $$\text{thumb\_h} = \max\left(24, \frac{\text{sb\_h} \times \text{visible\_rows}}{\text{total\_rows}}\right)$$
  When content fits entirely (`max_scroll == 0`), $\text{thumb\_h} = \text{sb\_h}$.
- Travel distance: $\text{travel} = \text{sb\_h} - \text{thumb\_h}$
- Thumb Y Coordinate:
  $$\text{thumb\_y} = \text{sb\_y} + \text{round}\left(\frac{\text{max\_scroll} - \text{scroll\_offset}}{\text{max\_scroll}} \times \text{travel}\right)$$

#### Interaction Handlers
- **Track Clicking (Page Navigation)**:
  - Clicking on the track *above* the thumb calls `terminal_scroll_up(tab, visible_rows - 1)` (Page Up).
  - Clicking on the track *below* the thumb calls `terminal_scroll_down(tab, visible_rows - 1)` (Page Down).
- **Smooth Thumb Dragging**:
  - Clicking on the thumb sets `gui->sb_dragging = true` and records `sb_thumb_grab_y = event->y - thumb_y`.
  - Mouse motion with Button 1 held (`Button1MotionMask`) computes:
    $$\text{fraction} = \frac{\text{clamp}(\text{event->y} - \text{sb\_thumb\_grab\_y}, \text{sb\_y}, \text{sb\_y} + \text{travel}) - \text{sb\_y}}{\text{travel}}$$
    $$\text{new\_offset} = \text{round}((1 - \text{fraction}) \times \text{max\_scroll})$$
- **Bottom-Following Behavior**:
  - When the viewport is at the bottom (`scroll_offset == 0`), incoming output automatically keeps the viewport pinned to the bottom.
  - When the user has scrolled up (`scroll_offset > 0`), incoming output increments `scroll_offset++` so the user's visible text position remains undisturbed without jumping to the bottom.

### 2. X11 Double-Buffered Event Loop
The event loop in `gui_run_event_loop()` uses POSIX `poll()` without busy-waiting:
1. `fds[0]` watches `ConnectionNumber(gui->dpy)` (`POLLIN`).
2. `fds[1..N]` watch `tab->from_worker_pipe[0]` across all open tabs (`POLLIN | POLLHUP | POLLERR`).
3. Render calls write to an off-screen `Pixmap` back-buffer and blit via `XCopyArea()` to eliminate screen flicker.

### 3. Key Binding Architecture & `.terminatorrc` Configuration
- Action mapping table (`KeyBindingEntry`) associates actions with KeySyms and modifier masks (`ControlMask`, `ShiftMask`, `Mod1Mask`).
- Persistent storage in `~/.terminatorrc`.
- Built-in `bind` command: `bind` (lists active bindings), `bind <action> <combo>` (sets custom binding), and `bind --reset` (restores defaults).

### 4. Premium Gradient Text Selection Engine
- Mouse drag selection highlights character spans.
- Renders with a 64-step precomputed horizontal gradient interpolating smoothly from Cyan (`#00E5FF`) to Electric Blue (`#4776FF`) to Vivid Violet (`#A855F7`).
- Selected characters render in high-contrast deep navy (`#08071A`), enclosed within a glowing `#00E5FF` border.
- Integrated with X11 `CLIPBOARD` and `PRIMARY` selections via `Ctrl+Shift+C` and `Ctrl+Shift+V`.

### 5. Searchable Persistent History & LCS Engine (Task 9)
- Stores up to 10,000 commands in `~/.terminator_history` with advisory locking (`fcntl(F_SETLK)`).
- 2-row dynamic programming Longest Contiguous Common Substring (LCS) algorithm with $O(N)$ space.
- 3-tier match priority:
  1. Priority 1 (Exact Match): Most recent exact match only.
  2. Priority 2 (LCS $L_{\max} > 2$): All tied matches achieving maximum substring length in newest-first order.
  3. Priority 3 ($L_{\max} \le 2$): Displays `No match for search term in history`.

### 6. Native Blinking Cursor & Focus Architecture
- 1–2px slim vertical insertion caret drawn in `#22D3EE` Electric Cyan.
- Pure presentation layer artifact; never inserted into the input string buffer.
- 500ms blink timer reset instantly on any user interaction (`gui_cursor_reset_blink`).
- Automatically hidden on focus loss, active command execution, and text selection.

### 7. File-name Auto-completion Engine (Task 10)
- Isolates pre-cursor word token, supporting relative, absolute, and tilde (`~`) paths.
- Uses `opendir()` / `readdir()` to scan filesystem entries.
- Single match: completes directly (appends `/` if directory via `S_ISDIR`).
- Multiple matches sharing common prefix: extends word to Longest Common Prefix (LCP).
- Ambiguous matches without common prefix: displays numbered list (1..N) and activates choice mode; pressing a digit (1–9) selects the corresponding item.

---

## Section H: Known Limitations, Assumptions, and Design Tradeoffs

### 1. Known Architectural Limitations
- **Curses & Raw-Terminal Interactive Applications**:
  - Phase 1 of the Terminator project implements a custom Xlib terminal canvas and shell execution engine without embedding pseudo-terminal (`PTY`) kernel devices.
  - Interactive full-screen curses applications requiring terminal-raw mode (`vi`, `less`, `htop`, interactive `top`) are handled either by spawning a dedicated external terminal window (when X11 desktop is available) or providing clean batch/help guidance (in headless environments).
  - `nano` is explicitly guarded and forbidden with exit code 1 per assignment guidelines.
- **VT100 Terminal Emulation Boundary**:
  - Implements complete ANSI SGR color sequences (`\033[...m`), erase display (`\033[2J`, `\033[3J`), erase line (`\033[K`), and cursor home (`\033[H`). Arbitrary alternate screen buffers (`\033[?1049h`) and complex DEC private modes are omitted to maintain a clean, compact presentation engine.
- **Script Interpreter Grammar**:
  - The Task 11 script engine natively supports assignments (`VAR=val`), expansions (`$VAR`, `${VAR}`, `$?`, `$$`), positional parameters (`$0`..`$9`, `$#`), nested conditionals (`if`, `elif`, `else`, `fi`), test brackets (`[ ... ]`), pipelines, redirections, and comments (`#`). Loop constructs (`for`, `while`) and shell functions are deferred beyond the project scope.

### 2. Systems Assumptions
- **POSIX-Compliant Linux Environment**: Assumes Linux kernel 4.x or later with POSIX 2008 interfaces (`pipe`, `dup2`, `fork`, `poll`, `waitpid`, `setpgid`, `kill`, `shm_open`, `mmap`, `sem_t`).
- **Monospace Font Availability**: Assumes at least one standard X11 monospace font family is installed (`DejaVu Sans Mono`, `Liberation Mono`, `Ubuntu Mono`, or standard `fixed`).
- **Filesystem Permissions**: Assumes read/write access to the user's home directory for `~/.terminator_history` and `~/.terminatorrc`, with fallback to `/tmp`.

### 3. Engineering Design Tradeoffs
| Design Decision | Chosen Approach | Alternative Considered | Rationale / Viva Defense |
| :--- | :--- | :--- | :--- |
| **Worker Concurrency** | Dual Modes: Mode A (POSIX Threads) & Mode B (Forked Child Processes) | Single monolithic GUI loop or Threads only | Fulfills assignment requirement for architectural flexibility; provides memory-efficient thread sharing by default and hard crash isolation in process mode. |
| **IPC Protocol Framing** | 8-byte Length-Prefixed Header (`uint32_t type`, `uint32_t len`) over anonymous pipes | UNIX domain datagram sockets or raw text streams | Anonymous pipes avoid lingering filesystem socket files; fixed 8-byte header eliminates stream message boundary ambiguity and partial read corruption. |
| **Shared History Across Tabs** | Mode A: Heap + `pthread_mutex_t`<br>Mode B: POSIX SHM (`mmap`) + `sem_t(pshared=1)` | File polling or per-tab isolated history | Immediate synchronous history availability across tabs without disk I/O latency; protected by OS-level process-shared semaphore in multi-process mode. |
| **History Search Algorithm** | 2-Row Dynamic Programming Longest Contiguous Common Substring (LCS) | PCRE / POSIX Regex or full 2D matrix | Dynamic programming guarantees deterministic $O(M \times N)$ time with $O(N)$ space; avoids third-party regex library dependencies and prevents memory explosion. |
| **Script Validation** | Two-Phase Interpreter: Static AST Pre-validation before execution | Single-pass line-by-line interpretation | Guarantees zero side-effects if syntax errors exist anywhere in the script file; strictly complies with assignment test expectations. |
| **Double-Buffered Rendering** | Off-screen X11 `Pixmap` back-buffer blitted via `XCopyArea()` | Direct drawing to X11 `Window` | Completely eliminates screen flickering during typing and high-frequency output streaming. |
