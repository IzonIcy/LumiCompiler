#ifndef C-Compiler_CODEGEN_H
#define C-Compiler_CODEGEN_H

#include "C-Compiler/parser.h"

typedef struct {
    char *text;
    CCDiagnosticBuffer diagnostics;
} CCCodegenResult;

void cc_codegen_translation_unit(const CCParseResult *parse_result, CCCodegenResult *result);
void cc_codegen_result_free(CCCodegenResult *result);

#endif
