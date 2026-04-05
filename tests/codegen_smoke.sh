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

cat > "$TMP_DIR/control_flow_codegen.c" <<'EOF'
int main(void) {
    int value = 1;
top:
    do {
        switch (value + 2 - 2) {
            case 0:
                value = value + 10;
                break;
            case 1:
                goto done;
            default:
                value = value + 100;
        }
        value++;
    } while (value < 3);
done:
    return value;
}
EOF

cat > "$TMP_DIR/shadow_codegen.c" <<'EOF'
int main(int value) {
    int x = value;
    {
        int x = value + 1;
        return x;
    }
}
EOF

expect_success "$ROOT_DIR/examples/feature_showcase.c"
expect_output_contains "func mix(seed: ulong, scale: double) -> ulong"
expect_output_contains "jump for.cond."
expect_output_contains "call mix(7, 0.25)"
expect_output_contains "cast int"

expect_success "$TMP_DIR/loop_codegen.c"
expect_output_contains "while.cond."
expect_output_contains "if.then."
expect_output_contains "jump while.cond."

expect_success "$TMP_DIR/control_flow_codegen.c"
expect_output_contains "label.main.top:"
expect_output_contains "do.body."
expect_output_contains "switch.dispatch."
expect_output_contains "switch.case."
expect_output_contains "switch.default."
expect_output_contains "jump label.main.done"
expect_output_contains "cjump t"

expect_success "$TMP_DIR/shadow_codegen.c"
expect_output_contains "func main(value: int) -> int"
expect_output_contains "var x : int"
expect_output_contains "var x.0 : int"
expect_output_contains "ret x.0"
