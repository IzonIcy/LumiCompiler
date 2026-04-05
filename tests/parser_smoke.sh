#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BINARY="$ROOT_DIR/build/bin/C-Compiler"
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

cat > "$TMP_DIR/typedef_scope_error.c" <<'EOF'
int main(void) {
    typedef int T;
    return 0;
}

T value;
EOF

cat > "$TMP_DIR/typedef_name_as_local_identifier.c" <<'EOF'
typedef int T;

int main(void) {
    int T = 0;
    return T;
}
EOF

cat > "$TMP_DIR/control_flow_parse.c" <<'EOF'
int main(void) {
entry:
    do {
        switch (1) {
            case 0:
                break;
            default:
                goto entry;
        }
    } while (0);
    return 0;
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

expect_parse_success "$TMP_DIR/control_flow_parse.c"
expect_output_contains "do_while_statement"
expect_output_contains "switch_statement"
expect_output_contains "case_statement"
expect_output_contains "default_statement"
expect_output_contains "goto_statement: entry"
expect_output_contains "label_statement: entry"

expect_parse_success "$TMP_DIR/typedef_name_as_local_identifier.c"
expect_output_contains "declarator: T"
expect_output_contains "return_statement"

expect_parse_failure "$TMP_DIR/parser_error.c"
expect_output_contains "expected ')' after if condition"

expect_parse_failure "$TMP_DIR/typedef_scope_error.c"
expect_output_contains "expected declaration specifier"
