#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BINARY="$ROOT_DIR/build/bin/ccompiler"
TMP_DIR=$(mktemp -d)
LAST_OUTPUT="$TMP_DIR/last-output.txt"

cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

expect_success() {
    input_file=$1

    if ! "$BINARY" check "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected semantic success for $input_file" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

expect_failure() {
    input_file=$1

    if "$BINARY" check "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected semantic failure for $input_file" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

expect_output_contains() {
    needle=$1

    if ! grep -Fq "$needle" "$LAST_OUTPUT"; then
        echo "expected output to contain: $needle" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

cat > "$TMP_DIR/undeclared_value.c" <<'EOF'
int main(void) {
    return missing + 1;
}
EOF

cat > "$TMP_DIR/break_outside_loop.c" <<'EOF'
int main(void) {
    break;
}
EOF

cat > "$TMP_DIR/missing_return.c" <<'EOF'
int maybe(int value) {
    if (value) {
        return value;
    }
}
EOF

cat > "$TMP_DIR/wrong_arg_count.c" <<'EOF'
int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(1);
}
EOF

expect_success "$ROOT_DIR/examples/feature_showcase.c"
expect_output_contains "semantic analysis succeeded"
expect_output_contains "functions: 2"
expect_output_contains "globals: 0"
expect_output_contains "typedefs: 1"

expect_failure "$TMP_DIR/undeclared_value.c"
expect_output_contains "use of undeclared identifier 'missing'"

expect_failure "$TMP_DIR/break_outside_loop.c"
expect_output_contains "'break' is only valid inside a loop"

expect_failure "$TMP_DIR/missing_return.c"
expect_output_contains "may exit without returning a value"

expect_failure "$TMP_DIR/wrong_arg_count.c"
expect_output_contains "function call expected 2 arguments but received 1"
