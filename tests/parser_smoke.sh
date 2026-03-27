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

expect_parse_success() {
    input_file=$1

    if ! "$BINARY" parse "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected parse success for $input_file" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

expect_parse_failure() {
    input_file=$1

    if "$BINARY" parse "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected parse failure for $input_file" >&2
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

cat > "$TMP_DIR/typedef_casts.c" <<'EOF'
typedef unsigned long ulong;

int main(void) {
    ulong value = 1;
    return (int)value;
}
EOF

cat > "$TMP_DIR/parser_error.c" <<'EOF'
int main(void) {
    if (1 {
        return 0;
    }
}
EOF

expect_parse_success "$ROOT_DIR/examples/feature_showcase.c"
expect_output_contains "translation_unit"
expect_output_contains "preprocessor_line: #define GLUE(a, b) a ## b"
expect_output_contains "function_definition: mix"
expect_output_contains "function_definition: main"
expect_output_contains "for_statement"
expect_output_contains "if_statement"
expect_output_contains "conditional_expression"
expect_output_contains "cast_expression"

expect_parse_success "$TMP_DIR/typedef_casts.c"
expect_output_contains "function_definition: main"
expect_output_contains "cast_expression"

expect_parse_failure "$TMP_DIR/parser_error.c"
expect_output_contains "expected ')' after if condition"
