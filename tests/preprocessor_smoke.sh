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

    if ! "$BINARY" preprocess "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected preprocess success for $input_file" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

expect_failure() {
    input_file=$1

    if "$BINARY" preprocess "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected preprocess failure for $input_file" >&2
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

expect_output_not_contains() {
    needle=$1

    if grep -Fq "$needle" "$LAST_OUTPUT"; then
        echo "expected output not to contain: $needle" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

cat > "$TMP_DIR/defs.h" <<'EOF'
#define VALUE 41
#define ADD_ONE(x) ((x) + 1)
EOF

cat > "$TMP_DIR/preprocess_ok.c" <<'EOF'
#include "defs.h"
#ifndef FLAG
int value = VALUE;
int value2 = ADD_ONE(7);
#endif
EOF

cat > "$TMP_DIR/preprocess_angle.c" <<'EOF'
#include <defs.h>
#if defined VALUE
int value3 = VALUE;
#endif
EOF

cat > "$TMP_DIR/preprocess_missing_include.c" <<'EOF'
#include "missing.h"
int value = 0;
EOF

expect_success "$TMP_DIR/preprocess_ok.c"
expect_output_contains "int value = 41;"
expect_output_contains "int value2 = ((7) + 1);"
expect_output_not_contains "#define"

expect_success "$TMP_DIR/preprocess_angle.c"
expect_output_contains "int value3 = 41;"

expect_failure "$TMP_DIR/preprocess_missing_include.c"
expect_output_contains "unable to open include 'missing.h'"
