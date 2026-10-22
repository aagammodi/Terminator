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
