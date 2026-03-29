#include "ccompiler/preprocessor.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define CC_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define CC_MAX_MACRO_EXPANSION_DEPTH 32
#define CC_PATH_BUFFER_SIZE 4096

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} CCStringBuilder;

typedef struct {
    char *name;
    bool is_function_like;
    char **parameters;
    size_t parameter_count;
    char *replacement;
    bool expanding;
} CCMacro;

typedef struct {
    bool parent_active;
    bool current_active;
    bool branch_taken;
} CCConditionalFrame;

typedef struct {
    CCDiagnosticBuffer diagnostics;
    CCMacro *macros;
    size_t macro_count;
    size_t macro_capacity;
    CCConditionalFrame *conditionals;
    size_t conditional_count;
    size_t conditional_capacity;
    bool in_block_comment;
} CCPreprocessor;

typedef struct {
    char *data;
    size_t length;
} CCLoadedFile;

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

static char *cc_duplicate_range(const char *text, size_t length) {
    char *copy;

    copy = cc_reallocate_or_die(NULL, length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void cc_builder_append_range(CCStringBuilder *builder, const char *text, size_t length) {
    size_t required;

    required = builder->length + length + 1;
    if (required > builder->capacity) {
        size_t new_capacity;

        new_capacity = builder->capacity == 0 ? 64 : builder->capacity;
        while (new_capacity < required) {
            new_capacity *= 2;
        }

        builder->data = cc_reallocate_or_die(builder->data, new_capacity);
        builder->capacity = new_capacity;
    }

    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
}

static void cc_builder_append_cstring(CCStringBuilder *builder, const char *text) {
    cc_builder_append_range(builder, text, strlen(text));
}

static void cc_builder_append_char(CCStringBuilder *builder, char ch) {
    cc_builder_append_range(builder, &ch, 1);
}

static char *cc_builder_take_string(CCStringBuilder *builder) {
    char *data;

    if (builder->data == NULL) {
        data = cc_duplicate_string("");
    } else {
        data = builder->data;
    }

    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
    return data;
}

static void cc_grow_diagnostic_buffer(CCDiagnosticBuffer *buffer) {
    size_t new_capacity;

    new_capacity = buffer->capacity == 0 ? 8 : buffer->capacity * 2;
    buffer->items = cc_reallocate_or_die(buffer->items, new_capacity * sizeof(*buffer->items));
    buffer->capacity = new_capacity;
}

static void cc_add_diagnostic(
    CCPreprocessor *preprocessor,
    const char *path,
    size_t offset,
    size_t line,
    size_t column,
    size_t length,
    const char *format,
    ...
) {
    char small_buffer[256];
    char *message;
    int needed;
    va_list args;
    va_list copy;

    if (preprocessor->diagnostics.count == preprocessor->diagnostics.capacity) {
        cc_grow_diagnostic_buffer(&preprocessor->diagnostics);
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
        size_t text_length;

        text_length = (size_t)needed + 1;
        message = cc_reallocate_or_die(NULL, text_length);
        vsnprintf(message, text_length, format, copy);
        va_end(copy);
    }

    preprocessor->diagnostics.items[preprocessor->diagnostics.count].span.offset = offset;
    preprocessor->diagnostics.items[preprocessor->diagnostics.count].span.line = line;
    preprocessor->diagnostics.items[preprocessor->diagnostics.count].span.column = column;
    preprocessor->diagnostics.items[preprocessor->diagnostics.count].span.length = length == 0 ? 1 : length;
    preprocessor->diagnostics.items[preprocessor->diagnostics.count].path = path == NULL ? NULL : cc_duplicate_string(path);
    preprocessor->diagnostics.items[preprocessor->diagnostics.count].message = message;
    preprocessor->diagnostics.count++;
}

static bool cc_load_file(const char *path, CCLoadedFile *file) {
    FILE *stream;
    long file_length;
    size_t bytes_read;

    memset(file, 0, sizeof(*file));

    stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }

    if (fseek(stream, 0, SEEK_END) != 0) {
        fclose(stream);
        return false;
    }

    file_length = ftell(stream);
    if (file_length < 0) {
        fclose(stream);
        return false;
    }

    if (fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return false;
    }

    file->data = cc_reallocate_or_die(NULL, (size_t)file_length + 1);
    bytes_read = fread(file->data, 1, (size_t)file_length, stream);
    fclose(stream);

    if (bytes_read != (size_t)file_length) {
        free(file->data);
        file->data = NULL;
        return false;
    }

    file->data[file_length] = '\0';
    file->length = (size_t)file_length;
    return true;
}

static void cc_free_loaded_file(CCLoadedFile *file) {
    free(file->data);
    file->data = NULL;
    file->length = 0;
}

static void cc_free_macro(CCMacro *macro) {
    size_t index;

    free(macro->name);
    for (index = 0; index < macro->parameter_count; index++) {
        free(macro->parameters[index]);
    }
    free(macro->parameters);
    free(macro->replacement);

    macro->name = NULL;
    macro->parameters = NULL;
    macro->replacement = NULL;
    macro->parameter_count = 0;
}

static void cc_grow_macro_table(CCPreprocessor *preprocessor) {
    size_t new_capacity;

    new_capacity = preprocessor->macro_capacity == 0 ? 8 : preprocessor->macro_capacity * 2;
    preprocessor->macros = cc_reallocate_or_die(preprocessor->macros, new_capacity * sizeof(*preprocessor->macros));
    preprocessor->macro_capacity = new_capacity;
}

static void cc_grow_conditionals(CCPreprocessor *preprocessor) {
    size_t new_capacity;

    new_capacity = preprocessor->conditional_capacity == 0 ? 8 : preprocessor->conditional_capacity * 2;
    preprocessor->conditionals = cc_reallocate_or_die(
        preprocessor->conditionals,
        new_capacity * sizeof(*preprocessor->conditionals)
    );
    preprocessor->conditional_capacity = new_capacity;
}

static CCMacro *cc_find_macro(CCPreprocessor *preprocessor, const char *name) {
    size_t index;

    for (index = 0; index < preprocessor->macro_count; index++) {
        if (strcmp(preprocessor->macros[index].name, name) == 0) {
            return &preprocessor->macros[index];
        }
    }

    return NULL;
}

static void cc_remove_macro(CCPreprocessor *preprocessor, const char *name) {
    size_t index;

    for (index = 0; index < preprocessor->macro_count; index++) {
        if (strcmp(preprocessor->macros[index].name, name) == 0) {
            cc_free_macro(&preprocessor->macros[index]);
            memmove(
                &preprocessor->macros[index],
                &preprocessor->macros[index + 1],
                (preprocessor->macro_count - index - 1) * sizeof(*preprocessor->macros)
            );
            preprocessor->macro_count--;
            return;
        }
    }
}

static void cc_store_macro(CCPreprocessor *preprocessor, CCMacro macro) {
    CCMacro *existing;

    existing = cc_find_macro(preprocessor, macro.name);
    if (existing != NULL) {
        cc_free_macro(existing);
        *existing = macro;
        return;
    }

    if (preprocessor->macro_count == preprocessor->macro_capacity) {
        cc_grow_macro_table(preprocessor);
    }

    preprocessor->macros[preprocessor->macro_count++] = macro;
}

static bool cc_identifier_start(char ch) {
    return isalpha((unsigned char)ch) != 0 || ch == '_';
}

static bool cc_identifier_part(char ch) {
    return isalnum((unsigned char)ch) != 0 || ch == '_';
}

static size_t cc_skip_spaces(const char *text, size_t length, size_t index) {
    while (index < length && (text[index] == ' ' || text[index] == '\t')) {
        index++;
    }

    return index;
}

static bool cc_current_block_active(const CCPreprocessor *preprocessor) {
    if (preprocessor->conditional_count == 0) {
        return true;
    }

    return preprocessor->conditionals[preprocessor->conditional_count - 1].current_active;
}

static bool cc_parse_identifier(
    const char *text,
    size_t length,
    size_t *index,
    char **identifier
) {
    size_t start;

    if (*index >= length || !cc_identifier_start(text[*index])) {
        return false;
    }

    start = *index;
    (*index)++;
    while (*index < length && cc_identifier_part(text[*index])) {
        (*index)++;
    }

    *identifier = cc_duplicate_range(text + start, *index - start);
    return true;
}

static bool cc_resolve_include_path(
    const char *including_path,
    const char *include_name,
    bool search_local_directory,
    char resolved_path[CC_PATH_BUFFER_SIZE]
) {
    const char *slash;
    size_t prefix_length;

    if (include_name == NULL || include_name[0] == '\0') {
        return false;
    }

    if (include_name[0] == '/') {
        if (strlen(include_name) + 1 > CC_PATH_BUFFER_SIZE) {
            return false;
        }
        strcpy(resolved_path, include_name);
        return true;
    }

    if (!search_local_directory) {
        return false;
    }

    slash = strrchr(including_path, '/');
    if (slash == NULL) {
        prefix_length = 0;
    } else {
        prefix_length = (size_t)(slash - including_path + 1);
    }

    if (prefix_length + strlen(include_name) + 1 > CC_PATH_BUFFER_SIZE) {
        return false;
    }

    memcpy(resolved_path, including_path, prefix_length);
    strcpy(resolved_path + prefix_length, include_name);
    return true;
}

static bool cc_parse_include_name(
    const char *text,
    size_t length,
    size_t *index,
    char **include_name
) {
    char delimiter;
    size_t start;

    *index = cc_skip_spaces(text, length, *index);
    if (*index >= length || (text[*index] != '"' && text[*index] != '<')) {
        return false;
    }

    delimiter = text[*index];
    (*index)++;
    start = *index;

    while (*index < length && text[*index] != (delimiter == '<' ? '>' : '"')) {
        (*index)++;
    }

    if (*index >= length) {
        return false;
    }

    *include_name = cc_duplicate_range(text + start, *index - start);
    (*index)++;
    return true;
}

static bool cc_parse_macro_parameters(
    const char *text,
    size_t length,
    size_t *index,
    char ***parameters,
    size_t *parameter_count
) {
    char **items;
    size_t count;
    size_t capacity;

    items = NULL;
    count = 0;
    capacity = 0;

    if (*index >= length || text[*index] != '(') {
        *parameters = NULL;
        *parameter_count = 0;
        return false;
    }

    (*index)++;
    *index = cc_skip_spaces(text, length, *index);

    while (*index < length && text[*index] != ')') {
        char *parameter;

        if (!cc_parse_identifier(text, length, index, &parameter)) {
            goto fail;
        }

        if (count == capacity) {
            size_t new_capacity;

            new_capacity = capacity == 0 ? 4 : capacity * 2;
            items = cc_reallocate_or_die(items, new_capacity * sizeof(*items));
            capacity = new_capacity;
        }

        items[count++] = parameter;
        *index = cc_skip_spaces(text, length, *index);

        if (*index < length && text[*index] == ',') {
            (*index)++;
            *index = cc_skip_spaces(text, length, *index);
            continue;
        }

        break;
    }

    if (*index >= length || text[*index] != ')') {
        goto fail;
    }

    (*index)++;
    *parameters = items;
    *parameter_count = count;
    return true;

fail:
    while (count > 0) {
        free(items[--count]);
    }
    free(items);
    *parameters = NULL;
    *parameter_count = 0;
    return false;
}

static char *cc_trim_argument_copy(const char *text, size_t length) {
    size_t start;
    size_t end;

    start = 0;
    end = length;

    while (start < end && isspace((unsigned char)text[start]) != 0) {
        start++;
    }

    while (end > start && isspace((unsigned char)text[end - 1]) != 0) {
        end--;
    }

    return cc_duplicate_range(text + start, end - start);
}

static bool cc_parse_macro_arguments(
    const char *text,
    size_t length,
    size_t *index,
    char ***arguments,
    size_t *argument_count
) {
    char **items;
    size_t count;
    size_t capacity;
    size_t argument_start;
    size_t depth;
    bool in_string;
    bool in_char;

    items = NULL;
    count = 0;
    capacity = 0;

    *index = cc_skip_spaces(text, length, *index);
    if (*index >= length || text[*index] != '(') {
        *arguments = NULL;
        *argument_count = 0;
        return false;
    }

    (*index)++;
    argument_start = *index;
    depth = 1;
    in_string = false;
    in_char = false;

    while (*index < length) {
        char ch;

        ch = text[*index];

        if ((in_string || in_char) && ch == '\\' && *index + 1 < length) {
            *index += 2;
            continue;
        }

        if (!in_char && ch == '"') {
            in_string = !in_string;
            (*index)++;
            continue;
        }

        if (!in_string && ch == '\'') {
            in_char = !in_char;
            (*index)++;
            continue;
        }

        if (in_string || in_char) {
            (*index)++;
            continue;
        }

        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                char *argument;

                argument = cc_trim_argument_copy(text + argument_start, *index - argument_start);
                if (count == capacity) {
                    size_t new_capacity;

                    new_capacity = capacity == 0 ? 4 : capacity * 2;
                    items = cc_reallocate_or_die(items, new_capacity * sizeof(*items));
                }
                items[count++] = argument;
                (*index)++;
                *arguments = items;
                *argument_count = count == 1 && items[0][0] == '\0' ? 0 : count;
                if (*argument_count == 0) {
                    free(items[0]);
                    free(items);
                    *arguments = NULL;
                }
                return true;
            }
        } else if (ch == ',' && depth == 1) {
            char *argument;

            argument = cc_trim_argument_copy(text + argument_start, *index - argument_start);
            if (count == capacity) {
                size_t new_capacity;

                new_capacity = capacity == 0 ? 4 : capacity * 2;
                items = cc_reallocate_or_die(items, new_capacity * sizeof(*items));
                capacity = new_capacity;
            }
            items[count++] = argument;
            (*index)++;
            argument_start = *index;
            continue;
        }

        (*index)++;
    }

    while (count > 0) {
        free(items[--count]);
    }
    free(items);
    *arguments = NULL;
    *argument_count = 0;
    return false;
}

static ssize_t cc_find_parameter_index(const CCMacro *macro, const char *identifier) {
    size_t index;

    for (index = 0; index < macro->parameter_count; index++) {
        if (strcmp(macro->parameters[index], identifier) == 0) {
            return (ssize_t)index;
        }
    }

    return -1;
}

static char *cc_collapse_token_paste(char *text) {
    CCStringBuilder builder;
    size_t index;

    memset(&builder, 0, sizeof(builder));
    index = 0;

    while (text[index] != '\0') {
        if (text[index] == '#' && text[index + 1] == '#') {
            while (builder.length > 0 && isspace((unsigned char)builder.data[builder.length - 1]) != 0) {
                builder.length--;
            }
            if (builder.data != NULL) {
                builder.data[builder.length] = '\0';
            }

            index += 2;
            while (text[index] != '\0' && isspace((unsigned char)text[index]) != 0) {
                index++;
            }
            continue;
        }

        cc_builder_append_char(&builder, text[index]);
        index++;
    }

    free(text);
    return cc_builder_take_string(&builder);
}

static char *cc_substitute_macro_arguments(const CCMacro *macro, char **arguments, size_t argument_count) {
    CCStringBuilder builder;
    size_t index;

    memset(&builder, 0, sizeof(builder));
    index = 0;

    while (macro->replacement[index] != '\0') {
        if (cc_identifier_start(macro->replacement[index])) {
            size_t start;
            size_t end;
            char *identifier;
            ssize_t parameter_index;

            start = index;
            index++;
            while (cc_identifier_part(macro->replacement[index])) {
                index++;
            }
            end = index;

            identifier = cc_duplicate_range(macro->replacement + start, end - start);
            parameter_index = cc_find_parameter_index(macro, identifier);
            if (parameter_index >= 0 && (size_t)parameter_index < argument_count) {
                cc_builder_append_cstring(&builder, arguments[parameter_index]);
            } else {
                cc_builder_append_range(&builder, macro->replacement + start, end - start);
            }
            free(identifier);
            continue;
        }

        cc_builder_append_char(&builder, macro->replacement[index]);
        index++;
    }

    return cc_collapse_token_paste(cc_builder_take_string(&builder));
}

static char *cc_expand_text(CCPreprocessor *preprocessor, const char *text, size_t depth);

static char cc_text_char(const char *text, size_t length, size_t index) {
    char ch;

    if (text == NULL || index >= length) {
        return '\0';
    }

    ch = '\0';
    memcpy(&ch, text + index, sizeof(ch));
    return ch;
}

static char *cc_expand_function_macro(
    CCPreprocessor *preprocessor,
    CCMacro *macro,
    const char *text,
    size_t *index,
    size_t depth
) {
    char **arguments;
    size_t argument_count;
    char *substituted;
    char *expanded;
    size_t argument_index;

    arguments = NULL;
    argument_count = 0;

    if (!cc_parse_macro_arguments(text, strlen(text), index, &arguments, &argument_count)) {
        return cc_duplicate_string(macro->name);
    }

    if (argument_count != macro->parameter_count) {
        for (argument_index = 0; argument_index < argument_count; argument_index++) {
            free(arguments[argument_index]);
        }
        free(arguments);
        return cc_duplicate_string(macro->name);
    }

    substituted = cc_substitute_macro_arguments(macro, arguments, argument_count);
    macro->expanding = true;
    expanded = cc_expand_text(preprocessor, substituted, depth + 1);
    macro->expanding = false;

    free(substituted);
    for (argument_index = 0; argument_index < argument_count; argument_index++) {
        free(arguments[argument_index]);
    }
    free(arguments);
    return expanded;
}

static char *cc_expand_text(CCPreprocessor *preprocessor, const char *text, size_t depth) {
    CCStringBuilder builder;
    size_t index;
    size_t length;

    if (text == NULL) {
        return cc_duplicate_string("");
    }

    if (depth > CC_MAX_MACRO_EXPANSION_DEPTH) {
        return cc_duplicate_string(text);
    }

    memset(&builder, 0, sizeof(builder));
    index = 0;
    length = strlen(text);

    while (index < length) {
        char ch;

        ch = cc_text_char(text, length, index);

        if (preprocessor->in_block_comment) {
            if (index + 1 < length && ch == '*' && cc_text_char(text, length, index + 1) == '/') {
                cc_builder_append_range(&builder, "*/", 2);
                preprocessor->in_block_comment = false;
                index += 2;
            } else {
                cc_builder_append_char(&builder, ch);
                index++;
            }
            continue;
        }

        if (index + 1 < length && ch == '/' && cc_text_char(text, length, index + 1) == '*') {
            cc_builder_append_range(&builder, "/*", 2);
            preprocessor->in_block_comment = true;
            index += 2;
            continue;
        }

        if (index + 1 < length && ch == '/' && cc_text_char(text, length, index + 1) == '/') {
            cc_builder_append_range(&builder, text + index, length - index);
            break;
        }

        if (ch == '"' || ch == '\'') {
            char terminator;

            terminator = ch;
            cc_builder_append_char(&builder, ch);
            index++;

            while (index < length) {
                char inner_ch;

                inner_ch = cc_text_char(text, length, index);
                cc_builder_append_char(&builder, inner_ch);
                if (inner_ch == '\\' && index + 1 < length) {
                    index++;
                    cc_builder_append_char(&builder, cc_text_char(text, length, index));
                } else if (inner_ch == terminator) {
                    index++;
                    break;
                }
                index++;
            }
            continue;
        }

        if (cc_identifier_start(ch)) {
            size_t start;
            size_t name_end;
            char *identifier;
            CCMacro *macro;

            start = index;
            index++;
            while (index < length && cc_identifier_part(cc_text_char(text, length, index))) {
                index++;
            }
            name_end = index;

            identifier = cc_duplicate_range(text + start, name_end - start);
            macro = cc_find_macro(preprocessor, identifier);
            if (macro == NULL || macro->expanding) {
                cc_builder_append_range(&builder, text + start, name_end - start);
                free(identifier);
                continue;
            }

            if (!macro->is_function_like) {
                char *expanded;

                macro->expanding = true;
                expanded = cc_expand_text(preprocessor, macro->replacement, depth + 1);
                macro->expanding = false;
                cc_builder_append_cstring(&builder, expanded);
                free(expanded);
                free(identifier);
                continue;
            }

            {
                size_t after_name;
                char *expanded;

                after_name = cc_skip_spaces(text, length, name_end);
                if (after_name >= length || text[after_name] != '(') {
                    cc_builder_append_range(&builder, text + start, name_end - start);
                    free(identifier);
                    continue;
                }

                index = after_name;
                expanded = cc_expand_function_macro(preprocessor, macro, text, &index, depth);
                cc_builder_append_cstring(&builder, expanded);
                free(expanded);
            }

            free(identifier);
            continue;
        }

        cc_builder_append_char(&builder, ch);
        index++;
    }

    return cc_builder_take_string(&builder);
}

static void cc_process_define(
    CCPreprocessor *preprocessor,
    const char *path,
    const char *line_text,
    size_t line_length,
    size_t line_offset,
    size_t line_number
) {
    size_t index;
    CCMacro macro;

    memset(&macro, 0, sizeof(macro));
    index = 0;

    index = cc_skip_spaces(line_text, line_length, index);
    if (!cc_parse_identifier(line_text, line_length, &index, &macro.name)) {
        cc_add_diagnostic(preprocessor, path, line_offset, line_number, 1, 1, "expected macro name after #define");
        return;
    }

    if (index < line_length && line_text[index] == '(') {
        macro.is_function_like = true;
        if (!cc_parse_macro_parameters(line_text, line_length, &index, &macro.parameters, &macro.parameter_count)) {
            cc_add_diagnostic(
                preprocessor,
                path,
                line_offset,
                line_number,
                index + 1,
                1,
                "invalid function-like macro parameters"
            );
            cc_free_macro(&macro);
            return;
        }
    }

    index = cc_skip_spaces(line_text, line_length, index);
    macro.replacement = cc_duplicate_range(line_text + index, line_length - index);
    cc_store_macro(preprocessor, macro);
}

static void cc_process_conditional(
    CCPreprocessor *preprocessor,
    bool condition,
    bool is_else_branch
) {
    CCConditionalFrame *frame;
    bool parent_active;

    if (is_else_branch) {
        if (preprocessor->conditional_count == 0) {
            return;
        }

        frame = &preprocessor->conditionals[preprocessor->conditional_count - 1];
        frame->current_active = frame->parent_active && !frame->branch_taken;
        frame->branch_taken = true;
        return;
    }

    if (preprocessor->conditional_count == preprocessor->conditional_capacity) {
        cc_grow_conditionals(preprocessor);
    }

    parent_active = cc_current_block_active(preprocessor);
    frame = &preprocessor->conditionals[preprocessor->conditional_count++];
    frame->parent_active = parent_active;
    frame->current_active = frame->parent_active && condition;
    frame->branch_taken = condition;
}

static bool cc_simple_if_condition(CCPreprocessor *preprocessor, const char *text, size_t length) {
    char *identifier;
    size_t index;

    index = cc_skip_spaces(text, length, 0);
    if (index >= length) {
        return false;
    }

    if (text[index] == '0') {
        return false;
    }

    if (text[index] == '1') {
        return true;
    }

    if (strncmp(text + index, "defined", 7) == 0) {
        index += 7;
        index = cc_skip_spaces(text, length, index);
        if (index < length && text[index] == '(') {
            index++;
            index = cc_skip_spaces(text, length, index);
            if (cc_parse_identifier(text, length, &index, &identifier)) {
                bool defined;

                index = cc_skip_spaces(text, length, index);
                if (index >= length || text[index] != ')') {
                    free(identifier);
                    return false;
                }
                defined = cc_find_macro(preprocessor, identifier) != NULL;
                free(identifier);
                return defined;
            }
        } else if (cc_parse_identifier(text, length, &index, &identifier)) {
            bool defined;

            defined = cc_find_macro(preprocessor, identifier) != NULL;
            free(identifier);
            return defined;
        }
    }

    if (cc_parse_identifier(text, length, &index, &identifier)) {
        CCMacro *macro;
        bool result;

        macro = cc_find_macro(preprocessor, identifier);
        result = macro != NULL && strcmp(macro->replacement, "0") != 0;
        free(identifier);
        return result;
    }

    return false;
}

static void cc_preprocess_path(CCPreprocessor *preprocessor, const char *path, CCStringBuilder *output);

static void cc_process_directive(
    CCPreprocessor *preprocessor,
    const char *path,
    const char *line_text,
    size_t line_length,
    size_t line_offset,
    size_t line_number,
    CCStringBuilder *output
) {
    char *directive;
    size_t index;
    bool active;

    index = 0;
    active = cc_current_block_active(preprocessor);

    index = cc_skip_spaces(line_text, line_length, index);
    if (index >= line_length || line_text[index] != '#') {
        if (active) {
            char *expanded;
            char *line_copy;

            line_copy = cc_duplicate_range(line_text, line_length);
            expanded = cc_expand_text(preprocessor, line_copy, 0);
            cc_builder_append_cstring(output, expanded);
            free(line_copy);
            free(expanded);
        }
        cc_builder_append_char(output, '\n');
        return;
    }

    index++;
    index = cc_skip_spaces(line_text, line_length, index);

    if (!cc_parse_identifier(line_text, line_length, &index, &directive)) {
        cc_builder_append_char(output, '\n');
        return;
    }

    if (strcmp(directive, "include") == 0) {
        char *include_name;
        char resolved_path[CC_PATH_BUFFER_SIZE];

        if (active) {
            include_name = NULL;
            if (!cc_parse_include_name(line_text, line_length, &index, &include_name)) {
                cc_add_diagnostic(preprocessor, path, line_offset, line_number, 1, 1, "expected include path in quotes or angle brackets");
            } else if (!cc_resolve_include_path(path, include_name, true, resolved_path) || access(resolved_path, R_OK) != 0) {
                cc_add_diagnostic(
                    preprocessor,
                    path,
                    line_offset,
                    line_number,
                    1,
                    1,
                    "unable to open include '%s'",
                    include_name
                );
            } else {
                cc_preprocess_path(preprocessor, resolved_path, output);
            }
            free(include_name);
        }
        free(directive);
        return;
    }

    if (strcmp(directive, "define") == 0) {
        if (active) {
            cc_process_define(preprocessor, path, line_text + index, line_length - index, line_offset, line_number);
        }
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (strcmp(directive, "undef") == 0) {
        char *macro_name;

        if (active) {
            macro_name = NULL;
            index = cc_skip_spaces(line_text, line_length, index);
            if (cc_parse_identifier(line_text, line_length, &index, &macro_name)) {
                cc_remove_macro(preprocessor, macro_name);
            } else {
                cc_add_diagnostic(preprocessor, path, line_offset, line_number, 1, 1, "expected macro name after #undef");
            }
            free(macro_name);
        }
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (strcmp(directive, "ifdef") == 0 || strcmp(directive, "ifndef") == 0) {
        char *macro_name;
        bool condition;

        macro_name = NULL;
        index = cc_skip_spaces(line_text, line_length, index);
        if (!cc_parse_identifier(line_text, line_length, &index, &macro_name)) {
            cc_add_diagnostic(
                preprocessor,
                path,
                line_offset,
                line_number,
                1,
                1,
                "expected macro name after #%s",
                directive
            );
            condition = false;
        } else {
            condition = cc_find_macro(preprocessor, macro_name) != NULL;
            if (strcmp(directive, "ifndef") == 0) {
                condition = !condition;
            }
        }

        cc_process_conditional(preprocessor, condition, false);
        free(macro_name);
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (strcmp(directive, "if") == 0) {
        bool condition;

        condition = cc_simple_if_condition(preprocessor, line_text + index, line_length - index);
        cc_process_conditional(preprocessor, condition, false);
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (strcmp(directive, "elif") == 0) {
        bool condition;
        CCConditionalFrame *frame;

        if (preprocessor->conditional_count == 0) {
            cc_add_diagnostic(preprocessor, path, line_offset, line_number, 1, 1, "#elif without matching #if");
            cc_builder_append_char(output, '\n');
            free(directive);
            return;
        }

        frame = &preprocessor->conditionals[preprocessor->conditional_count - 1];
        condition = cc_simple_if_condition(preprocessor, line_text + index, line_length - index);
        frame->current_active = frame->parent_active && !frame->branch_taken && condition;
        if (condition) {
            frame->branch_taken = true;
        }
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (strcmp(directive, "else") == 0) {
        if (preprocessor->conditional_count == 0) {
            cc_add_diagnostic(preprocessor, path, line_offset, line_number, 1, 1, "#else without matching #if");
        } else {
            cc_process_conditional(preprocessor, false, true);
        }
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (strcmp(directive, "endif") == 0) {
        if (preprocessor->conditional_count == 0) {
            cc_add_diagnostic(preprocessor, path, line_offset, line_number, 1, 1, "#endif without matching #if");
        } else {
            preprocessor->conditional_count--;
        }
        cc_builder_append_char(output, '\n');
        free(directive);
        return;
    }

    if (active) {
        cc_add_diagnostic(
            preprocessor,
            path,
            line_offset,
            line_number,
            1,
            1,
            "unsupported preprocessor directive #%s",
            directive
        );
    }
    cc_builder_append_char(output, '\n');
    free(directive);
}

static void cc_preprocess_path(CCPreprocessor *preprocessor, const char *path, CCStringBuilder *output) {
    CCLoadedFile file;
    size_t index;
    size_t line_number;

    if (!cc_load_file(path, &file)) {
        cc_add_diagnostic(preprocessor, path, 0, 1, 1, 1, "unable to open source file");
        return;
    }

    index = 0;
    line_number = 1;

    while (index < file.length) {
        size_t line_start;
        size_t line_end;
        const char *line_text;
        size_t line_length;

        line_start = index;
        while (index < file.length && file.data[index] != '\n' && file.data[index] != '\r') {
            index++;
        }
        line_end = index;
        line_text = file.data + line_start;
        line_length = line_end - line_start;

        cc_process_directive(preprocessor, path, line_text, line_length, line_start, line_number, output);

        if (index < file.length && file.data[index] == '\r') {
            index++;
            if (index < file.length && file.data[index] == '\n') {
                index++;
            }
        } else if (index < file.length && file.data[index] == '\n') {
            index++;
        }

        line_number++;
    }

    cc_free_loaded_file(&file);
}

void cc_preprocess_file(const char *path, CCPreprocessResult *result) {
    CCPreprocessor preprocessor;
    CCStringBuilder output;

    memset(&preprocessor, 0, sizeof(preprocessor));
    memset(&output, 0, sizeof(output));

    cc_preprocess_path(&preprocessor, path, &output);

    if (preprocessor.conditional_count > 0) {
        cc_add_diagnostic(&preprocessor, path, 0, 1, 1, 1, "unterminated conditional directive");
    }

    memset(result, 0, sizeof(*result));
    result->text = cc_builder_take_string(&output);
    result->source.path = path;
    result->source.text = NULL;
    result->source.length = 0;
    result->diagnostics = preprocessor.diagnostics;

    while (preprocessor.macro_count > 0) {
        cc_free_macro(&preprocessor.macros[--preprocessor.macro_count]);
    }
    free(preprocessor.macros);
    free(preprocessor.conditionals);
}

void cc_preprocess_result_free(CCPreprocessResult *result) {
    size_t index;

    for (index = 0; index < result->diagnostics.count; index++) {
        free(result->diagnostics.items[index].path);
        free(result->diagnostics.items[index].message);
    }

    free(result->diagnostics.items);
    result->diagnostics.items = NULL;
    result->diagnostics.count = 0;
    result->diagnostics.capacity = 0;

    free(result->text);
    result->text = NULL;
    result->source.text = NULL;
    result->source.length = 0;
}
