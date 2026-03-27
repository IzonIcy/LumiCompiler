#include "ccompiler/parser.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
    CCAstNode *node;
    bool is_typedef;
    bool has_any;
} CCParsedSpecifiers;

typedef struct {
    CCAstNode *node;
    bool is_function;
} CCParsedDeclarator;

typedef struct {
    const CCLexResult *lex_result;
    size_t index;
    CCDiagnosticBuffer diagnostics;
    char **typedef_names;
    size_t typedef_count;
    size_t typedef_capacity;
} CCParser;

static const char *const cc_ast_kind_names[] = {
    [CC_AST_TRANSLATION_UNIT] = "translation_unit",
    [CC_AST_PREPROCESSOR_LINE] = "preprocessor_line",
    [CC_AST_FUNCTION_DEFINITION] = "function_definition",
    [CC_AST_DECLARATION] = "declaration",
    [CC_AST_DECLARATION_SPECIFIERS] = "declaration_specifiers",
    [CC_AST_SPECIFIER] = "specifier",
    [CC_AST_INIT_DECLARATOR] = "init_declarator",
    [CC_AST_DECLARATOR] = "declarator",
    [CC_AST_POINTER] = "pointer",
    [CC_AST_ARRAY_DECLARATOR] = "array_declarator",
    [CC_AST_PARAMETER_LIST] = "parameter_list",
    [CC_AST_PARAMETER] = "parameter",
    [CC_AST_TYPE_NAME] = "type_name",
    [CC_AST_COMPOUND_STATEMENT] = "compound_statement",
    [CC_AST_IF_STATEMENT] = "if_statement",
    [CC_AST_FOR_STATEMENT] = "for_statement",
    [CC_AST_WHILE_STATEMENT] = "while_statement",
    [CC_AST_RETURN_STATEMENT] = "return_statement",
    [CC_AST_BREAK_STATEMENT] = "break_statement",
    [CC_AST_CONTINUE_STATEMENT] = "continue_statement",
    [CC_AST_EXPRESSION_STATEMENT] = "expression_statement",
    [CC_AST_EMPTY_STATEMENT] = "empty_statement",
    [CC_AST_IDENTIFIER] = "identifier",
    [CC_AST_LITERAL] = "literal",
    [CC_AST_UNARY_EXPRESSION] = "unary_expression",
    [CC_AST_BINARY_EXPRESSION] = "binary_expression",
    [CC_AST_CONDITIONAL_EXPRESSION] = "conditional_expression",
    [CC_AST_CALL_EXPRESSION] = "call_expression",
    [CC_AST_SUBSCRIPT_EXPRESSION] = "subscript_expression",
    [CC_AST_MEMBER_EXPRESSION] = "member_expression",
    [CC_AST_CAST_EXPRESSION] = "cast_expression",
    [CC_AST_ARGUMENT_LIST] = "argument_list",
};

static const char *cc_ast_safe_kind_name(CCAstKind kind) {
    if ((size_t)kind >= CC_ARRAY_COUNT(cc_ast_kind_names) || cc_ast_kind_names[kind] == NULL) {
        return "<unknown_ast_kind>";
    }

    return cc_ast_kind_names[kind];
}

const char *cc_ast_kind_name(CCAstKind kind) {
    return cc_ast_safe_kind_name(kind);
}

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

static char *cc_duplicate_source_range(const CCSourceView *source, size_t start, size_t end) {
    char *copy;
    size_t length;

    if (start > source->length) {
        start = source->length;
    }
    if (end > source->length) {
        end = source->length;
    }
    if (end < start) {
        end = start;
    }

    length = end - start;
    copy = cc_reallocate_or_die(NULL, length + 1);
    memcpy(copy, source->text + start, length);
    copy[length] = '\0';
    return copy;
}

static size_t cc_span_end_offset(CCSpan span) {
    return span.offset + span.length;
}

static CCSpan cc_merge_spans(CCSpan first, CCSpan second) {
    CCSpan merged;
    size_t end_offset;

    merged = first;
    end_offset = cc_span_end_offset(second);
    if (end_offset < first.offset) {
        end_offset = first.offset;
    }

    merged.length = end_offset - first.offset;
    return merged;
}

static void cc_grow_diagnostic_buffer(CCDiagnosticBuffer *buffer) {
    size_t new_capacity;

    new_capacity = buffer->capacity == 0 ? 8 : buffer->capacity * 2;
    buffer->items = cc_reallocate_or_die(buffer->items, new_capacity * sizeof(*buffer->items));
    buffer->capacity = new_capacity;
}

static const CCToken *cc_current_token(const CCParser *parser) {
    return &parser->lex_result->tokens.items[parser->index];
}

static const CCToken *cc_previous_token(const CCParser *parser) {
    if (parser->index == 0) {
        return NULL;
    }

    return &parser->lex_result->tokens.items[parser->index - 1];
}

static const CCToken *cc_peek_token(const CCParser *parser, size_t lookahead) {
    size_t index;

    index = parser->index + lookahead;
    if (index >= parser->lex_result->tokens.count) {
        return &parser->lex_result->tokens.items[parser->lex_result->tokens.count - 1];
    }

    return &parser->lex_result->tokens.items[index];
}

static bool cc_at(const CCParser *parser, CCTokenKind kind) {
    return cc_current_token(parser)->kind == kind;
}

static CCToken cc_advance(CCParser *parser) {
    CCToken token;

    token = *cc_current_token(parser);
    if (token.kind != CC_TOKEN_EOF) {
        parser->index++;
    }

    return token;
}

static bool cc_match(CCParser *parser, CCTokenKind kind) {
    if (!cc_at(parser, kind)) {
        return false;
    }

    cc_advance(parser);
    return true;
}

static CCSpan cc_current_span(const CCParser *parser) {
    return cc_current_token(parser)->span;
}

static CCSpan cc_previous_span(const CCParser *parser) {
    const CCToken *token;

    token = cc_previous_token(parser);
    if (token == NULL) {
        return cc_current_span(parser);
    }

    return token->span;
}

static CCSpan cc_span_from_start_to_previous(const CCParser *parser, CCSpan start) {
    return cc_merge_spans(start, cc_previous_span(parser));
}

static void cc_add_diagnostic(CCParser *parser, CCSpan span, const char *format, ...) {
    char small_buffer[256];
    char *message;
    int needed;
    va_list args;
    va_list copy;

    if (parser->diagnostics.count == parser->diagnostics.capacity) {
        cc_grow_diagnostic_buffer(&parser->diagnostics);
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

    if (span.length == 0 && span.offset < parser->lex_result->source.length) {
        span.length = 1;
    }

    parser->diagnostics.items[parser->diagnostics.count].span = span;
    parser->diagnostics.items[parser->diagnostics.count].path = NULL;
    parser->diagnostics.items[parser->diagnostics.count].message = message;
    parser->diagnostics.count++;
}

static void cc_expected(CCParser *parser, const char *message) {
    cc_add_diagnostic(parser, cc_current_span(parser), "%s", message);
}

static CCAstNode *cc_new_ast_node(CCAstKind kind, CCSpan span, const char *text) {
    CCAstNode *node;

    node = cc_reallocate_or_die(NULL, sizeof(*node));
    node->kind = kind;
    node->span = span;
    node->text = text == NULL ? NULL : cc_duplicate_string(text);
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    return node;
}

static CCAstNode *cc_new_ast_node_with_owned_text(CCAstKind kind, CCSpan span, char *text) {
    CCAstNode *node;

    node = cc_reallocate_or_die(NULL, sizeof(*node));
    node->kind = kind;
    node->span = span;
    node->text = text;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    return node;
}

static void cc_ast_add_child(CCAstNode *node, CCAstNode *child) {
    size_t new_capacity;

    if (child == NULL) {
        return;
    }

    if (node->child_count == node->child_capacity) {
        new_capacity = node->child_capacity == 0 ? 4 : node->child_capacity * 2;
        node->children = cc_reallocate_or_die(node->children, new_capacity * sizeof(*node->children));
        node->child_capacity = new_capacity;
    }

    node->children[node->child_count++] = child;
}

static char *cc_token_text(const CCParser *parser, const CCToken *token) {
    return cc_duplicate_source_range(
        &parser->lex_result->source,
        token->span.offset,
        cc_span_end_offset(token->span)
    );
}

static CCAstNode *cc_new_token_node(CCParser *parser, CCAstKind kind, const CCToken *token) {
    return cc_new_ast_node_with_owned_text(kind, token->span, cc_token_text(parser, token));
}

static void cc_free_ast(CCAstNode *node) {
    size_t index;

    if (node == NULL) {
        return;
    }

    for (index = 0; index < node->child_count; index++) {
        cc_free_ast(node->children[index]);
    }

    free(node->children);
    free(node->text);
    free(node);
}

static void cc_grow_typedef_table(CCParser *parser) {
    size_t new_capacity;

    new_capacity = parser->typedef_capacity == 0 ? 8 : parser->typedef_capacity * 2;
    parser->typedef_names = cc_reallocate_or_die(parser->typedef_names, new_capacity * sizeof(*parser->typedef_names));
    parser->typedef_capacity = new_capacity;
}

static bool cc_typedef_name_exists(const CCParser *parser, const char *name) {
    size_t index;

    for (index = 0; index < parser->typedef_count; index++) {
        if (strcmp(parser->typedef_names[index], name) == 0) {
            return true;
        }
    }

    return false;
}

static void cc_add_typedef_name(CCParser *parser, const char *name) {
    if (name == NULL || name[0] == '\0' || cc_typedef_name_exists(parser, name)) {
        return;
    }

    if (parser->typedef_count == parser->typedef_capacity) {
        cc_grow_typedef_table(parser);
    }

    parser->typedef_names[parser->typedef_count++] = cc_duplicate_string(name);
}

static void cc_seed_typedef_table(CCParser *parser) {
    static const char *const builtin_names[] = {
        "size_t",
        "ptrdiff_t",
        "wchar_t",
        "char16_t",
        "char32_t",
        "max_align_t",
    };
    size_t index;

    for (index = 0; index < CC_ARRAY_COUNT(builtin_names); index++) {
        cc_add_typedef_name(parser, builtin_names[index]);
    }
}

static void cc_free_typedef_table(CCParser *parser) {
    size_t index;

    for (index = 0; index < parser->typedef_count; index++) {
        free(parser->typedef_names[index]);
    }

    free(parser->typedef_names);
    parser->typedef_names = NULL;
    parser->typedef_count = 0;
    parser->typedef_capacity = 0;
}

static bool cc_token_is_typedef_name(const CCParser *parser, size_t lookahead) {
    const CCToken *token;
    char *name;
    bool found;

    token = cc_peek_token(parser, lookahead);
    if (token->kind != CC_TOKEN_IDENTIFIER) {
        return false;
    }

    name = cc_token_text(parser, token);
    found = cc_typedef_name_exists(parser, name);
    free(name);
    return found;
}

static bool cc_is_storage_class_specifier(CCTokenKind kind) {
    switch (kind) {
        case CC_TOKEN_KW_TYPEDEF:
        case CC_TOKEN_KW_EXTERN:
        case CC_TOKEN_KW_STATIC:
        case CC_TOKEN_KW_AUTO:
        case CC_TOKEN_KW_REGISTER:
            return true;
        default:
            return false;
    }
}

static bool cc_is_type_qualifier(CCTokenKind kind) {
    switch (kind) {
        case CC_TOKEN_KW_CONST:
        case CC_TOKEN_KW_RESTRICT:
        case CC_TOKEN_KW_VOLATILE:
        case CC_TOKEN_KW_ATOMIC:
            return true;
        default:
            return false;
    }
}

static bool cc_is_function_specifier(CCTokenKind kind) {
    return kind == CC_TOKEN_KW_INLINE || kind == CC_TOKEN_KW_NORETURN;
}

static bool cc_is_type_specifier_keyword(CCTokenKind kind) {
    switch (kind) {
        case CC_TOKEN_KW_VOID:
        case CC_TOKEN_KW_CHAR:
        case CC_TOKEN_KW_SHORT:
        case CC_TOKEN_KW_INT:
        case CC_TOKEN_KW_LONG:
        case CC_TOKEN_KW_FLOAT:
        case CC_TOKEN_KW_DOUBLE:
        case CC_TOKEN_KW_SIGNED:
        case CC_TOKEN_KW_UNSIGNED:
        case CC_TOKEN_KW_BOOL:
        case CC_TOKEN_KW_COMPLEX:
        case CC_TOKEN_KW_STRUCT:
        case CC_TOKEN_KW_UNION:
        case CC_TOKEN_KW_ENUM:
            return true;
        default:
            return false;
    }
}

static bool cc_token_begins_declaration_specifier(const CCParser *parser, size_t lookahead) {
    CCTokenKind kind;

    kind = cc_peek_token(parser, lookahead)->kind;
    return cc_is_storage_class_specifier(kind)
        || cc_is_type_qualifier(kind)
        || cc_is_function_specifier(kind)
        || cc_is_type_specifier_keyword(kind)
        || cc_token_is_typedef_name(parser, lookahead);
}

static bool cc_token_begins_type_name(const CCParser *parser, size_t lookahead) {
    CCTokenKind kind;

    kind = cc_peek_token(parser, lookahead)->kind;
    return cc_is_type_qualifier(kind)
        || cc_is_type_specifier_keyword(kind)
        || cc_token_is_typedef_name(parser, lookahead);
}

static bool cc_is_literal_token(CCTokenKind kind) {
    switch (kind) {
        case CC_TOKEN_INTEGER_LITERAL:
        case CC_TOKEN_FLOAT_LITERAL:
        case CC_TOKEN_STRING_LITERAL:
        case CC_TOKEN_CHAR_LITERAL:
            return true;
        default:
            return false;
    }
}

static bool cc_is_assignment_operator(CCTokenKind kind) {
    switch (kind) {
        case CC_TOKEN_ASSIGN:
        case CC_TOKEN_PLUS_ASSIGN:
        case CC_TOKEN_MINUS_ASSIGN:
        case CC_TOKEN_STAR_ASSIGN:
        case CC_TOKEN_SLASH_ASSIGN:
        case CC_TOKEN_PERCENT_ASSIGN:
        case CC_TOKEN_LEFT_SHIFT_ASSIGN:
        case CC_TOKEN_RIGHT_SHIFT_ASSIGN:
        case CC_TOKEN_AMPERSAND_ASSIGN:
        case CC_TOKEN_CARET_ASSIGN:
        case CC_TOKEN_PIPE_ASSIGN:
            return true;
        default:
            return false;
    }
}

static bool cc_expect(CCParser *parser, CCTokenKind kind, const char *message) {
    if (cc_match(parser, kind)) {
        return true;
    }

    cc_expected(parser, message);
    return false;
}

static void cc_synchronize_to_semicolon_or_brace(CCParser *parser) {
    while (!cc_at(parser, CC_TOKEN_EOF) && !cc_at(parser, CC_TOKEN_SEMICOLON) && !cc_at(parser, CC_TOKEN_RBRACE)) {
        cc_advance(parser);
    }

    if (cc_at(parser, CC_TOKEN_SEMICOLON)) {
        cc_advance(parser);
    }
}

static CCAstNode *cc_parse_expression(CCParser *parser);
static CCAstNode *cc_parse_assignment_expression(CCParser *parser);
static CCAstNode *cc_parse_statement(CCParser *parser);
static CCAstNode *cc_parse_declaration(CCParser *parser);
static CCParsedDeclarator cc_parse_declarator(CCParser *parser, bool allow_abstract);

static CCAstNode *cc_parse_struct_union_enum_specifier(CCParser *parser) {
    CCSpan start;
    char *text;
    size_t brace_depth;

    start = cc_current_span(parser);
    cc_advance(parser);

    if (cc_at(parser, CC_TOKEN_IDENTIFIER)) {
        cc_advance(parser);
    }

    if (cc_match(parser, CC_TOKEN_LBRACE)) {
        brace_depth = 1;
        while (!cc_at(parser, CC_TOKEN_EOF) && brace_depth > 0) {
            if (cc_match(parser, CC_TOKEN_LBRACE)) {
                brace_depth++;
            } else if (cc_match(parser, CC_TOKEN_RBRACE)) {
                brace_depth--;
            } else {
                cc_advance(parser);
            }
        }

        if (brace_depth > 0) {
            cc_add_diagnostic(parser, start, "unterminated %s specifier", cc_token_kind_name(cc_peek_token(parser, 0)->kind));
        }
    }

    text = cc_duplicate_source_range(
        &parser->lex_result->source,
        start.offset,
        cc_span_end_offset(cc_previous_span(parser))
    );
    return cc_new_ast_node_with_owned_text(CC_AST_SPECIFIER, cc_span_from_start_to_previous(parser, start), text);
}

static CCParsedSpecifiers cc_parse_declaration_specifiers(CCParser *parser, bool require_specifier) {
    CCParsedSpecifiers parsed;
    CCSpan start;

    parsed.node = NULL;
    parsed.is_typedef = false;
    parsed.has_any = false;

    start = cc_current_span(parser);
    parsed.node = cc_new_ast_node(CC_AST_DECLARATION_SPECIFIERS, start, NULL);

    while (cc_token_begins_declaration_specifier(parser, 0)) {
        const CCToken *token;
        CCAstNode *child;

        parsed.has_any = true;
        token = cc_current_token(parser);

        if (token->kind == CC_TOKEN_KW_TYPEDEF) {
            parsed.is_typedef = true;
        }

        if (token->kind == CC_TOKEN_KW_STRUCT || token->kind == CC_TOKEN_KW_UNION || token->kind == CC_TOKEN_KW_ENUM) {
            child = cc_parse_struct_union_enum_specifier(parser);
        } else {
            child = cc_new_token_node(parser, CC_AST_SPECIFIER, token);
            cc_advance(parser);
        }

        cc_ast_add_child(parsed.node, child);
    }

    if (!parsed.has_any && require_specifier) {
        cc_expected(parser, "expected declaration specifier");
    }

    parsed.node->span = parsed.has_any ? cc_span_from_start_to_previous(parser, start) : start;
    return parsed;
}

static CCAstNode *cc_parse_parameter_declaration(CCParser *parser) {
    CCParsedSpecifiers specifiers;
    CCParsedDeclarator declarator;
    CCAstNode *parameter;
    CCSpan start;

    start = cc_current_span(parser);
    specifiers = cc_parse_declaration_specifiers(parser, true);
    if (!specifiers.has_any) {
        cc_free_ast(specifiers.node);
        return NULL;
    }

    parameter = cc_new_ast_node(CC_AST_PARAMETER, start, NULL);
    cc_ast_add_child(parameter, specifiers.node);

    declarator = cc_parse_declarator(parser, true);
    if (declarator.node != NULL) {
        cc_ast_add_child(parameter, declarator.node);
    }

    parameter->span = cc_span_from_start_to_previous(parser, start);
    return parameter;
}

static CCAstNode *cc_parse_parameter_list(CCParser *parser) {
    CCAstNode *parameter_list;
    CCSpan start;

    start = cc_current_span(parser);
    parameter_list = cc_new_ast_node(CC_AST_PARAMETER_LIST, start, NULL);

    cc_expect(parser, CC_TOKEN_LPAREN, "expected '(' to start parameter list");

    if (!cc_at(parser, CC_TOKEN_RPAREN)) {
        for (;;) {
            CCAstNode *parameter;

            if (cc_match(parser, CC_TOKEN_ELLIPSIS)) {
                cc_ast_add_child(parameter_list, cc_new_ast_node(CC_AST_PARAMETER, cc_previous_span(parser), "..."));
                break;
            }

            parameter = cc_parse_parameter_declaration(parser);
            if (parameter == NULL) {
                cc_synchronize_to_semicolon_or_brace(parser);
                break;
            }

            cc_ast_add_child(parameter_list, parameter);

            if (!cc_match(parser, CC_TOKEN_COMMA)) {
                break;
            }
        }
    }

    cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after parameter list");
    parameter_list->span = cc_span_from_start_to_previous(parser, start);
    return parameter_list;
}

static CCAstNode *cc_parse_array_suffix(CCParser *parser) {
    CCAstNode *node;
    CCAstNode *size_expression;
    CCSpan start;

    start = cc_current_span(parser);
    node = cc_new_ast_node(CC_AST_ARRAY_DECLARATOR, start, NULL);
    cc_expect(parser, CC_TOKEN_LBRACKET, "expected '[' to start array declarator");

    if (!cc_at(parser, CC_TOKEN_RBRACKET)) {
        size_expression = cc_parse_expression(parser);
        cc_ast_add_child(node, size_expression);
    }

    cc_expect(parser, CC_TOKEN_RBRACKET, "expected ']' after array declarator");
    node->span = cc_span_from_start_to_previous(parser, start);
    return node;
}

static void cc_parse_pointer_suffixes(CCParser *parser, CCAstNode *declarator) {
    while (cc_match(parser, CC_TOKEN_STAR)) {
        CCAstNode *pointer;
        CCSpan start;

        start = cc_previous_span(parser);
        pointer = cc_new_ast_node(CC_AST_POINTER, start, "*");

        while (cc_is_type_qualifier(cc_current_token(parser)->kind)) {
            cc_ast_add_child(pointer, cc_new_token_node(parser, CC_AST_SPECIFIER, cc_current_token(parser)));
            cc_advance(parser);
        }

        pointer->span = cc_span_from_start_to_previous(parser, start);
        cc_ast_add_child(declarator, pointer);
    }
}

static CCParsedDeclarator cc_parse_declarator(CCParser *parser, bool allow_abstract) {
    CCParsedDeclarator parsed;
    CCSpan start;
    bool saw_direct_part;

    parsed.node = NULL;
    parsed.is_function = false;

    start = cc_current_span(parser);
    parsed.node = cc_new_ast_node(CC_AST_DECLARATOR, start, NULL);

    cc_parse_pointer_suffixes(parser, parsed.node);
    saw_direct_part = false;

    if (cc_at(parser, CC_TOKEN_IDENTIFIER)) {
        const CCToken *identifier;

        identifier = cc_current_token(parser);
        parsed.node->text = cc_token_text(parser, identifier);
        cc_advance(parser);
        saw_direct_part = true;
    } else if (cc_match(parser, CC_TOKEN_LPAREN)) {
        CCParsedDeclarator nested;

        nested = cc_parse_declarator(parser, allow_abstract);
        cc_ast_add_child(parsed.node, nested.node);
        parsed.is_function = nested.is_function;

        if (nested.node != NULL && nested.node->text != NULL && parsed.node->text == NULL) {
            parsed.node->text = cc_duplicate_string(nested.node->text);
        }

        cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after parenthesized declarator");
        saw_direct_part = true;
    } else if (!allow_abstract) {
        cc_expected(parser, "expected identifier in declarator");
        cc_free_ast(parsed.node);
        parsed.node = NULL;
        return parsed;
    }

    while (cc_at(parser, CC_TOKEN_LPAREN) || cc_at(parser, CC_TOKEN_LBRACKET)) {
        if (cc_at(parser, CC_TOKEN_LPAREN)) {
            parsed.is_function = true;
            cc_ast_add_child(parsed.node, cc_parse_parameter_list(parser));
        } else {
            cc_ast_add_child(parsed.node, cc_parse_array_suffix(parser));
        }
    }

    if (!saw_direct_part && parsed.node->child_count == 0 && !allow_abstract) {
        cc_expected(parser, "expected identifier in declarator");
        cc_free_ast(parsed.node);
        parsed.node = NULL;
        return parsed;
    }

    parsed.node->span = cc_span_from_start_to_previous(parser, start);
    return parsed;
}

static CCAstNode *cc_parse_type_name(CCParser *parser) {
    CCParsedSpecifiers specifiers;
    CCParsedDeclarator declarator;
    CCAstNode *type_name;
    CCSpan start;

    start = cc_current_span(parser);
    specifiers = cc_parse_declaration_specifiers(parser, true);
    if (!specifiers.has_any) {
        cc_free_ast(specifiers.node);
        return NULL;
    }

    type_name = cc_new_ast_node(CC_AST_TYPE_NAME, start, NULL);
    cc_ast_add_child(type_name, specifiers.node);

    declarator = cc_parse_declarator(parser, true);
    if (declarator.node != NULL && (declarator.node->child_count > 0 || declarator.node->text != NULL)) {
        cc_ast_add_child(type_name, declarator.node);
    } else {
        cc_free_ast(declarator.node);
    }

    type_name->span = cc_span_from_start_to_previous(parser, start);
    return type_name;
}

static CCAstNode *cc_finish_init_declarator(CCParser *parser, CCAstNode *declarator) {
    CCAstNode *node;
    CCSpan start;

    if (declarator == NULL) {
        return NULL;
    }

    start = declarator->span;
    node = cc_new_ast_node(CC_AST_INIT_DECLARATOR, start, NULL);
    cc_ast_add_child(node, declarator);

    if (cc_match(parser, CC_TOKEN_ASSIGN)) {
        CCAstNode *initializer;

        initializer = cc_parse_assignment_expression(parser);
        if (initializer == NULL) {
            cc_expected(parser, "expected initializer expression");
        } else {
            cc_ast_add_child(node, initializer);
        }
    }

    node->span = cc_merge_spans(start, cc_previous_span(parser));
    return node;
}

static void cc_record_typedef_names(CCParser *parser, const CCAstNode *declaration) {
    size_t index;

    for (index = 0; index < declaration->child_count; index++) {
        const CCAstNode *child;

        child = declaration->children[index];
        if (child->kind != CC_AST_INIT_DECLARATOR || child->child_count == 0) {
            continue;
        }

        if (child->children[0]->text != NULL) {
            cc_add_typedef_name(parser, child->children[0]->text);
        }
    }
}

static CCAstNode *cc_parse_declaration_from_parts(
    CCParser *parser,
    CCParsedSpecifiers specifiers,
    CCAstNode *first_declarator
) {
    CCAstNode *declaration;
    CCSpan start;

    start = specifiers.node->span;
    declaration = cc_new_ast_node(CC_AST_DECLARATION, start, NULL);
    cc_ast_add_child(declaration, specifiers.node);

    if (first_declarator != NULL) {
        cc_ast_add_child(declaration, cc_finish_init_declarator(parser, first_declarator));
    }

    while (cc_match(parser, CC_TOKEN_COMMA)) {
        CCParsedDeclarator declarator;

        declarator = cc_parse_declarator(parser, false);
        if (declarator.node == NULL) {
            break;
        }

        cc_ast_add_child(declaration, cc_finish_init_declarator(parser, declarator.node));
    }

    cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after declaration");
    declaration->span = cc_span_from_start_to_previous(parser, start);

    if (specifiers.is_typedef) {
        cc_record_typedef_names(parser, declaration);
    }

    return declaration;
}

static CCAstNode *cc_parse_compound_statement(CCParser *parser) {
    CCAstNode *compound;
    CCSpan start;

    start = cc_current_span(parser);
    compound = cc_new_ast_node(CC_AST_COMPOUND_STATEMENT, start, NULL);
    cc_expect(parser, CC_TOKEN_LBRACE, "expected '{' to start compound statement");

    while (!cc_at(parser, CC_TOKEN_RBRACE) && !cc_at(parser, CC_TOKEN_EOF)) {
        CCAstNode *child;

        if (cc_token_begins_declaration_specifier(parser, 0)) {
            child = cc_parse_declaration(parser);
        } else {
            child = cc_parse_statement(parser);
        }

        if (child == NULL) {
            cc_synchronize_to_semicolon_or_brace(parser);
            continue;
        }

        cc_ast_add_child(compound, child);
    }

    cc_expect(parser, CC_TOKEN_RBRACE, "expected '}' after compound statement");
    compound->span = cc_span_from_start_to_previous(parser, start);
    return compound;
}

static CCAstNode *cc_make_binary_expression(CCParser *parser, CCAstNode *left, CCToken operator_token, CCAstNode *right) {
    CCAstNode *node;
    char *text;

    text = cc_token_text(parser, &operator_token);
    node = cc_new_ast_node_with_owned_text(CC_AST_BINARY_EXPRESSION, cc_merge_spans(left->span, right->span), text);
    cc_ast_add_child(node, left);
    cc_ast_add_child(node, right);
    return node;
}

static CCAstNode *cc_make_unary_expression(const char *op_text, CCSpan span, CCAstNode *operand) {
    CCAstNode *node;

    node = cc_new_ast_node(CC_AST_UNARY_EXPRESSION, span, op_text);
    cc_ast_add_child(node, operand);
    return node;
}

static CCAstNode *cc_parse_primary_expression(CCParser *parser) {
    const CCToken *token;
    CCAstNode *expression;

    token = cc_current_token(parser);

    if (token->kind == CC_TOKEN_IDENTIFIER) {
        cc_advance(parser);
        return cc_new_token_node(parser, CC_AST_IDENTIFIER, token);
    }

    if (cc_is_literal_token(token->kind)) {
        cc_advance(parser);
        return cc_new_token_node(parser, CC_AST_LITERAL, token);
    }

    if (cc_match(parser, CC_TOKEN_LPAREN)) {
        expression = cc_parse_expression(parser);
        cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after parenthesized expression");
        return expression;
    }

    cc_expected(parser, "expected expression");
    return NULL;
}

static CCAstNode *cc_parse_postfix_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_primary_expression(parser);
    if (expression == NULL) {
        return NULL;
    }

    for (;;) {
        if (cc_match(parser, CC_TOKEN_LBRACKET)) {
            CCAstNode *index_expression;
            CCAstNode *node;
            CCSpan start;

            start = expression->span;
            index_expression = cc_parse_expression(parser);
            cc_expect(parser, CC_TOKEN_RBRACKET, "expected ']' after subscript expression");

            node = cc_new_ast_node(CC_AST_SUBSCRIPT_EXPRESSION, cc_span_from_start_to_previous(parser, start), NULL);
            cc_ast_add_child(node, expression);
            cc_ast_add_child(node, index_expression);
            expression = node;
            continue;
        }

        if (cc_at(parser, CC_TOKEN_LPAREN)) {
            CCAstNode *arguments;
            CCAstNode *call;
            CCSpan start;
            CCSpan lparen_span;

            start = expression->span;
            lparen_span = cc_current_span(parser);
            arguments = cc_new_ast_node(CC_AST_ARGUMENT_LIST, lparen_span, NULL);
            cc_advance(parser);

            if (!cc_at(parser, CC_TOKEN_RPAREN)) {
                for (;;) {
                    CCAstNode *argument;

                    argument = cc_parse_assignment_expression(parser);
                    cc_ast_add_child(arguments, argument);

                    if (!cc_match(parser, CC_TOKEN_COMMA)) {
                        break;
                    }
                }
            }

            cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after argument list");
            arguments->span = cc_span_from_start_to_previous(parser, lparen_span);

            call = cc_new_ast_node(CC_AST_CALL_EXPRESSION, cc_span_from_start_to_previous(parser, start), NULL);
            cc_ast_add_child(call, expression);
            cc_ast_add_child(call, arguments);
            expression = call;
            continue;
        }

        if (cc_match(parser, CC_TOKEN_DOT) || cc_match(parser, CC_TOKEN_ARROW)) {
            CCToken operator_token;
            CCAstNode *member;
            CCSpan start;

            operator_token = *cc_previous_token(parser);
            start = expression->span;

            if (!cc_at(parser, CC_TOKEN_IDENTIFIER)) {
                cc_expected(parser, "expected field name after member operator");
                return expression;
            }

            member = cc_new_ast_node_with_owned_text(
                CC_AST_MEMBER_EXPRESSION,
                start,
                cc_token_text(parser, &operator_token)
            );
            cc_ast_add_child(member, expression);
            cc_ast_add_child(member, cc_new_token_node(parser, CC_AST_IDENTIFIER, cc_current_token(parser)));
            cc_advance(parser);
            member->span = cc_span_from_start_to_previous(parser, start);
            expression = member;
            continue;
        }

        if (cc_match(parser, CC_TOKEN_INCREMENT) || cc_match(parser, CC_TOKEN_DECREMENT)) {
            CCAstNode *node;
            CCToken operator_token;
            char *raw_operator;
            char *postfix_operator;
            size_t length;

            operator_token = *cc_previous_token(parser);
            raw_operator = cc_token_text(parser, &operator_token);
            length = strlen(raw_operator) + strlen("post") + 1;
            postfix_operator = cc_reallocate_or_die(NULL, length);
            snprintf(postfix_operator, length, "post%s", raw_operator);
            free(raw_operator);

            node = cc_new_ast_node_with_owned_text(
                CC_AST_UNARY_EXPRESSION,
                cc_merge_spans(expression->span, operator_token.span),
                postfix_operator
            );
            cc_ast_add_child(node, expression);
            expression = node;
            continue;
        }

        break;
    }

    return expression;
}

static CCAstNode *cc_parse_unary_expression(CCParser *parser) {
    CCToken operator_token;
    CCSpan start;
    CCAstNode *operand;
    CCAstNode *type_name;
    CCAstNode *node;

    if (cc_match(parser, CC_TOKEN_INCREMENT)
        || cc_match(parser, CC_TOKEN_DECREMENT)
        || cc_match(parser, CC_TOKEN_AMPERSAND)
        || cc_match(parser, CC_TOKEN_STAR)
        || cc_match(parser, CC_TOKEN_PLUS)
        || cc_match(parser, CC_TOKEN_MINUS)
        || cc_match(parser, CC_TOKEN_BANG)
        || cc_match(parser, CC_TOKEN_TILDE)) {
        operator_token = *cc_previous_token(parser);
        operand = cc_parse_unary_expression(parser);
        if (operand == NULL) {
            return NULL;
        }

        return cc_make_unary_expression(
            cc_token_kind_name(operator_token.kind),
            cc_merge_spans(operator_token.span, operand->span),
            operand
        );
    }

    if (cc_match(parser, CC_TOKEN_KW_SIZEOF) || cc_match(parser, CC_TOKEN_KW_ALIGNOF)) {
        operator_token = *cc_previous_token(parser);
        start = operator_token.span;

        if (cc_match(parser, CC_TOKEN_LPAREN) && cc_token_begins_type_name(parser, 0)) {
            type_name = cc_parse_type_name(parser);
            cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after type name");
            node = cc_make_unary_expression(
                cc_token_kind_name(operator_token.kind),
                cc_span_from_start_to_previous(parser, start),
                type_name
            );
            return node;
        }

        if (cc_previous_token(parser)->kind == CC_TOKEN_LPAREN) {
            operand = cc_parse_expression(parser);
            cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after expression");
        } else {
            operand = cc_parse_unary_expression(parser);
        }

        if (operand == NULL) {
            return NULL;
        }

        return cc_make_unary_expression(
            cc_token_kind_name(operator_token.kind),
            cc_span_from_start_to_previous(parser, start),
            operand
        );
    }

    return cc_parse_postfix_expression(parser);
}

static CCAstNode *cc_parse_cast_expression(CCParser *parser) {
    if (cc_at(parser, CC_TOKEN_LPAREN) && cc_token_begins_type_name(parser, 1)) {
        CCSpan start;
        CCAstNode *type_name;
        CCAstNode *operand;
        CCAstNode *node;

        start = cc_current_span(parser);
        cc_advance(parser);
        type_name = cc_parse_type_name(parser);
        cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after cast type");
        operand = cc_parse_cast_expression(parser);
        if (type_name == NULL || operand == NULL) {
            return NULL;
        }

        node = cc_new_ast_node(CC_AST_CAST_EXPRESSION, cc_span_from_start_to_previous(parser, start), NULL);
        cc_ast_add_child(node, type_name);
        cc_ast_add_child(node, operand);
        return node;
    }

    return cc_parse_unary_expression(parser);
}

static CCAstNode *cc_parse_multiplicative_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_cast_expression(parser);
    while (cc_at(parser, CC_TOKEN_STAR) || cc_at(parser, CC_TOKEN_SLASH) || cc_at(parser, CC_TOKEN_PERCENT)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_cast_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_additive_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_multiplicative_expression(parser);
    while (cc_at(parser, CC_TOKEN_PLUS) || cc_at(parser, CC_TOKEN_MINUS)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_multiplicative_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_shift_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_additive_expression(parser);
    while (cc_at(parser, CC_TOKEN_LEFT_SHIFT) || cc_at(parser, CC_TOKEN_RIGHT_SHIFT)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_additive_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_relational_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_shift_expression(parser);
    while (cc_at(parser, CC_TOKEN_LESS)
        || cc_at(parser, CC_TOKEN_LESS_EQUAL)
        || cc_at(parser, CC_TOKEN_GREATER)
        || cc_at(parser, CC_TOKEN_GREATER_EQUAL)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_shift_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_equality_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_relational_expression(parser);
    while (cc_at(parser, CC_TOKEN_EQUAL) || cc_at(parser, CC_TOKEN_NOT_EQUAL)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_relational_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_bitwise_and_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_equality_expression(parser);
    while (cc_at(parser, CC_TOKEN_AMPERSAND)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_equality_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_bitwise_xor_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_bitwise_and_expression(parser);
    while (cc_at(parser, CC_TOKEN_CARET)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_bitwise_and_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_bitwise_or_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_bitwise_xor_expression(parser);
    while (cc_at(parser, CC_TOKEN_PIPE)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_bitwise_xor_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_logical_and_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_bitwise_or_expression(parser);
    while (cc_at(parser, CC_TOKEN_LOGICAL_AND)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_bitwise_or_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_logical_or_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_logical_and_expression(parser);
    while (cc_at(parser, CC_TOKEN_LOGICAL_OR)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_logical_and_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_conditional_expression(CCParser *parser) {
    CCAstNode *condition;

    condition = cc_parse_logical_or_expression(parser);
    if (!cc_match(parser, CC_TOKEN_QUESTION)) {
        return condition;
    }

    {
        CCAstNode *node;
        CCAstNode *then_expression;
        CCAstNode *else_expression;
        CCSpan start;

        start = condition->span;
        then_expression = cc_parse_expression(parser);
        cc_expect(parser, CC_TOKEN_COLON, "expected ':' in conditional expression");
        else_expression = cc_parse_conditional_expression(parser);

        node = cc_new_ast_node(CC_AST_CONDITIONAL_EXPRESSION, cc_span_from_start_to_previous(parser, start), NULL);
        cc_ast_add_child(node, condition);
        cc_ast_add_child(node, then_expression);
        cc_ast_add_child(node, else_expression);
        return node;
    }
}

static CCAstNode *cc_parse_assignment_expression(CCParser *parser) {
    CCAstNode *left;

    left = cc_parse_conditional_expression(parser);
    if (left == NULL || !cc_is_assignment_operator(cc_current_token(parser)->kind)) {
        return left;
    }

    {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = cc_advance(parser);
        right = cc_parse_assignment_expression(parser);
        if (right == NULL) {
            return left;
        }

        return cc_make_binary_expression(parser, left, operator_token, right);
    }
}

static CCAstNode *cc_parse_expression(CCParser *parser) {
    CCAstNode *expression;

    expression = cc_parse_assignment_expression(parser);
    while (cc_match(parser, CC_TOKEN_COMMA)) {
        CCToken operator_token;
        CCAstNode *right;

        operator_token = *cc_previous_token(parser);
        right = cc_parse_assignment_expression(parser);
        if (expression == NULL || right == NULL) {
            cc_free_ast(right);
            return expression;
        }

        expression = cc_make_binary_expression(parser, expression, operator_token, right);
    }

    return expression;
}

static CCAstNode *cc_parse_return_statement(CCParser *parser) {
    CCAstNode *node;
    CCSpan start;

    start = cc_current_span(parser);
    node = cc_new_ast_node(CC_AST_RETURN_STATEMENT, start, NULL);
    cc_expect(parser, CC_TOKEN_KW_RETURN, "expected 'return'");

    if (!cc_at(parser, CC_TOKEN_SEMICOLON)) {
        cc_ast_add_child(node, cc_parse_expression(parser));
    }

    cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after return statement");
    node->span = cc_span_from_start_to_previous(parser, start);
    return node;
}

static CCAstNode *cc_parse_if_statement(CCParser *parser) {
    CCAstNode *node;
    CCSpan start;

    start = cc_current_span(parser);
    node = cc_new_ast_node(CC_AST_IF_STATEMENT, start, NULL);
    cc_expect(parser, CC_TOKEN_KW_IF, "expected 'if'");
    cc_expect(parser, CC_TOKEN_LPAREN, "expected '(' after 'if'");
    cc_ast_add_child(node, cc_parse_expression(parser));
    cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after if condition");
    cc_ast_add_child(node, cc_parse_statement(parser));

    if (cc_match(parser, CC_TOKEN_KW_ELSE)) {
        cc_ast_add_child(node, cc_parse_statement(parser));
    }

    node->span = cc_span_from_start_to_previous(parser, start);
    return node;
}

static CCAstNode *cc_parse_while_statement(CCParser *parser) {
    CCAstNode *node;
    CCSpan start;

    start = cc_current_span(parser);
    node = cc_new_ast_node(CC_AST_WHILE_STATEMENT, start, NULL);
    cc_expect(parser, CC_TOKEN_KW_WHILE, "expected 'while'");
    cc_expect(parser, CC_TOKEN_LPAREN, "expected '(' after 'while'");
    cc_ast_add_child(node, cc_parse_expression(parser));
    cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after while condition");
    cc_ast_add_child(node, cc_parse_statement(parser));
    node->span = cc_span_from_start_to_previous(parser, start);
    return node;
}

static CCAstNode *cc_parse_for_statement(CCParser *parser) {
    CCAstNode *node;
    CCSpan start;

    start = cc_current_span(parser);
    node = cc_new_ast_node(CC_AST_FOR_STATEMENT, start, NULL);
    cc_expect(parser, CC_TOKEN_KW_FOR, "expected 'for'");
    cc_expect(parser, CC_TOKEN_LPAREN, "expected '(' after 'for'");

    if (cc_token_begins_declaration_specifier(parser, 0)) {
        cc_ast_add_child(node, cc_parse_declaration(parser));
    } else if (cc_match(parser, CC_TOKEN_SEMICOLON)) {
        cc_ast_add_child(node, cc_new_ast_node(CC_AST_EMPTY_STATEMENT, cc_previous_span(parser), NULL));
    } else {
        CCAstNode *init_statement;

        init_statement = cc_new_ast_node(CC_AST_EXPRESSION_STATEMENT, cc_current_span(parser), NULL);
        cc_ast_add_child(init_statement, cc_parse_expression(parser));
        cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after for-loop initializer");
        init_statement->span = cc_span_from_start_to_previous(parser, init_statement->span);
        cc_ast_add_child(node, init_statement);
    }

    if (!cc_at(parser, CC_TOKEN_SEMICOLON)) {
        cc_ast_add_child(node, cc_parse_expression(parser));
    }
    cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after for-loop condition");

    if (!cc_at(parser, CC_TOKEN_RPAREN)) {
        cc_ast_add_child(node, cc_parse_expression(parser));
    }
    cc_expect(parser, CC_TOKEN_RPAREN, "expected ')' after for-loop clauses");

    cc_ast_add_child(node, cc_parse_statement(parser));
    node->span = cc_span_from_start_to_previous(parser, start);
    return node;
}

static CCAstNode *cc_parse_expression_statement(CCParser *parser) {
    CCAstNode *node;
    CCSpan start;

    start = cc_current_span(parser);

    if (cc_match(parser, CC_TOKEN_SEMICOLON)) {
        return cc_new_ast_node(CC_AST_EMPTY_STATEMENT, cc_previous_span(parser), NULL);
    }

    node = cc_new_ast_node(CC_AST_EXPRESSION_STATEMENT, start, NULL);
    cc_ast_add_child(node, cc_parse_expression(parser));
    cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after expression");
    node->span = cc_span_from_start_to_previous(parser, start);
    return node;
}

static CCAstNode *cc_parse_statement(CCParser *parser) {
    switch (cc_current_token(parser)->kind) {
        case CC_TOKEN_LBRACE:
            return cc_parse_compound_statement(parser);
        case CC_TOKEN_KW_IF:
            return cc_parse_if_statement(parser);
        case CC_TOKEN_KW_FOR:
            return cc_parse_for_statement(parser);
        case CC_TOKEN_KW_WHILE:
            return cc_parse_while_statement(parser);
        case CC_TOKEN_KW_RETURN:
            return cc_parse_return_statement(parser);
        case CC_TOKEN_KW_BREAK:
            cc_advance(parser);
            cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after break");
            return cc_new_ast_node(CC_AST_BREAK_STATEMENT, cc_previous_span(parser), NULL);
        case CC_TOKEN_KW_CONTINUE:
            cc_advance(parser);
            cc_expect(parser, CC_TOKEN_SEMICOLON, "expected ';' after continue");
            return cc_new_ast_node(CC_AST_CONTINUE_STATEMENT, cc_previous_span(parser), NULL);
        default:
            return cc_parse_expression_statement(parser);
    }
}

static CCAstNode *cc_parse_declaration(CCParser *parser) {
    CCParsedSpecifiers specifiers;
    CCParsedDeclarator declarator;

    specifiers = cc_parse_declaration_specifiers(parser, true);
    if (!specifiers.has_any) {
        cc_free_ast(specifiers.node);
        return NULL;
    }

    if (cc_match(parser, CC_TOKEN_SEMICOLON)) {
        CCAstNode *declaration;

        declaration = cc_new_ast_node(CC_AST_DECLARATION, specifiers.node->span, NULL);
        cc_ast_add_child(declaration, specifiers.node);
        declaration->span = cc_span_from_start_to_previous(parser, specifiers.node->span);
        return declaration;
    }

    declarator = cc_parse_declarator(parser, false);
    if (declarator.node == NULL) {
        cc_free_ast(specifiers.node);
        return NULL;
    }

    return cc_parse_declaration_from_parts(parser, specifiers, declarator.node);
}

static CCAstNode *cc_parse_preprocessor_line(CCParser *parser) {
    const CCToken *first_token;
    CCSpan span;
    size_t line;

    first_token = cc_current_token(parser);
    line = first_token->span.line;
    cc_advance(parser);

    while (!cc_at(parser, CC_TOKEN_EOF) && cc_current_token(parser)->span.line == line) {
        cc_advance(parser);
    }

    span = cc_merge_spans(first_token->span, cc_previous_span(parser));
    return cc_new_ast_node_with_owned_text(
        CC_AST_PREPROCESSOR_LINE,
        span,
        cc_duplicate_source_range(&parser->lex_result->source, span.offset, cc_span_end_offset(span))
    );
}

static CCAstNode *cc_parse_function_definition(
    CCParser *parser,
    CCParsedSpecifiers specifiers,
    CCAstNode *declarator
) {
    CCAstNode *node;
    CCAstNode *body;
    CCSpan start;

    start = specifiers.node->span;
    body = cc_parse_compound_statement(parser);

    node = cc_new_ast_node(CC_AST_FUNCTION_DEFINITION, cc_merge_spans(start, body->span), declarator->text);
    cc_ast_add_child(node, specifiers.node);
    cc_ast_add_child(node, declarator);
    cc_ast_add_child(node, body);
    return node;
}

static CCAstNode *cc_parse_external_declaration(CCParser *parser) {
    CCParsedSpecifiers specifiers;
    CCParsedDeclarator declarator;

    if (cc_at(parser, CC_TOKEN_HASH) && cc_current_token(parser)->span.column == 1) {
        return cc_parse_preprocessor_line(parser);
    }

    specifiers = cc_parse_declaration_specifiers(parser, true);
    if (!specifiers.has_any) {
        cc_free_ast(specifiers.node);
        return NULL;
    }

    declarator = cc_parse_declarator(parser, false);
    if (declarator.node == NULL) {
        cc_free_ast(specifiers.node);
        return NULL;
    }

    if (cc_at(parser, CC_TOKEN_LBRACE) && declarator.is_function) {
        return cc_parse_function_definition(parser, specifiers, declarator.node);
    }

    return cc_parse_declaration_from_parts(parser, specifiers, declarator.node);
}

static void cc_print_indent(FILE *out, unsigned indent) {
    while (indent-- > 0) {
        fputc(' ', out);
    }
}

void cc_ast_print(FILE *out, const CCAstNode *node, unsigned indent) {
    size_t index;

    if (node == NULL) {
        return;
    }

    cc_print_indent(out, indent);
    fprintf(out, "%s", cc_ast_safe_kind_name(node->kind));
    if (node->text != NULL) {
        fprintf(out, ": %s", node->text);
    }
    fputc('\n', out);

    for (index = 0; index < node->child_count; index++) {
        cc_ast_print(out, node->children[index], indent + 2);
    }
}

void cc_parse_translation_unit(const CCLexResult *lex_result, CCParseResult *result) {
    CCParser parser;
    CCAstNode *root;

    memset(&parser, 0, sizeof(parser));
    parser.lex_result = lex_result;
    cc_seed_typedef_table(&parser);

    root = cc_new_ast_node(CC_AST_TRANSLATION_UNIT, lex_result->tokens.items[0].span, NULL);

    while (!cc_at(&parser, CC_TOKEN_EOF)) {
        CCAstNode *child;

        child = cc_parse_external_declaration(&parser);
        if (child == NULL) {
            cc_synchronize_to_semicolon_or_brace(&parser);
            if (!cc_at(&parser, CC_TOKEN_EOF) && cc_at(&parser, CC_TOKEN_RBRACE)) {
                cc_advance(&parser);
            }
            continue;
        }

        cc_ast_add_child(root, child);
    }

    if (root->child_count > 0) {
        root->span = cc_merge_spans(root->children[0]->span, root->children[root->child_count - 1]->span);
    }

    memset(result, 0, sizeof(*result));
    result->source = lex_result->source;
    result->translation_unit = root;
    result->diagnostics = parser.diagnostics;

    cc_free_typedef_table(&parser);
}

void cc_parse_result_free(CCParseResult *result) {
    size_t index;

    cc_free_ast(result->translation_unit);
    result->translation_unit = NULL;

    for (index = 0; index < result->diagnostics.count; index++) {
        free(result->diagnostics.items[index].path);
        free(result->diagnostics.items[index].message);
    }

    free(result->diagnostics.items);
    result->diagnostics.items = NULL;
    result->diagnostics.count = 0;
    result->diagnostics.capacity = 0;
}
