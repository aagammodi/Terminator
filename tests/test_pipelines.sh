#!/usr/bin/env bash
set -e

TERMINATOR="./terminator"
PASS=0
FAIL=0

cleanup() {
    rm -f tests/pipe_*.tmp
}
trap cleanup EXIT
cleanup

assert_eq() {
    local desc="$1"
    local actual="$2"
    local expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        echo "    Expected: $expected"
        echo "    Got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Running Task 5 Tests: Pipelines of Arbitrary Length ==="

# 1. Two-command pipeline
out=$($TERMINATOR -c "printf 'pear\napple\norange\n' | sort")
exp=$(printf 'apple\norange\npear')
assert_eq "Two-command pipeline (printf | sort)" "$out" "$exp"

# 2. Three-command pipeline
out=$($TERMINATOR -c "printf 'pear\napple\norange\n' | sort | head -2")
exp=$(printf 'apple\norange')
assert_eq "Three-command pipeline (printf | sort | head -2)" "$out" "$exp"

# 3. Four-command pipeline
out=$($TERMINATOR -c "printf 'err1\nok\nerr2\nerr1\n' | grep err | sort | uniq -c | tr -s ' '")
exp=$(printf ' 2 err1\n 1 err2')
assert_eq "Four-command pipeline (printf | grep | sort | uniq)" "$out" "$exp"

# 4. Five-command pipeline
out=$($TERMINATOR -c "printf 'z\na\nb\nc\nd\ne\n' | sort | head -4 | tail -2 | tr 'a-z' 'A-Z'")
exp=$(printf 'C\nD')
assert_eq "Five-command pipeline (5 stages)" "$out" "$exp"

# 5. Pipeline with input redirection
printf "banana\napple\ncherry\n" > tests/pipe_in.tmp
out=$($TERMINATOR -c "sort < tests/pipe_in.tmp | tr 'a-z' 'A-Z'")
exp=$(printf 'APPLE\nBANANA\nCHERRY')
assert_eq "Pipeline with input redirection (< in | cmd)" "$out" "$exp"

# 6. Pipeline with output redirection
$TERMINATOR -c "printf 'banana\napple\n' | sort > tests/pipe_out.tmp"
out=$(cat tests/pipe_out.tmp)
exp=$(printf 'apple\nbanana')
assert_eq "Pipeline with output redirection (cmd | cmd > out)" "$out" "$exp"

# 7. Pipeline with both input and output redirection
$TERMINATOR -c "sort < tests/pipe_in.tmp | head -2 > tests/pipe_res.tmp"
out=$(cat tests/pipe_res.tmp)
exp=$(printf 'apple\nbanana')
assert_eq "Pipeline with both input and output redirection (< in | cmd > out)" "$out" "$exp"

# 8. Deadlock prevention with large stream (1 MB through pipeline)
out=$($TERMINATOR -c "head -c 1000000 /dev/zero | wc -c")
assert_eq "Deadlock prevention test (1MB stream)" "$out" "1000000"

# 9. Pipeline status reflects final command
set +e
$TERMINATOR -c "true | false" >/dev/null 2>&1
status_false=$?
$TERMINATOR -c "false | true" >/dev/null 2>&1
status_true=$?
set -e
if [ $status_false -ne 0 ] && [ $status_true -eq 0 ]; then
    echo "  [PASS] Pipeline exit status reflects final command status"
    PASS=$((PASS + 1))
else
    echo "  [FAIL] Pipeline status propagation (false|true => $status_true, true|false => $status_false)"
    FAIL=$((FAIL + 1))
fi

echo "=== Task 5 Result: $PASS Passed, $FAIL Failed ==="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
