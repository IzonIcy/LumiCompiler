#ifndef CCOMPILER_SEMA_H
#define CCOMPILER_SEMA_H

#include <stdbool.h>
#include <stddef.h>

#include "ccompiler/parser.h"

typedef struct {
    CCDiagnosticBuffer diagnostics;
    size_t function_count;
    size_t global_count;
    size_t typedef_count;
} CCSemaResult;

typedef struct {
    bool warnings_enabled;
} CCSemaOptions;

void cc_sema_check_translation_unit(
    const CCParseResult *parse_result,
    const CCSemaOptions *options,
    CCSemaResult *result
);
void cc_sema_result_free(CCSemaResult *result);

#endif
