#include "ccompiler/lexer.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
    const char *text;
    size_t length;
    CCTokenKind kind;
} CCKeywordEntry;

typedef struct {
    const char *text;
    size_t length;
    CCTokenKind kind;
} CCPunctuatorEntry;

typedef struct {
    CCSourceView source;
    size_t offset;
    size_t line;
    size_t column;
    CCTokenBuffer tokens;
    CCDiagnosticBuffer diagnostics;
} CCLexer;

static const CCKeywordEntry cc_keyword_table[] = {
    {"auto", 4, CC_TOKEN_KW_AUTO},
    {"break", 5, CC_TOKEN_KW_BREAK},
    {"case", 4, CC_TOKEN_KW_CASE},
    {"char", 4, CC_TOKEN_KW_CHAR},
    {"const", 5, CC_TOKEN_KW_CONST},
    {"continue", 8, CC_TOKEN_KW_CONTINUE},
    {"default", 7, CC_TOKEN_KW_DEFAULT},
    {"do", 2, CC_TOKEN_KW_DO},
    {"double", 6, CC_TOKEN_KW_DOUBLE},
    {"else", 4, CC_TOKEN_KW_ELSE},
    {"enum", 4, CC_TOKEN_KW_ENUM},
    {"extern", 6, CC_TOKEN_KW_EXTERN},
    {"float", 5, CC_TOKEN_KW_FLOAT},
    {"for", 3, CC_TOKEN_KW_FOR},
    {"goto", 4, CC_TOKEN_KW_GOTO},
    {"if", 2, CC_TOKEN_KW_IF},
    {"inline", 6, CC_TOKEN_KW_INLINE},
    {"int", 3, CC_TOKEN_KW_INT},
    {"long", 4, CC_TOKEN_KW_LONG},
    {"register", 8, CC_TOKEN_KW_REGISTER},
    {"restrict", 8, CC_TOKEN_KW_RESTRICT},
    {"return", 6, CC_TOKEN_KW_RETURN},
    {"short", 5, CC_TOKEN_KW_SHORT},
    {"signed", 6, CC_TOKEN_KW_SIGNED},
    {"sizeof", 6, CC_TOKEN_KW_SIZEOF},
    {"static", 6, CC_TOKEN_KW_STATIC},
    {"struct", 6, CC_TOKEN_KW_STRUCT},
    {"switch", 6, CC_TOKEN_KW_SWITCH},
    {"typedef", 7, CC_TOKEN_KW_TYPEDEF},
    {"union", 5, CC_TOKEN_KW_UNION},
    {"unsigned", 8, CC_TOKEN_KW_UNSIGNED},
    {"void", 4, CC_TOKEN_KW_VOID},
    {"volatile", 8, CC_TOKEN_KW_VOLATILE},
    {"while", 5, CC_TOKEN_KW_WHILE},
    {"_Alignas", 8, CC_TOKEN_KW_ALIGNAS},
    {"_Alignof", 8, CC_TOKEN_KW_ALIGNOF},
    {"_Atomic", 7, CC_TOKEN_KW_ATOMIC},
    {"_Bool", 5, CC_TOKEN_KW_BOOL},
    {"_Complex", 8, CC_TOKEN_KW_COMPLEX},
    {"_Generic", 8, CC_TOKEN_KW_GENERIC},
    {"_Imaginary", 10, CC_TOKEN_KW_IMAGINARY},
    {"_Noreturn", 9, CC_TOKEN_KW_NORETURN},
    {"_Static_assert", 14, CC_TOKEN_KW_STATIC_ASSERT},
    {"_Thread_local", 13, CC_TOKEN_KW_THREAD_LOCAL},
};

/*
 * Ordered from longest to shortest so the lexer naturally takes the longest
 * valid punctuator at the current position.
 */
static const CCPunctuatorEntry cc_punctuator_table[] = {
    {"%:%:", 4, CC_TOKEN_HASH_HASH},
    {"<<=", 3, CC_TOKEN_LEFT_SHIFT_ASSIGN},
    {">>=", 3, CC_TOKEN_RIGHT_SHIFT_ASSIGN},
    {"...", 3, CC_TOKEN_ELLIPSIS},
    {"##", 2, CC_TOKEN_HASH_HASH},
    {"->", 2, CC_TOKEN_ARROW},
    {"++", 2, CC_TOKEN_INCREMENT},
    {"--", 2, CC_TOKEN_DECREMENT},
    {"+=", 2, CC_TOKEN_PLUS_ASSIGN},
    {"-=", 2, CC_TOKEN_MINUS_ASSIGN},
    {"*=", 2, CC_TOKEN_STAR_ASSIGN},
    {"/=", 2, CC_TOKEN_SLASH_ASSIGN},
    {"%=", 2, CC_TOKEN_PERCENT_ASSIGN},
    {"&=", 2, CC_TOKEN_AMPERSAND_ASSIGN},
    {"|=", 2, CC_TOKEN_PIPE_ASSIGN},
    {"^=", 2, CC_TOKEN_CARET_ASSIGN},
    {"<<", 2, CC_TOKEN_LEFT_SHIFT},
    {">>", 2, CC_TOKEN_RIGHT_SHIFT},
    {"&&", 2, CC_TOKEN_LOGICAL_AND},
    {"||", 2, CC_TOKEN_LOGICAL_OR},
    {"==", 2, CC_TOKEN_EQUAL},
    {"!=", 2, CC_TOKEN_NOT_EQUAL},
    {"<=", 2, CC_TOKEN_LESS_EQUAL},
    {">=", 2, CC_TOKEN_GREATER_EQUAL},
    {"<:", 2, CC_TOKEN_LBRACKET},
    {":>", 2, CC_TOKEN_RBRACKET},
    {"<%", 2, CC_TOKEN_LBRACE},
    {"%>", 2, CC_TOKEN_RBRACE},
    {"%:", 2, CC_TOKEN_HASH},
    {"(", 1, CC_TOKEN_LPAREN},
    {")", 1, CC_TOKEN_RPAREN},
    {"{", 1, CC_TOKEN_LBRACE},
    {"}", 1, CC_TOKEN_RBRACE},
    {"[", 1, CC_TOKEN_LBRACKET},
    {"]", 1, CC_TOKEN_RBRACKET},
    {",", 1, CC_TOKEN_COMMA},
    {";", 1, CC_TOKEN_SEMICOLON},
    {":", 1, CC_TOKEN_COLON},
    {".", 1, CC_TOKEN_DOT},
    {"?", 1, CC_TOKEN_QUESTION},
    {"#", 1, CC_TOKEN_HASH},
    {"+", 1, CC_TOKEN_PLUS},
    {"-", 1, CC_TOKEN_MINUS},
    {"*", 1, CC_TOKEN_STAR},
    {"/", 1, CC_TOKEN_SLASH},
    {"%", 1, CC_TOKEN_PERCENT},
    {"&", 1, CC_TOKEN_AMPERSAND},
    {"|", 1, CC_TOKEN_PIPE},
    {"^", 1, CC_TOKEN_CARET},
    {"~", 1, CC_TOKEN_TILDE},
    {"!", 1, CC_TOKEN_BANG},
    {"=", 1, CC_TOKEN_ASSIGN},
    {"<", 1, CC_TOKEN_LESS},
    {">", 1, CC_TOKEN_GREATER},
};

static void *cc_reallocate_or_die(void *memory, size_t size) {
    void *result;

    if (size == 0) {
        size = 1;
    }

    result = realloc(memory, size);
    if (result == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(EXIT_FAILURE);
    }

    return result;
}

static char *cc_duplicate_string(const char *text) {
    size_t length;
    char *copy;

    length = strlen(text) + 1;
    copy = cc_reallocate_or_die(NULL, length);
    memcpy(copy, text, length);
    return copy;
}

static void cc_grow_token_buffer(CCTokenBuffer *buffer) {
    size_t new_capacity;

    new_capacity = buffer->capacity == 0 ? 64 : buffer->capacity * 2;
    buffer->items = cc_reallocate_or_die(buffer->items, new_capacity * sizeof(*buffer->items));
    buffer->capacity = new_capacity;
}

static void cc_grow_diagnostic_buffer(CCDiagnosticBuffer *buffer) {
    size_t new_capacity;

    new_capacity = buffer->capacity == 0 ? 16 : buffer->capacity * 2;
    buffer->items = cc_reallocate_or_die(buffer->items, new_capacity * sizeof(*buffer->items));
    buffer->capacity = new_capacity;
}

static bool cc_at_end(const CCLexer *lexer) {
    return lexer->offset >= lexer->source.length;
}

static char cc_peek(const CCLexer *lexer, size_t lookahead) {
    size_t index;

    index = lexer->offset + lookahead;
    if (index >= lexer->source.length) {
        return '\0';
    }

    return lexer->source.text[index];
}

static CCSpan cc_begin_span(const CCLexer *lexer) {
    CCSpan span;

    span.offset = lexer->offset;
    span.line = lexer->line;
    span.column = lexer->column;
    span.length = 0;
    return span;
}

static CCSpan cc_span_from_start(const CCLexer *lexer, CCSpan start) {
    start.length = lexer->offset - start.offset;
    return start;
}

static char cc_advance(CCLexer *lexer) {
    char ch;

    if (cc_at_end(lexer)) {
        return '\0';
    }

    ch = lexer->source.text[lexer->offset++];

    /*
     * Treat CRLF as one newline so location tracking stays sane on files that
     * originated on Windows.
     */
    if (ch == '\r') {
        if (!cc_at_end(lexer) && lexer->source.text[lexer->offset] == '\n') {
            lexer->offset++;
        }

        lexer->line++;
        lexer->column = 1;
        return '\n';
    }

    if (ch == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }

    return ch;
}

static void cc_add_token(CCLexer *lexer, CCTokenKind kind, CCSpan start) {
    if (lexer->tokens.count == lexer->tokens.capacity) {
        cc_grow_token_buffer(&lexer->tokens);
    }

    lexer->tokens.items[lexer->tokens.count].kind = kind;
    lexer->tokens.items[lexer->tokens.count].span = cc_span_from_start(lexer, start);
    lexer->tokens.count++;
}

static void cc_add_diagnostic(CCLexer *lexer, CCSpan span, const char *format, ...) {
    char small_buffer[256];
    char *message;
    int needed;
    va_list args;
    va_list copy;

    if (lexer->diagnostics.count == lexer->diagnostics.capacity) {
        cc_grow_diagnostic_buffer(&lexer->diagnostics);
    }

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(small_buffer, sizeof(small_buffer), format, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        message = cc_duplicate_string("diagnostic formatting failure");
    } else if ((size_t)needed < sizeof(small_buffer)) {
        va_end(copy);
        message = cc_duplicate_string(small_buffer);
    } else {
        size_t length;

        length = (size_t)needed + 1;
        message = cc_reallocate_or_die(NULL, length);
        vsnprintf(message, length, format, copy);
        va_end(copy);
    }

    if (span.length == 0 && span.offset < lexer->source.length) {
        span.length = 1;
    }

    lexer->diagnostics.items[lexer->diagnostics.count].span = span;
    lexer->diagnostics.items[lexer->diagnostics.count].path = NULL;
    lexer->diagnostics.items[lexer->diagnostics.count].message = message;
    lexer->diagnostics.items[lexer->diagnostics.count].severity = CC_DIAGNOSTIC_ERROR;
    lexer->diagnostics.count++;
}

static void cc_report_error(CCLexer *lexer, CCSpan start, const char *message) {
    cc_add_diagnostic(lexer, cc_span_from_start(lexer, start), "%s", message);
}

static bool cc_is_identifier_start(char ch) {
    return isalpha((unsigned char)ch) != 0 || ch == '_';
}

static bool cc_is_identifier_part(char ch) {
    return isalnum((unsigned char)ch) != 0 || ch == '_';
}

static bool cc_is_binary_digit(char ch) {
    return ch == '0' || ch == '1';
}

static bool cc_is_octal_digit(char ch) {
    return ch >= '0' && ch <= '7';
}

static bool cc_is_hex_digit(char ch) {
    return isxdigit((unsigned char)ch) != 0;
}

static size_t cc_consume_decimal_digits(CCLexer *lexer) {
    size_t count;

    count = 0;
    while (isdigit((unsigned char)cc_peek(lexer, 0)) != 0) {
        cc_advance(lexer);
        count++;
    }

    return count;
}

static size_t cc_consume_hex_digits(CCLexer *lexer) {
    size_t count;

    count = 0;
    while (cc_is_hex_digit(cc_peek(lexer, 0))) {
        cc_advance(lexer);
        count++;
    }

    return count;
}

static size_t cc_consume_binary_digits(CCLexer *lexer) {
    size_t count;

    count = 0;
    while (cc_is_binary_digit(cc_peek(lexer, 0))) {
        cc_advance(lexer);
        count++;
    }

    return count;
}

static void cc_consume_integer_suffix(CCLexer *lexer) {
    while (cc_peek(lexer, 0) == 'u' || cc_peek(lexer, 0) == 'U' || cc_peek(lexer, 0) == 'l' || cc_peek(lexer, 0) == 'L') {
        cc_advance(lexer);
    }
}

static void cc_consume_float_suffix(CCLexer *lexer) {
    if (cc_peek(lexer, 0) == 'f' || cc_peek(lexer, 0) == 'F' || cc_peek(lexer, 0) == 'l' || cc_peek(lexer, 0) == 'L') {
        cc_advance(lexer);
    }
}

static CCTokenKind cc_lookup_keyword(const char *text, size_t length) {
    size_t index;

    for (index = 0; index < CC_ARRAY_COUNT(cc_keyword_table); index++) {
        const CCKeywordEntry *entry;

        entry = &cc_keyword_table[index];
        if (entry->length == length && memcmp(entry->text, text, length) == 0) {
            return entry->kind;
        }
    }

    return CC_TOKEN_IDENTIFIER;
}

static const CCPunctuatorEntry *cc_match_punctuator(const CCLexer *lexer) {
    size_t index;

    for (index = 0; index < CC_ARRAY_COUNT(cc_punctuator_table); index++) {
        const CCPunctuatorEntry *entry;

        entry = &cc_punctuator_table[index];
        if (lexer->offset + entry->length > lexer->source.length) {
            continue;
        }

        if (memcmp(lexer->source.text + lexer->offset, entry->text, entry->length) == 0) {
            return entry;
        }
    }

    return NULL;
}

static void cc_skip_line_comment(CCLexer *lexer) {
    cc_advance(lexer);
    cc_advance(lexer);

    while (!cc_at_end(lexer) && cc_peek(lexer, 0) != '\n' && cc_peek(lexer, 0) != '\r') {
        cc_advance(lexer);
    }
}

static void cc_skip_block_comment(CCLexer *lexer) {
    CCSpan start;

    start = cc_begin_span(lexer);
    cc_advance(lexer);
    cc_advance(lexer);

    while (!cc_at_end(lexer)) {
        if (cc_peek(lexer, 0) == '*' && cc_peek(lexer, 1) == '/') {
            cc_advance(lexer);
            cc_advance(lexer);
            return;
        }

        cc_advance(lexer);
    }

    cc_report_error(lexer, start, "unterminated block comment");
}

static void cc_skip_trivia(CCLexer *lexer) {
    for (;;) {
        char ch;

        ch = cc_peek(lexer, 0);

        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') {
            cc_advance(lexer);
            continue;
        }

        if (ch == '/' && cc_peek(lexer, 1) == '/') {
            cc_skip_line_comment(lexer);
            continue;
        }

        if (ch == '/' && cc_peek(lexer, 1) == '*') {
            cc_skip_block_comment(lexer);
            continue;
        }

        return;
    }
}

static void cc_lex_identifier(CCLexer *lexer) {
    CCSpan start;
    CCTokenKind kind;

    start = cc_begin_span(lexer);
    cc_advance(lexer);
    while (cc_is_identifier_part(cc_peek(lexer, 0))) {
        cc_advance(lexer);
    }

    kind = cc_lookup_keyword(lexer->source.text + start.offset, lexer->offset - start.offset);
    cc_add_token(lexer, kind, start);
}

static void cc_consume_invalid_number_tail(CCLexer *lexer, CCSpan start, const char *message) {
    while (cc_is_identifier_part(cc_peek(lexer, 0))) {
        cc_advance(lexer);
    }

    cc_report_error(lexer, start, message);
}

static void cc_finish_number(CCLexer *lexer, CCSpan start, bool is_float, bool invalid_octal) {
    if (is_float) {
        cc_consume_float_suffix(lexer);
    } else {
        if (invalid_octal) {
            cc_report_error(lexer, start, "octal literal contains digits outside the range 0-7");
        }
        cc_consume_integer_suffix(lexer);
    }

    if (isdigit((unsigned char)cc_peek(lexer, 0)) != 0 || cc_is_identifier_start(cc_peek(lexer, 0))) {
        cc_consume_invalid_number_tail(lexer, start, "invalid suffix on numeric literal");
    }
}

static void cc_lex_hex_literal(CCLexer *lexer, CCSpan start) {
    bool is_float;
    bool saw_whole_digits;
    bool saw_fraction_digits;
    bool saw_exponent;

    is_float = false;
    saw_fraction_digits = false;
    saw_exponent = false;

    cc_advance(lexer);
    cc_advance(lexer);

    saw_whole_digits = cc_consume_hex_digits(lexer) > 0;

    if (cc_peek(lexer, 0) == '.') {
        is_float = true;
        cc_advance(lexer);
        saw_fraction_digits = cc_consume_hex_digits(lexer) > 0;
    }

    if (!saw_whole_digits && !saw_fraction_digits) {
        cc_report_error(lexer, start, "hexadecimal literal requires at least one hex digit");
    }

    if (cc_peek(lexer, 0) == 'p' || cc_peek(lexer, 0) == 'P') {
        is_float = true;
        saw_exponent = true;
        cc_advance(lexer);

        if (cc_peek(lexer, 0) == '+' || cc_peek(lexer, 0) == '-') {
            cc_advance(lexer);
        }

        if (cc_consume_decimal_digits(lexer) == 0) {
            cc_report_error(lexer, start, "hexadecimal floating literal requires digits after the exponent");
        }
    }

    if (is_float && !saw_exponent) {
        cc_report_error(lexer, start, "hexadecimal floating literal requires a 'p' exponent");
    }

    cc_finish_number(lexer, start, is_float, false);
    cc_add_token(lexer, is_float ? CC_TOKEN_FLOAT_LITERAL : CC_TOKEN_INTEGER_LITERAL, start);
}

static void cc_lex_binary_literal(CCLexer *lexer, CCSpan start) {
    size_t digits;

    cc_advance(lexer);
    cc_advance(lexer);

    digits = cc_consume_binary_digits(lexer);
    if (digits == 0) {
        cc_report_error(lexer, start, "binary literal requires at least one binary digit");
    }

    if (isdigit((unsigned char)cc_peek(lexer, 0)) != 0) {
        while (isdigit((unsigned char)cc_peek(lexer, 0)) != 0) {
            cc_advance(lexer);
        }
        cc_report_error(lexer, start, "binary literal contains digits other than 0 and 1");
    }

    cc_finish_number(lexer, start, false, false);
    cc_add_token(lexer, CC_TOKEN_INTEGER_LITERAL, start);
}

static void cc_lex_decimal_literal(CCLexer *lexer, CCSpan start, bool starts_with_dot) {
    bool is_float;
    bool invalid_octal;

    is_float = starts_with_dot;
    invalid_octal = false;

    if (starts_with_dot) {
        cc_advance(lexer);
        cc_consume_decimal_digits(lexer);
    } else {
        while (isdigit((unsigned char)cc_peek(lexer, 0)) != 0) {
            if (lexer->offset > start.offset && lexer->source.text[start.offset] == '0') {
                char digit;

                digit = cc_peek(lexer, 0);
                if (digit == '8' || digit == '9') {
                    invalid_octal = true;
                }
            }

            cc_advance(lexer);
        }

        if (cc_peek(lexer, 0) == '.') {
            is_float = true;
            cc_advance(lexer);
            cc_consume_decimal_digits(lexer);
        }
    }

    if (cc_peek(lexer, 0) == 'e' || cc_peek(lexer, 0) == 'E') {
        is_float = true;
        cc_advance(lexer);

        if (cc_peek(lexer, 0) == '+' || cc_peek(lexer, 0) == '-') {
            cc_advance(lexer);
        }

        if (cc_consume_decimal_digits(lexer) == 0) {
            cc_report_error(lexer, start, "floating literal requires digits after the exponent");
        }
    }

    cc_finish_number(lexer, start, is_float, invalid_octal);
    cc_add_token(lexer, is_float ? CC_TOKEN_FLOAT_LITERAL : CC_TOKEN_INTEGER_LITERAL, start);
}

static void cc_lex_number(CCLexer *lexer, bool starts_with_dot) {
    CCSpan start;

    start = cc_begin_span(lexer);

    if (!starts_with_dot && cc_peek(lexer, 0) == '0') {
        if (cc_peek(lexer, 1) == 'x' || cc_peek(lexer, 1) == 'X') {
            cc_lex_hex_literal(lexer, start);
            return;
        }

        if (cc_peek(lexer, 1) == 'b' || cc_peek(lexer, 1) == 'B') {
            cc_lex_binary_literal(lexer, start);
            return;
        }
    }

    cc_lex_decimal_literal(lexer, start, starts_with_dot);
}

static void cc_consume_unicode_escape(CCLexer *lexer, CCSpan start, size_t digits, const char *label) {
    size_t count;

    count = 0;
    while (count < digits && cc_is_hex_digit(cc_peek(lexer, 0))) {
        cc_advance(lexer);
        count++;
    }

    if (count != digits) {
        cc_add_diagnostic(lexer, cc_span_from_start(lexer, start), "%s escape requires exactly %zu hexadecimal digits", label, digits);
    }
}

static void cc_lex_escape_sequence(CCLexer *lexer) {
    CCSpan start;
    char ch;

    start = cc_begin_span(lexer);
    ch = cc_peek(lexer, 0);

    if (ch == '\0') {
        cc_add_diagnostic(lexer, start, "unfinished escape sequence at end of file");
        return;
    }

    if (ch == '\n' || ch == '\r') {
        cc_advance(lexer);
        return;
    }

    switch (ch) {
        case '\'':
        case '"':
        case '?':
        case '\\':
        case 'a':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
        case 'v':
            cc_advance(lexer);
            return;
        case 'x':
            cc_advance(lexer);
            if (cc_consume_hex_digits(lexer) == 0) {
                cc_report_error(lexer, start, "\\x escape requires at least one hexadecimal digit");
            }
            return;
        case 'u':
            cc_advance(lexer);
            cc_consume_unicode_escape(lexer, start, 4, "\\u");
            return;
        case 'U':
            cc_advance(lexer);
            cc_consume_unicode_escape(lexer, start, 8, "\\U");
            return;
        default:
            if (cc_is_octal_digit(ch)) {
                size_t count;

                count = 0;
                while (count < 3 && cc_is_octal_digit(cc_peek(lexer, 0))) {
                    cc_advance(lexer);
                    count++;
                }
                return;
            }

            cc_advance(lexer);
            cc_report_error(lexer, start, "unknown escape sequence");
            return;
    }
}

static void cc_lex_quoted_literal(CCLexer *lexer, size_t prefix_length, bool is_char) {
    CCSpan start;
    char terminator;
    bool closed;

    start = cc_begin_span(lexer);
    terminator = is_char ? '\'' : '"';
    closed = false;

    while (prefix_length > 0) {
        cc_advance(lexer);
        prefix_length--;
    }

    cc_advance(lexer);

    while (!cc_at_end(lexer)) {
        char ch;

        ch = cc_peek(lexer, 0);

        if (ch == terminator) {
            cc_advance(lexer);
            closed = true;
            break;
        }

        if (ch == '\\') {
            cc_advance(lexer);
            cc_lex_escape_sequence(lexer);
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            cc_add_diagnostic(
                lexer,
                cc_span_from_start(lexer, start),
                "unterminated %s literal",
                is_char ? "character" : "string"
            );
            break;
        }

        cc_advance(lexer);
    }

    if (!closed && cc_at_end(lexer)) {
        cc_add_diagnostic(
            lexer,
            cc_span_from_start(lexer, start),
            "unterminated %s literal",
            is_char ? "character" : "string"
        );
    }

    cc_add_token(lexer, is_char ? CC_TOKEN_CHAR_LITERAL : CC_TOKEN_STRING_LITERAL, start);
}

static bool cc_is_prefixed_literal(const CCLexer *lexer, size_t *prefix_length, bool *is_char) {
    char first;
    char second;
    char third;

    first = cc_peek(lexer, 0);
    second = cc_peek(lexer, 1);
    third = cc_peek(lexer, 2);

    if (first == 'u' && second == '8' && third == '"') {
        *prefix_length = 2;
        *is_char = false;
        return true;
    }

    if ((first == 'L' || first == 'u' || first == 'U') && (second == '"' || second == '\'')) {
        *prefix_length = 1;
        *is_char = second == '\'';
        return true;
    }

    return false;
}

static void cc_lex_invalid_character(CCLexer *lexer) {
    CCSpan start;
    unsigned char byte;

    start = cc_begin_span(lexer);
    byte = (unsigned char)cc_peek(lexer, 0);
    cc_advance(lexer);

    if (isprint(byte) != 0) {
        cc_add_diagnostic(lexer, cc_span_from_start(lexer, start), "unexpected character '%c'", byte);
    } else {
        cc_add_diagnostic(lexer, cc_span_from_start(lexer, start), "unexpected byte 0x%02X", byte);
    }

    cc_add_token(lexer, CC_TOKEN_INVALID, start);
}

void cc_lex_source(const CCSourceView *source, CCLexResult *result) {
    CCLexer lexer;

    memset(&lexer, 0, sizeof(lexer));
    lexer.source = *source;
    lexer.line = 1;
    lexer.column = 1;

    while (!cc_at_end(&lexer)) {
        const CCPunctuatorEntry *punctuator;
        size_t prefix_length;
        bool is_char;
        char ch;

        cc_skip_trivia(&lexer);
        if (cc_at_end(&lexer)) {
            break;
        }

        prefix_length = 0;
        is_char = false;
        ch = cc_peek(&lexer, 0);

        if (cc_is_prefixed_literal(&lexer, &prefix_length, &is_char)) {
            cc_lex_quoted_literal(&lexer, prefix_length, is_char);
            continue;
        }

        if (ch == '"') {
            cc_lex_quoted_literal(&lexer, 0, false);
            continue;
        }

        if (ch == '\'') {
            cc_lex_quoted_literal(&lexer, 0, true);
            continue;
        }

        if (cc_is_identifier_start(ch)) {
            cc_lex_identifier(&lexer);
            continue;
        }

        if (isdigit((unsigned char)ch) != 0) {
            cc_lex_number(&lexer, false);
            continue;
        }

        if (ch == '.' && isdigit((unsigned char)cc_peek(&lexer, 1)) != 0) {
            cc_lex_number(&lexer, true);
            continue;
        }

        punctuator = cc_match_punctuator(&lexer);
        if (punctuator != NULL) {
            CCSpan start;
            size_t index;

            start = cc_begin_span(&lexer);
            for (index = 0; index < punctuator->length; index++) {
                cc_advance(&lexer);
            }
            cc_add_token(&lexer, punctuator->kind, start);
            continue;
        }

        cc_lex_invalid_character(&lexer);
    }

    cc_add_token(&lexer, CC_TOKEN_EOF, cc_begin_span(&lexer));

    memset(result, 0, sizeof(*result));
    result->source = lexer.source;
    result->tokens = lexer.tokens;
    result->diagnostics = lexer.diagnostics;
}

void cc_lex_result_free(CCLexResult *result) {
    size_t index;

    free(result->tokens.items);
    result->tokens.items = NULL;
    result->tokens.count = 0;
    result->tokens.capacity = 0;

    for (index = 0; index < result->diagnostics.count; index++) {
        free(result->diagnostics.items[index].path);
        free(result->diagnostics.items[index].message);
    }

    free(result->diagnostics.items);
    result->diagnostics.items = NULL;
    result->diagnostics.count = 0;
    result->diagnostics.capacity = 0;
}
