# Terminator — Custom Linux Terminal Emulator & Shell

**Terminator** is an Operating Systems course project (CS69201, Computing Lab, IIT Kharagpur) implementing a custom terminal emulator and command shell from first principles in C using Xlib and standard Linux system calls.

It features a custom-built GUI window, genuine shell lexer and parser, process orchestration (`fork`, `execvp`, `waitpid`), I/O redirection (`<`, `>`, `>>` via `dup2`), arbitrary-length pipelines with kernel pipe management, command substitution, variable expansion, in-memory history, and a modern, cohesive **Neon Aurora** visual theme.

---

## 1. Implemented Features (Tasks 1–11)

### Task 1 — X11 GUI, Multi-Tab Interaction, Thread/Process Modes & Functional Scrollbar
- **Native Xlib Window**: Built directly with `XOpenDisplay`, `XCreateWindow`, `XMapWindow`, `XSelectInput`, `XNextEvent`, and `XDrawString`. Clean window title set strictly to `"Terminator"` across standard ICCCM and Extended Window Manager Hints (`_NET_WM_NAME`).
- **Double-Buffered Rendering**: Smooth rendering via an off-screen `Pixmap` back-buffer, preventing flicker during typing and rapid output streaming.
- **Neon Aurora Theme**: Bespoke visual palette (#19172B rich dark background, #24213B secondary surface, #302A4B elevated tab cards, #A78BFA purple accents, #22D3EE bright turquoise/cyan prompts, #F472B6 neon pink user badges, #60A5FA blue directory accents, #A6E3A1 soft green status indicators, #F0ABFC cursor).
- **Dual Architecture Execution Modes (Mode A & Mode B)**:
  - **Mode A — Thread Mode (`./terminator`, default)**: Exactly one Terminator process owns the X11 GUI. Each tab is represented by an independent POSIX thread (`pthread_create`), isolated shell state/CWD/jobs, and shared command history synchronized via a thread mutex.
  - **Mode B — Process Mode (`./terminator useProcess=True`)**: Parent process owns X11. Each newly opened tab is represented by an independent child process (`fork()`) communicating via bidirectional IPC pipes. Command history is shared in POSIX shared memory (`shm_open` + `mmap(MAP_SHARED)`) protected by a **process-shared semaphore** (`sem_init(..., 1, 1)`). Tab failure or exit does not crash unrelated tabs or the GUI.
- **Foreground Job Isolation, Ctrl+C & Hang-Free GUI Shutdown (e.g. `geany`, long-running tasks)**:
  - Assigns commands and pipelines their own process group (`setpgid`).
  - Pressing `Ctrl+C` sends `SIGINT` directly to the foreground process group (`kill(-tab->foreground_pgid, SIGINT)`), interrupting commands (such as `geany`, `sleep`, or `cat`) without crashing Terminator or worker threads/processes.
  - Non-blocking multiplexed `poll()` loops prevent worker hangs while GUI applications or long-running commands run.
  - Window close button (`WM_DELETE_WINDOW`) and tab close immediately terminate running child processes before joining threads, eliminating application freezes during exit.
- **Interactive Full-Screen TUI & Editor Support (`nano`, `vim`, `vi`, `htop`, `geany`)**:
  - Full-screen curses and raw-terminal applications (`nano`, `vim`, `vi`, `htop`, `top`, `less`, `more`, `man`) are fully supported. In graphical GUI mode, Terminator automatically routes interactive TUI commands to a dedicated terminal window (`xterm`, `gnome-terminal`, etc.) via `get_external_terminal_emulator()`, enabling full interactive editing without corrupting Terminator's graphical canvas or line editor.
  - GUI applications (such as `geany`) execute asynchronously in their own process group, fully interruptible with `Ctrl+C` without freezing Terminator.
- **Assignment-Compliant Functional Vertical Scrollbar**:
  - Visible scrollbar track and proportionally sized thumb (`thumb_h = (sb_h * visible_rows) / total_rows`, minimum 24px).
  - Accurate viewport positioning matching exact scrollback buffer depth.
  - Track clicking: clicking above the thumb pages up; clicking below the thumb pages down.
  - Smooth thumb dragging: grabs the thumb with pixel-accurate offset and updates `scroll_offset` smoothly during pointer motion (`Button1MotionMask`).
  - Bottom-following behavior: when at the bottom (`scroll_offset == 0`), new output automatically follows the bottom. Scrolled up (`scroll_offset > 0`), new output accumulates in the buffer without forcefully jerking the user's viewport back down.
  - Resizing responsiveness: recalculates viewport lines and scroll geometry dynamically on window resize.
- **Interactive Multi-Tab Support**:
  - Independent tabs (up to 16) each with their own isolated working directory, input buffer, scrollback buffer, history navigation, worker thread/process, running process, and cursor.
  - Interactive top bar showing all open tabs: `[ 1: ~/Downloads ] [ 2: /tmp ] [ + ]`.
  - Open new tabs via mouse click on `[ + ]` or `Ctrl+Shift+T`.
  - Switch tabs via mouse click on any tab card, `Ctrl+PageUp` / `Ctrl+PageDown`, or `Ctrl+1` through `Ctrl+9`.
  - Close tabs via mouse click on the `x` affordance, `Ctrl+Shift+W`, typing `exit`, or pressing `Ctrl+D` on an empty line. Closing the last remaining tab cleanly exits the application.
  - Active tab highlighted with elevated surface background, glowing violet accent underline, and vibrant typography.
  - Text truncation with ellipses (`...`) dynamically accommodates window resizing without overlapping.
- **Asynchronous Multi-Tab Background Execution**:
  - The `poll()` event loop simultaneously watches the X11 connection and the worker IPC pipes of **all** open tabs.
  - Hidden background tabs continue running commands and collecting output asynchronously into their own buffers without blocking the GUI or other tabs.
- **ANSI SGR Color Rendering**:
  - Full Select Graphic Rendition (SGR) parser (`\033[...m`) supporting standard (30–37), bold/bright, and high-intensity (90–97) color codes.
  - Commands such as `ls --color=always` render in vibrant colors (directories in blue, executables in green, errors in red, etc.) mapped directly to the Neon Aurora palette.
- **Status Area**: Subtle bottom status bar showing active tab index and total count `[TAB x/y]`, compact current working directory, previous exit status `$?`, and line count.
- **Full Line Editor & Single-Prompt Invariant**: Real-time character insertion, Backspace, Delete, Left/Right arrow cursor movement, Home, End, Ctrl+A, Ctrl+E, Ctrl+U (delete to start), Ctrl+K (delete to end), Ctrl+W (delete word backward), and Ctrl+L (clear screen). Enforces an explicit `ShellState` lifecycle guaranteeing exactly one active prompt at any time without phantom duplicates.

### Task 2 — Built-in & External Commands
- **External Commands**: Executed via `fork()` + `execvp()`. When a command is not found, displays `terminator: <cmd>: command not found` and sets exit code 127 without crashing the GUI.
- **cd Built-in**: Changes the directory of the shell tab itself using `chdir()`. Supports `cd` (HOME), `cd ~`, `cd ~/path`, absolute and relative paths, gracefully reports errors on missing directories, and updates the prompt. Independent across tabs.
- **pwd Built-in**: Prints current working directory from `getcwd()`.
- **clear Built-in & ANSI CSI Parser**: Implements `clear` as a native shell built-in to immediately reset visible terminal output, and integrates an ANSI CSI escape-sequence parser supporting `\033[2J`, `\033[3J`, cursor positioning, and SGR sequences so external tools never produce raw escape codes like `^[[H^[[2J`.
- **exit Built-in**: Closes the active tab (or cleanly terminates the application if only one tab remains).
- **history Built-in**: Displays recent numbered commands from in-memory history.
- **source Built-in**: Genuinely reads and executes command script files line-by-line within the tab's shell context, handling comments (`#`), multiline continuations, directory changes, and sequentially executed pipelines.
- **Reserved Built-in Stubs**: Dispatch interface and informative stubs for `multiWatch` (requires Tasks 6–11), and clean empty output for `jobs`, with clean non-fatal errors for `fg` and `bg` (`no current job`).
- **Status Tracking**: Tracks and maintains `$?` across built-in and external commands.

### Task 3 — Multiline Input, Quoting, and Command Substitution
- **State-Machine Shell Parser**: Genuine lexical parser tracking unquoted, single-quoted, double-quoted, backslash-escaped, and nested substitution contexts.
- **Incomplete Command Detection**: Detects open single quotes, open double quotes, trailing backslashes, and unclosed `$(` or backticks. Shows continuation prompt `> ` and preserves accumulated multiline input until complete.
- **Quoting Semantics**:
  - `'single quotes'`: Enclosed text is completely literal; no variable expansion or substitutions occur.
  - `"double quotes"`: Preserves spaces and argument boundaries while permitting variable expansion and command substitutions.
  - Empty quotes preservation: Preserves `""` and `''` as valid distinct empty string arguments (`argv[i] = ""`).
  - Backslash `\`: Escapes the next character outside single quotes; trailing `\` performs line continuation.
- **Variable Expansion**: Expands `$VAR`, `${VAR}`, `$?` (exit status), and `$$` (PID) in unquoted and double-quoted contexts.
- **Command Substitution**: Evaluates `$(cmd)` and `` `cmd` `` recursively from innermost to outermost through our own shell execution engine, captures output through pipes, strips trailing newlines, and splices the result into the command stream.
- **Pathname Expansion (Globbing)**: Unquoted wildcards (`*`, `?`, `[...]`) are expanded via POSIX `glob()`. Quoted wildcards are preserved literally.

### Task 4 — I/O Redirection
- **Syntax Supported**:
  - `cmd < input_file` (stdin redirection via `O_RDONLY`)
  - `cmd > output_file` (stdout redirection via `O_WRONLY | O_CREAT | O_TRUNC`, 0644)
  - `cmd >> output_file` (stdout append redirection via `O_APPEND`)
  - `cmd < in > out` and `cmd > out < in` (combined redirections in any order)
- **Child-Side Rewiring**: `dup2()` is performed inside the forked child process before `execvp()`. The parent shell's file descriptors remain untouched.
- **Missing File Protection**: If an input file does not exist, reports `terminator: <file>: No such file or directory`, exits child with status 1, does NOT execute the target program, and keeps the shell and GUI intact.
- **Built-in Redirection**: Built-ins (e.g. `pwd > out.txt`) correctly redirect stdout to files in parent mode using descriptor save and restore (`dup` / `dup2`).

### Task 5 — Arbitrary-Length Pipelines
- **N Commands, N-1 Pipes**: Arbitrary pipeline length (`cmd1 | cmd2 | ... | cmdN`).
- **Deadlock-Free Descriptor Management**:
  - For stage $i$: STDIN duped from pipe $i-1$, STDOUT duped to pipe $i$.
  - First command supports input redirection (`< in`).
  - Last command supports output redirection (`> out`) or connects to the tab's output capture pipe.
  - Stderr from all child commands is routed to the terminal capture pipe for visual reporting.
  - Every child closes all unused pipe read/write ends.
  - Parent closes all $N-1$ pipe ends immediately after launching all children, ensuring EOF propagation.
- **Pipeline Status**: Reaps all child PIDs and records the final pipeline command's exit code in `$?`.
- **Pipeline Stage Safety Limits**: Strictly validates against `MAX_PIPELINE_CMDS` (64 stages); gracefully rejects overflow pipelines with exit code 1 to protect process table and descriptor table limits.
- **Large Stream Handling**: Asynchronous output reading prevents deadlocks when commands produce streams exceeding the Linux kernel pipe capacity (verified with 1MB+ streams).

### Task 6 — multiWatch (Concurrent Periodic Command Monitoring)
- **Built-in Syntax**: `multiWatch ["cmd1", "cmd2", ...]`
  - Robust JSON-style bracketed list parsing supporting single and double quotes.
  - Commands may contain internal commas, quotes, flags, and complex shell pipelines (e.g. `multiWatch ["date +%T", "ls -l | wc -l"]`).
  - Graceful rejection with clear diagnostics for syntax errors, missing brackets, unquoted entries, and empty command lists.
- **Concurrent Non-Blocking Execution**:
  - Every monitored command runs in its own child process group (`setpgid(0, 0)`), executing through Terminator's shell parser and pipeline engine.
  - Output streams are monitored simultaneously using multiplexed `poll()`, avoiding busy-waiting and conserving CPU resources.
  - Asynchronous chunk buffering accumulates output per command without interleaving or truncating lines.
- **Periodic Scheduling (2-Second Independence)**:
  - Each command runs independently on a 2-second monotonic cycle (`CLOCK_MONOTONIC`) starting after its individual execution completes.
  - Fast commands do not tight-loop or starve other commands; slow or blocking commands do not delay the execution or output of faster commands.
- **Exact Output Formatting**:
  - Outputs atomic blocks matching the assignment specification with 52 dashes:
    ```text
    "<cmd>", <unix_timestamp>:
    ----------------------------------------------------
    <command output>
    ----------------------------------------------------
    ```
- **Instant Ctrl+C Cancellation**:
  - Pressing `Ctrl+C` immediately broadcasts `SIGINT` (followed by `SIGKILL` after grace period) to all monitored process groups and reaps them with `waitpid()`.
  - Prints `^C\n`, displays the primary prompt, and returns control to the user without leaving orphan processes or crashing the shell.
- **Dual Mode Support**: Works identically in Mode A (Thread Mode) and Mode B (Process Mode), passing IPC control messages seamlessly.

### Task 7 — Hotkeys, Key Bindings, Selection & Configuration
- **Full In-Line Cursor Navigation & Editing**:
  - `Left` / `Right` arrows: Move cursor horizontally, strictly clamped to input bounds.
  - `Ctrl+A` / `Home`: Jump to the beginning of the line (`0`).
  - `Ctrl+E` / `End`: Jump to the end of the line (`len`).
  - `Ctrl+U`: Delete all characters from cursor backward to start of line; preserves characters after cursor.
  - `Ctrl+K`: Delete all characters from cursor forward to end of line; preserves characters before cursor.
  - `Ctrl+W`: Delete previous word backward relative to cursor; skips whitespace and preserves text after cursor.
  - `Up` / `Down` arrows: History navigation across shared command history with draft preservation (unsubmitted input restored when returning to draft).
  - `Ctrl+L`: Clear screen while preserving current active input and cursor position.
  - `Ctrl+D`: Context-sensitive EOF handling: closes active tab if input line is empty; protects unsubmitted input when non-empty.
- **Premium Gradient Text Selection & X11 Clipboard**:
  - Mouse text selection: Drag left mouse button (`Button1`) across terminal lines to highlight text with clean sub-character clamping.
  - **Horizontal Aurora Gradient Highlight**: Selected regions render with a rich horizontal gradient interpolating smoothly from Cyan (`#00E5FF`) to Electric Blue (`#4776FF`) to Vivid Violet (`#A855F7`), enclosed within a subtle glowing border (`#00E5FF`).
  - **High-Contrast Text**: Text inside the selection renders in deep dark navy (`#08071A`), ensuring razor-sharp legibility and contrast against the vibrant gradient fill.
  - Multi-line selection support preserving newline semantics across line boundaries.
  - Per-tab selection isolation: each tab maintains its own active selection state without cross-tab interference.
  - `Ctrl+Shift+C`: Copy highlighted selection to X11 `CLIPBOARD` and `PRIMARY` selections.
  - `Ctrl+Shift+V`: Paste selection asynchronously from X11 clipboard into the input line at cursor position.
  - Middle-click (`Button2`): Paste primary selection directly at cursor.
- **Persistent Key Binding Engine & `bind` Built-in**:
  - Fully configurable via `~/.terminatorrc` configuration file.
  - Built-in command:
    - `bind`: Lists all active key bindings and combinations.
    - `bind <action> <combo>`: Binds an action (e.g., `bind clear-screen Ctrl+Alt+L`) with immediate validation and conflict detection.
    - `bind --reset`: Restores all key bindings to factory defaults.
  - Fallback safety: Automatically falls back to safe default bindings on parsing errors or syntax conflicts.

### Task 8 — Signals and Job Control
- **Process Group Isolation (`setpgid`)**: Every foreground and background command/pipeline is placed in its own unique process group (`setpgid(0, 0)`), preventing signal leakage into Terminator or other tabs.
- **`Ctrl+C` Foreground Interruption**:
  - Delivers `SIGINT` directly to the active foreground process group (`kill(-tab->foreground_pgid, SIGINT)`).
  - Child processes terminate immediately; the terminal prints `^C\n` and cleanly restores the prompt.
  - Terminator itself and all tab worker threads/processes remain completely unharmed.
  - Preserves `multiWatch` cancellation.
- **`Ctrl+Z` Foreground Suspension**:
  - Delivers `SIGTSTP` directly to the active foreground process group (`kill(-tab->foreground_pgid, SIGTSTP)`).
  - Suspended job is recorded in the tab's `JobTable` with state `Stopped`.
  - Prints `\n[<id>]+ Stopped <cmd>\n` and immediately restores the interactive shell prompt without blocking or hanging.
- **Background Execution Operator (`command &`)**:
  - Commands and pipelines ending in `&` launch in the background.
  - Prints `[<id>] <pgid>` and returns control to the user immediately.
  - Background jobs stream output asynchronously to the tab scrollback without corrupting the active input line or freezing the GUI.
- **`jobs` Built-in Command**:
  - Lists all active and completed background/suspended jobs for the current tab:
    ```text
    [1]+ Running              sleep 100 &
    [2]- Stopped              sleep 200
    ```
  - Displays `+` for the current job (most recently suspended/started) and `-` for the previous job.
  - Automatically reaps and reports `Done` jobs before removing them from the table.
  - Strictly isolated per tab (background jobs in Tab 1 do not appear in Tab 2).
- **`fg [%id]` Built-in Command**:
  - Brings a stopped or running background job to the foreground.
  - Sends `SIGCONT` to resume stopped jobs, prints the command line, and monitors until completion or subsequent suspension.
- **`bg [%id]` Built-in Command**:
  - Resumes a stopped job in the background by sending `SIGCONT`.
  - Updates job state to `Running`, prints `[<id>]+ <cmd> &`, and returns control to the prompt immediately.
- **Mode A (Thread Mode) and Mode B (Process Mode) Parity**:
  - Full signal routing and job control parity across both threading and multi-process architectures via framed IPC messages (`IPC_MSG_INT`, `IPC_MSG_TSTP`, `IPC_MSG_CONT`).

### Task 9 — Searchable History
- **Persistent Disk Storage (`~/.terminator_history`)**:
  - Stores up to the 10,000 most recent commands in `~/.terminator_history` (or custom path via `TERMINATOR_HISTORY`).
  - Supports multiline command escaping (newlines stored as `\n`, literal backslashes as `\\`).
  - Advisory file locking via `fcntl(F_SETLK)` prevents write collisions across concurrent Terminator processes.
- **Shared History Across All Tabs**:
  - Synchronized across threads in Mode A using a mutex.
  - Synchronized across child processes in Mode B using POSIX shared memory and a process-shared semaphore.
- **`history` Built-in Command**:
  - Prints the 1,000 most recent commands in chronological order (oldest first) with 1-based indexing:
    ```text
    1  ls -la
    2  git status
    3  make test
    ```
- **`Ctrl+R` Interactive Search Engine**:
  - Enters search mode, prompting: `Enter search term: ` with live cursor editing.
  - Pressing `Escape` or `Ctrl+C` cancels search and restores previous draft input.
  - Pressing `Enter` searches persistent history according to strict course priorities:
    1. **Priority 1 (Case A — Exact Match)**: If history contains commands exactly matching the search term, returns the **most recent exact match only**.
    2. **Priority 2 (Case B — Longest Contiguous Common Substring, $L_{\max} > 2$)**: If no exact match exists, finds commands sharing the maximum contiguous substring length $L_{\max}$. If $L_{\max} > 2$, returns **all tied matching commands** in newest-first order.
    3. **Priority 3 (Case C — No Match, $L_{\max} \le 2$)**: If the longest contiguous common substring is 2 characters or fewer, displays `No match for search term in history`.
  - Returns cleanly to prompt, restores active editing state, does NOT execute the command automatically, and does not record the search prompt or query in history.

### Native Terminal Blinking Cursor & Focus / Mouse Selection Behavior
- **Slim Neon Caret**: Standard 1–2px slim vertical insertion caret drawn in `#22D3EE` Electric Cyan accent color, spanning full font height.
- **Strict In-Memory Separation**: The caret is purely a graphical viewport artifact and is **never** inserted into the tab's input buffer or string memory, eliminating corruption.
- **Blink Interval & Reactive Reset**: Configured with a 500ms blink timer via `gui_cursor_reset_blink()`. Blinking immediately restarts and shows the cursor on any keystroke, arrow navigation, editing command, mouse click, or tab switch.
- **State-Aware Visibility Constraints**:
  - **Focus-Aware**: Hides cursor and suspends blinking when the Terminator window loses X11 focus (`FocusOut`), resuming automatically on `FocusIn`.
  - **Execution Guard**: Cursor is automatically hidden while commands or pipelines are executing (`SHELL_STATE_RUNNING`), reappearing only when the prompt returns.
  - **Selection Hiding**: When mouse text selection is active or being dragged (`sel_active || sel_dragging`), the caret is hidden to ensure clean visual readability.
- **Mouse Click Insertion Positioning**: Clicking within the prompt/input line calculates character boundary geometry accounting for prompt width, positioning the insertion point precisely at the clicked character. Clicking in the scrollback area preserves the cursor position.

### Task 10 — File-name Auto-completion
- **Directory Path & Prefix Detection**: Pressing `Tab` isolates the token immediately before the cursor, supporting relative, absolute, and tilde (`~`) paths as well as hidden files/directories starting with `.`.
- **Suffix Preservation**: Mid-line completion seamlessly preserves the suffix of the command line after the cursor.
- **Single Unique Match**: Directly completes the word and advances the cursor. If the completed path is a directory (`S_ISDIR`), a trailing `/` is automatically appended.
- **Longest Common Prefix (LCP)**: If multiple entries match, extends the typed prefix to the longest common substring shared by all matches without displaying a menu.
- **Numbered Disambiguation Menu**: When multiple matches share no further prefix, displays an alphabetically sorted numbered list (e.g. `1. alpha.txt  2. beta.txt`), placing the tab into choice completion state.
- **Choice Selection (1–9)**: Typing a single numeric digit (`1..9`) completes the corresponding entry (appending `/` if a directory) and returns to normal editing without executing the command.
- **Per-Tab Isolation**: Completion state, match lists, and directory buffers are strictly isolated per tab.

### Task 11 — Internal Bash Script Interpreter
- **Native Implementation**: Entirely implemented in native C without delegating to `/bin/bash`, `system()`, or `popen()`.
- **Supported Invocation Forms**:
  - Executable script: `./script.sh [args...]` or `path/to/script.sh [args...]`
  - Explicit bash call: `bash script.sh [args...]`
  - Current-context sourcing: `source script.sh [args...]` or `. script.sh [args...]`
  - Binary discrimination: Distinguishes script files from compiled ELF binaries via header magic bytes (`\x7f\x45\x4c\x46`).
- **Two-Phase Architecture**:
  - **Phase 1 — Static Validation**: Pre-parses and validates the entire script before executing any command. Validates matching `if`/`elif`/`else`/`fi` control flow, test brackets `[ ... ]`, and quotation balance.
  - **Strict Syntax Safety**: Any syntax error reports the script filename and line number and aborts execution immediately. **No command preceding the error is ever executed** (verified via marker file test).
  - **Phase 2 — AST Execution**: Executes the validated Abstract Syntax Tree.
- **Supported Grammar & Features**:
  - Comments: Full-line (`# comment`) and inline (`cmd # comment`).
  - Positional arguments: `$0` (script path), `$1`..`$9`, and `$#` (argument count).
  - Variables: Assignment `VAR=val` and expansion `$VAR`, `${VAR}`.
  - Exit code propagation: Command exit codes and explicit `exit [n]` statements update `$?`.
  - Conditionals: `if`, `elif`, `else`, `fi`, including nested conditional blocks.
  - Test expressions: Supports `[ $a -eq $b ]`, `[ $a -gt $b ]`, `[ "$a" = "$b" ]`, `[ -f file ]`, and `[ -d dir ]`.
  - Pipelines and Redirection: Arbitrary pipelines (`cmd1 | cmd2`) and redirections (`<`, `>`, `>>`) inside script bodies.
  - Context scoping: `source` modifies the calling tab's variables and working directory; direct execution (`./script.sh`, `bash script.sh`) executes in an isolated subshell context.

---

## 2. Dependencies & Installation

### Requirements
- Linux OS (Ubuntu 20.04 / 22.04 / 24.04 LTS or compatible distribution)
- GCC compiler (C11 support)
- GNU Make
- X11 client library headers (`libx11-dev`)
- Standard POSIX C libraries

### Installation on Ubuntu / Debian
```bash
sudo apt update
sudo apt install -y build-essential libx11-dev pkg-config
```

---

## 3. Build Instructions

Build the application from the project root:
```bash
make
```
This compiles all sources with `-std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -lX11` and creates the executable:
```
./terminator
```

To clean build artifacts:
```bash
make clean
```

---

## 4. How to Run

### Graphical GUI Mode: Thread Mode (Default)
When running on an X11 desktop or XWayland session:
```bash
./terminator
```
This opens the Terminator X11 window where each tab executes on its own independent POSIX worker thread (`pthread_create`), sharing command history synchronized with a mutex.

### Graphical GUI Mode: Process Mode (Architecture Flag)
To launch Terminator in full multiprocess mode:
```bash
./terminator useProcess=True
```
In Process Mode, the parent process manages X11 while each tab runs in its own child process created via `fork()`. Command history is stored in POSIX shared memory (`shm_open`, `mmap`) protected by a **process-shared semaphore** (`sem_init(..., 1, 1)`).

### Headless / Shell Test Mode
If running in a container, remote SSH terminal without X11, or for automated testing:
```bash
./terminator --test
./terminator useProcess=True --test
```
You will enter an interactive Terminator shell session with full prompt, multiline input, built-ins, pipelines, and redirections.

### Single Command Execution Mode
To execute a command string through the Terminator engine and print output:
```bash
./terminator -c "printf 'pear\napple\norange\n' | sort | head -2"
./terminator useProcess=True -c 'echo "Current Directory: $(basename $(pwd))"'
```

---

## 5. Automated Test Suite

Run the complete 280-test automated test suite covering Tasks 1–9, Thread Mode, Process Mode, Ctrl+C / Ctrl+Z signal routing, Job Control, Searchable History, and Scrollbar mechanics:
```bash
make test
```
Or execute the test components individually:
```bash
./tests/test_tabs                    # Task 1: Multi-tab lifecycle, bounds, and directory isolation
./tests/test_modes_and_scrollbar     # Task 1: Scrollbar geometry, Mode A threads, Mode B processes, crash isolation
./tests/test_tasks6_and_7            # Tasks 6 & 7: multiWatch concurrency, keybindings, selection geometry
./tests/test_tasks8_and_9            # Tasks 8 & 9: Job control, process groups, LCS history search engine
./tests/test_tasks10_and_11          # Tasks 10 & 11: Autocomplete, script validator & AST execution
python3 tests/test_ui_and_lifecycle.py # Interactive PTY lifecycle, Ctrl+C, Ctrl+Z, jobs, bg, fg in both modes
python3 tests/command_compatibility/run_all_audit.py # Full 100 Linux command suite audit
bash tests/test_commands.sh          # Task 2: Built-ins, external commands, $?
bash tests/test_redirection.sh       # Task 4: <, >, >>, missing files, built-in redirection
bash tests/test_pipelines.sh         # Task 5: 2, 3, 4, 5-stage pipes, deadlock tests
bash tests/test_multiline.sh         # Task 3: Quotes, multiline \, $(...), backticks, globs
```

---

## 6. Supported Command Syntax & Examples

### Built-in Commands
```bash
pwd                               # Print working directory
cd /tmp                           # Change directory
cd                                # Return to $HOME
cd ~/Downloads                    # Tilde expansion
history                           # Show up to 1,000 recent commands chronologically
jobs                              # List background and suspended jobs for active tab
fg %1                             # Bring job 1 to foreground
bg %1                             # Resume stopped job 1 in background
multiWatch ["date", "uptime"]     # Run concurrent periodic monitoring
bind                              # List active keybindings
exit                              # Terminate active tab/shell
```

### Job Control & Signals (Task 8)
```bash
# Run command in background:
sleep 60 &                        # Prints [1] <pgid> and returns prompt immediately

# Suspend foreground job:
sleep 200
# Press Ctrl+Z:
# Prints [2]+ Stopped sleep 200 and restores prompt

# Resume stopped job in background:
bg %2                             # Prints [2]+ sleep 200 &

# Bring job to foreground and interrupt:
fg %2
# Press Ctrl+C:
# Terminates sleep 200 process group, prints ^C, and restores prompt
```

### Searchable History & Hotkeys (Task 9 & Task 7)
```bash
# Press Ctrl+R:
# Displays: Enter search term:
# Type query and press Enter:
# - Case A: Most recent exact match
# - Case B: All tied commands achieving maximum common substring length (L > 2)
# - Case C: Prints "No match for search term in history" (if L <= 2)

# Select text with mouse:
# Highlights with horizontal Cyan -> Electric Blue -> Vivid Violet gradient
# Press Ctrl+Shift+C to copy, Ctrl+Shift+V to paste
```

### External Commands & Status
```bash
uname -a
ls -la
false
echo $?                           # Prints 1
notacommand
echo $?                           # Prints 127
```

### Multiline Commands & Quoting
```bash
echo "hello world"                # Preserves space
echo 'literal: $HOME'             # Single quotes: no expansion
echo "expanded: $HOME"            # Double quotes: expanded $HOME

# Multiline line continuation:
echo one \
two                               # Prints: one two

# Multiline double quotes:
echo "first line
second line"                      # Prints both lines

# Command substitution:
echo "Dir: $(basename $(pwd))"    # Nested substitution
echo "Files: $(ls | wc -l)"       # Pipeline substitution
echo "Year: `date +%Y`"           # Backticks
```

### Redirection & Pipelines
```bash
sort < names.txt > sorted.txt
echo "log line" >> activity.log
cat < missing.txt                 # Reports error, preserves shell

# Pipelines:
ls *.txt | wc -l
printf 'pear\napple\norange\n' | sort | head -2
cat input.txt | grep error | sort | uniq -c | head -10
sort < input.txt | uniq -c > counts.txt
```

---

## 7. Architectural Boundaries & System Design Choices

All functional requirements for Tasks 1 through 11 are fully implemented and verified across both Thread Mode and Process Mode. The following design choices and architectural boundaries reflect standard Unix systems programming practices:

1. **Interactive Full-Screen TUI & Editor Applications (`nano`, `vim`, `vi`, `htop`, `less`)**:
   - Full-screen curses applications requiring terminal-raw `termios` attributes and full cursor addressing (`nano`, `vim`, `vi`, `htop`, `top`, `less`, `more`, `man`) are fully supported. When running in graphical GUI mode, Terminator automatically routes them to a dedicated terminal emulator window (`xterm`, `gnome-terminal`, etc.), providing complete interactive functionality while keeping Terminator's custom X11 canvas and line-editor state clean and uncorrupted.
   - In headless or test environments where no X11 display server is available, Terminator provides clear, informative diagnostic messages. Non-interactive queries (such as `nano --version`, `vi --version`, `top -b -n 1`) stream output directly to stdout.
2. **Terminal Control Sequences & ANSI Parser**:
   - Terminator implements a robust ANSI CSI & SGR escape sequence parser supporting full 16-color palettes (standard 30–37 and high-intensity 90–97), bold, default attributes, erase display (`\033[2J`, `\033[3J`), erase in line (`\033[K`), and cursor positioning (`\033[H`, `\033[f`).
   - Advanced DEC private modes (such as alternate screen buffers `\033[?1049h`) and OSC title sequences (`\033]...`) are safely parsed and consumed without leaking raw escape sequences onto the terminal canvas.
3. **Internal Script Interpreter Grammar & Scope (Task 11)**:
   - The native Task 11 script engine implements the complete grammar specified in the IIT Kharagpur course requirements: two-phase static pre-execution syntax validation, sequential command execution, variable assignments (`VAR=val`), expansions (`$VAR`, `${VAR}`, `$?`, `$$`), positional parameters (`$0`..`$9`, `$#`), nested conditionals (`if`, `elif`, `else`, `fi`), test brackets (`[ ... ]` with string, numeric, and file operators), arbitrary pipelines, and I/O redirections.
   - Constructs beyond the course specification (such as `for`/`while` loops and shell functions) are deferred beyond the project scope.
4. **Superuser Operations & GUI Authentication (`sudo`, `su`)**:
   - Terminator integrates an automatic GUI authentication helper (`ensure_askpass_helper()`) that invokes system password dialogs (`zenity`, `kdialog`, `ssh-askpass`, `xmessage`) via `SUDO_ASKPASS` for commands requiring privilege elevation (such as `sudo apt update`).
   - In non-interactive and headless environments, standard diagnostic queries (such as `su --help` and `sudo -V`) execute cleanly.
