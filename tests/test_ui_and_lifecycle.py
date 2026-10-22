#!/usr/bin/env python3
import os
import pty
import select
import sys
import time

def find_terminator():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(script_dir, "..", "terminator"),
        os.path.join(script_dir, "terminator"),
        os.path.abspath("./terminator"),
        os.path.abspath("../terminator"),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    return None

def run_interactive_session(extra_args=None, mode_name="Thread Mode"):
    terminator_bin = find_terminator()
    if not terminator_bin:
        sys.stderr.write("Error: 'terminator' executable not found. Please run 'make' first.\n")
        return 1

    extra_args = extra_args or []
    master, slave = pty.openpty()
    pid = os.fork()

    if pid == 0:
        os.close(master)
        os.setsid()
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(slave)
        os.environ["TERM"] = "xterm-256color"
        cmd = [terminator_bin, "--test"] + extra_args
        try:
            os.execv(terminator_bin, cmd)
        except Exception as e:
            sys.stderr.write(f"Failed to exec {terminator_bin}: {e}\n")
        os._exit(1)

    os.close(slave)

    def read_until(pattern, timeout=2.0):
        buf = ""
        start = time.time()
        while time.time() - start < timeout:
            r, _, _ = select.select([master], [], [], 0.1)
            if r:
                try:
                    data = os.read(master, 1024).decode('utf-8', errors='ignore')
                    if not data:
                        break
                    buf += data
                    if pattern in buf:
                        return buf
                except OSError:
                    break
        return buf

    def send_cmd(cmd):
        os.write(master, (cmd + "\n").encode('utf-8'))
        time.sleep(0.05)

    print(f"=== Running Terminal Correctness & Lifecycle Tests ({mode_name}) ===")
    passes = 0
    failures = 0

    def check(desc, cond, details=""):
        nonlocal passes, failures
        if cond:
            print(f"  [PASS] {desc}")
            passes += 1
        else:
            print(f"  [FAIL] {desc} - {details}")
            failures += 1

    # 1. Initial prompt appears exactly once
    initial = read_until("@terminator> ", timeout=1.5)
    check("Initial launch shows exactly one prompt", initial.count("@terminator> ") == 1, f"Count was {initial.count('@terminator> ')}")

    # 2. Built-in command generates exactly one subsequent prompt
    send_cmd("pwd")
    res_pwd = read_until("@terminator> ", timeout=1.5)
    check("Command 'pwd' execution yields exactly one subsequent prompt", res_pwd.count("@terminator> ") == 1, f"Output was: {repr(res_pwd)}")

    # 3. External command generates exactly one subsequent prompt
    send_cmd("echo test_run")
    res_ext = read_until("@terminator> ", timeout=1.5)
    check("External command 'echo' yields exactly one subsequent prompt", res_ext.count("@terminator> ") == 1, f"Output was: {repr(res_ext)}")

    # 4. Five consecutive commands don't multiply prompts
    total_new_prompts = 0
    for i in range(5):
        send_cmd(f"echo seq_{i}")
        res_seq = read_until("@terminator> ", timeout=1.5)
        total_new_prompts += res_seq.count("@terminator> ")
    check("5 consecutive commands generate exactly 5 new prompts", total_new_prompts == 5, f"Total was {total_new_prompts}")

    # 5. Incomplete multiline input shows continuation prompt '> '
    send_cmd("echo one \\")
    res_cont = read_until("> ", timeout=1.5)
    check("Multiline trailing backslash shows continuation prompt '> '", "> " in res_cont and "@terminator>" not in res_cont, f"Output was: {repr(res_cont)}")

    # 6. Completing multiline command yields output and exactly one primary prompt
    send_cmd("two")
    res_done = read_until("@terminator> ", timeout=1.5)
    check("Completing multiline input prints 'one two' and exactly one prompt", "one two" in res_done and res_done.count("@terminator> ") == 1, f"Output was: {repr(res_done)}")

    # 7. Clear command execution
    send_cmd("clear")
    res_clear = read_until("@terminator> ", timeout=1.5)
    has_raw_ansi = "^[[" in res_clear or "\x1b[H\x1b[2J" in res_clear or "\x1b[2J" in res_clear
    check("Command 'clear' does not output raw escape sequences", not has_raw_ansi, f"Output was: {repr(res_clear)}")
    check("Command 'clear' leaves exactly one prompt available", res_clear.count("@terminator> ") == 1, f"Count was {res_clear.count('@terminator> ')}")

    # 8. Subsequent command works after clear
    send_cmd("echo after_clear_ok")
    res_after = read_until("@terminator> ", timeout=1.5)
    check("Commands execute normally after clear", "after_clear_ok" in res_after, f"Output was: {repr(res_after)}")

    # 9. Empty quoted strings preserved as distinct arguments
    send_cmd('python3 -c "import sys; print(len(sys.argv), sys.argv[1])" "" "foo"')
    res_quotes = read_until("@terminator> ", timeout=1.5)
    check("Empty quotes preserved as distinct empty arguments", "3 " in res_quotes, f"Output was: {repr(res_quotes)}")

    # 10. ANSI SGR escape sequence handling
    send_cmd('printf "\\033[32mcolored_output_test\\033[0m\\n"')
    res_sgr = read_until("@terminator> ", timeout=1.5)
    check("ANSI SGR codes parsed cleanly without raw escape leaks", "colored_output_test" in res_sgr and "\033[32m" not in res_sgr, f"Output was: {repr(res_sgr)}")

    # 11. Clean exit
    send_cmd("exit")
    time.sleep(0.1)
    os.close(master)
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass

    print(f"=== {mode_name} Result: {passes} Passed, {failures} Failed ===")
    return 0 if failures == 0 else 1

def main():
    ret1 = run_interactive_session([], "Mode A: Thread Mode")
    ret2 = run_interactive_session(["useProcess=True"], "Mode B: Process Mode")
    return ret1 or ret2

if __name__ == "__main__":
    sys.exit(main())
