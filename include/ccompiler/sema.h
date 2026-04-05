#ifndef C-Compiler_SEMA_H
#define C-Compiler_SEMA_H

#include <stddef.h>

#include "C-Compiler/parser.h"

typedef struct {
    CCDiagnosticBuffer diagnostics;
    size_t function_count;
    size_t global_count;
    size_t typedef_count;
} CCSemaResult;

void cc_sema_check_translation_unit(const CCParseResult *parse_result, CCSemaResult *result);
void cc_sema_result_free(CCSemaResult *result);

#endif
