#ifndef CCOMPILER_LEXER_H
#define CCOMPILER_LEXER_H

#include <stddef.h>
#include <stdio.h>

#include "ccompiler/token.h"

typedef struct {
    const char *path;
    const char *text;
    size_t length;
} CCSourceView;

typedef enum {
    CC_DIAGNOSTIC_ERROR,
    CC_DIAGNOSTIC_WARNING
} CCDiagnosticSeverity;

typedef struct {
    CCSpan span;
    char *path;
    char *message;
    CCDiagnosticSeverity severity;
} CCDiagnostic;

typedef struct {
    CCToken *items;
    size_t count;
    /// @brief 
    size_t capacity;
} CCTokenBuffer;

typedef struct {
    CCDiagnostic *items;
    size_t count;
    size_t capacity;
} CCDiagnosticBuffer;

typedef struct {
    CCSourceView source;
    CCTokenBuffer tokens;
    CCDiagnosticBuffer diagnostics;
} CCLexResult;

void cc_lex_source(const CCSourceView *source, CCLexResult *result);
void cc_lex_result_free(CCLexResult *result);
void cc_print_diagnostics(
    FILE *out,
    const CCSourceView *source,
    const CCDiagnostic *diagnostics,
    size_t diagnostic_count
);

#endif
