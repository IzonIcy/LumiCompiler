#include "C-Compiler/lexer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *cc_source_path(const CCSourceView *source) {
    if (source->path == NULL || source->path[0] == '\0') {
        return "<input>";
    }

    return source->path;
}

static const char *cc_diagnostic_path(const CCSourceView *source, const CCDiagnostic *diagnostic) {
    if (diagnostic->path != NULL && diagnostic->path[0] != '\0') {
        return diagnostic->path;
    }

    return cc_source_path(source);
}

static size_t cc_clamp_offset(const CCSourceView *source, size_t offset) {
    if (offset > source->length) {
        return source->length;
    }

    return offset;
}

static size_t cc_find_line_start(const CCSourceView *source, size_t offset) {
    size_t index;

    index = cc_clamp_offset(source, offset);
    while (index > 0 && source->text[index - 1] != '\n' && source->text[index - 1] != '\r') {
        index--;
    }

    return index;
}

static size_t cc_find_line_end(const CCSourceView *source, size_t offset) {
    size_t index;

    index = cc_clamp_offset(source, offset);
    while (index < source->length && source->text[index] != '\n' && source->text[index] != '\r') {
        index++;
    }

    return index;
}

void cc_print_diagnostics(
    FILE *out,
    const CCSourceView *source,
    const CCDiagnostic *diagnostics,
    size_t diagnostic_count
) {
    size_t index;

    for (index = 0; index < diagnostic_count; index++) {
        const CCDiagnostic *diagnostic;
        const char *diagnostic_path;
        bool can_print_source_line;
        size_t line_start;
        size_t line_end;
        size_t underline_length;
        size_t caret_index;

        diagnostic = &diagnostics[index];
        diagnostic_path = cc_diagnostic_path(source, diagnostic);
        can_print_source_line = source->text != NULL
            && source->length > 0
            && strcmp(diagnostic_path, cc_source_path(source)) == 0;

        fprintf(
            out,
            "%s:%zu:%zu: error: %s\n",
            diagnostic_path,
            diagnostic->span.line,
            diagnostic->span.column,
            diagnostic->message
        );

        if (!can_print_source_line) {
            continue;
        }

        line_start = cc_find_line_start(source, diagnostic->span.offset);
        line_end = cc_find_line_end(source, diagnostic->span.offset);
        underline_length = diagnostic->span.length == 0 ? 1 : diagnostic->span.length;

        if (diagnostic->span.offset + underline_length > line_end) {
            underline_length = line_end > diagnostic->span.offset ? line_end - diagnostic->span.offset : 1;
        }

        fwrite(source->text + line_start, 1, line_end - line_start, out);
        fputc('\n', out);

        for (caret_index = line_start; caret_index < diagnostic->span.offset; caret_index++) {
            fputc(source->text[caret_index] == '\t' ? '\t' : ' ', out);
        }

        fputc('^', out);
        for (caret_index = 1; caret_index < underline_length; caret_index++) {
            fputc('~', out);
        }
        fputc('\n', out);
    }
}
