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

assert_eq() {
    local desc="$1"
    local actual="$2"
    local expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        echo "    Expected: '$expected'"
        echo "    Got:      '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Running Task 3 Tests: Multiline Input, Quoting & Substitution ==="

# 1. Quoted spaces preserved
out=$($TERMINATOR -c 'echo "hello    world"')
assert_eq "Quoted spaces preserved" "$out" "hello    world"

# 2. Single quotes are literal
out=$($TERMINATOR -c "echo 'literal: \$HOME'")
assert_eq "Single quotes prevent variable expansion" "$out" 'literal: $HOME'

# 3. Double quotes expand variables
out=$($TERMINATOR -c 'echo "expanded: $HOME"')
assert_eq "Double quotes expand variables" "$out" "expanded: $HOME"

# 4. Mixed quotes in one command line
out=$($TERMINATOR -c 'echo '\''literal: $HOME'\'' "expanded: $HOME"')
assert_eq "Mixed single and double quotes" "$out" "literal: \$HOME expanded: $HOME"

# 5. Multiline command via trailing backslash
out=$(printf 'echo one \\\ntwo\nexit\n' | $TERMINATOR --test 2>&1 | grep -F "one two" | head -1)
assert_eq "Trailing backslash line continuation" "$out" "one two"

# 6. Multiline command via open double quote
out=$(printf 'echo "hello\nworld"\nexit\n' | $TERMINATOR --test 2>&1 | grep -A1 -F "hello" | head -2)
exp=$(printf 'hello\nworld')
assert_eq "Multiline double quotes spanning lines" "$out" "$exp"

# 7. Command substitution $(cmd)
out=$($TERMINATOR -c 'echo "Current user: $(whoami)"')
assert_eq "Command substitution \$(whoami)" "$out" "Current user: $(whoami)"

# 8. Nested command substitution $(basename $(pwd))
out=$($TERMINATOR -c 'echo "Dir: $(basename $(pwd))"')
assert_eq "Nested substitution \$(basename \$(pwd))" "$out" "Dir: $(basename $(pwd))"

# 9. Pipeline inside command substitution
SRC_DIR="$SCRIPT_DIR/../src"
out=$($TERMINATOR -c "echo \"Count: \$(ls '$SRC_DIR' | wc -l)\"")
exp_count=$(ls "$SRC_DIR" | wc -l)
assert_eq "Pipeline inside command substitution \$(ls | wc -l)" "$out" "Count: $exp_count"

# 10. Backticks command substitution
out=$($TERMINATOR -c 'echo "Year: `date +%Y`"')
assert_eq "Backtick command substitution \`date +%Y\`" "$out" "Year: $(date +%Y)"

# 11. Globbing expansion (ls *.sh)
out=$($TERMINATOR -c "ls '$SCRIPT_DIR'/*.sh | sort")
exp=$(ls "$SCRIPT_DIR"/*.sh | sort)
assert_eq "Globbing expansion *.sh" "$out" "$exp"

# 12. Escaped characters inside double quotes
out=$($TERMINATOR -c 'echo "dollar: \$HOME escaped quote: \"hi\""')
assert_eq "Escaped dollar and quote inside double quotes" "$out" 'dollar: $HOME escaped quote: "hi"'

# 13. Empty quoted strings preserved as distinct arguments
out=$($TERMINATOR -c 'python3 -c "import sys; print(len(sys.argv), repr(sys.argv[1]), repr(sys.argv[2]))" "" "bar"')
assert_eq "Empty double quotes preserved as empty argument" "$out" "3 '' 'bar'"

out=$($TERMINATOR -c "python3 -c 'import sys; print(len(sys.argv), repr(sys.argv[1]))' ''")
assert_eq "Empty single quotes preserved as empty argument" "$out" "2 ''"

echo "=== Task 3 Result: $PASS Passed, $FAIL Failed ==="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
