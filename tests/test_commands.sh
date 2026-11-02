#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -x "$SCRIPT_DIR/../terminator" ]; then
    TERMINATOR="$SCRIPT_DIR/../terminator"
elif [ -x "./terminator" ]; then
    TERMINATOR="./terminator"
else
    TERMINATOR="terminator"
fi
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
assert_session "clear built-in exit status" "clear\necho status: \$?\nexit" "status: 0"
assert_output "command not found error" "notacommand_xyz" "terminator: notacommand_xyz: command not found"

assert_session "cd /tmp changes directory" "cd /tmp\npwd\nexit" "/tmp"
assert_session "cd with no args goes to HOME" "cd /tmp\ncd\npwd\nexit" "$HOME"
assert_session "cd ~ goes to HOME" "cd /tmp\ncd ~\npwd\nexit" "$HOME"
assert_session "cd invalid dir reports error" "cd /nonexistent_dir_98765\nexit" "No such file or directory"
assert_session "clear built-in clears screen cleanly" "echo line_before_clear\nclear\npwd\nexit" "$(pwd)"
assert_session "exit status \$? after false" "false\necho status: \$?\nexit" "status: 1"
assert_session "exit status \$? after true" "true\necho status: \$?\nexit" "status: 0"
assert_session "exit status \$? after command not found" "notacommand_xyz\necho status: \$?\nexit" "status: 127"
assert_session "history displays recent commands" "echo cmd1\necho cmd2\nhistory\nexit" "echo cmd1"
assert_session "history preserves sequence" "echo cmd1\necho cmd2\nhistory\nexit" "echo cmd2"

assert_output "built-in multiWatch missing argument" "multiWatch" "missing argument"
assert_output "built-in fg no current job" "fg" "no current job"
assert_output "built-in bg no current job" "bg" "no current job"
assert_output "source missing argument reports error" "source" "filename argument required"

# Test genuine script execution with source built-in
TMP_SCRIPT=$(mktemp /tmp/terminator_test_script_XXXXXX.sh)
cat << 'EOF' > "$TMP_SCRIPT"
# This is a comment in the sourced script
echo "sourced_script_executed_successfully"
cd /tmp
EOF

assert_session "source executes commands and modifies shell cwd" "source $TMP_SCRIPT\npwd\nexit" "/tmp"
assert_session "source prints command output" "source $TMP_SCRIPT\nexit" "sourced_script_executed_successfully"
rm -f "$TMP_SCRIPT"

# Test Issue 2: Builtin exit status propagation in script
TMP_STATUS_SCRIPT=$(mktemp /tmp/terminator_test_status_XXXXXX.sh)
cat << 'EOF' > "$TMP_STATUS_SCRIPT"
cd /nonexistent_directory_for_test_12345
if [ $? -eq 0 ]; then
    echo "STATUS_BUG"
else
    echo "STATUS_CORRECT"
fi
EOF
assert_output "Script builtin exit status propagation (Thread mode)" "bash $TMP_STATUS_SCRIPT" "STATUS_CORRECT"
out=$($TERMINATOR useProcess=True -c "bash $TMP_STATUS_SCRIPT" 2>&1 || true)
if echo "$out" | grep -qF "STATUS_CORRECT"; then
    echo "  [PASS] Script builtin exit status propagation (Process mode)"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Script builtin exit status propagation (Process mode)"
    FAIL=$((FAIL + 1))
fi

# Test Issue 3: No duplicate output in headless script execution
TMP_DUP_SCRIPT=$(mktemp /tmp/terminator_test_dup_XXXXXX.sh)
cat << 'EOF' > "$TMP_DUP_SCRIPT"
echo "single_output_token_xyz"
EOF
out=$($TERMINATOR -c "bash $TMP_DUP_SCRIPT" 2>&1 || true)
count=$(echo "$out" | grep -c "single_output_token_xyz" || true)
if [ "$count" -eq 1 ]; then
    echo "  [PASS] Script headless execution produces output exactly once (Thread mode)"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Script headless execution produces output exactly once (Thread mode, count: $count)"
    FAIL=$((FAIL + 1))
fi

out=$($TERMINATOR useProcess=True -c "bash $TMP_DUP_SCRIPT" 2>&1 || true)
count=$(echo "$out" | grep -c "single_output_token_xyz" || true)
if [ "$count" -eq 1 ]; then
    echo "  [PASS] Script headless execution produces output exactly once (Process mode)"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Script headless execution produces output exactly once (Process mode, count: $count)"
    FAIL=$((FAIL + 1))
fi
rm -f "$TMP_STATUS_SCRIPT" "$TMP_DUP_SCRIPT"

echo "=== Task 2 Result: $PASS Passed, $FAIL Failed ==="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
