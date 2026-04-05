#include "ccompiler/codegen.h"
#include "ccompiler/lexer.h"
#include "ccompiler/parser.h"
#include "ccompiler/preprocessor.h"
#include "ccompiler/sema.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_LEXEME_PREVIEW_LIMIT 48

typedef struct {
    char *data;
    size_t length;
} CCLoadedFile;

static void cc_print_usage(FILE *out, const char *program_name) {
    fprintf(out, "usage: %s lex <path>\n", program_name);
    fprintf(out, "       %s preprocess <path>\n", program_name);
    fprintf(out, "       %s parse <path>\n", program_name);
    fprintf(out, "       %s check <path>\n", program_name);
    fprintf(out, "       %s codegen <path>\n", program_name);
    fprintf(out, "       %s <path>\n", program_name);
}

static bool cc_append_fragment(char *buffer, size_t capacity, size_t *length, const char *fragment) {
    size_t remaining;
    size_t index;

    if (*length >= capacity) {
        return false;
    }

    remaining = capacity - *length;
    for (index = 0; fragment[index] != '\0'; index++) {
        if (index + 1 >= remaining) {
            return false;
        }
        buffer[*length + index] = fragment[index];
    }

    *length += index;
    buffer[*length] = '\0';
    return true;
}

static void cc_format_lexeme_preview(const CCSourceView *source, CCSpan span, char *buffer, size_t capacity) {
    size_t index;
    size_t length;
    bool truncated;

    if (capacity == 0) {
        return;
    }

    if (span.length == 0) {
        snprintf(buffer, capacity, "<eof>");
        return;
    }

    buffer[0] = '\0';
    length = 0;
    truncated = false;

    for (index = 0; index < span.length; index++) {
        unsigned char ch;
        char escaped[8];
        const char *fragment;

        if (index >= CC_LEXEME_PREVIEW_LIMIT) {
            truncated = true;
            break;
        }

        ch = (unsigned char)source->text[span.offset + index];
        switch (ch) {
            case '\n':
                fragment = "\\n";
                break;
            case '\r':
                fragment = "\\r";
                break;
            case '\t':
                fragment = "\\t";
                break;
            case '\v':
                fragment = "\\v";
                break;
            case '\f':
                fragment = "\\f";
                break;
            case '\\':
                fragment = "\\\\";
                break;
            default:
                if (ch >= 32 && ch <= 126) {
                    escaped[0] = (char)ch;
                    escaped[1] = '\0';
                    fragment = escaped;
                } else {
                    snprintf(escaped, sizeof(escaped), "\\x%02X", ch);
                    fragment = escaped;
                }
                break;
        }

        if (!cc_append_fragment(buffer, capacity, &length, fragment)) {
            truncated = true;
            break;
        }
    }

    if (truncated) {
        cc_append_fragment(buffer, capacity, &length, "...");
    }
}

static bool cc_load_file(const char *path, CCLoadedFile *file) {
    FILE *stream;
    long file_length;
    size_t bytes_read;

    memset(file, 0, sizeof(*file));

    stream = fopen(path, "rb");
    if (stream == NULL) {
        fprintf(stderr, "error: unable to open '%s': %s\n", path, strerror(errno));
        return false;
    }

    if (fseek(stream, 0, SEEK_END) != 0) {
        fprintf(stderr, "error: unable to seek '%s'\n", path);
        fclose(stream);
        return false;
    }

    file_length = ftell(stream);
    if (file_length < 0) {
        fprintf(stderr, "error: unable to determine file length for '%s'\n", path);
        fclose(stream);
        return false;
    }

    if (fseek(stream, 0, SEEK_SET) != 0) {
        fprintf(stderr, "error: unable to rewind '%s'\n", path);
        fclose(stream);
        return false;
    }

    file->data = malloc((size_t)file_length + 1);
    if (file->data == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        fclose(stream);
        return false;
    }

    bytes_read = fread(file->data, 1, (size_t)file_length, stream);
    if (bytes_read != (size_t)file_length) {
        fprintf(stderr, "error: short read while loading '%s'\n", path);
        free(file->data);
        fclose(stream);
        return false;
    }

    file->data[file_length] = '\0';
    file->length = (size_t)file_length;
    fclose(stream);
    return true;
}

static void cc_free_loaded_file(CCLoadedFile *file) {
    free(file->data);
    file->data = NULL;
    file->length = 0;
}

static void cc_print_tokens(const CCLexResult *result) {
    size_t index;

    printf("%-6s %-6s %-24s %s\n", "line", "col", "kind", "lexeme");
    printf("%-6s %-6s %-24s %s\n", "----", "---", "------------------------", "------");

    for (index = 0; index < result->tokens.count; index++) {
        char preview[256];
        const CCToken *token;

        token = &result->tokens.items[index];
        cc_format_lexeme_preview(&result->source, token->span, preview, sizeof(preview));

        printf(
            "%-6zu %-6zu %-24s %s\n",
            token->span.line,
            token->span.column,
            cc_token_kind_name(token->kind),
            preview
        );
    }
}

static int cc_run_lex(const char *path) {
    CCLoadedFile loaded_file;
    CCLexResult result;
    int exit_code;

    if (!cc_load_file(path, &loaded_file)) {
        return 1;
    }

    cc_lex_source(
        &(CCSourceView){
            .path = path,
            .text = loaded_file.data,
            .length = loaded_file.length,
        },
        &result
    );

    cc_print_tokens(&result);

    if (result.diagnostics.count > 0) {
        fflush(stdout);
        fputc('\n', stderr);
        cc_print_diagnostics(stderr, &result.source, result.diagnostics.items, result.diagnostics.count);
    }

    exit_code = result.diagnostics.count == 0 ? 0 : 1;
    cc_lex_result_free(&result);
    cc_free_loaded_file(&loaded_file);
    return exit_code;
}

static int cc_run_parse(const char *path) {
    CCLoadedFile loaded_file;
    CCLexResult lex_result;
    CCParseResult parse_result;
    int exit_code;

    if (!cc_load_file(path, &loaded_file)) {
        return 1;
    }

    cc_lex_source(
        &(CCSourceView){
            .path = path,
            .text = loaded_file.data,
            .length = loaded_file.length,
        },
        &lex_result
    );

    if (lex_result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &lex_result.source, lex_result.diagnostics.items, lex_result.diagnostics.count);
        cc_lex_result_free(&lex_result);
        cc_free_loaded_file(&loaded_file);
        return 1;
    }

    cc_parse_translation_unit(&lex_result, &parse_result);

    if (parse_result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &parse_result.source, parse_result.diagnostics.items, parse_result.diagnostics.count);
        exit_code = 1;
    } else {
        cc_ast_print(stdout, parse_result.translation_unit, 0);
        exit_code = 0;
    }

    cc_parse_result_free(&parse_result);
    cc_lex_result_free(&lex_result);
    cc_free_loaded_file(&loaded_file);
    return exit_code;
}

static bool cc_prepare_processed_translation_unit(
    const char *path,
    CCPreprocessResult *preprocess_result,
    CCLexResult *lex_result,
    CCParseResult *parse_result
) {
    memset(preprocess_result, 0, sizeof(*preprocess_result));
    memset(lex_result, 0, sizeof(*lex_result));
    memset(parse_result, 0, sizeof(*parse_result));

    cc_preprocess_file(path, preprocess_result);
    if (preprocess_result->diagnostics.count > 0) {
        cc_print_diagnostics(
            stderr,
            &preprocess_result->source,
            preprocess_result->diagnostics.items,
            preprocess_result->diagnostics.count
        );
        return false;
    }

    cc_lex_source(
        &(CCSourceView){
            .path = path,
            .text = preprocess_result->text,
            .length = strlen(preprocess_result->text),
        },
        lex_result
    );

    if (lex_result->diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &lex_result->source, lex_result->diagnostics.items, lex_result->diagnostics.count);
        return false;
    }

    cc_parse_translation_unit(lex_result, parse_result);
    if (parse_result->diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &parse_result->source, parse_result->diagnostics.items, parse_result->diagnostics.count);
        return false;
    }

    return true;
}

static int cc_run_preprocess(const char *path) {
    CCPreprocessResult result;
    int exit_code;

    memset(&result, 0, sizeof(result));
    cc_preprocess_file(path, &result);

    if (result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &result.source, result.diagnostics.items, result.diagnostics.count);
        exit_code = 1;
    } else {
        fputs(result.text, stdout);
        exit_code = 0;
    }

    cc_preprocess_result_free(&result);
    return exit_code;
}

static int cc_run_check(const char *path) {
    CCPreprocessResult preprocess_result;
    CCLexResult lex_result;
    CCParseResult parse_result;
    CCSemaResult sema_result;
    int exit_code;

    memset(&sema_result, 0, sizeof(sema_result));

    if (!cc_prepare_processed_translation_unit(path, &preprocess_result, &lex_result, &parse_result)) {
        cc_parse_result_free(&parse_result);
        cc_lex_result_free(&lex_result);
        cc_preprocess_result_free(&preprocess_result);
        return 1;
    }

    cc_sema_check_translation_unit(&parse_result, &sema_result);
    if (sema_result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &parse_result.source, sema_result.diagnostics.items, sema_result.diagnostics.count);
        exit_code = 1;
    } else {
        printf("semantic analysis succeeded\n");
        printf("functions: %zu\n", sema_result.function_count);
        printf("globals: %zu\n", sema_result.global_count);
        printf("typedefs: %zu\n", sema_result.typedef_count);
        exit_code = 0;
    }

    cc_sema_result_free(&sema_result);
    cc_parse_result_free(&parse_result);
    cc_lex_result_free(&lex_result);
    cc_preprocess_result_free(&preprocess_result);
    return exit_code;
}

static int cc_run_codegen(const char *path) {
    CCPreprocessResult preprocess_result;
    CCLexResult lex_result;
    CCParseResult parse_result;
    CCSemaResult sema_result;
    CCCodegenResult codegen_result;
    int exit_code;

    memset(&sema_result, 0, sizeof(sema_result));
    memset(&codegen_result, 0, sizeof(codegen_result));

    if (!cc_prepare_processed_translation_unit(path, &preprocess_result, &lex_result, &parse_result)) {
        cc_parse_result_free(&parse_result);
        cc_lex_result_free(&lex_result);
        cc_preprocess_result_free(&preprocess_result);
        return 1;
    }

    cc_sema_check_translation_unit(&parse_result, &sema_result);
    if (sema_result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &parse_result.source, sema_result.diagnostics.items, sema_result.diagnostics.count);
        exit_code = 1;
        goto cleanup;
    }

    cc_codegen_translation_unit(&parse_result, &codegen_result);
    if (codegen_result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &parse_result.source, codegen_result.diagnostics.items, codegen_result.diagnostics.count);
        exit_code = 1;
    } else {
        fputs(codegen_result.text, stdout);
        exit_code = 0;
    }

cleanup:
    cc_codegen_result_free(&codegen_result);
    cc_sema_result_free(&sema_result);
    cc_parse_result_free(&parse_result);
    cc_lex_result_free(&lex_result);
    cc_preprocess_result_free(&preprocess_result);
    return exit_code;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        cc_print_usage(stdout, argv[0]);
        return 0;
    }

    if (argc == 2) {
        return cc_run_lex(argv[1]);
    }

    if (argc == 3 && strcmp(argv[1], "lex") == 0) {
        return cc_run_lex(argv[2]);
    }

    if (argc == 3 && strcmp(argv[1], "preprocess") == 0) {
        return cc_run_preprocess(argv[2]);
    }

    if (argc == 3 && strcmp(argv[1], "parse") == 0) {
        return cc_run_parse(argv[2]);
    }

    if (argc == 3 && strcmp(argv[1], "check") == 0) {
        return cc_run_check(argv[2]);
    }

    if (argc == 3 && strcmp(argv[1], "codegen") == 0) {
        return cc_run_codegen(argv[2]);
    }

    cc_print_usage(stderr, argv[0]);
    return 1;
}
