#!/usr/bin/env python3
"""
Terminator Shell Integration Suite
Tests pipelines, redirection, variable expansion, command substitution,
and quoting across real external commands in both Thread Mode and Process Mode.
"""

import os
from test_framework import (
    run_terminator_cmd,
    run_terminator_session,
    setup_sandbox
)

def run_tests():
    sandbox = setup_sandbox()
    passes = 0
    failures = 0

    def assert_test(desc, ok, details=""):
        nonlocal passes, failures
        if ok:
            print(f"  [PASS] {desc}")
            passes += 1
        else:
            print(f"  [FAIL] {desc} - {details}")
            failures += 1

    print("=== Running Shell Integration & Command Compatibility Suite ===")

    for mode in ["thread", "process"]:
        mode_label = f"Mode ({mode.capitalize()} Mode)"

        # 1. Pipeline of external commands: cat | grep | wc -l
        test_file = f"{sandbox}/pipe_test_{mode}.txt"
        with open(test_file, "w") as f:
            f.write("apple\nbanana\napricot\nberry\navocado\n")
        code, out, err = run_terminator_cmd(f"cat {test_file} | grep ^a | wc -l", mode=mode)
        assert_test(f"{mode_label}: 3-stage pipeline (cat | grep | wc -l)", code == 0 and out.strip() == "3", f"got {out.strip()}")

        # 2. Output redirection to file (> and >>)
        out_f = f"{sandbox}/redirection_{mode}.txt"
        code1, _, _ = run_terminator_cmd(f"echo first_line > {out_f}", mode=mode)
        code2, _, _ = run_terminator_cmd(f"echo second_line >> {out_f}", mode=mode)
        content = ""
        if os.path.exists(out_f):
            with open(out_f, "r") as f:
                content = f.read()
        assert_test(f"{mode_label}: Output redirection (> and >>)", code1 == 0 and code2 == 0 and "first_line\nsecond_line" in content)

        # 3. Input redirection (<)
        in_f = f"{sandbox}/input_redir_{mode}.txt"
        with open(in_f, "w") as f:
            f.write("redirected_stdin_data")
        code, out, _ = run_terminator_cmd(f"cat < {in_f}", mode=mode)
        assert_test(f"{mode_label}: Input redirection (<)", code == 0 and "redirected_stdin_data" in out)

        # 4. Command substitution: $(uname -s)
        code, out, _ = run_terminator_cmd('echo "System is $(uname -s)"', mode=mode)
        assert_test(f"{mode_label}: Command substitution $(uname -s)", code == 0 and "System is Linux" in out)

        # 5. Exit status propagation: $?
        code, out, _ = run_terminator_session(["false", "echo status: $?"], mode=mode)
        assert_test(f"{mode_label}: Exit status propagation $? after failure", "status: 1" in out)

        code, out, _ = run_terminator_session(["true", "echo status: $?"], mode=mode)
        assert_test(f"{mode_label}: Exit status propagation $? after success", "status: 0" in out)

        # 6. Quoted arguments with spaces and globbing
        space_file = f"{sandbox}/file with spaces {mode}.txt"
        with open(space_file, "w") as f:
            f.write("content in space file")
        code, out, _ = run_terminator_cmd(f'cat "{space_file}"', mode=mode)
        assert_test(f"{mode_label}: Quoted path with spaces", code == 0 and "content in space file" in out)

    print(f"=== Shell Integration Result: {passes} Passed, {failures} Failed ===")
    return failures == 0

if __name__ == "__main__":
    ok = run_tests()
    exit(0 if ok else 1)
