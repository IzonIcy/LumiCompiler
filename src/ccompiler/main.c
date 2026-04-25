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
    bool dump_tokens;
    bool dump_ast;
    bool dump_ir;
    bool dump_ast_json;
    bool warnings_enabled;
} CCCompilerOptions;

typedef struct {
    char *data;
    size_t length;
} CCLoadedFile;

static CCCompilerOptions g_options;

static void cc_print_usage(FILE *out, const char *program_name) {
    fprintf(out, "usage: %s [options] lex <path>\n", program_name);
    fprintf(out, "       %s [options] preprocess <path>\n", program_name);
    fprintf(out, "       %s [options] parse <path>\n", program_name);
    fprintf(out, "       %s [options] check <path>\n", program_name);
    fprintf(out, "       %s [options] codegen <path>\n", program_name);
    fprintf(out, "       %s [options] <path>\n", program_name);
    fprintf(out, "\noptions:\n");
    fprintf(out, "  --dump-tokens     print token stream after lexing\n");
    fprintf(out, "  --dump-ast        print AST after parsing\n");
    fprintf(out, "  --dump-ast-json   print AST as JSON after parsing\n");
    fprintf(out, "  --dump-ir         print IR after code generation\n");
    fprintf(out, "  -Wall             enable all warnings\n");
    fprintf(out, "  -h, --help        show this help message\n");
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

static int cc_parse_options(int argc, char **argv, CCCompilerOptions *options, int *command_arg_index, int *file_arg_index, int *num_positional) {
    int i;

    memset(options, 0, sizeof(*options));
    *command_arg_index = -1;
    *file_arg_index = -1;
    *num_positional = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            options->dump_tokens = true;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            options->dump_ast = true;
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            options->dump_ast_json = true;
        } else if (strcmp(argv[i], "--dump-ir") == 0) {
            options->dump_ir = true;
        } else if (strcmp(argv[i], "-Wall") == 0) {
            options->warnings_enabled = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unrecognized option '%s'\n", argv[i]);
            cc_print_usage(stderr, argv[0]);
            return -1;
        } else {
            if (*command_arg_index == -1) {
                *command_arg_index = i;
            } else if (*file_arg_index == -1) {
                *file_arg_index = i;
            }
            (*num_positional)++;
        }
    }

    return 1;
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

static void cc_escape_json_string(const char *src, char *dest, size_t dest_size) {
    size_t j = 0;
    size_t i;

    for (i = 0; src[i] != '\0' && j < dest_size - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dest_size) break;
            dest[j++] = '\\';
            dest[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= dest_size) break;
            dest[j++] = '\\';
            dest[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dest_size) break;
            dest[j++] = '\\';
            dest[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dest_size) break;
            dest[j++] = '\\';
            dest[j++] = 't';
        } else if (c >= 32 && c <= 126) {
            dest[j++] = c;
        }
    }
    dest[j] = '\0';
}

static void cc_ast_print_json(FILE *out, const CCAstNode *node, unsigned indent) {
    size_t i;
    const char *indent_str = "  ";

    if (node == NULL) {
        return;
    }

    for (i = 0; i < indent; i++) {
        fputs(indent_str, out);
    }

    fputs("{\n", out);

    for (i = 0; i < indent + 1; i++) {
        fputs(indent_str, out);
    }
    {
        char kind_escaped[512];
        cc_escape_json_string(cc_ast_kind_name(node->kind), kind_escaped, sizeof(kind_escaped));
        fprintf(out, "\"kind\": \"%s\",\n", kind_escaped);
    }

    for (i = 0; i < indent + 1; i++) {
        fputs(indent_str, out);
    }
    fprintf(out, "\"span\": {\"line\": %zu, \"column\": %zu, \"offset\": %zu, \"length\": %zu}",
            node->span.line, node->span.column, node->span.offset, node->span.length);

    if (node->text != NULL) {
        fputc(',', out);
        fputc('\n', out);
        for (i = 0; i < indent + 1; i++) {
            fputs(indent_str, out);
        }
        {
            char text_escaped[512];
            cc_escape_json_string(node->text, text_escaped, sizeof(text_escaped));
            fprintf(out, "\"text\": \"%s\"", text_escaped);
        }
    }

    if (node->child_count > 0) {
        fputc(',', out);
        fputc('\n', out);
        for (i = 0; i < indent + 1; i++) {
            fputs(indent_str, out);
        }
        fprintf(out, "\"children\": [\n");

        for (i = 0; i < node->child_count; i++) {
            cc_ast_print_json(out, node->children[i], indent + 2);
            if (i < node->child_count - 1) {
                fputc(',', out);
            }
            fputc('\n', out);
        }

        for (i = 0; i < indent + 1; i++) {
            fputs(indent_str, out);
        }
        fputs("]", out);
    }

    fputc('\n', out);
    for (i = 0; i < indent; i++) {
        fputs(indent_str, out);
    }
    fputs("}", out);
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

    if (g_options.dump_tokens) {
        cc_print_tokens(&result);
    }

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
        if (g_options.dump_ast_json) {
            cc_ast_print_json(stdout, parse_result.translation_unit, 0);
            fputc('\n', stdout);
        } else {
            cc_ast_print(stdout, parse_result.translation_unit, 0);
        }
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

static size_t cc_count_errors(const CCDiagnostic *diagnostics, size_t count) {
    size_t i;
    size_t error_count = 0;

    for (i = 0; i < count; i++) {
        if (diagnostics[i].severity == CC_DIAGNOSTIC_ERROR) {
            error_count++;
        }
    }

    return error_count;
}

static int cc_run_check(const char *path) {
    CCPreprocessResult preprocess_result;
    CCLexResult lex_result;
    CCParseResult parse_result;
    CCSemaResult sema_result;
    CCSemaOptions sema_options;
    int exit_code;

    memset(&sema_result, 0, sizeof(sema_result));
    sema_options.warnings_enabled = g_options.warnings_enabled;

    if (!cc_prepare_processed_translation_unit(path, &preprocess_result, &lex_result, &parse_result)) {
        cc_parse_result_free(&parse_result);
        cc_lex_result_free(&lex_result);
        cc_preprocess_result_free(&preprocess_result);
        return 1;
    }

    cc_sema_check_translation_unit(&parse_result, &sema_options, &sema_result);
    if (sema_result.diagnostics.count > 0) {
        size_t error_count;
        
        cc_print_diagnostics(stderr, &parse_result.source, sema_result.diagnostics.items, sema_result.diagnostics.count);
        error_count = cc_count_errors(sema_result.diagnostics.items, sema_result.diagnostics.count);
        if (error_count > 0) {
            exit_code = 1;
        } else {
            printf("semantic analysis succeeded\n");
            printf("functions: %zu\n", sema_result.function_count);
            printf("globals: %zu\n", sema_result.global_count);
            printf("typedefs: %zu\n", sema_result.typedef_count);
            exit_code = 0;
        }
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
    CCSemaOptions sema_options;
    CCCodegenResult codegen_result;
    int exit_code;

    memset(&sema_result, 0, sizeof(sema_result));
    memset(&codegen_result, 0, sizeof(codegen_result));
    sema_options.warnings_enabled = g_options.warnings_enabled;

    if (!cc_prepare_processed_translation_unit(path, &preprocess_result, &lex_result, &parse_result)) {
        cc_parse_result_free(&parse_result);
        cc_lex_result_free(&lex_result);
        cc_preprocess_result_free(&preprocess_result);
        return 1;
    }

    cc_sema_check_translation_unit(&parse_result, &sema_options, &sema_result);
    if (sema_result.diagnostics.count > 0) {
        size_t error_count;
        
        cc_print_diagnostics(stderr, &parse_result.source, sema_result.diagnostics.items, sema_result.diagnostics.count);
        error_count = cc_count_errors(sema_result.diagnostics.items, sema_result.diagnostics.count);
        if (error_count > 0) {
            exit_code = 1;
            goto cleanup;
        }
    }

    cc_codegen_translation_unit(&parse_result, &codegen_result);
    if (codegen_result.diagnostics.count > 0) {
        cc_print_diagnostics(stderr, &parse_result.source, codegen_result.diagnostics.items, codegen_result.diagnostics.count);
        exit_code = 1;
    } else {
        if (g_options.dump_ir) {
            fputs(codegen_result.text, stdout);
        }
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
    CCCompilerOptions options;
    int command_arg_index;
    int file_arg_index;
    int num_positional;
    const char *command;
    const char *filepath;
    int parse_result;

    parse_result = cc_parse_options(argc, argv, &options, &command_arg_index, &file_arg_index, &num_positional);
    if (parse_result < 0) {
        return 1;
    }
    if (parse_result == 0) {
        cc_print_usage(stdout, argv[0]);
        return 0;
    }

    if (num_positional == 0) {
        cc_print_usage(stderr, argv[0]);
        return 1;
    }

    g_options = options;

    if (num_positional == 1) {
        filepath = argv[command_arg_index];
        return cc_run_lex(filepath);
    }

    if (num_positional == 2) {
        command = argv[command_arg_index];
        filepath = argv[file_arg_index];
    } else {
        cc_print_usage(stderr, argv[0]);
        return 1;
    }

    if (strcmp(command, "lex") == 0) {
        return cc_run_lex(filepath);
    }

    if (strcmp(command, "preprocess") == 0) {
        return cc_run_preprocess(filepath);
    }

    if (strcmp(command, "parse") == 0) {
        return cc_run_parse(filepath);
    }

    if (strcmp(command, "check") == 0) {
        return cc_run_check(filepath);
    }

    if (strcmp(command, "codegen") == 0) {
        return cc_run_codegen(filepath);
    }

    cc_print_usage(stderr, argv[0]);
    return 1;
}
