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

cat > "$TMP_DIR/switch_and_goto_ok.c" <<'EOF'
int main(void) {
    int value = 1;
start:
    do {
        switch (value) {
            case 0:
                value = value + 1;
                break;
            case 1:
                goto done;
            default:
                value = value + 2;
        }
    } while (value < 3);
done:
    return value;
}
EOF

cat > "$TMP_DIR/case_outside_switch.c" <<'EOF'
int main(void) {
    case 1:
        return 0;
}
EOF

cat > "$TMP_DIR/default_outside_switch.c" <<'EOF'
int main(void) {
    default:
        return 0;
}
EOF

cat > "$TMP_DIR/undefined_label.c" <<'EOF'
int main(void) {
    goto missing;
}
EOF

cat > "$TMP_DIR/duplicate_case.c" <<'EOF'
int main(void) {
    switch (1) {
        case 1:
            return 0;
        case 1:
            return 1;
        default:
            return 2;
    }
}
EOF

cat > "$TMP_DIR/duplicate_default.c" <<'EOF'
int main(void) {
    switch (1) {
        default:
            return 0;
        default:
            return 1;
    }
}
EOF

cat > "$TMP_DIR/for_scope_leak.c" <<'EOF'
int main(void) {
    for (int i = 0; i < 1; ++i) {
    }
    return i;
}
EOF

cat > "$TMP_DIR/for_scope_shadow_ok.c" <<'EOF'
int main(void) {
    int i = 7;
    for (int i = 0; i < 1; ++i) {
    }
    return i;
}
EOF

cat > "$TMP_DIR/parameter_redeclaration.c" <<'EOF'
int f(int x) {
    int x = 1;
    return x;
}
EOF

cat > "$TMP_DIR/typedef_name_as_local_identifier.c" <<'EOF'
typedef int T;

int main(void) {
    int T = 0;
    return T;
}
EOF

cat > "$TMP_DIR/incompatible_pointer_assignment.c" <<'EOF'
int main(void) {
    int *ip;
    double *dp;
    ip = dp;
    return 0;
}
EOF

cat > "$TMP_DIR/incompatible_pointer_argument.c" <<'EOF'
int takes_int_ptr(int *p) {
    return *p;
}

int main(void) {
    double value = 0.0;
    double *dp = &value;
    return takes_int_ptr(dp);
}
EOF

cat > "$TMP_DIR/function_to_int_cast.c" <<'EOF'
int f(void) {
    return 1;
}

int main(void) {
    return (int)f;
}
EOF

cat > "$TMP_DIR/void_pointer_roundtrip.c" <<'EOF'
int main(void) {
    int value = 7;
    int *ip = &value;
    void *vp = ip;
    ip = vp;
    return *ip;
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
expect_output_contains "'break' is only valid inside a loop or switch"

expect_failure "$TMP_DIR/missing_return.c"
expect_output_contains "may exit without returning a value"

expect_failure "$TMP_DIR/wrong_arg_count.c"
expect_output_contains "function call expected 2 arguments but received 1"

expect_success "$TMP_DIR/switch_and_goto_ok.c"
expect_output_contains "semantic analysis succeeded"

expect_failure "$TMP_DIR/case_outside_switch.c"
expect_output_contains "'case' is only valid inside a switch"

expect_failure "$TMP_DIR/default_outside_switch.c"
expect_output_contains "'default' is only valid inside a switch"

expect_failure "$TMP_DIR/undefined_label.c"
expect_output_contains "goto references undefined label 'missing'"

expect_failure "$TMP_DIR/duplicate_case.c"
expect_output_contains "duplicate case value 1"

expect_failure "$TMP_DIR/duplicate_default.c"
expect_output_contains "duplicate default label"

expect_failure "$TMP_DIR/for_scope_leak.c"
expect_output_contains "use of undeclared identifier 'i'"

expect_success "$TMP_DIR/for_scope_shadow_ok.c"
expect_output_contains "semantic analysis succeeded"

expect_failure "$TMP_DIR/parameter_redeclaration.c"
expect_output_contains "redeclaration of 'x' in the same scope"

expect_success "$TMP_DIR/typedef_name_as_local_identifier.c"
expect_output_contains "semantic analysis succeeded"

expect_failure "$TMP_DIR/incompatible_pointer_assignment.c"
expect_output_contains "cannot assign value of type pointer to pointer"

expect_failure "$TMP_DIR/incompatible_pointer_argument.c"
expect_output_contains "argument 1 has incompatible type"

expect_failure "$TMP_DIR/function_to_int_cast.c"
expect_output_contains "invalid cast from function to int"

expect_success "$TMP_DIR/void_pointer_roundtrip.c"
expect_output_contains "semantic analysis succeeded"
