#!/usr/bin/env bash
set -e

TERMINATOR="./terminator"
PASS=0
FAIL=0

cleanup() {
    rm -f tests/test_*.tmp tests/out*.txt tests/res*.txt
}
trap cleanup EXIT
cleanup

assert_file_content() {
    local desc="$1"
    local file="$2"
    local expected="$3"
    if [ -f "$file" ] && [ "$(cat "$file")" = "$expected" ]; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        echo "    Expected: '$expected'"
        echo "    File content: '$(cat "$file" 2>/dev/null || echo "<file missing>")'"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Running Task 4 Tests: I/O Redirection ==="

# 1. Output redirection creates file
$TERMINATOR -c "echo hello > tests/test_out.tmp"
assert_file_content "Output redirection > creates file" "tests/test_out.tmp" "hello"

# 2. Output redirection truncates existing file
$TERMINATOR -c "echo replacement > tests/test_out.tmp"
assert_file_content "Output redirection > truncates file" "tests/test_out.tmp" "replacement"

# 3. Output append redirection >>
$TERMINATOR -c "echo line2 >> tests/test_out.tmp"
expected_multi=$(printf "replacement\nline2")
assert_file_content "Output redirection >> appends" "tests/test_out.tmp" "$expected_multi"

# 4. Input redirection <
printf "orange\napple\nbanana\n" > tests/test_in.tmp
out=$($TERMINATOR -c "sort < tests/test_in.tmp")
expected_sorted=$(printf "apple\nbanana\norange")
if [ "$out" = "$expected_sorted" ]; then
    echo "  [PASS] Input redirection < works"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Input redirection <"
    echo "    Expected: $expected_sorted"
    echo "    Got:      $out"
    FAIL=$((FAIL + 1))
fi

# 5. Combined redirection < in > out
$TERMINATOR -c "sort < tests/test_in.tmp > tests/test_sorted.tmp"
assert_file_content "Combined redirection < in > out" "tests/test_sorted.tmp" "$expected_sorted"

# 6. Combined redirection > out < in
rm -f tests/test_sorted.tmp
$TERMINATOR -c "sort > tests/test_sorted.tmp < tests/test_in.tmp"
assert_file_content "Combined redirection > out < in" "tests/test_sorted.tmp" "$expected_sorted"

# 7. Missing input file error reporting
err_out=$($TERMINATOR -c "cat < tests/nonexistent_file_12345.tmp" 2>&1 || true)
if echo "$err_out" | grep -q "No such file or directory"; then
    echo "  [PASS] Missing input file reports clear error"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Missing input file error reporting"
    echo "    Got: $err_out"
    FAIL=$((FAIL + 1))
fi

# 8. Failed redirection exit code is non-zero
set +e
$TERMINATOR -c "cat < tests/nonexistent_file_12345.tmp" >/dev/null 2>&1
status=$?
set -e
if [ $status -ne 0 ]; then
    echo "  [PASS] Failed redirection returns non-zero status ($status)"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Failed redirection returned status 0"
    FAIL=$((FAIL + 1))
fi

# 9. Built-in redirection
$TERMINATOR -c "pwd > tests/test_pwd.tmp"
assert_file_content "Built-in redirection: pwd > file" "tests/test_pwd.tmp" "$(pwd)"

echo "=== Task 4 Result: $PASS Passed, $FAIL Failed ==="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
