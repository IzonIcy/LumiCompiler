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

    if ! "$BINARY" lex "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected success for $input_file" >&2
        cat "$LAST_OUTPUT" >&2
        exit 1
    fi
}

expect_failure() {
    input_file=$1

    if "$BINARY" lex "$input_file" >"$LAST_OUTPUT" 2>&1; then
        echo "expected failure for $input_file" >&2
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

cat > "$TMP_DIR/valid_hex_int.c" <<'EOF'
int value = 0x1e3;
EOF

cat > "$TMP_DIR/valid_float_from_zero.c" <<'EOF'
double value = 09e1;
EOF

cat > "$TMP_DIR/prefixed_literals.c" <<'EOF'
int main(void) {
    wchar_t a = L'a';
    char16_t b = u'Z';
    char32_t c = U"wide";
    return 0;
}
EOF

cat > "$TMP_DIR/comment_trivia.c" <<'EOF'
int answer = 41; // stripped_identifier
/* hidden_name */ int next = answer + 1;
EOF

cat > "$TMP_DIR/digraphs.c" <<'EOF'
<%
int values<:2:> = <% 1, 2 %>;
%:define JOIN(a, b) a %:%: b
%>
EOF

cat > "$TMP_DIR/invalid_hex_float.c" <<'EOF'
double value = 0x1.;
EOF

cat > "$TMP_DIR/unterminated_escape.c" <<'EOF'
const char *message = "abc\
EOF

cat > "$TMP_DIR/malformed_suffixes.c" <<'EOF'
int a = 123abc;
int b = 0b10foo;
double c = 0x1p;
EOF

expect_success "$ROOT_DIR/examples/feature_showcase.c"
expect_output_contains "float                    0x1.fp3"

expect_success "$TMP_DIR/valid_hex_int.c"
expect_output_contains "integer                  0x1e3"

expect_success "$TMP_DIR/valid_float_from_zero.c"
expect_output_contains "float                    09e1"

expect_success "$TMP_DIR/prefixed_literals.c"
expect_output_contains "char                     L'a'"
expect_output_contains "char                     u'Z'"
expect_output_contains "string                   U\"wide\""

expect_success "$TMP_DIR/comment_trivia.c"
expect_output_contains "identifier               answer"
expect_output_contains "identifier               next"
expect_output_not_contains "stripped_identifier"
expect_output_not_contains "hidden_name"

expect_success "$TMP_DIR/digraphs.c"
expect_output_contains "lbrace                   <%"
expect_output_contains "lbracket                 <:"
expect_output_contains "rbracket                 :>"
expect_output_contains "rbrace                   %>"
expect_output_contains "hash                     %:"
expect_output_contains "hash_hash                %:%:"

expect_failure "$ROOT_DIR/examples/lexer_errors.c"
expect_output_contains "unterminated string literal"
expect_output_contains "unterminated block comment"

expect_failure "$TMP_DIR/invalid_hex_float.c"
expect_output_contains "hexadecimal floating literal requires a 'p' exponent"

expect_failure "$TMP_DIR/unterminated_escape.c"
expect_output_contains "unterminated string literal"

expect_failure "$TMP_DIR/malformed_suffixes.c"
expect_output_contains "invalid suffix on numeric literal"
expect_output_contains "hexadecimal floating literal requires digits after the exponent"
