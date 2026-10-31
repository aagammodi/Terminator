# Terminator Architecture & Systems Design

This document details the engineering architecture, data structures, POSIX system-call workflows, and execution models of **Terminator**.

---

## 1. Overall System Architecture

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

## 2. Framed IPC Protocol (Common to Modes A & B)

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
    IPC_MSG_EXIT   = 8  /* Worker -> GUI: exit command requested */
} IPCMsgType;

typedef struct {
    uint32_t type;
    uint32_t length;
} IPCHeader;
```

Each message consists of an 8-byte `IPCHeader` followed immediately by `length` bytes of payload. Robust I/O helpers `write_all()` and `read_all()` loop over `EINTR` and `EAGAIN` to guarantee atomic delivery.

---

## 3. Shared History Architecture

The course specification dictates that command history must be shared across all tabs:

### Thread Mode (Mode A)
- Stored on the heap as `SharedHistory`.
- Synchronized using a standard POSIX `pthread_mutex_t`.
- When any tab executes a command, `history_add()` acquires the mutex, appends the line into circular history entries, and releases the mutex.

### Process Mode (Mode B)
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

## 4. Working Directory & Job Isolation

In a multi-tab terminal, each tab must maintain an independent working directory:
- **Thread Mode Isolation**: In Linux, the `chdir()` system call affects the entire process (all threads). To achieve true directory isolation in Thread Mode without kernel modifications:
  1. Each tab records its own path in `tab->cwd`.
  2. The built-in `cd` updates `tab->cwd` (after validating with `stat()`) without altering the global process directory.
  3. When an external command is launched, the forked child process executes `chdir(tab->cwd)` immediately after `fork()` before redirection or `execvp()`.
  4. Built-in `pwd` prints `tab->cwd`.
- **Process Mode Isolation**: Because each tab has an independent process address space, `builtin_cd` calls `chdir(tab->cwd)` directly inside the tab child process.

---

## 5. Functional Vertical Scrollbar Design & Mathematics

Terminator features a fully functional vertical scrollbar built directly in Xlib:

### Geometry & Proportional Sizing
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

### Interaction Handlers
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
  - When the user has scrolled up to inspect previous output (`scroll_offset > 0`), incoming output increments `scroll_offset++` so the user's visible text position remains undisturbed without jumping to the bottom.

---

## 6. X11 Double-Buffered Event Loop & Child Reaping

The event loop in `gui_run_event_loop()` uses POSIX `poll()` without busy-waiting:
1. `fds[0]` watches `ConnectionNumber(gui->dpy)` (`POLLIN`).
2. `fds[1..N]` watch `tab->from_worker_pipe[0]` across all open tabs (`POLLIN | POLLHUP | POLLERR`).
3. `poll()` sleeps in the kernel with a 20ms timeout, ensuring instantaneous response to keyboard, mouse, and resize events at 50fps while consuming near 0% CPU at idle.
4. If a tab worker process is killed (`POLLHUP`), the GUI reaps the child via `waitpid(..., WNOHANG)` and displays a clean error banner without crashing unrelated tabs or the X11 GUI.

---

## 7. Pipeline & Redirection Execution Details

### Pipeline Execution Workflow (`cmd1 | cmd2 | ... | cmdN`)
1. Create $N-1$ pipes via `pipe()`.
2. For each command $0 \le i < N$:
   - `fork()` child process.
   - Child connects stdin from pipe $i-1$ (or `< file` if $i=0$).
   - Child connects stdout to pipe $i$ (or `> file` or `capture_pipe` if $i=N-1$).
   - Child closes all unused pipe ends.
   - Child executes `chdir(tab->cwd)`.
   - Child invokes `execvp()`.
3. Parent closes all $2 \times (N-1)$ pipe descriptors immediately to prevent EOF deadlocks.
4. Output is streamed incrementally through `capture_pipe` to prevent kernel pipe buffer deadlock on large outputs (verified with 1MB+ streams).
5. `waitpid()` reaps all child processes and records the exit status of command $N-1$ into `tab->last_exit_status`.

---

## 8. Process Group Management, Signal Routing & Hang-Free Shutdown (e.g. `geany`, long-running tasks)

### Process Group Separation (`setpgid`)
Every launched command or pipeline is placed in its own process group (`first_pgid = pid` of first stage; all stages join `first_pgid`). This isolates child processes from Terminator's main GUI process and tab worker threads.

### Non-Blocking Worker Multiplexing (`poll`)
Previously, worker loops blocked on `read(out_pipe_fd)`. When running graphical applications like `geany` or commands without immediate output, the worker was unable to process incoming IPC messages, ignoring `Ctrl+C` and causing GUI hangs on exit.

The revised architecture uses `poll()` in `executor_run_line_worker()` watching:
- `read_ipc_fd` (`to_worker_pipe[0]`): For `IPC_MSG_INT` (Ctrl+C) and `IPC_MSG_QUIT` (shutdown).
- `tab->out_pipe_fd`: For output streaming from child processes.
- Non-blocking checks via `waitpid(..., WNOHANG)` every 20ms.

### Clean Ctrl+C Handling
When the user presses `Ctrl+C`:
1. If a foreground command is running, the GUI sends `SIGINT` directly to `tab->foreground_pgid` and sends `IPC_MSG_INT` through the worker pipe.
2. The child process group receives `SIGINT` and terminates cleanly.
3. The worker reaps the child, records its exit status, and sends `IPC_MSG_PROMPT` back to the GUI.
4. The terminal restores the standard prompt. Unrelated tabs and Terminator itself remain completely unaffected.

### Responsive Window Close & Tab Destruction
When closing a tab (`shell_close_tab`) or closing the window (`shell_destroy`):
1. Terminator delivers `SIGTERM` and `SIGKILL` to `tab->foreground_pgid` and all pipeline PIDs **before** joining worker threads.
2. It sends `IPC_MSG_QUIT` and closes the worker pipes.
3. It joins worker threads or reaps worker child processes (`pthread_join` / `waitpid`).
Because running child processes are terminated first, worker threads unblock immediately, preventing deadlocks or application freezes during exit.

---

## 9. Forbidden Command Policy (`nano`)

Terminator's primary objective in Tasks 1–5 is solid shell and terminal emulation (built-ins, pipelines, redirection, multiline input, multi-tab thread/process architectures). Full-screen raw-terminal curses programs (such as `nano`) require pseudo-terminal raw modes and full cursor-addressable terminal matrices.

To prevent terminal corruption, blank screens, or hangs, the `nano` command is explicitly intercepted and forbidden before execution:
1. `is_forbidden_command(cmd)` inspects command base names.
2. If `nano` is invoked (e.g. `nano`, `/bin/nano`, `nano file.txt`):
   - In synchronous execution (`-c` flag), prints `terminator: nano: command forbidden\n` and sets exit code `1`.
   - In interactive worker execution, sends `IPC_MSG_OUTPUT` with `terminator: nano: command forbidden\n`, records exit status `1`, and emits `IPC_MSG_PROMPT` immediately.
3. The prompt is restored without blanking or freezing the interface.

---

## 10. multiWatch Concurrency & Periodic Execution Architecture (Task 6)

### Dedicated Lexer for Bracketed Commands
The `multiWatch` built-in accepts a bracketed list of commands:
```bash
multiWatch ["cmd1", "cmd2", ...]
```
Because command strings inside brackets can contain internal commas (e.g. `awk -F,`), semicolons, and pipes, the raw command line is passed directly to `multiwatch_parse_args()` in `src/multiwatch.c`. The parser enforces:
- Opening `[` and closing `]` brackets.
- Non-empty command lists (rejection of `[]`).
- Quoted command tokens (supporting both single and double quotes) with exact whitespace and internal character preservation.
- Strict rejection of unquoted trailing arguments or malformed syntax with clear diagnostics.

### Independent Concurrency & Process Isolation
For each monitored command:
1. An output pipe is created via `pipe()`.
2. A child process is forked and places itself into its own process group via `setpgid(0, 0)`.
3. The child connects `stdout` and `stderr` to the pipe write end and executes the command line using Terminator's shell parser and pipeline executor (`executor_run_line()`).

### Non-Blocking Multiplexing with `poll()`
Rather than executing commands sequentially or blocking on individual pipes, the main `multiwatch_run()` loop multiplexes:
1. The GUI worker IPC channel (`read_ipc_fd`): Watches for `IPC_MSG_INT` or `IPC_MSG_QUIT`.
2. All active child command output pipes.

Output is read in non-blocking chunks into dynamically growing buffers (`output_buf`).

### 2-Second Independent Periodic Scheduling
When a child process terminates:
1. `waitpid()` reaps the child process and EOF is detected on its pipe.
2. The collected output is formatted into an atomic output block and emitted:
   ```text
   "<cmd>", <unix_timestamp>:
   ----------------------------------------------------
   <output>
   ----------------------------------------------------
   ```
3. Monotonic time is sampled (`clock_gettime(CLOCK_MONOTONIC)`), and `next_run_time` is set to `finish_time + 2.0s`.
4. The command does not block any other monitored command. Fast commands rerun independently every 2s after completion, while slow commands take as long as necessary without starvation.

### Signal Handling and Child Process Group Teardown
When `Ctrl+C` is triggered (via `SIGINT` or `IPC_MSG_INT`):
1. `multiwatch_run` immediately breaks from the event loop.
2. It sends `SIGINT` to all active child process groups (`kill(-cmd->pid, SIGINT)`).
3. After a brief grace period (20ms), it delivers `SIGKILL` and reaps any remaining children with `waitpid()`.
4. It outputs `^C\n`, sends `IPC_MSG_PROMPT` with status 130, and restores the interactive prompt cleanly.

---

## 11. Key Binding Architecture, Selection & Configuration Engine (Task 7)

### Unified Key Action Mapping
Key binding management is decoupled into `include/keybind.h` and `src/keybind.c`. Actions are represented by the `KeyAction` enum:
- Navigation: `ACTION_MOVE_LEFT`, `ACTION_MOVE_RIGHT`, `ACTION_BEGINNING_OF_LINE`, `ACTION_END_OF_LINE`.
- History: `ACTION_HISTORY_PREV`, `ACTION_HISTORY_NEXT` (with draft preservation).
- Editing: `ACTION_DELETE_TO_START` (Ctrl+U), `ACTION_DELETE_TO_END` (Ctrl+K), `ACTION_DELETE_WORD` (Ctrl+W).
- Terminal Control: `ACTION_CLEAR_SCREEN` (Ctrl+L), `ACTION_CLOSE_TAB` (Ctrl+D context-sensitive).
- Clipboard: `ACTION_COPY` (Ctrl+Shift+C), `ACTION_PASTE` (Ctrl+Shift+V).
- Tab Management: `ACTION_NEW_TAB`, `ACTION_CLOSE_TAB_SHORTCUT`, `ACTION_PREV_TAB`, `ACTION_NEXT_TAB`.

### Configuration Persistence (`~/.terminatorrc`)
- Loaded on startup via `keybind_init()` -> `keybind_load_config()`.
- Syntax: `bind <action> <key_combo>` (e.g. `bind clear-screen Ctrl+Alt+L`).
- Comments starting with `#` and blank lines are ignored.
- Unknown actions, invalid modifiers, or conflicting combinations generate clear warnings without failing startup.
- Custom bindings are modified dynamically at runtime using the `bind` built-in and automatically saved to disk.

### Text Selection & X11 Clipboard Integration
1. **Mouse Drag Selection**:
   - `ButtonPress` on terminal body initializes anchor `(sel_start_col, sel_start_row)`.
   - `MotionNotify` updates `(sel_end_col, sel_end_row)` and requests window redrawing.
   - Per-tab isolation: Selection coordinates are tracked on `Tab`, preventing selection bleed between tabs.
2. **X11 Clipboard & Primary Selection**:
   - On `Ctrl+Shift+C`, `XSetSelectionOwner()` acquires ownership of both `CLIPBOARD` and `XA_PRIMARY` atoms.
   - Responds to `SelectionRequest` events by serving UTF-8 string chunks to requesting applications.
   - On `Ctrl+Shift+V`, requests selection data asynchronously via `XConvertSelection` with atom `UTF8_STRING` and delivers contents directly into the active tab's input buffer at cursor position.

---

## 12. Premium Gradient Text Selection Engine (Part A)

### Horizontal Aurora Gradient Architecture
To deliver a modern visual experience replacing harsh flat inversion, Terminator implements a hardware-efficient, precomputed horizontal gradient renderer:
- **Gradient Stops**:
  1. Cyan: `#00E5FF` (`RGB(0, 229, 255)`)
  2. Electric Blue: `#4776FF` (`RGB(71, 118, 255)`)
  3. Vivid Violet: `#A855F7` (`RGB(168, 85, 247)`)
- **Precomputed Color Table**:
  - `theme_init_selection_gradient()` precalculates 64 smooth gradient steps across the two interpolation segments ($0.0 \to 0.5$ and $0.5 \to 1.0$) in `src/theme.c`.
  - Allocated on the default X11 colormap (`XAllocColor`) during initialization, avoiding per-frame allocation overhead.
- **Visual Contrast & Hierarchy**:
  - **High-Contrast Navy Text**: Text within selected cells renders in deep `#08071A` (`RGB(8, 7, 26)`), ensuring sharp legibility over all gradient regions.
  - **Glowing Border**: An outline drawn with `COLOR_SELECTION_BORDER` (`#00E5FF`) encloses the selection block.
- **Rendering Pipeline in `gui_redraw()`**:
  1. Pass 1: Render terminal surface background (`#19172B`).
  2. Pass 2: Calculate selection span `(start_col, start_row)` to `(end_col, end_row)`.
  3. Pass 3: Draw gradient fill rectangles underneath selected character cells column-by-column across 64 discrete horizontal color steps.
  4. Pass 4: Render text — unselected characters use their standard syntax colors; selected characters use high-contrast dark navy `#08071A`.
  5. Pass 5: Render glowing border around the selection boundary.
  6. Pass 6: Draw cursor and status overlay.

---

## 13. Signals & Job Control Architecture (Task 8)

### Process Group Management (`setpgid`)
Every launched command or pipeline stage is assigned its own process group:
```c
if (i == 0) {
    first_pgid = pid;
    setpgid(pid, first_pgid);
} else {
    setpgid(pid, first_pgid);
}
```
This isolates the process group completely from Terminator's main GUI and worker processes.

### Job Table State Machine (`include/jobs.h`, `src/jobs.c`)
Each tab owns an independent `JobTable` tracking up to `MAX_JOBS` (32) concurrent background and suspended jobs:
- `JobState`: `JOB_STATE_RUNNING`, `JOB_STATE_STOPPED`, `JOB_STATE_DONE`.
- Job Markers:
  - `+` marks `table->current_id` (most recently suspended or placed in background).
  - `-` marks `table->previous_id`.
- Non-Blocking State Updates:
  - `jobs_update_status()` queries all tracked processes with `waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED)`.
  - Non-blocking output draining: background jobs with open pipes are read with `O_NONBLOCK` into the tab's scrollback without corrupting the active input prompt.

### Signal Handling & Routing
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

---

## 14. Searchable Persistent History & Longest Common Substring Engine (Task 9)

### Persistent File Storage (`~/.terminator_history`)
- Path Resolution: Checks `getenv("TERMINATOR_HISTORY")`, defaulting to `$HOME/.terminator_history`.
- Multiline Serialization: Newlines within multiline commands are escaped to `\n`; literal backslashes are escaped to `\\`.
- Advisory File Locking: Reads and writes are protected with POSIX `fcntl()` advisory write locks (`F_SETLK`), preventing corruptions from concurrent Terminator instances.
- Capacity Clamping: File loading and saving strictly enforces the 10,000 most recent command limit.

### Shared Synchronization Across Modes
- **Mode A (Thread Mode)**: In-memory `SharedHistory` protected by `pthread_mutex_t`.
- **Mode B (Process Mode)**: `SharedHistory` placed in POSIX shared memory (`shm_open`, `mmap`) protected by a process-shared POSIX semaphore (`sem_init(..., 1, 1)`).

### 2-Row Dynamic Programming LCS Algorithm
To match history entries without regex engines or third-party dependencies, `history.c` implements the Longest Contiguous Common Substring algorithm:
- Given search term $S$ of length $M$ and candidate history command $T$ of length $N$:
- Transition recurrence:
  $$\text{LCS}[i][j] = \begin{cases} \text{LCS}[i-1][j-1] + 1 & \text{if } S[i-1] == T[j-1] \\ 0 & \text{otherwise} \end{cases}$$
- Memory optimization: Computed using two rotating rows of size $N+1$ (`prev` and `curr`), reducing space complexity from $O(M \times N)$ to $O(N)$ with optimal cache locality.

### 3-Tier Match Priority Engine
When the user executes history search (`history_search()`):
1. **Priority 1 (Case A — Exact Match)**:
   - Scans history newest-to-oldest for $T == S$.
   - If found, returns **only the most recent exact match**.
2. **Priority 2 (Case B — Longest Contiguous Common Substring, $L_{\max} > 2$)**:
   - If no exact match exists, computes $L(T, S)$ across all history entries.
   - Identifies global maximum substring length $L_{\max}$.
   - If $L_{\max} > 2$, collects **all history commands achieving $L_{\max}$** and displays them in newest-first order.
3. **Priority 3 (Case C — No Match, $L_{\max} \le 2$)**:
   - If $L_{\max} \le 2$, prints `No match for search term in history`.

### Interactive Search Mode (`Ctrl+R`)
- Triggers search prompt `Enter search term: ` directly on the terminal input line.
- Supports live typing, Backspace, and cancellation via `Escape` or `Ctrl+C`.
- Pressing `Enter` executes the 3-tier search engine, prints the matching results, and cleanly restores the prompt without auto-executing or logging the search query into history.

---

## 15. Native Terminal Blinking Cursor & Focus / Selection Architecture

### Visual Geometry & Rendering
- **Geometry**: The input caret is drawn as a slim 1–2px vertical rectangle (`caret_w = (font_width >= 9) ? 2 : 1`) matching the full font ascent + descent height.
- **Theme Color**: Rendered using `#22D3EE` Electric Cyan accent color (`COLOR_CURSOR`).
- **Memory Separation**: The caret is rendered strictly in the presentation layer (`gui_redraw()`). It is never inserted into `tab->input` or string storage, ensuring input strings remain standard C strings without control artifacts.

### Blink Timing & Event-Driven Redraws
- **Timer Interval**: 500ms blink cycle managed in `gui_redraw()` using monotonic millisecond timestamps (`gui_get_time_ms()`).
- **Immediate Wakeup**: Any user action (`input_handle_key_press()`, mouse clicks, tab switching, history browsing) immediately invokes `gui_cursor_reset_blink(gui)`, setting `cursor_visible = true` and resetting the blink timer.
- **Non-Busy Polling**: In `gui_run_event_loop()`, when the cursor is eligible to blink, the `poll()` timeout is dynamically clamped to the remaining time until the next 500ms transition (`500 - (now - last_blink)`), eliminating busy-waiting.

### Multi-State Visibility Suppression
Drawing of the caret is automatically suppressed under any of the following conditions:
1. `!gui->has_focus`: Window lost X11 focus (`FocusOut`).
2. `tab->state == SHELL_STATE_RUNNING`: Active foreground command or pipeline executing.
3. `gui->sel_active || tab->sel_active`: Mouse selection highlight active.
4. `gui->sel_dragging || tab->sel_dragging`: Active pointer dragging.
5. `!gui->cursor_visible`: Cursor off-phase in blink cycle.

### Mouse Click Insertion Point Calculation
When clicking inside the visible input row, `input_handle_button_release()` maps mouse $X$ coordinate to character index:
- Computes `prompt_len` from prompt width.
- Clamps click to character boundaries: clicks before the input text place the cursor at index 0; clicks beyond text place it at `strlen(tab->input)`.
- Clicks outside the input row or in scrollback history leave the active cursor undisturbed.

---

## 16. File-name Auto-completion Engine (Task 10)

### Token Boundary Detection
- In `autocomplete_handle_tab()`, the cursor position in `tab->input` is analyzed backwards.
- Tracks quoting state (single and double quotes) and shell delimiters (` `, `\t`, `|`, `>`, `<`, `;`, `&`).
- Splits the token into directory path prefix (`dir_part`) and match prefix (`file_part`).
- Supports tilde expansion (`~`), relative paths (`src/`), absolute paths (`/etc/`), and hidden entries (`.git`).

### Directory Enumeration & Matching
- Scans candidate directory via POSIX `opendir()` and `readdir()`.
- Filters directory entries matching `file_part` using `strncmp()` (case-sensitive).
- Omits hidden files unless `file_part` explicitly begins with `.`.
- Sorts matches alphabetically using `qsort()`.

### Completion State Machine
1. **Case A (Unique Single Match)**:
   - Completes the word directly.
   - Inspects entry using `stat()`. If `S_ISDIR(st.st_mode)`, automatically appends a trailing `/`.
   - Advances cursor past completed token, preserving any trailing line suffix.
2. **Case B (Longest Common Prefix Extension)**:
   - If multiple entries match and share a common prefix longer than what was typed, extends the word to the Longest Common Prefix (LCP).
   - Cursor updates to the end of the extended prefix; completion choice state remains inactive.
3. **Case C (Ambiguous Matches / Disambiguation Menu)**:
   - If multiple entries match with no further common prefix, activates `tab->completion_active = true`.
   - Formats a compact, numbered list: `1. alpha.txt  2. beta.txt`.
   - Appends line to terminal scrollback.
   - User typing a digit `1`..`9` triggers `autocomplete_handle_choice()`, replacing the token with the selected file (and trailing `/` if directory).
   - Any other keystroke or action cancels ambiguous selection state via `autocomplete_cancel()`.

---

## 17. Internal Bash Script Interpreter & AST Execution Engine (Task 11)

### Execution Model & Invocation Forms
Terminator executes scripts through its own native recursive-descent interpreter without invoking `/bin/bash` or `execvp("bash", ...)`:
- Invocation Form A: `./script.sh [args...]` or `path/to/script.sh`
- Invocation Form B: `bash script.sh [args...]`
- Invocation Form C: `source script.sh [args...]` or `. script.sh`
- Binary Discrimination: Inspects file headers to distinguish compiled ELF binaries (`\x7f\x45\x4c\x46`) from text scripts.

### Phase 1: Static Pre-Execution Validation
Before executing any command, `script_validate_file()` validates the entire script syntax:
1. **Logical Line Reconstruction**: Handles comments (`#`), trailing backslash continuations (`\`), and multiline quoted blocks (`'...'`, `"..."`). Detects unexpected EOF with unclosed quotes.
2. **Control Flow Validation**: Validates balanced `if` ... `elif` ... `else` ... `fi` nesting blocks. Rejects unclosed `if`, orphaned `elif`/`else`/`fi`, and malformed control branches.
3. **Test Condition Validation**: Validates syntax within `[ ... ]` brackets, ensuring test operators (`-eq`, `-gt`, `=`, `-f`, `-d`) receive exact operand counts.
4. **Zero-Side-Effect Guarantee**: Any syntax error reports `line <N>: syntax error` and aborts execution immediately. **No preceding commands are executed** (validated with marker file creation tests).

### Phase 2: AST Construction & Execution
Once validated, `script_execute_file()` transforms statements into an Abstract Syntax Tree (AST):
- **`Stmt` Types**:
  - `STMT_CMD`: Arbitrary shell command, pipeline, or redirection.
  - `STMT_ASSIGN`: Variable assignment `NAME=value`.
  - `STMT_IF`: Conditional block containing branches and bodies.
  - `STMT_EXIT`: Script termination with exit status `exit [n]`.
- **Positional Parameters**:
  - Sets `$0` (script path), `$1`..`$9` (arguments), and `$#` (argument count) in `ScriptVarContext`.
  - Nested AST statement evaluator supports `$VAR`, `${VAR}`, `$?`, and `$$`.
- **Conditionals & Test Operators**:
  - Evaluates test expressions:
    - `-eq`, `-gt`: Integer numeric comparisons.
    - `=`: String equality comparison.
    - `-f`: Regular file existence check (`stat()` + `S_ISREG`).
    - `-d`: Directory existence check (`stat()` + `S_ISDIR`).
- **Context Scoping**:
  - Direct execution (`./script.sh`, `bash script.sh`): Executes in an isolated subshell environment; script variables do not pollute tab environment.
  - Sourcing (`source script.sh`, `. script.sh`): Executes in current tab context; variables and working directory updates persist directly in the calling tab.
