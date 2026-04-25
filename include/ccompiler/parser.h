#ifndef CCOMPILER_PARSER_H
#define CCOMPILER_PARSER_H

#include <stddef.h>
#include <stdio.h>

#include "ccompiler/lexer.h"

typedef enum {
    CC_AST_TRANSLATION_UNIT,
    CC_AST_PREPROCESSOR_LINE,
    CC_AST_FUNCTION_DEFINITION,
    CC_AST_DECLARATION,
    CC_AST_DECLARATION_SPECIFIERS,
    CC_AST_SPECIFIER,
    CC_AST_RECORD_SPECIFIER,
    CC_AST_INIT_DECLARATOR,
    CC_AST_DECLARATOR,
    CC_AST_POINTER,
    CC_AST_ARRAY_DECLARATOR,
    CC_AST_PARAMETER_LIST,
    CC_AST_PARAMETER,
    CC_AST_TYPE_NAME,
    CC_AST_COMPOUND_STATEMENT,
    CC_AST_IF_STATEMENT,
    CC_AST_FOR_STATEMENT,
    CC_AST_WHILE_STATEMENT,
    CC_AST_DO_WHILE_STATEMENT,
    CC_AST_SWITCH_STATEMENT,
    CC_AST_CASE_STATEMENT,
    CC_AST_DEFAULT_STATEMENT,
    CC_AST_GOTO_STATEMENT,
    CC_AST_LABEL_STATEMENT,
    CC_AST_RETURN_STATEMENT,
    CC_AST_BREAK_STATEMENT,
    CC_AST_CONTINUE_STATEMENT,
    CC_AST_EXPRESSION_STATEMENT,
    CC_AST_EMPTY_STATEMENT,
    CC_AST_IDENTIFIER,
    CC_AST_LITERAL,
    CC_AST_UNARY_EXPRESSION,
    CC_AST_BINARY_EXPRESSION,
    CC_AST_CONDITIONAL_EXPRESSION,
    CC_AST_CALL_EXPRESSION,
    CC_AST_SUBSCRIPT_EXPRESSION,
    CC_AST_MEMBER_EXPRESSION,
    CC_AST_CAST_EXPRESSION,
    CC_AST_ARGUMENT_LIST
} CCAstKind;

typedef struct CCAstNode {
    CCAstKind kind;
    CCSpan span;
    char *text;
    struct CCAstNode **children;
    size_t child_count;
    size_t child_capacity;
} CCAstNode;

typedef struct {
    CCSourceView source;
    CCAstNode *translation_unit;
    CCDiagnosticBuffer diagnostics;
} CCParseResult;

void cc_parse_translation_unit(const CCLexResult *lex_result, CCParseResult *result);
void cc_parse_result_free(CCParseResult *result);
void cc_ast_print(FILE *out, const CCAstNode *node, unsigned indent);
const char *cc_ast_kind_name(CCAstKind kind);

#endif
