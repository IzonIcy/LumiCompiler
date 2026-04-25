#include "ccompiler/sema.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef enum {
    CC_SEMA_TYPE_INVALID,
    CC_SEMA_TYPE_VOID,
    CC_SEMA_TYPE_BOOL,
    CC_SEMA_TYPE_CHAR,
    CC_SEMA_TYPE_INT,
    CC_SEMA_TYPE_LONG,
    CC_SEMA_TYPE_FLOAT,
    CC_SEMA_TYPE_DOUBLE,
    CC_SEMA_TYPE_POINTER,
    CC_SEMA_TYPE_FUNCTION
} CCSemaTypeKind;

typedef struct CCSemaType CCSemaType;

struct CCSemaType {
    CCSemaTypeKind kind;
    const CCSemaType *base;
    const CCSemaType **parameters;
    size_t parameter_count;
    bool variadic;
};

typedef enum {
    CC_SYMBOL_OBJECT,
    CC_SYMBOL_FUNCTION,
    CC_SYMBOL_TYPEDEF
} CCSymbolKind;

typedef struct {
    char *name;
    CCSymbolKind kind;
    const CCSemaType *type;
    CCSpan span;
    bool defined;
} CCSymbol;

typedef struct {
    CCSymbol *symbols;
    size_t count;
    size_t capacity;
} CCScope;

typedef struct {
    const CCSemaType *type;
    bool is_lvalue;
    bool is_function_designator;
    bool ok;
} CCSemaExpr;

typedef struct {
    char *name;
    CCSpan span;
} CCLabelRecord;

typedef struct {
    bool ok;
    long long value;
} CCConstValue;

typedef struct {
    long long *case_values;
    CCSpan *case_spans;
    size_t case_count;
    size_t case_capacity;
    bool has_default;
    CCSpan default_span;
} CCSwitchRecord;

typedef struct {
    const CCParseResult *parse_result;
    const CCSemaOptions *options;
    CCDiagnosticBuffer diagnostics;
    CCScope *scopes;
    size_t scope_count;
    size_t scope_capacity;
    CCSemaType **owned_types;
    size_t owned_type_count;
    size_t owned_type_capacity;
    size_t function_count;
    size_t global_count;
    size_t typedef_count;
    size_t break_depth;
    size_t loop_depth;
    size_t switch_depth;
    const CCSemaType *current_return_type;
    const char *current_function_name;
    CCLabelRecord *defined_labels;
    size_t defined_label_count;
    size_t defined_label_capacity;
    CCLabelRecord *goto_references;
    size_t goto_reference_count;
    size_t goto_reference_capacity;
    CCSwitchRecord *switch_records;
    size_t switch_record_count;
    size_t switch_record_capacity;
} CCSemaContext;

static const CCSemaType cc_invalid_type = {
    .kind = CC_SEMA_TYPE_INVALID,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_void_type = {
    .kind = CC_SEMA_TYPE_VOID,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_bool_type = {
    .kind = CC_SEMA_TYPE_BOOL,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_char_type = {
    .kind = CC_SEMA_TYPE_CHAR,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_int_type = {
    .kind = CC_SEMA_TYPE_INT,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_long_type = {
    .kind = CC_SEMA_TYPE_LONG,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_float_type = {
    .kind = CC_SEMA_TYPE_FLOAT,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static const CCSemaType cc_double_type = {
    .kind = CC_SEMA_TYPE_DOUBLE,
    .base = NULL,
    .parameters = NULL,
    .parameter_count = 0,
    .variadic = false,
};

static bool cc_type_equal(const CCSemaType *left, const CCSemaType *right);

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

static void cc_grow_diagnostic_buffer(CCDiagnosticBuffer *buffer) {
    size_t new_capacity;

    new_capacity = buffer->capacity == 0 ? 8 : buffer->capacity * 2;
    buffer->items = cc_reallocate_or_die(buffer->items, new_capacity * sizeof(*buffer->items));
    buffer->capacity = new_capacity;
}

static void cc_add_diagnostic(CCSemaContext *context, CCSpan span, const char *format, ...) {
    char small_buffer[256];
    char *message;
    int needed;
    va_list args;
    va_list copy;

    if (context->diagnostics.count == context->diagnostics.capacity) {
        cc_grow_diagnostic_buffer(&context->diagnostics);
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
        size_t length;

        length = (size_t)needed + 1;
        message = cc_reallocate_or_die(NULL, length);
        vsnprintf(message, length, format, copy);
        va_end(copy);
    }

    if (span.length == 0 && span.offset < context->parse_result->source.length) {
        span.length = 1;
    }

    context->diagnostics.items[context->diagnostics.count].span = span;
    context->diagnostics.items[context->diagnostics.count].path = NULL;
    context->diagnostics.items[context->diagnostics.count].message = message;
    context->diagnostics.items[context->diagnostics.count].severity = CC_DIAGNOSTIC_ERROR;
    context->diagnostics.count++;
}

static void cc_add_warning(CCSemaContext *context, CCSpan span, const char *format, ...) {
    char small_buffer[256];
    char *message;
    int needed;
    va_list args;
    va_list copy;

    if (!context->options->warnings_enabled) {
        return;
    }

    if (context->diagnostics.count == context->diagnostics.capacity) {
        cc_grow_diagnostic_buffer(&context->diagnostics);
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
        size_t length;

        length = (size_t)needed + 1;
        message = cc_reallocate_or_die(NULL, length);
        vsnprintf(message, length, format, copy);
        va_end(copy);
    }

    if (span.length == 0 && span.offset < context->parse_result->source.length) {
        span.length = 1;
    }

    context->diagnostics.items[context->diagnostics.count].span = span;
    context->diagnostics.items[context->diagnostics.count].path = NULL;
    context->diagnostics.items[context->diagnostics.count].message = message;
    context->diagnostics.items[context->diagnostics.count].severity = CC_DIAGNOSTIC_WARNING;
    context->diagnostics.count++;
}

static void cc_grow_label_records(CCLabelRecord **items, size_t *capacity) {
    size_t new_capacity;

    new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    *items = cc_reallocate_or_die(*items, new_capacity * sizeof(**items));
    *capacity = new_capacity;
}

static const CCLabelRecord *cc_find_label_record(const CCLabelRecord *items, size_t count, const char *name) {
    size_t index;

    if (name == NULL) {
        return NULL;
    }

    for (index = 0; index < count; index++) {
        if (strcmp(items[index].name, name) == 0) {
            return &items[index];
        }
    }

    return NULL;
}

static void cc_add_defined_label(CCSemaContext *context, const char *name, CCSpan span) {
    if (name == NULL || name[0] == '\0') {
        return;
    }

    if (cc_find_label_record(context->defined_labels, context->defined_label_count, name) != NULL) {
        const CCLabelRecord *existing;

        existing = cc_find_label_record(context->defined_labels, context->defined_label_count, name);
        cc_add_diagnostic(
            context,
            span,
            "redefinition of label '%s' (previous definition on line %zu)",
            name,
            existing == NULL ? 0 : existing->span.line
        );
        return;
    }

    if (context->defined_label_count == context->defined_label_capacity) {
        cc_grow_label_records(&context->defined_labels, &context->defined_label_capacity);
    }

    context->defined_labels[context->defined_label_count].name = cc_duplicate_string(name);
    context->defined_labels[context->defined_label_count].span = span;
    context->defined_label_count++;
}

static void cc_add_goto_reference(CCSemaContext *context, const char *name, CCSpan span) {
    if (name == NULL || name[0] == '\0') {
        return;
    }

    if (context->goto_reference_count == context->goto_reference_capacity) {
        cc_grow_label_records(&context->goto_references, &context->goto_reference_capacity);
    }

    context->goto_references[context->goto_reference_count].name = cc_duplicate_string(name);
    context->goto_references[context->goto_reference_count].span = span;
    context->goto_reference_count++;
}

static void cc_clear_label_records(CCLabelRecord **items, size_t *count, size_t *capacity) {
    size_t index;

    for (index = 0; index < *count; index++) {
        free((*items)[index].name);
    }

    free(*items);
    *items = NULL;
    *count = 0;
    *capacity = 0;
}

static void cc_grow_switch_records(CCSemaContext *context) {
    size_t new_capacity;

    new_capacity = context->switch_record_capacity == 0 ? 4 : context->switch_record_capacity * 2;
    context->switch_records = cc_reallocate_or_die(
        context->switch_records,
        new_capacity * sizeof(*context->switch_records)
    );
    context->switch_record_capacity = new_capacity;
}

static void cc_grow_switch_cases(CCSwitchRecord *record) {
    size_t new_capacity;

    new_capacity = record->case_capacity == 0 ? 4 : record->case_capacity * 2;
    record->case_values = cc_reallocate_or_die(record->case_values, new_capacity * sizeof(*record->case_values));
    record->case_spans = cc_reallocate_or_die(record->case_spans, new_capacity * sizeof(*record->case_spans));
    record->case_capacity = new_capacity;
}

static void cc_push_switch_record(CCSemaContext *context) {
    if (context->switch_record_count == context->switch_record_capacity) {
        cc_grow_switch_records(context);
    }

    memset(&context->switch_records[context->switch_record_count], 0, sizeof(context->switch_records[0]));
    context->switch_record_count++;
}

static CCSwitchRecord *cc_current_switch_record(CCSemaContext *context) {
    if (context->switch_record_count == 0) {
        return NULL;
    }

    return &context->switch_records[context->switch_record_count - 1];
}

static void cc_record_switch_case(CCSemaContext *context, long long value, CCSpan span) {
    CCSwitchRecord *record;
    size_t index;

    record = cc_current_switch_record(context);
    if (record == NULL) {
        return;
    }

    for (index = 0; index < record->case_count; index++) {
        if (record->case_values[index] == value) {
            cc_add_diagnostic(
                context,
                span,
                "duplicate case value %lld (previous case on line %zu)",
                value,
                record->case_spans[index].line
            );
            return;
        }
    }

    if (record->case_count == record->case_capacity) {
        cc_grow_switch_cases(record);
    }

    record->case_values[record->case_count] = value;
    record->case_spans[record->case_count] = span;
    record->case_count++;
}

static void cc_record_switch_default(CCSemaContext *context, CCSpan span) {
    CCSwitchRecord *record;

    record = cc_current_switch_record(context);
    if (record == NULL) {
        return;
    }

    if (record->has_default) {
        cc_add_diagnostic(
            context,
            span,
            "duplicate default label (previous default on line %zu)",
            record->default_span.line
        );
        return;
    }

    record->has_default = true;
    record->default_span = span;
}

static void cc_pop_switch_record(CCSemaContext *context) {
    CCSwitchRecord *record;

    if (context->switch_record_count == 0) {
        return;
    }

    context->switch_record_count--;
    record = &context->switch_records[context->switch_record_count];
    free(record->case_values);
    free(record->case_spans);
    memset(record, 0, sizeof(*record));
}

static void cc_clear_switch_records(CCSemaContext *context) {
    while (context->switch_record_count > 0) {
        cc_pop_switch_record(context);
    }

    free(context->switch_records);
    context->switch_records = NULL;
    context->switch_record_capacity = 0;
}

static void cc_grow_scopes(CCSemaContext *context) {
    size_t new_capacity;

    new_capacity = context->scope_capacity == 0 ? 4 : context->scope_capacity * 2;
    context->scopes = cc_reallocate_or_die(context->scopes, new_capacity * sizeof(*context->scopes));
    context->scope_capacity = new_capacity;
}

static void cc_grow_symbols(CCScope *scope) {
    size_t new_capacity;

    new_capacity = scope->capacity == 0 ? 8 : scope->capacity * 2;
    scope->symbols = cc_reallocate_or_die(scope->symbols, new_capacity * sizeof(*scope->symbols));
    scope->capacity = new_capacity;
}

static void cc_push_scope(CCSemaContext *context) {
    if (context->scope_count == context->scope_capacity) {
        cc_grow_scopes(context);
    }

    memset(&context->scopes[context->scope_count], 0, sizeof(context->scopes[context->scope_count]));
    context->scope_count++;
}

static void cc_free_scope(CCScope *scope) {
    size_t index;

    for (index = 0; index < scope->count; index++) {
        free(scope->symbols[index].name);
    }

    free(scope->symbols);
    scope->symbols = NULL;
    scope->count = 0;
    scope->capacity = 0;
}

static void cc_pop_scope(CCSemaContext *context) {
    if (context->scope_count == 0) {
        return;
    }

    context->scope_count--;
    cc_free_scope(&context->scopes[context->scope_count]);
}

static CCScope *cc_current_scope(CCSemaContext *context) {
    if (context->scope_count == 0) {
        return NULL;
    }

    return &context->scopes[context->scope_count - 1];
}

static const CCSymbol *cc_lookup_symbol(const CCSemaContext *context, const char *name) {
    size_t scope_index;

    for (scope_index = context->scope_count; scope_index > 0; scope_index--) {
        const CCScope *scope;
        size_t symbol_index;

        scope = &context->scopes[scope_index - 1];
        for (symbol_index = 0; symbol_index < scope->count; symbol_index++) {
            if (strcmp(scope->symbols[symbol_index].name, name) == 0) {
                return &scope->symbols[symbol_index];
            }
        }
    }

    return NULL;
}

static CCSymbol *cc_lookup_symbol_in_scope(CCScope *scope, const char *name) {
    size_t index;

    for (index = 0; index < scope->count; index++) {
        if (strcmp(scope->symbols[index].name, name) == 0) {
            return &scope->symbols[index];
        }
    }

    return NULL;
}

static void cc_grow_owned_types(CCSemaContext *context) {
    size_t new_capacity;

    new_capacity = context->owned_type_capacity == 0 ? 8 : context->owned_type_capacity * 2;
    context->owned_types = cc_reallocate_or_die(context->owned_types, new_capacity * sizeof(*context->owned_types));
    context->owned_type_capacity = new_capacity;
}

static const CCSemaType *cc_take_owned_type(CCSemaContext *context, CCSemaType *type) {
    if (context->owned_type_count == context->owned_type_capacity) {
        cc_grow_owned_types(context);
    }

    context->owned_types[context->owned_type_count++] = type;
    return type;
}

static const CCSemaType *cc_find_pointer_type(const CCSemaContext *context, const CCSemaType *base) {
    size_t index;

    for (index = 0; index < context->owned_type_count; index++) {
        const CCSemaType *candidate;

        candidate = context->owned_types[index];
        if (candidate->kind == CC_SEMA_TYPE_POINTER && candidate->base == base) {
            return candidate;
        }
    }

    return NULL;
}

static bool cc_parameter_lists_equal(
    const CCSemaType *const *left_parameters,
    size_t left_count,
    const CCSemaType *const *right_parameters,
    size_t right_count
) {
    size_t index;

    if (left_count != right_count) {
        return false;
    }

    for (index = 0; index < left_count; index++) {
        if (!cc_type_equal(left_parameters[index], right_parameters[index])) {
            return false;
        }
    }

    return true;
}

static const CCSemaType *cc_find_function_type(
    const CCSemaContext *context,
    const CCSemaType *return_type,
    const CCSemaType *const *parameters,
    size_t parameter_count,
    bool variadic
) {
    size_t index;

    for (index = 0; index < context->owned_type_count; index++) {
        const CCSemaType *candidate;

        candidate = context->owned_types[index];
        if (candidate->kind != CC_SEMA_TYPE_FUNCTION) {
            continue;
        }

        if (!cc_type_equal(candidate->base, return_type)
            || candidate->variadic != variadic
            || !cc_parameter_lists_equal(candidate->parameters, candidate->parameter_count, parameters, parameter_count)) {
            continue;
        }

        return candidate;
    }

    return NULL;
}

static const CCSemaType *cc_new_pointer_type(CCSemaContext *context, const CCSemaType *base) {
    CCSemaType *type;
    const CCSemaType *existing;

    existing = cc_find_pointer_type(context, base);
    if (existing != NULL) {
        return existing;
    }

    type = cc_reallocate_or_die(NULL, sizeof(*type));
    type->kind = CC_SEMA_TYPE_POINTER;
    type->base = base;
    type->parameters = NULL;
    type->parameter_count = 0;
    type->variadic = false;
    return cc_take_owned_type(context, type);
}

static const CCSemaType *cc_new_function_type(
    CCSemaContext *context,
    const CCSemaType *return_type,
    const CCSemaType **parameters,
    size_t parameter_count,
    bool variadic
) {
    CCSemaType *type;
    const CCSemaType **owned_parameters;
    const CCSemaType *existing;

    existing = cc_find_function_type(context, return_type, parameters, parameter_count, variadic);
    if (existing != NULL) {
        return existing;
    }

    type = cc_reallocate_or_die(NULL, sizeof(*type));
    type->kind = CC_SEMA_TYPE_FUNCTION;
    type->base = return_type;

    if (parameter_count > 0) {
        owned_parameters = cc_reallocate_or_die(NULL, parameter_count * sizeof(*owned_parameters));
        memcpy((void *)owned_parameters, parameters, parameter_count * sizeof(*owned_parameters));
    } else {
        owned_parameters = NULL;
    }

    type->parameters = owned_parameters;
    type->parameter_count = parameter_count;
    type->variadic = variadic;
    return cc_take_owned_type(context, type);
}

static const CCSemaType *cc_pointer_to_char(CCSemaContext *context) {
    return cc_new_pointer_type(context, &cc_char_type);
}

static const char *cc_type_name(const CCSemaType *type) {
    switch (type->kind) {
        case CC_SEMA_TYPE_VOID:
            return "void";
        case CC_SEMA_TYPE_BOOL:
            return "_Bool";
        case CC_SEMA_TYPE_CHAR:
            return "char";
        case CC_SEMA_TYPE_INT:
            return "int";
        case CC_SEMA_TYPE_LONG:
            return "long";
        case CC_SEMA_TYPE_FLOAT:
            return "float";
        case CC_SEMA_TYPE_DOUBLE:
            return "double";
        case CC_SEMA_TYPE_POINTER:
            return "pointer";
        case CC_SEMA_TYPE_FUNCTION:
            return "function";
        case CC_SEMA_TYPE_INVALID:
        default:
            return "<invalid>";
    }
}

static bool cc_type_equal(const CCSemaType *left, const CCSemaType *right) {
    size_t index;

    if (left == right) {
        return true;
    }

    if (left == NULL || right == NULL || left->kind != right->kind) {
        return false;
    }

    switch (left->kind) {
        case CC_SEMA_TYPE_POINTER:
            return cc_type_equal(left->base, right->base);
        case CC_SEMA_TYPE_FUNCTION:
            if (!cc_type_equal(left->base, right->base)
                || left->parameter_count != right->parameter_count
                || left->variadic != right->variadic) {
                return false;
            }

            for (index = 0; index < left->parameter_count; index++) {
                if (!cc_type_equal(left->parameters[index], right->parameters[index])) {
                    return false;
                }
            }
            return true;
        default:
            return true;
    }
}

static bool cc_type_is_integer(const CCSemaType *type) {
    return type == &cc_bool_type
        || type == &cc_char_type
        || type == &cc_int_type
        || type == &cc_long_type;
}

static bool cc_type_is_floating(const CCSemaType *type) {
    return type == &cc_float_type || type == &cc_double_type;
}

static bool cc_type_is_numeric(const CCSemaType *type) {
    return cc_type_is_integer(type) || cc_type_is_floating(type);
}

static bool cc_type_is_pointer(const CCSemaType *type) {
    return type != NULL && type->kind == CC_SEMA_TYPE_POINTER;
}

static bool cc_type_is_void(const CCSemaType *type) {
    return type == &cc_void_type;
}

static bool cc_type_is_scalar(const CCSemaType *type) {
    return cc_type_is_numeric(type) || cc_type_is_pointer(type);
}

static const CCSemaType *cc_arithmetic_result_type(const CCSemaType *left, const CCSemaType *right) {
    if (left == &cc_double_type || right == &cc_double_type) {
        return &cc_double_type;
    }

    if (left == &cc_float_type || right == &cc_float_type) {
        return &cc_float_type;
    }

    if (left == &cc_long_type || right == &cc_long_type) {
        return &cc_long_type;
    }

    return &cc_int_type;
}

static bool cc_types_assignable(const CCSemaType *target, const CCSemaType *value) {
    const CCSemaType *target_base;
    const CCSemaType *value_base;

    if (target == NULL || value == NULL || target == &cc_invalid_type || value == &cc_invalid_type) {
        return true;
    }

    if (cc_type_equal(target, value)) {
        return true;
    }

    if (cc_type_is_numeric(target) && cc_type_is_numeric(value)) {
        return true;
    }

    if (cc_type_is_pointer(target) && cc_type_is_pointer(value)) {
        target_base = target->base;
        value_base = value->base;
        if (target_base == NULL || value_base == NULL) {
            return false;
        }

        if (target_base->kind == CC_SEMA_TYPE_FUNCTION || value_base->kind == CC_SEMA_TYPE_FUNCTION) {
            return cc_type_equal(target_base, value_base);
        }

        return cc_type_equal(target_base, value_base)
            || cc_type_is_void(target_base)
            || cc_type_is_void(value_base);
    }

    return false;
}

static bool cc_types_castable(const CCSemaType *target, const CCSemaType *value) {
    if (target == NULL || value == NULL || target == &cc_invalid_type || value == &cc_invalid_type) {
        return true;
    }

    if (cc_type_equal(target, value)) {
        return true;
    }

    if (cc_type_is_void(target)) {
        return true;
    }

    if (cc_type_is_scalar(target) && cc_type_is_scalar(value)) {
        return true;
    }

    return false;
}

static const CCAstNode *cc_find_parameter_list(const CCAstNode *declarator) {
    size_t index;

    if (declarator == NULL) {
        return NULL;
    }

    for (index = 0; index < declarator->child_count; index++) {
        const CCAstNode *child;

        child = declarator->children[index];
        if (child->kind == CC_AST_PARAMETER_LIST) {
            return child;
        }
    }

    return NULL;
}

static const CCAstNode *cc_find_specifier_list(const CCAstNode *node) {
    size_t index;

    if (node == NULL) {
        return NULL;
    }

    for (index = 0; index < node->child_count; index++) {
        if (node->children[index]->kind == CC_AST_DECLARATION_SPECIFIERS) {
            return node->children[index];
        }
    }

    return NULL;
}

static bool cc_specifiers_contain_text(const CCAstNode *specifiers, const char *text) {
    size_t index;

    if (specifiers == NULL) {
        return false;
    }

    for (index = 0; index < specifiers->child_count; index++) {
        const CCAstNode *child;

        child = specifiers->children[index];
        if (child->text != NULL && strcmp(child->text, text) == 0) {
            return true;
        }
    }

    return false;
}

static const CCSemaType *cc_type_from_specifiers(CCSemaContext *context, const CCAstNode *specifiers) {
    bool saw_void;
    bool saw_bool;
    bool saw_char;
    bool saw_int;
    bool saw_long;
    bool saw_float;
    bool saw_double;
    const CCSemaType *typedef_type;
    size_t index;

    saw_void = false;
    saw_bool = false;
    saw_char = false;
    saw_int = false;
    saw_long = false;
    saw_float = false;
    saw_double = false;
    typedef_type = NULL;

    if (specifiers == NULL) {
        return &cc_invalid_type;
    }

    for (index = 0; index < specifiers->child_count; index++) {
        const CCAstNode *child;
        const CCSymbol *symbol;

        child = specifiers->children[index];
        if (child->text == NULL) {
            continue;
        }

        if (strcmp(child->text, "void") == 0) {
            saw_void = true;
            continue;
        }
        if (strcmp(child->text, "_Bool") == 0 || strcmp(child->text, "bool") == 0) {
            saw_bool = true;
            continue;
        }
        if (strcmp(child->text, "char") == 0) {
            saw_char = true;
            continue;
        }
        if (strcmp(child->text, "int") == 0 || strcmp(child->text, "short") == 0) {
            saw_int = true;
            continue;
        }
        if (strcmp(child->text, "long") == 0) {
            saw_long = true;
            continue;
        }
        if (strcmp(child->text, "float") == 0) {
            saw_float = true;
            continue;
        }
        if (strcmp(child->text, "double") == 0) {
            saw_double = true;
            continue;
        }

        symbol = cc_lookup_symbol(context, child->text);
        if (symbol != NULL && symbol->kind == CC_SYMBOL_TYPEDEF) {
            typedef_type = symbol->type;
        }
    }

    if (typedef_type != NULL) {
        return typedef_type;
    }
    if (saw_void) {
        return &cc_void_type;
    }
    if (saw_bool) {
        return &cc_bool_type;
    }
    if (saw_char) {
        return &cc_char_type;
    }
    if (saw_double) {
        return &cc_double_type;
    }
    if (saw_float) {
        return &cc_float_type;
    }
    if (saw_long) {
        return &cc_long_type;
    }
    if (saw_int) {
        return &cc_int_type;
    }

    cc_add_diagnostic(context, specifiers->span, "unable to resolve declaration type");
    return &cc_invalid_type;
}

static const CCSemaType *cc_type_from_parameter(CCSemaContext *context, const CCAstNode *parameter);

static const CCSemaType *cc_apply_declarator_type(
    CCSemaContext *context,
    const CCAstNode *declarator,
    const CCSemaType *base_type
) {
    const CCSemaType *type;
    const CCAstNode *parameter_list;
    size_t index;

    if (declarator == NULL) {
        return base_type;
    }

    type = base_type;

    for (index = 0; index < declarator->child_count; index++) {
        const CCAstNode *child;

        child = declarator->children[index];
        if (child->kind == CC_AST_POINTER || child->kind == CC_AST_ARRAY_DECLARATOR) {
            type = cc_new_pointer_type(context, type);
        }
    }

    parameter_list = cc_find_parameter_list(declarator);
    if (parameter_list != NULL) {
        const CCSemaType **parameters;
        size_t parameter_count;
        size_t parameter_capacity;
        bool variadic;

        parameters = NULL;
        parameter_count = 0;
        parameter_capacity = 0;
        variadic = false;

        if (parameter_list->child_count == 1
            && parameter_list->children[0]->kind == CC_AST_PARAMETER
            && parameter_list->children[0]->text == NULL
            && cc_specifiers_contain_text(cc_find_specifier_list(parameter_list->children[0]), "void")) {
            parameter_count = 0;
        } else {
            for (index = 0; index < parameter_list->child_count; index++) {
                const CCAstNode *child;

                child = parameter_list->children[index];
                if (child->kind != CC_AST_PARAMETER) {
                    continue;
                }

                if (child->text != NULL && strcmp(child->text, "...") == 0) {
                    variadic = true;
                    break;
                }

                if (parameter_count == parameter_capacity) {
                    size_t new_capacity;

                    new_capacity = parameter_capacity == 0 ? 4 : parameter_capacity * 2;
                    parameters = cc_reallocate_or_die(parameters, new_capacity * sizeof(*parameters));
                    parameter_capacity = new_capacity;
                }

                parameters[parameter_count++] = cc_type_from_parameter(context, child);
            }
        }

        type = cc_new_function_type(context, type, parameters, parameter_count, variadic);
        free(parameters);
    }

    for (index = 0; index < declarator->child_count; index++) {
        const CCAstNode *child;

        child = declarator->children[index];
        if (child->kind == CC_AST_DECLARATOR) {
            type = cc_apply_declarator_type(context, child, type);
        }
    }

    return type;
}

static const CCSemaType *cc_type_from_parameter(CCSemaContext *context, const CCAstNode *parameter) {
    const CCAstNode *specifiers;
    const CCAstNode *declarator;
    const CCSemaType *base_type;
    size_t index;

    specifiers = NULL;
    declarator = NULL;

    for (index = 0; index < parameter->child_count; index++) {
        if (parameter->children[index]->kind == CC_AST_DECLARATION_SPECIFIERS) {
            specifiers = parameter->children[index];
        } else if (parameter->children[index]->kind == CC_AST_DECLARATOR) {
            declarator = parameter->children[index];
        }
    }

    base_type = cc_type_from_specifiers(context, specifiers);
    base_type = cc_apply_declarator_type(context, declarator, base_type);
    if (base_type->kind == CC_SEMA_TYPE_FUNCTION) {
        return cc_new_pointer_type(context, base_type);
    }

    return base_type;
}

static const CCAstNode *cc_declaration_specifiers(const CCAstNode *declaration) {
    if (declaration != NULL
        && declaration->child_count > 0
        && declaration->children[0]->kind == CC_AST_DECLARATION_SPECIFIERS) {
        return declaration->children[0];
    }

    return NULL;
}

static const CCAstNode *cc_init_declarator_declarator(const CCAstNode *init_declarator) {
    if (init_declarator != NULL
        && init_declarator->child_count > 0
        && init_declarator->children[0]->kind == CC_AST_DECLARATOR) {
        return init_declarator->children[0];
    }

    return NULL;
}

static const CCAstNode *cc_init_declarator_initializer(const CCAstNode *init_declarator) {
    if (init_declarator != NULL && init_declarator->child_count > 1) {
        return init_declarator->children[1];
    }

    return NULL;
}

static CCSymbol *cc_add_symbol_to_current_scope(
    CCSemaContext *context,
    const char *name,
    CCSymbolKind kind,
    const CCSemaType *type,
    CCSpan span,
    bool defined,
    bool *was_added
) {
    CCScope *scope;
    CCSymbol *existing;

    *was_added = false;
    scope = cc_current_scope(context);
    if (scope == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }

    existing = cc_lookup_symbol_in_scope(scope, name);
    if (existing != NULL) {
        if (existing->kind != kind) {
            cc_add_diagnostic(
                context,
                span,
                "conflicting declaration of '%s' (previous declaration on line %zu)",
                name,
                existing->span.line
            );
            return existing;
        }

        if (kind == CC_SYMBOL_FUNCTION) {
            if (!cc_type_equal(existing->type, type)) {
                cc_add_diagnostic(
                    context,
                    span,
                    "conflicting function declaration for '%s' (previous declaration on line %zu)",
                    name,
                    existing->span.line
                );
                return existing;
            }

            if (defined && existing->defined) {
                cc_add_diagnostic(
                    context,
                    span,
                    "redefinition of function '%s' (previous definition on line %zu)",
                    name,
                    existing->span.line
                );
                return existing;
            }

            if (defined) {
                existing->defined = true;
                existing->span = span;
            }
            return existing;
        }

        cc_add_diagnostic(
            context,
            span,
            "redeclaration of '%s' in the same scope (previous declaration on line %zu)",
            name,
            existing->span.line
        );
        return existing;
    }

    if (scope->count == scope->capacity) {
        cc_grow_symbols(scope);
    }

    scope->symbols[scope->count].name = cc_duplicate_string(name);
    scope->symbols[scope->count].kind = kind;
    scope->symbols[scope->count].type = type;
    scope->symbols[scope->count].span = span;
    scope->symbols[scope->count].defined = defined;
    *was_added = true;
    scope->count++;
    return &scope->symbols[scope->count - 1];
}

static CCSemaExpr cc_invalid_expr(void) {
    CCSemaExpr expr;

    expr.type = &cc_invalid_type;
    expr.is_lvalue = false;
    expr.is_function_designator = false;
    expr.ok = false;
    return expr;
}

static CCSemaExpr cc_make_expr(const CCSemaType *type, bool is_lvalue, bool is_function_designator) {
    CCSemaExpr expr;

    expr.type = type;
    expr.is_lvalue = is_lvalue;
    expr.is_function_designator = is_function_designator;
    expr.ok = true;
    return expr;
}

static bool cc_is_assignment_operator(const char *text) {
    static const char *const operators[] = {
        "=",
        "+=",
        "-=",
        "*=",
        "/=",
        "%=",
        "<<=",
        ">>=",
        "&=",
        "^=",
        "|=",
    };
    size_t index;

    if (text == NULL) {
        return false;
    }

    for (index = 0; index < CC_ARRAY_COUNT(operators); index++) {
        if (strcmp(text, operators[index]) == 0) {
            return true;
        }
    }

    return false;
}

static bool cc_is_comparison_operator(const char *text) {
    static const char *const operators[] = {
        "<",
        "<=",
        ">",
        ">=",
        "==",
        "!=",
    };
    size_t index;

    if (text == NULL) {
        return false;
    }

    for (index = 0; index < CC_ARRAY_COUNT(operators); index++) {
        if (strcmp(text, operators[index]) == 0) {
            return true;
        }
    }

    return false;
}

static bool cc_is_bitwise_operator(const char *text) {
    static const char *const operators[] = {
        "&",
        "|",
        "^",
        "<<",
        ">>",
        "%",
    };
    size_t index;

    if (text == NULL) {
        return false;
    }

    for (index = 0; index < CC_ARRAY_COUNT(operators); index++) {
        if (strcmp(text, operators[index]) == 0) {
            return true;
        }
    }

    return false;
}

static bool cc_is_arithmetic_operator(const char *text) {
    static const char *const operators[] = {
        "+",
        "-",
        "*",
        "/",
    };
    size_t index;

    if (text == NULL) {
        return false;
    }

    for (index = 0; index < CC_ARRAY_COUNT(operators); index++) {
        if (strcmp(text, operators[index]) == 0) {
            return true;
        }
    }

    return false;
}

static const CCSemaType *cc_literal_type(CCSemaContext *context, const char *text) {
    size_t length;

    if (text == NULL || text[0] == '\0') {
        return &cc_invalid_type;
    }

    length = strlen(text);

    if (strchr(text, '"') != NULL) {
        return cc_pointer_to_char(context);
    }

    if (text[0] == '\'') {
        return &cc_char_type;
    }

    if (strpbrk(text, ".eEpP") != NULL) {
        return &cc_double_type;
    }

    if (length > 0 && (text[length - 1] == 'l' || text[length - 1] == 'L')) {
        return &cc_long_type;
    }

    return &cc_int_type;
}

static bool cc_parse_integer_literal(const char *text, long long *value) {
    char *copy;
    char *end;
    size_t length;
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    length = strlen(text);
    while (length > 0) {
        char suffix;

        suffix = text[length - 1];
        if (suffix == 'u' || suffix == 'U' || suffix == 'l' || suffix == 'L') {
            length--;
            continue;
        }
        break;
    }

    copy = cc_reallocate_or_die(NULL, length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';

    if (length > 2 && copy[0] == '0' && (copy[1] == 'b' || copy[1] == 'B')) {
        long long parsed;

        parsed = 0;
        for (index = 2; index < length; index++) {
            if (copy[index] != '0' && copy[index] != '1') {
                free(copy);
                return false;
            }
            parsed = (parsed << 1) | (copy[index] - '0');
        }
        *value = parsed;
        free(copy);
        return true;
    }

    *value = strtoll(copy, &end, 0);
    {
        bool ok;

        ok = end != NULL && *end == '\0';
        free(copy);
        return ok;
    }
}

static bool cc_parse_char_literal(const char *text, long long *value) {
    const char *quote;
    unsigned char ch;

    if (text == NULL) {
        return false;
    }

    quote = strchr(text, '\'');
    if (quote == NULL || quote[1] == '\0') {
        return false;
    }

    quote++;
    if (*quote == '\\') {
        quote++;
        if (*quote == '0') {
            *value = 0;
            return true;
        }
        if (*quote == 'n') {
            *value = '\n';
            return true;
        }
        if (*quote == 'r') {
            *value = '\r';
            return true;
        }
        if (*quote == 't') {
            *value = '\t';
            return true;
        }
        if (*quote == '\\') {
            *value = '\\';
            return true;
        }
        if (*quote == '\'') {
            *value = '\'';
            return true;
        }
        if (*quote == '"') {
            *value = '"';
            return true;
        }
        if (*quote == 'x') {
            char *end;

            *value = strtoll(quote + 1, &end, 16);
            return end != quote + 1;
        }
        if (*quote >= '0' && *quote <= '7') {
            char buffer[8];
            size_t index;

            index = 0;
            while (index < sizeof(buffer) - 1 && quote[index] >= '0' && quote[index] <= '7') {
                buffer[index] = quote[index];
                index++;
            }
            buffer[index] = '\0';
            *value = strtoll(buffer, NULL, 8);
            return true;
        }
        *value = (unsigned char)*quote;
        return true;
    }

    ch = (unsigned char)*quote;
    *value = ch;
    return true;
}

static CCConstValue cc_invalid_const_value(void) {
    CCConstValue value;

    value.ok = false;
    value.value = 0;
    return value;
}

static CCConstValue cc_make_const_value(long long value) {
    CCConstValue result;

    result.ok = true;
    result.value = value;
    return result;
}

static CCConstValue cc_try_fold_integer_constant(const CCAstNode *node);

static CCConstValue cc_try_fold_binary_constant(const CCAstNode *node) {
    CCConstValue left;
    CCConstValue right;

    if (node->child_count < 2) {
        return cc_invalid_const_value();
    }

    left = cc_try_fold_integer_constant(node->children[0]);
    right = cc_try_fold_integer_constant(node->children[1]);
    if (!left.ok || !right.ok || node->text == NULL) {
        return cc_invalid_const_value();
    }

    if (strcmp(node->text, "+") == 0) {
        return cc_make_const_value(left.value + right.value);
    }
    if (strcmp(node->text, "-") == 0) {
        return cc_make_const_value(left.value - right.value);
    }
    if (strcmp(node->text, "*") == 0) {
        return cc_make_const_value(left.value * right.value);
    }
    if (strcmp(node->text, "/") == 0 && right.value != 0) {
        return cc_make_const_value(left.value / right.value);
    }
    if (strcmp(node->text, "%") == 0 && right.value != 0) {
        return cc_make_const_value(left.value % right.value);
    }
    if (strcmp(node->text, "<<") == 0) {
        return cc_make_const_value(left.value << right.value);
    }
    if (strcmp(node->text, ">>") == 0) {
        return cc_make_const_value(left.value >> right.value);
    }
    if (strcmp(node->text, "&") == 0) {
        return cc_make_const_value(left.value & right.value);
    }
    if (strcmp(node->text, "|") == 0) {
        return cc_make_const_value(left.value | right.value);
    }
    if (strcmp(node->text, "^") == 0) {
        return cc_make_const_value(left.value ^ right.value);
    }
    if (strcmp(node->text, "&&") == 0) {
        return cc_make_const_value((left.value != 0) && (right.value != 0));
    }
    if (strcmp(node->text, "||") == 0) {
        return cc_make_const_value((left.value != 0) || (right.value != 0));
    }
    if (strcmp(node->text, "==") == 0) {
        return cc_make_const_value(left.value == right.value);
    }
    if (strcmp(node->text, "!=") == 0) {
        return cc_make_const_value(left.value != right.value);
    }
    if (strcmp(node->text, "<") == 0) {
        return cc_make_const_value(left.value < right.value);
    }
    if (strcmp(node->text, "<=") == 0) {
        return cc_make_const_value(left.value <= right.value);
    }
    if (strcmp(node->text, ">") == 0) {
        return cc_make_const_value(left.value > right.value);
    }
    if (strcmp(node->text, ">=") == 0) {
        return cc_make_const_value(left.value >= right.value);
    }
    if (strcmp(node->text, ",") == 0) {
        return right;
    }

    return cc_invalid_const_value();
}

static CCConstValue cc_try_fold_integer_constant(const CCAstNode *node) {
    long long value;

    if (node == NULL) {
        return cc_invalid_const_value();
    }

    switch (node->kind) {
        case CC_AST_LITERAL:
            if (node->text == NULL) {
                return cc_invalid_const_value();
            }
            if (cc_parse_integer_literal(node->text, &value) || cc_parse_char_literal(node->text, &value)) {
                return cc_make_const_value(value);
            }
            return cc_invalid_const_value();
        case CC_AST_UNARY_EXPRESSION: {
            CCConstValue operand;

            if (node->child_count == 0 || node->text == NULL) {
                return cc_invalid_const_value();
            }

            operand = cc_try_fold_integer_constant(node->children[0]);
            if (!operand.ok) {
                return operand;
            }

            if (strcmp(node->text, "plus") == 0) {
                return cc_make_const_value(+operand.value);
            }
            if (strcmp(node->text, "minus") == 0) {
                return cc_make_const_value(-operand.value);
            }
            if (strcmp(node->text, "bang") == 0) {
                return cc_make_const_value(!operand.value);
            }
            if (strcmp(node->text, "tilde") == 0) {
                return cc_make_const_value(~operand.value);
            }
            return cc_invalid_const_value();
        }
        case CC_AST_BINARY_EXPRESSION:
            return cc_try_fold_binary_constant(node);
        case CC_AST_CONDITIONAL_EXPRESSION: {
            CCConstValue condition;

            if (node->child_count < 3) {
                return cc_invalid_const_value();
            }

            condition = cc_try_fold_integer_constant(node->children[0]);
            if (!condition.ok) {
                return condition;
            }

            return condition.value != 0
                ? cc_try_fold_integer_constant(node->children[1])
                : cc_try_fold_integer_constant(node->children[2]);
        }
        case CC_AST_CAST_EXPRESSION:
            if (node->child_count < 2) {
                return cc_invalid_const_value();
            }
            return cc_try_fold_integer_constant(node->children[1]);
        default:
            return cc_invalid_const_value();
    }
}

static CCSemaExpr cc_analyze_expression(CCSemaContext *context, const CCAstNode *node);

static CCSemaExpr cc_analyze_identifier(CCSemaContext *context, const CCAstNode *node) {
    const CCSymbol *symbol;

    if (node->text == NULL) {
        return cc_invalid_expr();
    }

    symbol = cc_lookup_symbol(context, node->text);
    if (symbol == NULL) {
        cc_add_diagnostic(context, node->span, "use of undeclared identifier '%s'", node->text);
        return cc_invalid_expr();
    }

    if (symbol->kind == CC_SYMBOL_TYPEDEF) {
        cc_add_diagnostic(context, node->span, "'%s' names a type, not a value", node->text);
        return cc_invalid_expr();
    }

    return cc_make_expr(symbol->type, symbol->kind == CC_SYMBOL_OBJECT, symbol->kind == CC_SYMBOL_FUNCTION);
}

static CCSemaExpr cc_analyze_unary_expression(CCSemaContext *context, const CCAstNode *node) {
    CCSemaExpr operand;
    const char *operator_text;

    if (node->child_count == 0) {
        return cc_invalid_expr();
    }

    operand = cc_analyze_expression(context, node->children[0]);
    operator_text = node->text;
    if (!operand.ok) {
        return operand;
    }

    if (operator_text == NULL) {
        return operand;
    }

    if (strcmp(operator_text, "ampersand") == 0) {
        if (!operand.is_lvalue) {
            cc_add_diagnostic(context, node->span, "cannot take the address of a non-lvalue expression");
            return cc_invalid_expr();
        }
        return cc_make_expr(cc_new_pointer_type(context, operand.type), false, false);
    }

    if (strcmp(operator_text, "star") == 0) {
        if (!cc_type_is_pointer(operand.type)) {
            cc_add_diagnostic(context, node->span, "cannot dereference operand of type %s", cc_type_name(operand.type));
            return cc_invalid_expr();
        }
        return cc_make_expr(operand.type->base, true, false);
    }

    if (strcmp(operator_text, "bang") == 0) {
        if (!cc_type_is_scalar(operand.type)) {
            cc_add_diagnostic(context, node->span, "logical negation requires a scalar operand");
        }
        return cc_make_expr(&cc_bool_type, false, false);
    }

    if (strcmp(operator_text, "tilde") == 0) {
        if (!cc_type_is_integer(operand.type)) {
            cc_add_diagnostic(context, node->span, "bitwise negation requires an integer operand");
            return cc_invalid_expr();
        }
        return cc_make_expr(operand.type, false, false);
    }

    if (strcmp(operator_text, "plus") == 0 || strcmp(operator_text, "minus") == 0) {
        if (!cc_type_is_numeric(operand.type)) {
            cc_add_diagnostic(context, node->span, "unary arithmetic requires a numeric operand");
            return cc_invalid_expr();
        }
        return cc_make_expr(operand.type, false, false);
    }

    if (strcmp(operator_text, "increment") == 0
        || strcmp(operator_text, "decrement") == 0
        || strcmp(operator_text, "post++") == 0
        || strcmp(operator_text, "post--") == 0) {
        if (!operand.is_lvalue || !cc_type_is_scalar(operand.type)) {
            cc_add_diagnostic(context, node->span, "increment and decrement require a scalar lvalue");
            return cc_invalid_expr();
        }
        return cc_make_expr(operand.type, false, false);
    }

    if (strcmp(operator_text, "kw_sizeof") == 0 || strcmp(operator_text, "kw__Alignof") == 0) {
        return cc_make_expr(&cc_long_type, false, false);
    }

    return cc_make_expr(operand.type, false, false);
}

static CCSemaExpr cc_analyze_call_expression(CCSemaContext *context, const CCAstNode *node) {
    CCSemaExpr callee;
    const CCSemaType *function_type;
    size_t argument_count;
    size_t index;

    if (node->child_count == 0) {
        return cc_invalid_expr();
    }

    callee = cc_analyze_expression(context, node->children[0]);
    if (!callee.ok) {
        return callee;
    }

    function_type = callee.type;
    if (function_type->kind == CC_SEMA_TYPE_POINTER && function_type->base != NULL && function_type->base->kind == CC_SEMA_TYPE_FUNCTION) {
        function_type = function_type->base;
    }

    if (function_type->kind != CC_SEMA_TYPE_FUNCTION) {
        cc_add_diagnostic(context, node->span, "called object is not a function");
        return cc_invalid_expr();
    }

    argument_count = 0;
    if (node->child_count > 1 && node->children[1]->kind == CC_AST_ARGUMENT_LIST) {
        for (index = 0; index < node->children[1]->child_count; index++) {
            CCSemaExpr argument;

            argument = cc_analyze_expression(context, node->children[1]->children[index]);
            if (index < function_type->parameter_count
                && argument.ok
                && !cc_types_assignable(function_type->parameters[index], argument.type)) {
                cc_add_diagnostic(
                    context,
                    node->children[1]->children[index]->span,
                    "argument %zu has incompatible type %s, expected %s",
                    index + 1,
                    cc_type_name(argument.type),
                    cc_type_name(function_type->parameters[index])
                );
            }
            argument_count++;
        }
    }

    if (!function_type->variadic && argument_count != function_type->parameter_count) {
        cc_add_diagnostic(
            context,
            node->span,
            "function call expected %zu argument%s but received %zu",
            function_type->parameter_count,
            function_type->parameter_count == 1 ? "" : "s",
            argument_count
        );
    } else if (function_type->variadic && argument_count < function_type->parameter_count) {
        cc_add_diagnostic(
            context,
            node->span,
            "function call expected at least %zu argument%s but received %zu",
            function_type->parameter_count,
            function_type->parameter_count == 1 ? "" : "s",
            argument_count
        );
    }

    return cc_make_expr(function_type->base, false, false);
}

static CCSemaExpr cc_analyze_binary_expression(CCSemaContext *context, const CCAstNode *node) {
    CCSemaExpr left;
    CCSemaExpr right;
    const char *operator_text;

    if (node->child_count < 2) {
        return cc_invalid_expr();
    }

    left = cc_analyze_expression(context, node->children[0]);
    right = cc_analyze_expression(context, node->children[1]);
    operator_text = node->text;

    if (!left.ok || !right.ok) {
        return cc_invalid_expr();
    }

    if (cc_is_assignment_operator(operator_text)) {
        if (!left.is_lvalue) {
            cc_add_diagnostic(context, node->children[0]->span, "left-hand side of assignment must be a modifiable lvalue");
            return cc_invalid_expr();
        }

        if (!cc_types_assignable(left.type, right.type)) {
            cc_add_diagnostic(
                context,
                node->children[1]->span,
                "cannot assign value of type %s to %s",
                cc_type_name(right.type),
                cc_type_name(left.type)
            );
            return cc_invalid_expr();
        }

        return cc_make_expr(left.type, false, false);
    }

    if (strcmp(operator_text, "&&") == 0 || strcmp(operator_text, "||") == 0) {
        if (!cc_type_is_scalar(left.type) || !cc_type_is_scalar(right.type)) {
            cc_add_diagnostic(context, node->span, "logical operators require scalar operands");
        }
        return cc_make_expr(&cc_bool_type, false, false);
    }

    if (cc_is_comparison_operator(operator_text)) {
        if (!cc_type_is_scalar(left.type) || !cc_type_is_scalar(right.type)) {
            cc_add_diagnostic(context, node->span, "comparison requires scalar operands");
        }
        return cc_make_expr(&cc_bool_type, false, false);
    }

    if (strcmp(operator_text, ",") == 0) {
        return cc_make_expr(right.type, false, false);
    }

    if (strcmp(operator_text, "+") == 0 || strcmp(operator_text, "-") == 0) {
        if (cc_type_is_pointer(left.type) && cc_type_is_integer(right.type)) {
            return cc_make_expr(left.type, false, false);
        }
        if (cc_type_is_integer(left.type) && cc_type_is_pointer(right.type) && strcmp(operator_text, "+") == 0) {
            return cc_make_expr(right.type, false, false);
        }
        if (cc_type_is_numeric(left.type) && cc_type_is_numeric(right.type)) {
            return cc_make_expr(cc_arithmetic_result_type(left.type, right.type), false, false);
        }

        cc_add_diagnostic(context, node->span, "operator '%s' requires numeric operands or pointer arithmetic", operator_text);
        return cc_invalid_expr();
    }

    if (cc_is_arithmetic_operator(operator_text) || cc_is_bitwise_operator(operator_text)) {
        if (!cc_type_is_numeric(left.type) || !cc_type_is_numeric(right.type)) {
            cc_add_diagnostic(context, node->span, "operator '%s' requires numeric operands", operator_text);
            return cc_invalid_expr();
        }

        if (cc_is_bitwise_operator(operator_text) && (!cc_type_is_integer(left.type) || !cc_type_is_integer(right.type))) {
            cc_add_diagnostic(context, node->span, "operator '%s' requires integer operands", operator_text);
            return cc_invalid_expr();
        }

        return cc_make_expr(cc_arithmetic_result_type(left.type, right.type), false, false);
    }

    return cc_make_expr(cc_arithmetic_result_type(left.type, right.type), false, false);
}

static CCSemaExpr cc_analyze_expression(CCSemaContext *context, const CCAstNode *node) {
    const CCSemaType *cast_type;

    if (node == NULL) {
        return cc_invalid_expr();
    }

    switch (node->kind) {
        case CC_AST_IDENTIFIER:
            return cc_analyze_identifier(context, node);
        case CC_AST_LITERAL:
            return cc_make_expr(cc_literal_type(context, node->text), false, false);
        case CC_AST_UNARY_EXPRESSION:
            return cc_analyze_unary_expression(context, node);
        case CC_AST_BINARY_EXPRESSION:
            return cc_analyze_binary_expression(context, node);
        case CC_AST_CONDITIONAL_EXPRESSION: {
            CCSemaExpr condition;
            CCSemaExpr then_expr;
            CCSemaExpr else_expr;

            if (node->child_count < 3) {
                return cc_invalid_expr();
            }

            condition = cc_analyze_expression(context, node->children[0]);
            then_expr = cc_analyze_expression(context, node->children[1]);
            else_expr = cc_analyze_expression(context, node->children[2]);

            if (!cc_type_is_scalar(condition.type)) {
                cc_add_diagnostic(context, node->children[0]->span, "conditional test must be a scalar expression");
            }

            if (cc_types_assignable(then_expr.type, else_expr.type)) {
                return cc_make_expr(then_expr.type, false, false);
            }
            if (cc_types_assignable(else_expr.type, then_expr.type)) {
                return cc_make_expr(else_expr.type, false, false);
            }

            cc_add_diagnostic(context, node->span, "conditional expression branches have incompatible types");
            return cc_invalid_expr();
        }
        case CC_AST_CALL_EXPRESSION:
            return cc_analyze_call_expression(context, node);
        case CC_AST_SUBSCRIPT_EXPRESSION: {
            CCSemaExpr base;
            CCSemaExpr index;

            if (node->child_count < 2) {
                return cc_invalid_expr();
            }

            base = cc_analyze_expression(context, node->children[0]);
            index = cc_analyze_expression(context, node->children[1]);
            if (!cc_type_is_pointer(base.type) || !cc_type_is_integer(index.type)) {
                cc_add_diagnostic(context, node->span, "subscript expression requires a pointer base and integer index");
                return cc_invalid_expr();
            }

            return cc_make_expr(base.type->base, true, false);
        }
        case CC_AST_MEMBER_EXPRESSION:
            if (node->child_count > 0) {
                (void)cc_analyze_expression(context, node->children[0]);
            }
            return cc_make_expr(&cc_int_type, true, false);
        case CC_AST_CAST_EXPRESSION:
            if (node->child_count < 2) {
                return cc_invalid_expr();
            }

            cast_type = cc_type_from_parameter(context, node->children[0]);
            {
                CCSemaExpr value;

                value = cc_analyze_expression(context, node->children[1]);
                if (!value.ok) {
                    return cc_invalid_expr();
                }

                if (!cc_types_castable(cast_type, value.type)) {
                    cc_add_diagnostic(
                        context,
                        node->span,
                        "invalid cast from %s to %s",
                        cc_type_name(value.type),
                        cc_type_name(cast_type)
                    );
                    return cc_invalid_expr();
                }
            }
            return cc_make_expr(cast_type, false, false);
        case CC_AST_ARGUMENT_LIST:
            return cc_invalid_expr();
        default:
            return cc_invalid_expr();
    }
}

static void cc_visit_declaration(CCSemaContext *context, const CCAstNode *declaration, bool file_scope);
static bool cc_visit_statement(CCSemaContext *context, const CCAstNode *statement);
static bool cc_visit_compound_statement_with_scope(CCSemaContext *context, const CCAstNode *statement, bool push_scope);

static void cc_register_parameter_symbols(CCSemaContext *context, const CCAstNode *parameter_list) {
    size_t index;

    if (parameter_list == NULL) {
        return;
    }

    for (index = 0; index < parameter_list->child_count; index++) {
        const CCAstNode *parameter;
        const CCAstNode *declarator;
        const CCSemaType *type;
        bool was_added;
        size_t child_index;

        parameter = parameter_list->children[index];
        if (parameter->kind != CC_AST_PARAMETER || parameter->text != NULL) {
            continue;
        }

        declarator = NULL;
        for (child_index = 0; child_index < parameter->child_count; child_index++) {
            if (parameter->children[child_index]->kind == CC_AST_DECLARATOR) {
                declarator = parameter->children[child_index];
                break;
            }
        }

        if (declarator == NULL || declarator->text == NULL || declarator->text[0] == '\0') {
            continue;
        }

        type = cc_type_from_parameter(context, parameter);
        cc_add_symbol_to_current_scope(
            context,
            declarator->text,
            CC_SYMBOL_OBJECT,
            type,
            declarator->span,
            true,
            &was_added
        );
    }
}

static bool cc_visit_compound_statement(CCSemaContext *context, const CCAstNode *statement) {
    return cc_visit_compound_statement_with_scope(context, statement, true);
}

static bool cc_visit_compound_statement_with_scope(CCSemaContext *context, const CCAstNode *statement, bool push_scope) {
    size_t index;
    bool returns;

    if (push_scope) {
        cc_push_scope(context);
    }
    returns = false;

    for (index = 0; index < statement->child_count; index++) {
        const CCAstNode *child;
        bool child_returns;

        child = statement->children[index];
        child_returns = cc_visit_statement(context, child);
        if (index + 1 == statement->child_count) {
            returns = child_returns;
        }
    }

    if (push_scope) {
        cc_pop_scope(context);
    }
    return returns;
}

static bool cc_visit_statement(CCSemaContext *context, const CCAstNode *statement) {
    if (statement == NULL) {
        return false;
    }

    switch (statement->kind) {
        case CC_AST_COMPOUND_STATEMENT:
            return cc_visit_compound_statement(context, statement);
        case CC_AST_DECLARATION:
            cc_visit_declaration(context, statement, false);
            return false;
        case CC_AST_IF_STATEMENT: {
            bool then_returns;
            bool else_returns;

            if (statement->child_count > 0) {
                CCSemaExpr condition;

                condition = cc_analyze_expression(context, statement->children[0]);
                if (!cc_type_is_scalar(condition.type)) {
                    cc_add_diagnostic(context, statement->children[0]->span, "if condition must be scalar");
                }
            }

            then_returns = statement->child_count > 1 ? cc_visit_statement(context, statement->children[1]) : false;
            else_returns = statement->child_count > 2 ? cc_visit_statement(context, statement->children[2]) : false;
            return statement->child_count > 2 && then_returns && else_returns;
        }
        case CC_AST_FOR_STATEMENT: {
            size_t index;

            cc_push_scope(context);
            context->break_depth++;
            context->loop_depth++;
            for (index = 0; index < statement->child_count; index++) {
                const CCAstNode *child;

                child = statement->children[index];
                if (index + 1 == statement->child_count) {
                    (void)cc_visit_statement(context, child);
                } else if (child->kind == CC_AST_DECLARATION) {
                    cc_visit_declaration(context, child, false);
                } else if (child->kind != CC_AST_EMPTY_STATEMENT) {
                    (void)cc_analyze_expression(context, child->kind == CC_AST_EXPRESSION_STATEMENT && child->child_count > 0
                        ? child->children[0]
                        : child);
                }
            }
            context->loop_depth--;
            context->break_depth--;
            cc_pop_scope(context);
            return false;
        }
        case CC_AST_WHILE_STATEMENT:
            if (statement->child_count > 0) {
                CCSemaExpr condition;

                condition = cc_analyze_expression(context, statement->children[0]);
                if (!cc_type_is_scalar(condition.type)) {
                    cc_add_diagnostic(context, statement->children[0]->span, "while condition must be scalar");
                }
            }
            context->break_depth++;
            context->loop_depth++;
            if (statement->child_count > 1) {
                (void)cc_visit_statement(context, statement->children[1]);
            }
            context->loop_depth--;
            context->break_depth--;
            return false;
        case CC_AST_DO_WHILE_STATEMENT:
            context->break_depth++;
            context->loop_depth++;
            if (statement->child_count > 0) {
                (void)cc_visit_statement(context, statement->children[0]);
            }
            if (statement->child_count > 1) {
                CCSemaExpr condition;

                condition = cc_analyze_expression(context, statement->children[1]);
                if (!cc_type_is_scalar(condition.type)) {
                    cc_add_diagnostic(context, statement->children[1]->span, "do-while condition must be scalar");
                }
            }
            context->loop_depth--;
            context->break_depth--;
            return false;
        case CC_AST_SWITCH_STATEMENT:
            if (statement->child_count > 0) {
                CCSemaExpr condition;

                condition = cc_analyze_expression(context, statement->children[0]);
                if (!cc_type_is_integer(condition.type)) {
                    cc_add_diagnostic(context, statement->children[0]->span, "switch condition must have integer type");
                }
            }
            context->break_depth++;
            context->switch_depth++;
            cc_push_switch_record(context);
            if (statement->child_count > 1) {
                (void)cc_visit_statement(context, statement->children[1]);
            }
            cc_pop_switch_record(context);
            context->switch_depth--;
            context->break_depth--;
            return false;
        case CC_AST_CASE_STATEMENT:
            if (context->switch_depth == 0) {
                cc_add_diagnostic(context, statement->span, "'case' is only valid inside a switch");
            }
            if (statement->child_count > 0) {
                CCConstValue folded;
                CCSemaExpr value_expr;

                value_expr = cc_analyze_expression(context, statement->children[0]);
                if (!cc_type_is_integer(value_expr.type)) {
                    cc_add_diagnostic(context, statement->children[0]->span, "case label must have integer type");
                }
                folded = cc_try_fold_integer_constant(statement->children[0]);
                if (!folded.ok) {
                    cc_add_diagnostic(context, statement->children[0]->span, "case label must be an integer constant expression");
                } else {
                    cc_record_switch_case(context, folded.value, statement->children[0]->span);
                }
            }
            if (statement->child_count > 1) {
                return cc_visit_statement(context, statement->children[1]);
            }
            return false;
        case CC_AST_DEFAULT_STATEMENT:
            if (context->switch_depth == 0) {
                cc_add_diagnostic(context, statement->span, "'default' is only valid inside a switch");
            } else {
                cc_record_switch_default(context, statement->span);
            }
            if (statement->child_count > 0) {
                return cc_visit_statement(context, statement->children[0]);
            }
            return false;
        case CC_AST_GOTO_STATEMENT:
            cc_add_goto_reference(context, statement->text, statement->span);
            return false;
        case CC_AST_LABEL_STATEMENT:
            cc_add_defined_label(context, statement->text, statement->span);
            if (statement->child_count > 0) {
                return cc_visit_statement(context, statement->children[0]);
            }
            return false;
        case CC_AST_RETURN_STATEMENT:
            if (statement->child_count == 0) {
                if (context->current_return_type != &cc_void_type) {
                    cc_add_diagnostic(
                        context,
                        statement->span,
                        "non-void function '%s' must return a value",
                        context->current_function_name == NULL ? "<anonymous>" : context->current_function_name
                    );
                }
            } else {
                CCSemaExpr value;

                value = cc_analyze_expression(context, statement->children[0]);
                if (context->current_return_type == &cc_void_type) {
                    cc_add_diagnostic(context, statement->children[0]->span, "void function should not return a value");
                } else if (!cc_types_assignable(context->current_return_type, value.type)) {
                    cc_add_diagnostic(
                        context,
                        statement->children[0]->span,
                        "return value has incompatible type %s, expected %s",
                        cc_type_name(value.type),
                        cc_type_name(context->current_return_type)
                    );
                } else if (cc_types_assignable(context->current_return_type, value.type)) {
                    if ((context->current_return_type->kind == CC_SEMA_TYPE_FLOAT || 
                         context->current_return_type->kind == CC_SEMA_TYPE_DOUBLE) &&
                        (value.type->kind == CC_SEMA_TYPE_INT || 
                         value.type->kind == CC_SEMA_TYPE_LONG ||
                         value.type->kind == CC_SEMA_TYPE_CHAR)) {
                        cc_add_warning(
                            context,
                            statement->children[0]->span,
                            "implicit conversion from %s to %s",
                            cc_type_name(value.type),
                            cc_type_name(context->current_return_type)
                        );
                    }
                }
            }
            return true;
        case CC_AST_BREAK_STATEMENT:
            if (context->break_depth == 0) {
                cc_add_diagnostic(context, statement->span, "'break' is only valid inside a loop or switch");
            }
            return false;
        case CC_AST_CONTINUE_STATEMENT:
            if (context->loop_depth == 0) {
                cc_add_diagnostic(context, statement->span, "'continue' is only valid inside a loop");
            }
            return false;
        case CC_AST_EXPRESSION_STATEMENT:
            if (statement->child_count > 0) {
                (void)cc_analyze_expression(context, statement->children[0]);
            }
            return false;
        case CC_AST_EMPTY_STATEMENT:
            return false;
        default:
            if (statement->kind >= CC_AST_IDENTIFIER && statement->kind <= CC_AST_ARGUMENT_LIST) {
                (void)cc_analyze_expression(context, statement);
            }
            return false;
    }
}

static void cc_visit_declaration(CCSemaContext *context, const CCAstNode *declaration, bool file_scope) {
    const CCAstNode *specifiers;
    const CCSemaType *base_type;
    bool is_typedef;
    size_t index;

    specifiers = cc_declaration_specifiers(declaration);
    base_type = cc_type_from_specifiers(context, specifiers);
    is_typedef = cc_specifiers_contain_text(specifiers, "typedef");

    for (index = 1; index < declaration->child_count; index++) {
        const CCAstNode *init_declarator;
        const CCAstNode *declarator;
        const CCAstNode *initializer;
        const CCSemaType *declared_type;
        const char *name;
        CCSymbolKind symbol_kind;
        bool was_added;

        init_declarator = declaration->children[index];
        if (init_declarator->kind != CC_AST_INIT_DECLARATOR) {
            continue;
        }

        declarator = cc_init_declarator_declarator(init_declarator);
        initializer = cc_init_declarator_initializer(init_declarator);
        if (declarator == NULL) {
            continue;
        }

        declared_type = cc_apply_declarator_type(context, declarator, base_type);
        name = declarator->text;

        if (is_typedef) {
            if (name != NULL) {
                cc_add_symbol_to_current_scope(
                    context,
                    name,
                    CC_SYMBOL_TYPEDEF,
                    declared_type,
                    declarator->span,
                    true,
                    &was_added
                );
                if (was_added) {
                    context->typedef_count++;
                }
            }
            continue;
        }

        symbol_kind = file_scope && declared_type->kind == CC_SEMA_TYPE_FUNCTION
            ? CC_SYMBOL_FUNCTION
            : CC_SYMBOL_OBJECT;

        cc_add_symbol_to_current_scope(
            context,
            name,
            symbol_kind,
            declared_type,
            declarator->span,
            false,
            &was_added
        );

        if (was_added && file_scope && symbol_kind == CC_SYMBOL_OBJECT) {
            context->global_count++;
        }
        if (was_added && file_scope && symbol_kind == CC_SYMBOL_FUNCTION) {
            context->function_count++;
        }

        if (initializer != NULL) {
            CCSemaExpr expr;

            if (symbol_kind == CC_SYMBOL_FUNCTION) {
                cc_add_diagnostic(context, initializer->span, "function declarations cannot have initializers");
                continue;
            }

            expr = cc_analyze_expression(context, initializer);
            if (!cc_types_assignable(declared_type, expr.type)) {
                cc_add_diagnostic(
                    context,
                    initializer->span,
                    "initializer for '%s' has incompatible type %s, expected %s",
                    name == NULL ? "<unnamed>" : name,
                    cc_type_name(expr.type),
                    cc_type_name(declared_type)
                );
            }
        }
    }
}

static void cc_visit_function_definition(CCSemaContext *context, const CCAstNode *function) {
    const CCAstNode *specifiers;
    const CCAstNode *declarator;
    const CCAstNode *body;
    const CCAstNode *parameter_list;
    const CCSemaType *function_type;
    const CCSemaType *saved_return_type;
    const char *saved_function_name;
    bool body_returns;
    bool was_added;

    if (function->child_count < 3) {
        return;
    }

    specifiers = function->children[0];
    declarator = function->children[1];
    body = function->children[2];
    function_type = cc_apply_declarator_type(context, declarator, cc_type_from_specifiers(context, specifiers));
    if (function_type->kind != CC_SEMA_TYPE_FUNCTION) {
        cc_add_diagnostic(context, function->span, "function definition for '%s' has a non-function declarator", function->text);
        return;
    }

    cc_add_symbol_to_current_scope(
        context,
        function->text,
        CC_SYMBOL_FUNCTION,
        function_type,
        function->span,
        true,
        &was_added
    );
    if (was_added) {
        context->function_count++;
    }

    saved_return_type = context->current_return_type;
    saved_function_name = context->current_function_name;
    context->current_return_type = function_type->base;
    context->current_function_name = function->text;
    cc_clear_label_records(&context->defined_labels, &context->defined_label_count, &context->defined_label_capacity);
    cc_clear_label_records(&context->goto_references, &context->goto_reference_count, &context->goto_reference_capacity);
    cc_clear_switch_records(context);

    cc_push_scope(context);
    parameter_list = cc_find_parameter_list(declarator);
    cc_register_parameter_symbols(context, parameter_list);
    body_returns = body != NULL && body->kind == CC_AST_COMPOUND_STATEMENT
        ? cc_visit_compound_statement_with_scope(context, body, false)
        : cc_visit_statement(context, body);

    for (size_t goto_index = 0; goto_index < context->goto_reference_count; goto_index++) {
        const CCLabelRecord *target;

        target = cc_find_label_record(
            context->defined_labels,
            context->defined_label_count,
            context->goto_references[goto_index].name
        );
        if (target == NULL) {
            cc_add_diagnostic(
                context,
                context->goto_references[goto_index].span,
                "goto references undefined label '%s'",
                context->goto_references[goto_index].name
            );
        }
    }

    cc_pop_scope(context);

    if (context->current_return_type != &cc_void_type && !body_returns) {
        cc_add_diagnostic(
            context,
            function->span,
            "non-void function '%s' may exit without returning a value",
            function->text == NULL ? "<anonymous>" : function->text
        );
    }

    cc_clear_label_records(&context->defined_labels, &context->defined_label_count, &context->defined_label_capacity);
    cc_clear_label_records(&context->goto_references, &context->goto_reference_count, &context->goto_reference_capacity);
    cc_clear_switch_records(context);
    context->current_return_type = saved_return_type;
    context->current_function_name = saved_function_name;
}

void cc_sema_check_translation_unit(
    const CCParseResult *parse_result,
    const CCSemaOptions *options,
    CCSemaResult *result
) {
    CCSemaContext context;
    size_t index;

    memset(&context, 0, sizeof(context));
    memset(result, 0, sizeof(*result));
    context.parse_result = parse_result;
    context.options = options;
    context.current_return_type = &cc_void_type;
    cc_push_scope(&context);

    if (parse_result->translation_unit != NULL) {
        for (index = 0; index < parse_result->translation_unit->child_count; index++) {
            const CCAstNode *child;

            child = parse_result->translation_unit->children[index];
            switch (child->kind) {
                case CC_AST_PREPROCESSOR_LINE:
                    break;
                case CC_AST_DECLARATION:
                    cc_visit_declaration(&context, child, true);
                    break;
                case CC_AST_FUNCTION_DEFINITION:
                    cc_visit_function_definition(&context, child);
                    break;
                default:
                    break;
            }
        }
    }

    while (context.scope_count > 0) {
        cc_pop_scope(&context);
    }
    free(context.scopes);
    cc_clear_label_records(&context.defined_labels, &context.defined_label_count, &context.defined_label_capacity);
    cc_clear_label_records(&context.goto_references, &context.goto_reference_count, &context.goto_reference_capacity);
    cc_clear_switch_records(&context);

    for (index = 0; index < context.owned_type_count; index++) {
        free((void *)context.owned_types[index]->parameters);
        free(context.owned_types[index]);
    }
    free(context.owned_types);

    result->diagnostics = context.diagnostics;
    result->function_count = context.function_count;
    result->global_count = context.global_count;
    result->typedef_count = context.typedef_count;
}

void cc_sema_result_free(CCSemaResult *result) {
    size_t index;

    for (index = 0; index < result->diagnostics.count; index++) {
        free(result->diagnostics.items[index].path);
        free(result->diagnostics.items[index].message);
    }

    free(result->diagnostics.items);
    result->diagnostics.items = NULL;
    result->diagnostics.count = 0;
    result->diagnostics.capacity = 0;
    result->function_count = 0;
    result->global_count = 0;
    result->typedef_count = 0;
}
