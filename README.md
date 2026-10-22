# Terminator — Custom Linux Terminal Emulator & Shell (Phase 1)

**Terminator** is an Operating Systems course project (CS69201, Computing Lab, IIT Kharagpur) implementing a custom terminal emulator and command shell from first principles in C using Xlib and standard Linux system calls.

It features a custom-built GUI window, genuine shell lexer and parser, process orchestration (`fork`, `execvp`, `waitpid`), I/O redirection (`<`, `>`, `>>` via `dup2`), arbitrary-length pipelines with kernel pipe management, command substitution, variable expansion, in-memory history, and a modern, cohesive **Neon Aurora** visual theme.

---

## 1. Implemented Features (Tasks 1–5)

### Task 1 — X11 GUI & Terminal Interaction
- **Native Xlib Window**: Built directly with `XOpenDisplay`, `XCreateWindow`, `XMapWindow`, `XSelectInput`, `XNextEvent`, and `XDrawString`.
- **Double-Buffered Rendering**: Smooth rendering via an off-screen `Pixmap` back-buffer, preventing flicker during typing and rapid output streaming.
- **Neon Aurora Theme**: Bespoke visual palette (#19172B rich dark background, #24213B secondary surface, #302A4B elevated tab cards, #A78BFA purple accents, #22D3EE bright turquoise/cyan prompts, #F472B6 neon pink user badges, #60A5FA blue directory accents, #A6E3A1 soft green status indicators, #F0ABFC cursor).
- **Interactive Top Bar & Tab Strip**: Distinct application branding, active tab card `[ 1: ~/dir ]` with glowing violet underline and text truncation ellipses (`...`) preventing any overlap, and an independent `[ + ]` add-tab affordance.
- **Status Area**: Subtle bottom status bar showing tab ID, compact current working directory, previous exit status `$?`, and line count.
- **Full Line Editor & Single-Prompt Invariant**: Real-time character insertion, Backspace, Delete, Left/Right arrow cursor movement, Home, End, Ctrl+A, Ctrl+E, Ctrl+U (delete to start), Ctrl+K (delete to end), Ctrl+W (delete word backward), and Ctrl+L (clear screen). Enforces an explicit `ShellState` lifecycle guaranteeing exactly one active prompt at any time without phantom duplicates.
- **Responsive poll() Event Loop**: Non-blocking multiplexing between the X11 connection (`ConnectionNumber(dpy)`) and child command output pipe(s). Window never freezes on long-running commands or commands without output.
- **Scrollback Buffer**: Dynamic line storage up to 10,000 lines, scrollable via PageUp / PageDown and mouse wheel (Button4 / Button5), with a proportional scrollbar indicator and automatic return-to-bottom on new input.

### Task 2 — Built-in & External Commands
- **External Commands**: Executed via `fork()` + `execvp()`. When a command is not found, displays `terminator: <cmd>: command not found` and sets exit code 127 without crashing the GUI.
- **cd Built-in**: Changes the directory of the shell process itself using `chdir()`. Supports `cd` (HOME), `cd ~`, `cd ~/path`, absolute and relative paths, gracefully reports errors on missing directories, and updates the prompt.
- **pwd Built-in**: Prints current working directory from `getcwd()`.
- **clear Built-in & ANSI CSI Parser**: Implements `clear` as a native shell built-in to immediately reset visible terminal output, and integrates an ANSI CSI escape-sequence parser supporting `\033[2J`, `\033[3J`, cursor positioning, and SGR sequences so external tools never produce raw escape codes like `^[[H^[[2J`.
- **exit Built-in**: Cleanly shuts down the shell and closes the GUI.
- **history Built-in**: Displays recent numbered commands from in-memory history.
- **Reserved Built-in Stubs**: Dispatch interface and informative stubs for `multiWatch`, `jobs`, `fg`, `bg`, and `source` reserved for Tasks 6–11.
- **Status Tracking**: Tracks and maintains `$?` across built-in and external commands.

### Task 3 — Multiline Input, Quoting, and Command Substitution
- **State-Machine Shell Parser**: Genuine lexical parser tracking unquoted, single-quoted, double-quoted, backslash-escaped, and nested substitution contexts.
- **Incomplete Command Detection**: Detects open single quotes, open double quotes, trailing backslashes, and unclosed `$(` or backticks. Shows continuation prompt `> ` and preserves accumulated multiline input until complete.
- **Quoting Semantics**:
  - `'single quotes'`: Enclosed text is completely literal; no variable expansion or substitutions occur.
  - `"double quotes"`: Preserves spaces and argument boundaries while permitting variable expansion and command substitutions.
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
- **Large Stream Handling**: Asynchronous output reading prevents deadlocks when commands produce streams exceeding the Linux kernel pipe capacity (verified with 1MB+ streams).

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

### Graphical GUI Mode (Default)
When running on an X11 desktop or XWayland session:
```bash
./terminator
```
This opens the Terminator X11 window with the Neon Aurora theme.

### Headless / Shell Test Mode
If running in a container, remote SSH terminal without X11, or for CLI evaluation:
```bash
./terminator --test
```
You will enter an interactive Terminator shell session with full prompt, multiline input, built-ins, pipelines, and redirections.

### Single Command Execution Mode
To execute a command string through the Terminator engine and print output:
```bash
./terminator -c "printf 'pear\napple\norange\n' | sort | head -2"
./terminator -c 'echo "Current Directory: $(basename $(pwd))"'
```

---

## 5. Automated Test Suite

Run the full automated test suite covering Tasks 1–5:
```bash
make test
```
Or execute the test scripts individually:
```bash
bash tests/test_commands.sh     # Built-ins, external commands, $?
bash tests/test_redirection.sh    # <, >, >>, missing files, built-in redirection
bash tests/test_pipelines.sh      # 2, 3, 4, 5-stage pipes, deadlock tests
bash tests/test_multiline.sh      # Quotes, multiline \, $(...), backticks, globs
```

---

## 6. Supported Command Syntax & Examples

### Built-in Commands
```bash
pwd                               # Print working directory
cd /tmp                           # Change directory
cd                                # Return to $HOME
cd ~/Downloads                    # Tilde expansion
history                           # Show recent command history
exit                              # Terminate shell
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

## 7. Known Limitations & Deferred Features (Tasks 6–11)

As specified in Phase 1 non-goals:
- **Multiple Functional Tabs**: Phase 1 includes 1 fully functional tab with an extensible tab data structure. Multi-tab concurrency (Thread mode and Process mode) is deferred to Tasks 6–11.
- **Signals & Process Group Job Control**: Ctrl+C and Ctrl+Z signal handling, background jobs (`&`), `jobs`, `fg`, `bg` are deferred to Task 8.
- **multiWatch**: Parallel command runner with periodic polling is deferred to Task 6.
- **Searchable History & Ctrl+R**: 10,000-entry persistent disk history with longest-common-substring search is deferred to Task 9.
- **Filename Tab Auto-completion**: Interactive Tab completion is deferred to Task 10.
- **Bash Script Interpreter**: Execution of `.sh` script files with if/elif/else constructs is deferred to Task 11.
