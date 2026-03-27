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

    if ! "$BINARY" codegen "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected codegen success for $input_file" >&2
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

cat > "$TMP_DIR/loop_codegen.c" <<'EOF'
int main(void) {
    int x = 0;
    while (x < 3) {
        if (x) {
            x++;
            continue;
        }
        ++x;
    }
    return x;
}
EOF

expect_success "$ROOT_DIR/examples/feature_showcase.c"
expect_output_contains "func mix(seed: ulong, scale: double) -> ulong"
expect_output_contains "jump for.cond."
expect_output_contains "call mix(7u, 0.25)"
expect_output_contains "cast int"

expect_success "$TMP_DIR/loop_codegen.c"
expect_output_contains "while.cond."
expect_output_contains "if.then."
expect_output_contains "jump while.cond."
