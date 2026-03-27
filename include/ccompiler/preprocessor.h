#ifndef CCOMPILER_PREPROCESSOR_H
#define CCOMPILER_PREPROCESSOR_H

#include <stddef.h>

#include "ccompiler/lexer.h"

typedef struct {
    CCSourceView source;
    char *text;
    CCDiagnosticBuffer diagnostics;
} CCPreprocessResult;

void cc_preprocess_file(const char *path, CCPreprocessResult *result);
void cc_preprocess_result_free(CCPreprocessResult *result);

#endif
