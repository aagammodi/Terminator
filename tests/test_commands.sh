#!/usr/bin/env bash
set -e

TERMINATOR="./terminator"
PASS=0
FAIL=0

assert_output() {
    local desc="$1"
    local cmd="$2"
    local expected="$3"
    local out
    out=$($TERMINATOR -c "$cmd" 2>&1 || true)
    if echo "$out" | grep -qF "$expected"; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        echo "    Expected: $expected"
        echo "    Got:      $out"
        FAIL=$((FAIL + 1))
    fi
}

assert_session() {
    local desc="$1"
    local input="$2"
    local expected="$3"
    local out
    out=$(echo -e "$input" | $TERMINATOR --test 2>&1 || true)
    if echo "$out" | grep -qF "$expected"; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        echo "    Expected: $expected"
        echo "    Got:      $out"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Running Task 2 Tests: Built-in & External Commands ==="

assert_output "echo external command" "echo hello" "hello"
assert_output "uname external command" "uname -s" "Linux"
assert_output "pwd built-in" "pwd" "$(pwd)"
assert_output "command not found error" "notacommand_xyz" "terminator: notacommand_xyz: command not found"

assert_session "cd /tmp changes directory" "cd /tmp\npwd\nexit" "/tmp"
assert_session "cd with no args goes to HOME" "cd /tmp\ncd\npwd\nexit" "$HOME"
assert_session "cd ~ goes to HOME" "cd /tmp\ncd ~\npwd\nexit" "$HOME"
assert_session "cd invalid dir reports error" "cd /nonexistent_dir_98765\nexit" "No such file or directory"
assert_session "exit status \$? after false" "false\necho status: \$?\nexit" "status: 1"
assert_session "exit status \$? after true" "true\necho status: \$?\nexit" "status: 0"
assert_session "exit status \$? after command not found" "notacommand_xyz\necho status: \$?\nexit" "status: 127"
assert_session "history displays recent commands" "echo cmd1\necho cmd2\nhistory\nexit" "echo cmd1"
assert_session "history preserves sequence" "echo cmd1\necho cmd2\nhistory\nexit" "echo cmd2"

assert_output "deferred built-in multiWatch stub" "multiWatch" "built-in deferred"
assert_output "deferred built-in jobs stub" "jobs" "built-in deferred"
assert_output "deferred built-in fg stub" "fg" "built-in deferred"
assert_output "deferred built-in bg stub" "bg" "built-in deferred"
assert_output "deferred built-in source stub" "source" "built-in deferred"

echo "=== Task 2 Result: $PASS Passed, $FAIL Failed ==="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
