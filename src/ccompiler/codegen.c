#include "C-Compiler/codegen.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} CCStringBuilder;

typedef struct {
    char *break_label;
    char *continue_label;
} CCControlContext;

typedef struct {
    char *source_name;
    char *emitted_name;
} CCNameBinding;

typedef struct {
    CCNameBinding *bindings;
    size_t count;
    size_t capacity;
} CCNameScope;

typedef struct {
    bool ok;
    long long value;
} CCConstValue;

typedef struct {
    const CCAstNode **case_nodes;
    long long *case_values;
    size_t case_count;
    size_t case_capacity;
    const CCAstNode *default_node;
} CCSwitchLayout;

typedef struct {
    const CCParseResult *parse_result;
    CCDiagnosticBuffer diagnostics;
    CCStringBuilder output;
    size_t temp_counter;
    size_t label_counter;
    CCControlContext *control_stack;
    size_t control_count;
    size_t control_capacity;
    CCNameScope *name_scopes;
    size_t name_scope_count;
    size_t name_scope_capacity;
    size_t unique_name_counter;
    const char *current_function_name;
} CCCodegenContext;

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

static char *cc_format_string(const char *format, ...) {
    char stack_buffer[256];
    char *heap_buffer;
    int needed;
    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        return cc_duplicate_string("<format_error>");
    }

    if ((size_t)needed < sizeof(stack_buffer)) {
        va_end(copy);
        return cc_duplicate_string(stack_buffer);
    }

    heap_buffer = cc_reallocate_or_die(NULL, (size_t)needed + 1);
    vsnprintf(heap_buffer, (size_t)needed + 1, format, copy);
    va_end(copy);
    return heap_buffer;
}

static void cc_builder_append_range(CCStringBuilder *builder, const char *text, size_t length) {
    size_t required;

    required = builder->length + length + 1;
    if (required > builder->capacity) {
        size_t new_capacity;

        new_capacity = builder->capacity == 0 ? 128 : builder->capacity;
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

static void cc_add_diagnostic(CCCodegenContext *context, CCSpan span, const char *format, ...) {
    char stack_buffer[256];
    char *message;
    int needed;
    va_list args;
    va_list copy;

    if (context->diagnostics.count == context->diagnostics.capacity) {
        cc_grow_diagnostic_buffer(&context->diagnostics);
    }

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        message = cc_duplicate_string("diagnostic formatting failure");
    } else if ((size_t)needed < sizeof(stack_buffer)) {
        va_end(copy);
        message = cc_duplicate_string(stack_buffer);
    } else {
        message = cc_reallocate_or_die(NULL, (size_t)needed + 1);
        vsnprintf(message, (size_t)needed + 1, format, copy);
        va_end(copy);
    }

    if (span.length == 0 && span.offset < context->parse_result->source.length) {
        span.length = 1;
    }

    context->diagnostics.items[context->diagnostics.count].span = span;
    context->diagnostics.items[context->diagnostics.count].path = NULL;
    context->diagnostics.items[context->diagnostics.count].message = message;
    context->diagnostics.count++;
}

static void cc_emit_line(CCCodegenContext *context, const char *format, ...) {
    char stack_buffer[256];
    char *heap_buffer;
    int needed;
    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        return;
    }

    if ((size_t)needed < sizeof(stack_buffer)) {
        cc_builder_append_cstring(&context->output, stack_buffer);
    } else {
        heap_buffer = cc_reallocate_or_die(NULL, (size_t)needed + 1);
        vsnprintf(heap_buffer, (size_t)needed + 1, format, copy);
        cc_builder_append_cstring(&context->output, heap_buffer);
        free(heap_buffer);
    }
    va_end(copy);

    cc_builder_append_char(&context->output, '\n');
}

static char *cc_new_temp(CCCodegenContext *context) {
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "t%zu", context->temp_counter++);
    return cc_duplicate_string(buffer);
}

static char *cc_new_label(CCCodegenContext *context, const char *prefix) {
    return cc_format_string("%s.%zu", prefix, context->label_counter++);
}

static void cc_grow_control_stack(CCCodegenContext *context) {
    size_t new_capacity;

    new_capacity = context->control_capacity == 0 ? 4 : context->control_capacity * 2;
    context->control_stack = cc_reallocate_or_die(context->control_stack, new_capacity * sizeof(*context->control_stack));
    context->control_capacity = new_capacity;
}

static void cc_push_control(CCCodegenContext *context, char *break_label, char *continue_label) {
    if (context->control_count == context->control_capacity) {
        cc_grow_control_stack(context);
    }

    context->control_stack[context->control_count].break_label = break_label;
    context->control_stack[context->control_count].continue_label = continue_label;
    context->control_count++;
}

static void cc_pop_control(CCCodegenContext *context) {
    if (context->control_count == 0) {
        return;
    }

    context->control_count--;
}

static const char *cc_current_break_label(const CCCodegenContext *context) {
    size_t index;

    for (index = context->control_count; index > 0; index--) {
        if (context->control_stack[index - 1].break_label != NULL) {
            return context->control_stack[index - 1].break_label;
        }
    }

    return NULL;
}

static void cc_grow_name_scopes(CCCodegenContext *context) {
    size_t new_capacity;

    new_capacity = context->name_scope_capacity == 0 ? 4 : context->name_scope_capacity * 2;
    context->name_scopes = cc_reallocate_or_die(context->name_scopes, new_capacity * sizeof(*context->name_scopes));
    context->name_scope_capacity = new_capacity;
}

static void cc_grow_name_bindings(CCNameScope *scope) {
    size_t new_capacity;

    new_capacity = scope->capacity == 0 ? 8 : scope->capacity * 2;
    scope->bindings = cc_reallocate_or_die(scope->bindings, new_capacity * sizeof(*scope->bindings));
    scope->capacity = new_capacity;
}

static void cc_push_name_scope(CCCodegenContext *context) {
    if (context->name_scope_count == context->name_scope_capacity) {
        cc_grow_name_scopes(context);
    }

    memset(&context->name_scopes[context->name_scope_count], 0, sizeof(context->name_scopes[0]));
    context->name_scope_count++;
}

static void cc_pop_name_scope(CCCodegenContext *context) {
    CCNameScope *scope;
    size_t index;

    if (context->name_scope_count == 0) {
        return;
    }

    context->name_scope_count--;
    scope = &context->name_scopes[context->name_scope_count];

    for (index = 0; index < scope->count; index++) {
        free(scope->bindings[index].source_name);
        free(scope->bindings[index].emitted_name);
    }

    free(scope->bindings);
    memset(scope, 0, sizeof(*scope));
}

static CCNameScope *cc_current_name_scope(CCCodegenContext *context) {
    if (context->name_scope_count == 0) {
        return NULL;
    }

    return &context->name_scopes[context->name_scope_count - 1];
}

static const char *cc_lookup_emitted_name(const CCCodegenContext *context, const char *source_name) {
    size_t scope_index;

    if (source_name == NULL) {
        return NULL;
    }

    for (scope_index = context->name_scope_count; scope_index > 0; scope_index--) {
        const CCNameScope *scope;
        size_t binding_index;

        scope = &context->name_scopes[scope_index - 1];
        for (binding_index = 0; binding_index < scope->count; binding_index++) {
            if (strcmp(scope->bindings[binding_index].source_name, source_name) == 0) {
                return scope->bindings[binding_index].emitted_name;
            }
        }
    }

    return NULL;
}

static char *cc_bind_local_name(CCCodegenContext *context, const char *source_name) {
    CCNameScope *scope;
    const char *existing;
    char *emitted_name;

    if (source_name == NULL || source_name[0] == '\0') {
        return NULL;
    }

    scope = cc_current_name_scope(context);
    if (scope == NULL) {
        return cc_duplicate_string(source_name);
    }

    existing = cc_lookup_emitted_name(context, source_name);
    if (existing == NULL) {
        emitted_name = cc_duplicate_string(source_name);
    } else {
        emitted_name = cc_format_string("%s.%zu", source_name, context->unique_name_counter++);
    }

    if (scope->count == scope->capacity) {
        cc_grow_name_bindings(scope);
    }

    scope->bindings[scope->count].source_name = cc_duplicate_string(source_name);
    scope->bindings[scope->count].emitted_name = cc_duplicate_string(emitted_name);
    scope->count++;
    return emitted_name;
}

static const char *cc_current_continue_label(const CCCodegenContext *context) {
    size_t index;

    for (index = context->control_count; index > 0; index--) {
        if (context->control_stack[index - 1].continue_label != NULL) {
            return context->control_stack[index - 1].continue_label;
        }
    }

    return NULL;
}

static char *cc_user_label_name(const CCCodegenContext *context, const char *label_name) {
    return cc_format_string(
        "label.%s.%s",
        context->current_function_name == NULL ? "anonymous" : context->current_function_name,
        label_name == NULL ? "unnamed" : label_name
    );
}

static char *cc_case_label_name(const CCAstNode *statement) {
    return cc_format_string("switch.case.%zu", statement == NULL ? 0 : statement->span.offset);
}

static char *cc_default_label_name(const CCAstNode *statement) {
    return cc_format_string("switch.default.%zu", statement == NULL ? 0 : statement->span.offset);
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

static CCConstValue cc_try_fold_integer_constant(const CCAstNode *expression);

static CCConstValue cc_try_fold_binary_constant(const CCAstNode *expression) {
    CCConstValue left;
    CCConstValue right;

    if (expression->child_count < 2) {
        return cc_invalid_const_value();
    }

    left = cc_try_fold_integer_constant(expression->children[0]);
    right = cc_try_fold_integer_constant(expression->children[1]);
    if (!left.ok || !right.ok || expression->text == NULL) {
        return cc_invalid_const_value();
    }

    if (strcmp(expression->text, "+") == 0) {
        return cc_make_const_value(left.value + right.value);
    }
    if (strcmp(expression->text, "-") == 0) {
        return cc_make_const_value(left.value - right.value);
    }
    if (strcmp(expression->text, "*") == 0) {
        return cc_make_const_value(left.value * right.value);
    }
    if (strcmp(expression->text, "/") == 0 && right.value != 0) {
        return cc_make_const_value(left.value / right.value);
    }
    if (strcmp(expression->text, "%") == 0 && right.value != 0) {
        return cc_make_const_value(left.value % right.value);
    }
    if (strcmp(expression->text, "<<") == 0) {
        return cc_make_const_value(left.value << right.value);
    }
    if (strcmp(expression->text, ">>") == 0) {
        return cc_make_const_value(left.value >> right.value);
    }
    if (strcmp(expression->text, "&") == 0) {
        return cc_make_const_value(left.value & right.value);
    }
    if (strcmp(expression->text, "|") == 0) {
        return cc_make_const_value(left.value | right.value);
    }
    if (strcmp(expression->text, "^") == 0) {
        return cc_make_const_value(left.value ^ right.value);
    }
    if (strcmp(expression->text, "&&") == 0) {
        return cc_make_const_value((left.value != 0) && (right.value != 0));
    }
    if (strcmp(expression->text, "||") == 0) {
        return cc_make_const_value((left.value != 0) || (right.value != 0));
    }
    if (strcmp(expression->text, "==") == 0) {
        return cc_make_const_value(left.value == right.value);
    }
    if (strcmp(expression->text, "!=") == 0) {
        return cc_make_const_value(left.value != right.value);
    }
    if (strcmp(expression->text, "<") == 0) {
        return cc_make_const_value(left.value < right.value);
    }
    if (strcmp(expression->text, "<=") == 0) {
        return cc_make_const_value(left.value <= right.value);
    }
    if (strcmp(expression->text, ">") == 0) {
        return cc_make_const_value(left.value > right.value);
    }
    if (strcmp(expression->text, ">=") == 0) {
        return cc_make_const_value(left.value >= right.value);
    }
    if (strcmp(expression->text, ",") == 0) {
        return right;
    }

    return cc_invalid_const_value();
}

static CCConstValue cc_try_fold_integer_constant(const CCAstNode *expression) {
    long long value;

    if (expression == NULL) {
        return cc_invalid_const_value();
    }

    switch (expression->kind) {
        case CC_AST_LITERAL:
            if (expression->text == NULL) {
                return cc_invalid_const_value();
            }
            if (cc_parse_integer_literal(expression->text, &value)
                || cc_parse_char_literal(expression->text, &value)) {
                return cc_make_const_value(value);
            }
            return cc_invalid_const_value();
        case CC_AST_UNARY_EXPRESSION: {
            CCConstValue operand;

            if (expression->child_count == 0 || expression->text == NULL) {
                return cc_invalid_const_value();
            }

            operand = cc_try_fold_integer_constant(expression->children[0]);
            if (!operand.ok) {
                return operand;
            }

            if (strcmp(expression->text, "plus") == 0) {
                return cc_make_const_value(+operand.value);
            }
            if (strcmp(expression->text, "minus") == 0) {
                return cc_make_const_value(-operand.value);
            }
            if (strcmp(expression->text, "bang") == 0) {
                return cc_make_const_value(!operand.value);
            }
            if (strcmp(expression->text, "tilde") == 0) {
                return cc_make_const_value(~operand.value);
            }
            return cc_invalid_const_value();
        }
        case CC_AST_BINARY_EXPRESSION:
            return cc_try_fold_binary_constant(expression);
        case CC_AST_CONDITIONAL_EXPRESSION: {
            CCConstValue condition;

            if (expression->child_count < 3) {
                return cc_invalid_const_value();
            }

            condition = cc_try_fold_integer_constant(expression->children[0]);
            if (!condition.ok) {
                return condition;
            }

            return condition.value != 0
                ? cc_try_fold_integer_constant(expression->children[1])
                : cc_try_fold_integer_constant(expression->children[2]);
        }
        case CC_AST_CAST_EXPRESSION:
            if (expression->child_count < 2) {
                return cc_invalid_const_value();
            }
            return cc_try_fold_integer_constant(expression->children[1]);
        default:
            return cc_invalid_const_value();
    }
}

static void cc_grow_switch_layout(CCSwitchLayout *layout) {
    size_t new_capacity;

    new_capacity = layout->case_capacity == 0 ? 4 : layout->case_capacity * 2;
    layout->case_nodes = cc_reallocate_or_die(layout->case_nodes, new_capacity * sizeof(*layout->case_nodes));
    layout->case_values = cc_reallocate_or_die(layout->case_values, new_capacity * sizeof(*layout->case_values));
    layout->case_capacity = new_capacity;
}

static void cc_switch_layout_add_case(CCSwitchLayout *layout, const CCAstNode *case_node, long long value) {
    if (layout->case_count == layout->case_capacity) {
        cc_grow_switch_layout(layout);
    }

    layout->case_nodes[layout->case_count] = case_node;
    layout->case_values[layout->case_count] = value;
    layout->case_count++;
}

static void cc_collect_switch_layout(CCCodegenContext *context, CCSwitchLayout *layout, const CCAstNode *statement) {
    size_t index;

    if (statement == NULL) {
        return;
    }

    switch (statement->kind) {
        case CC_AST_SWITCH_STATEMENT:
            return;
        case CC_AST_CASE_STATEMENT: {
            CCConstValue folded;

            if (statement->child_count == 0) {
                return;
            }

            folded = cc_try_fold_integer_constant(statement->children[0]);
            if (!folded.ok) {
                cc_add_diagnostic(context, statement->children[0]->span, "switch case is not a constant expression during code generation");
            } else {
                cc_switch_layout_add_case(layout, statement, folded.value);
            }

            if (statement->child_count > 1) {
                cc_collect_switch_layout(context, layout, statement->children[1]);
            }
            return;
        }
        case CC_AST_DEFAULT_STATEMENT:
            if (layout->default_node == NULL) {
                layout->default_node = statement;
            }
            if (statement->child_count > 0) {
                cc_collect_switch_layout(context, layout, statement->children[0]);
            }
            return;
        default:
            break;
    }

    for (index = 0; index < statement->child_count; index++) {
        cc_collect_switch_layout(context, layout, statement->children[index]);
    }
}

static void cc_free_switch_layout(CCSwitchLayout *layout) {
    free(layout->case_nodes);
    free(layout->case_values);
    memset(layout, 0, sizeof(*layout));
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

static const CCAstNode *cc_find_parameter_list(const CCAstNode *declarator) {
    size_t index;

    if (declarator == NULL) {
        return NULL;
    }

    for (index = 0; index < declarator->child_count; index++) {
        if (declarator->children[index]->kind == CC_AST_PARAMETER_LIST) {
            return declarator->children[index];
        }
    }

    return NULL;
}

static size_t cc_pointer_depth(const CCAstNode *declarator) {
    size_t index;
    size_t depth;

    if (declarator == NULL) {
        return 0;
    }

    depth = 0;
    for (index = 0; index < declarator->child_count; index++) {
        const CCAstNode *child;

        child = declarator->children[index];
        if (child->kind == CC_AST_POINTER || child->kind == CC_AST_ARRAY_DECLARATOR) {
            depth++;
        } else if (child->kind == CC_AST_DECLARATOR) {
            depth += cc_pointer_depth(child);
        }
    }

    return depth;
}

static char *cc_join_specifiers(const CCAstNode *specifiers, bool drop_storage) {
    CCStringBuilder builder;
    size_t index;
    bool first;

    memset(&builder, 0, sizeof(builder));
    first = true;

    if (specifiers != NULL) {
        for (index = 0; index < specifiers->child_count; index++) {
            const CCAstNode *child;
            bool skip;

            child = specifiers->children[index];
            if (child->text == NULL) {
                continue;
            }

            skip = drop_storage
                && (strcmp(child->text, "typedef") == 0
                    || strcmp(child->text, "static") == 0
                    || strcmp(child->text, "extern") == 0
                    || strcmp(child->text, "register") == 0
                    || strcmp(child->text, "auto") == 0
                    || strcmp(child->text, "inline") == 0);
            if (skip) {
                continue;
            }

            if (!first) {
                cc_builder_append_char(&builder, ' ');
            }
            cc_builder_append_cstring(&builder, child->text);
            first = false;
        }
    }

    if (builder.data == NULL) {
        return cc_duplicate_string("int");
    }

    return cc_builder_take_string(&builder);
}

static char *cc_render_declarator_type(const CCAstNode *specifiers, const CCAstNode *declarator) {
    char *base_type;
    size_t depth;
    size_t index;
    CCStringBuilder builder;

    base_type = cc_join_specifiers(specifiers, true);
    depth = cc_pointer_depth(declarator);
    memset(&builder, 0, sizeof(builder));
    cc_builder_append_cstring(&builder, base_type);

    for (index = 0; index < depth; index++) {
        cc_builder_append_cstring(&builder, " *");
    }

    free(base_type);
    return cc_builder_take_string(&builder);
}

static char *cc_render_type_name_node(const CCAstNode *type_name) {
    const CCAstNode *specifiers;
    const CCAstNode *declarator;
    size_t index;

    specifiers = NULL;
    declarator = NULL;

    if (type_name == NULL) {
        return cc_duplicate_string("int");
    }

    for (index = 0; index < type_name->child_count; index++) {
        if (type_name->children[index]->kind == CC_AST_DECLARATION_SPECIFIERS) {
            specifiers = type_name->children[index];
        } else if (type_name->children[index]->kind == CC_AST_DECLARATOR) {
            declarator = type_name->children[index];
        }
    }

    return cc_render_declarator_type(specifiers, declarator);
}

static const char *cc_binary_opcode(const char *text) {
    if (text == NULL) {
        return "op";
    }
    if (strcmp(text, "+") == 0) {
        return "add";
    }
    if (strcmp(text, "-") == 0) {
        return "sub";
    }
    if (strcmp(text, "*") == 0) {
        return "mul";
    }
    if (strcmp(text, "/") == 0) {
        return "div";
    }
    if (strcmp(text, "%") == 0) {
        return "mod";
    }
    if (strcmp(text, "<<") == 0) {
        return "shl";
    }
    if (strcmp(text, ">>") == 0) {
        return "shr";
    }
    if (strcmp(text, "&") == 0) {
        return "bitand";
    }
    if (strcmp(text, "|") == 0) {
        return "bitor";
    }
    if (strcmp(text, "^") == 0) {
        return "bitxor";
    }
    if (strcmp(text, "&&") == 0) {
        return "and";
    }
    if (strcmp(text, "||") == 0) {
        return "or";
    }
    if (strcmp(text, "==") == 0) {
        return "eq";
    }
    if (strcmp(text, "!=") == 0) {
        return "ne";
    }
    if (strcmp(text, "<") == 0) {
        return "lt";
    }
    if (strcmp(text, "<=") == 0) {
        return "le";
    }
    if (strcmp(text, ">") == 0) {
        return "gt";
    }
    if (strcmp(text, ">=") == 0) {
        return "ge";
    }
    if (strcmp(text, ",") == 0) {
        return "comma";
    }

    return "op";
}

static const char *cc_compound_assignment_opcode(const char *text) {
    if (text == NULL) {
        return NULL;
    }
    if (strcmp(text, "+=") == 0) {
        return "add";
    }
    if (strcmp(text, "-=") == 0) {
        return "sub";
    }
    if (strcmp(text, "*=") == 0) {
        return "mul";
    }
    if (strcmp(text, "/=") == 0) {
        return "div";
    }
    if (strcmp(text, "%=") == 0) {
        return "mod";
    }
    if (strcmp(text, "<<=") == 0) {
        return "shl";
    }
    if (strcmp(text, ">>=") == 0) {
        return "shr";
    }
    if (strcmp(text, "&=") == 0) {
        return "bitand";
    }
    if (strcmp(text, "|=") == 0) {
        return "bitor";
    }
    if (strcmp(text, "^=") == 0) {
        return "bitxor";
    }
    return NULL;
}

static bool cc_is_assignment_operator(const char *text) {
    return text != NULL
        && (strcmp(text, "=") == 0 || cc_compound_assignment_opcode(text) != NULL);
}

static char *cc_codegen_expression(CCCodegenContext *context, const CCAstNode *expression);
static void cc_codegen_compound_statement(CCCodegenContext *context, const CCAstNode *statement, bool push_scope);

static char *cc_codegen_place_expression(CCCodegenContext *context, const CCAstNode *expression) {
    if (expression == NULL) {
        return cc_duplicate_string("<invalid>");
    }

    switch (expression->kind) {
        case CC_AST_IDENTIFIER: {
            const char *emitted_name;

            if (expression->text == NULL) {
                return cc_duplicate_string("<unnamed>");
            }

            emitted_name = cc_lookup_emitted_name(context, expression->text);
            return emitted_name == NULL ? cc_duplicate_string(expression->text) : cc_duplicate_string(emitted_name);
        }
        case CC_AST_SUBSCRIPT_EXPRESSION: {
            char *base;
            char *index;
            char *place;

            if (expression->child_count < 2) {
                cc_add_diagnostic(context, expression->span, "malformed subscript expression");
                return cc_duplicate_string("<invalid_subscript>");
            }

            base = cc_codegen_expression(context, expression->children[0]);
            index = cc_codegen_expression(context, expression->children[1]);
            place = cc_format_string("%s[%s]", base, index);
            free(base);
            free(index);
            return place;
        }
        case CC_AST_MEMBER_EXPRESSION: {
            char *base;
            char *place;
            const char *field_name;

            if (expression->child_count < 2) {
                cc_add_diagnostic(context, expression->span, "malformed member expression");
                return cc_duplicate_string("<invalid_member>");
            }

            base = cc_codegen_expression(context, expression->children[0]);
            field_name = expression->children[1]->text == NULL ? "<field>" : expression->children[1]->text;
            place = cc_format_string(
                "%s%s%s",
                base,
                expression->text == NULL ? "." : expression->text,
                field_name
            );
            free(base);
            return place;
        }
        case CC_AST_UNARY_EXPRESSION:
            if (expression->text != NULL && strcmp(expression->text, "star") == 0 && expression->child_count > 0) {
                char *pointer_expr;
                char *place;

                pointer_expr = cc_codegen_expression(context, expression->children[0]);
                place = cc_format_string("*%s", pointer_expr);
                free(pointer_expr);
                return place;
            }
            break;
        default:
            break;
    }

    cc_add_diagnostic(context, expression->span, "unsupported assignment target in code generation");
    return cc_duplicate_string("<invalid_lvalue>");
}

static char *cc_codegen_conditional_expression(CCCodegenContext *context, const CCAstNode *expression) {
    char *condition_value;
    char *then_label;
    char *else_label;
    char *merge_label;
    char *result;
    char *then_value;
    char *else_value;

    condition_value = cc_codegen_expression(context, expression->children[0]);
    then_label = cc_new_label(context, "cond.then");
    else_label = cc_new_label(context, "cond.else");
    merge_label = cc_new_label(context, "cond.end");
    result = cc_new_temp(context);

    cc_emit_line(context, "  cjump %s, %s, %s", condition_value, then_label, else_label);
    cc_emit_line(context, "%s:", then_label);
    then_value = cc_codegen_expression(context, expression->children[1]);
    cc_emit_line(context, "  %s = %s", result, then_value);
    cc_emit_line(context, "  jump %s", merge_label);
    cc_emit_line(context, "%s:", else_label);
    else_value = cc_codegen_expression(context, expression->children[2]);
    cc_emit_line(context, "  %s = %s", result, else_value);
    cc_emit_line(context, "  jump %s", merge_label);
    cc_emit_line(context, "%s:", merge_label);

    free(condition_value);
    free(then_label);
    free(else_label);
    free(merge_label);
    free(then_value);
    free(else_value);
    return result;
}

static char *cc_codegen_expression(CCCodegenContext *context, const CCAstNode *expression) {
    CCConstValue folded;

    if (expression == NULL) {
        return cc_duplicate_string("<null>");
    }

    folded = cc_try_fold_integer_constant(expression);
    if (folded.ok) {
        return cc_format_string("%lld", folded.value);
    }

    switch (expression->kind) {
        case CC_AST_IDENTIFIER: {
            const char *emitted_name;

            if (expression->text == NULL) {
                return cc_duplicate_string("<value>");
            }

            emitted_name = cc_lookup_emitted_name(context, expression->text);
            return emitted_name == NULL ? cc_duplicate_string(expression->text) : cc_duplicate_string(emitted_name);
        }
        case CC_AST_LITERAL:
            return expression->text == NULL ? cc_duplicate_string("<value>") : cc_duplicate_string(expression->text);
        case CC_AST_UNARY_EXPRESSION: {
            char *operand;
            char *place;
            char *temp;

            if (expression->child_count == 0) {
                return cc_duplicate_string("<invalid_unary>");
            }

            if (expression->text != NULL
                && (strcmp(expression->text, "increment") == 0
                    || strcmp(expression->text, "decrement") == 0
                    || strcmp(expression->text, "post++") == 0
                    || strcmp(expression->text, "post--") == 0)) {
                bool is_postfix;
                const char *opcode;
                const char *step;

                place = cc_codegen_place_expression(context, expression->children[0]);
                is_postfix = strcmp(expression->text, "post++") == 0 || strcmp(expression->text, "post--") == 0;
                opcode = (strcmp(expression->text, "decrement") == 0 || strcmp(expression->text, "post--") == 0)
                    ? "sub"
                    : "add";
                step = "1";

                if (is_postfix) {
                    temp = cc_new_temp(context);
                    cc_emit_line(context, "  %s = %s", temp, place);
                    cc_emit_line(context, "  %s = %s %s, %s", place, opcode, place, step);
                    free(place);
                    return temp;
                }

                cc_emit_line(context, "  %s = %s %s, %s", place, opcode, place, step);
                return place;
            }

            operand = cc_codegen_expression(context, expression->children[0]);
            temp = cc_new_temp(context);

            if (expression->text != NULL && strcmp(expression->text, "ampersand") == 0) {
                place = cc_codegen_place_expression(context, expression->children[0]);
                cc_emit_line(context, "  %s = addr %s", temp, place);
                free(place);
            } else if (expression->text != NULL && strcmp(expression->text, "star") == 0) {
                cc_emit_line(context, "  %s = load %s", temp, operand);
            } else if (expression->text != NULL && strcmp(expression->text, "bang") == 0) {
                cc_emit_line(context, "  %s = not %s", temp, operand);
            } else if (expression->text != NULL && strcmp(expression->text, "tilde") == 0) {
                cc_emit_line(context, "  %s = bitnot %s", temp, operand);
            } else if (expression->text != NULL && strcmp(expression->text, "plus") == 0) {
                cc_emit_line(context, "  %s = identity %s", temp, operand);
            } else if (expression->text != NULL && strcmp(expression->text, "minus") == 0) {
                cc_emit_line(context, "  %s = neg %s", temp, operand);
            } else if (expression->text != NULL
                && (strcmp(expression->text, "kw_sizeof") == 0 || strcmp(expression->text, "kw__Alignof") == 0)) {
                cc_emit_line(context, "  %s = %s %s", temp, expression->text, operand);
            } else {
                cc_emit_line(context, "  %s = %s %s", temp, expression->text == NULL ? "unary" : expression->text, operand);
            }

            free(operand);
            return temp;
        }
        case CC_AST_BINARY_EXPRESSION: {
            char *left_value;
            char *right_value;
            char *result;
            char *place;
            const char *opcode;

            if (expression->child_count < 2) {
                return cc_duplicate_string("<invalid_binary>");
            }

            if (cc_is_assignment_operator(expression->text)) {
                place = cc_codegen_place_expression(context, expression->children[0]);
                right_value = cc_codegen_expression(context, expression->children[1]);

                if (strcmp(expression->text, "=") == 0) {
                    cc_emit_line(context, "  %s = %s", place, right_value);
                    free(right_value);
                    return place;
                }

                opcode = cc_compound_assignment_opcode(expression->text);
                result = cc_new_temp(context);
                cc_emit_line(context, "  %s = %s %s, %s", result, opcode == NULL ? "op" : opcode, place, right_value);
                cc_emit_line(context, "  %s = %s", place, result);
                free(place);
                free(right_value);
                return result;
            }

            if (expression->text != NULL && strcmp(expression->text, ",") == 0) {
                left_value = cc_codegen_expression(context, expression->children[0]);
                right_value = cc_codegen_expression(context, expression->children[1]);
                free(left_value);
                return right_value;
            }

            left_value = cc_codegen_expression(context, expression->children[0]);
            right_value = cc_codegen_expression(context, expression->children[1]);
            result = cc_new_temp(context);
            cc_emit_line(
                context,
                "  %s = %s %s, %s",
                result,
                cc_binary_opcode(expression->text),
                left_value,
                right_value
            );
            free(left_value);
            free(right_value);
            return result;
        }
        case CC_AST_CONDITIONAL_EXPRESSION:
            if (expression->child_count < 3) {
                return cc_duplicate_string("<invalid_conditional>");
            }
            return cc_codegen_conditional_expression(context, expression);
        case CC_AST_CALL_EXPRESSION: {
            CCStringBuilder builder;
            char *callee;
            char *result;
            size_t index;

            memset(&builder, 0, sizeof(builder));
            callee = expression->child_count > 0 ? cc_codegen_expression(context, expression->children[0]) : cc_duplicate_string("<callee>");
            cc_builder_append_cstring(&builder, callee);
            cc_builder_append_char(&builder, '(');

            if (expression->child_count > 1 && expression->children[1]->kind == CC_AST_ARGUMENT_LIST) {
                for (index = 0; index < expression->children[1]->child_count; index++) {
                    char *argument;

                    if (index > 0) {
                        cc_builder_append_cstring(&builder, ", ");
                    }

                    argument = cc_codegen_expression(context, expression->children[1]->children[index]);
                    cc_builder_append_cstring(&builder, argument);
                    free(argument);
                }
            }

            cc_builder_append_char(&builder, ')');
            result = cc_new_temp(context);
            cc_emit_line(context, "  %s = call %s", result, builder.data == NULL ? callee : builder.data);
            free(callee);
            free(builder.data);
            return result;
        }
        case CC_AST_SUBSCRIPT_EXPRESSION: {
            char *place;
            char *result;

            place = cc_codegen_place_expression(context, expression);
            result = cc_new_temp(context);
            cc_emit_line(context, "  %s = load %s", result, place);
            free(place);
            return result;
        }
        case CC_AST_MEMBER_EXPRESSION: {
            char *place;
            char *result;

            place = cc_codegen_place_expression(context, expression);
            result = cc_new_temp(context);
            cc_emit_line(context, "  %s = load %s", result, place);
            free(place);
            return result;
        }
        case CC_AST_CAST_EXPRESSION: {
            char *type_name;
            char *operand;
            char *result;

            if (expression->child_count < 2) {
                return cc_duplicate_string("<invalid_cast>");
            }

            type_name = cc_render_type_name_node(expression->children[0]);
            operand = cc_codegen_expression(context, expression->children[1]);
            result = cc_new_temp(context);
            cc_emit_line(context, "  %s = cast %s, %s", result, type_name, operand);
            free(type_name);
            free(operand);
            return result;
        }
        default:
            cc_add_diagnostic(context, expression->span, "unsupported expression in code generation");
            return cc_duplicate_string("<unsupported_expr>");
    }
}

static void cc_codegen_statement(CCCodegenContext *context, const CCAstNode *statement);

static void cc_codegen_declaration(CCCodegenContext *context, const CCAstNode *declaration, bool file_scope) {
    const CCAstNode *specifiers;
    size_t index;

    specifiers = declaration->child_count > 0 ? declaration->children[0] : NULL;
    if (cc_specifiers_contain_text(specifiers, "typedef")) {
        return;
    }

    for (index = 1; index < declaration->child_count; index++) {
        const CCAstNode *init_declarator;
        const CCAstNode *declarator;
        const CCAstNode *initializer;
        char *type_name;
        char *value;
        char *emitted_name;

        init_declarator = declaration->children[index];
        if (init_declarator->kind != CC_AST_INIT_DECLARATOR || init_declarator->child_count == 0) {
            continue;
        }

        declarator = init_declarator->children[0];
        initializer = init_declarator->child_count > 1 ? init_declarator->children[1] : NULL;
        type_name = cc_render_declarator_type(specifiers, declarator);

        if (declarator->text == NULL) {
            free(type_name);
            continue;
        }

        if (file_scope && cc_find_parameter_list(declarator) != NULL) {
            CCStringBuilder signature;
            const CCAstNode *parameter_list;
            size_t parameter_index;

            memset(&signature, 0, sizeof(signature));
            cc_builder_append_cstring(&signature, declarator->text);
            cc_builder_append_char(&signature, '(');
            parameter_list = cc_find_parameter_list(declarator);
            if (parameter_list != NULL) {
                for (parameter_index = 0; parameter_index < parameter_list->child_count; parameter_index++) {
                    const CCAstNode *parameter;
                    const CCAstNode *parameter_specifiers;
                    const CCAstNode *parameter_declarator;
                    size_t child_index;
                    char *parameter_type;

                    parameter = parameter_list->children[parameter_index];
                    if (parameter_index > 0) {
                        cc_builder_append_cstring(&signature, ", ");
                    }

                    if (parameter->text != NULL) {
                        cc_builder_append_cstring(&signature, parameter->text);
                        continue;
                    }

                    parameter_specifiers = NULL;
                    parameter_declarator = NULL;
                    for (child_index = 0; child_index < parameter->child_count; child_index++) {
                        if (parameter->children[child_index]->kind == CC_AST_DECLARATION_SPECIFIERS) {
                            parameter_specifiers = parameter->children[child_index];
                        } else if (parameter->children[child_index]->kind == CC_AST_DECLARATOR) {
                            parameter_declarator = parameter->children[child_index];
                        }
                    }

                    parameter_type = cc_render_declarator_type(parameter_specifiers, parameter_declarator);
                    if (parameter_declarator != NULL && parameter_declarator->text != NULL) {
                        cc_builder_append_cstring(&signature, parameter_declarator->text);
                        cc_builder_append_cstring(&signature, ": ");
                    }
                    cc_builder_append_cstring(&signature, parameter_type);
                    free(parameter_type);
                }
            }
            cc_builder_append_char(&signature, ')');
            cc_emit_line(context, "declare %s -> %s", signature.data, type_name);
            free(signature.data);
            free(type_name);
            continue;
        }

        if (file_scope) {
            cc_emit_line(context, "global %s : %s", declarator->text, type_name);
            emitted_name = cc_duplicate_string(declarator->text);
        } else {
            emitted_name = cc_bind_local_name(context, declarator->text);
            cc_emit_line(
                context,
                "  var %s : %s",
                emitted_name == NULL ? declarator->text : emitted_name,
                type_name
            );
        }

        if (initializer != NULL) {
            value = cc_codegen_expression(context, initializer);
            cc_emit_line(
                context,
                "  %s = %s",
                emitted_name == NULL ? declarator->text : emitted_name,
                value
            );
            free(value);
        }

        free(emitted_name);
        free(type_name);
    }
}

static void cc_codegen_compound_statement(CCCodegenContext *context, const CCAstNode *statement, bool push_scope) {
    size_t index;

    if (statement == NULL) {
        return;
    }

    if (push_scope) {
        cc_push_name_scope(context);
    }

    for (index = 0; index < statement->child_count; index++) {
        cc_codegen_statement(context, statement->children[index]);
    }

    if (push_scope) {
        cc_pop_name_scope(context);
    }
}

static void cc_codegen_if_statement(CCCodegenContext *context, const CCAstNode *statement) {
    char *condition;
    char *then_label;
    char *else_label;
    char *merge_label;
    bool has_else;

    condition = cc_codegen_expression(context, statement->children[0]);
    then_label = cc_new_label(context, "if.then");
    merge_label = cc_new_label(context, "if.end");
    has_else = statement->child_count > 2;
    else_label = has_else ? cc_new_label(context, "if.else") : cc_duplicate_string(merge_label);

    cc_emit_line(context, "  cjump %s, %s, %s", condition, then_label, else_label);
    cc_emit_line(context, "%s:", then_label);
    cc_codegen_statement(context, statement->children[1]);
    cc_emit_line(context, "  jump %s", merge_label);

    if (has_else) {
        cc_emit_line(context, "%s:", else_label);
        cc_codegen_statement(context, statement->children[2]);
        cc_emit_line(context, "  jump %s", merge_label);
    }

    cc_emit_line(context, "%s:", merge_label);

    free(condition);
    free(then_label);
    free(else_label);
    free(merge_label);
}

static void cc_codegen_while_statement(CCCodegenContext *context, const CCAstNode *statement) {
    char *condition_label;
    char *body_label;
    char *end_label;
    char *condition;

    condition_label = cc_new_label(context, "while.cond");
    body_label = cc_new_label(context, "while.body");
    end_label = cc_new_label(context, "while.end");

    cc_push_control(context, end_label, condition_label);
    cc_emit_line(context, "  jump %s", condition_label);
    cc_emit_line(context, "%s:", condition_label);
    condition = cc_codegen_expression(context, statement->children[0]);
    cc_emit_line(context, "  cjump %s, %s, %s", condition, body_label, end_label);
    cc_emit_line(context, "%s:", body_label);
    cc_codegen_statement(context, statement->children[1]);
    cc_emit_line(context, "  jump %s", condition_label);
    cc_emit_line(context, "%s:", end_label);
    cc_pop_control(context);

    free(condition);
    free(condition_label);
    free(body_label);
    free(end_label);
}

static void cc_codegen_do_while_statement(CCCodegenContext *context, const CCAstNode *statement) {
    char *body_label;
    char *condition_label;
    char *end_label;
    char *condition;

    body_label = cc_new_label(context, "do.body");
    condition_label = cc_new_label(context, "do.cond");
    end_label = cc_new_label(context, "do.end");

    cc_push_control(context, end_label, condition_label);
    cc_emit_line(context, "%s:", body_label);
    if (statement->child_count > 0) {
        cc_codegen_statement(context, statement->children[0]);
    }
    cc_emit_line(context, "%s:", condition_label);
    condition = statement->child_count > 1
        ? cc_codegen_expression(context, statement->children[1])
        : cc_duplicate_string("1");
    cc_emit_line(context, "  cjump %s, %s, %s", condition, body_label, end_label);
    cc_emit_line(context, "%s:", end_label);
    cc_pop_control(context);

    free(body_label);
    free(condition_label);
    free(end_label);
    free(condition);
}

static void cc_codegen_for_statement(CCCodegenContext *context, const CCAstNode *statement) {
    const CCAstNode *body;
    const CCAstNode *condition_node;
    const CCAstNode *iteration_node;
    char *condition_label;
    char *body_label;
    char *iteration_label;
    char *end_label;
    char *condition;
    size_t middle_count;

    if (statement->child_count == 0) {
        return;
    }

    cc_push_name_scope(context);

    body = statement->children[statement->child_count - 1];
    middle_count = statement->child_count - 1;
    condition_node = middle_count > 1 ? statement->children[1] : NULL;
    iteration_node = middle_count > 2 ? statement->children[2] : NULL;

    if (statement->children[0]->kind == CC_AST_DECLARATION) {
        cc_codegen_declaration(context, statement->children[0], false);
    } else if (statement->children[0]->kind == CC_AST_EXPRESSION_STATEMENT) {
        if (statement->children[0]->child_count > 0) {
            char *value;

            value = cc_codegen_expression(context, statement->children[0]->children[0]);
            free(value);
        }
    }

    condition_label = cc_new_label(context, "for.cond");
    body_label = cc_new_label(context, "for.body");
    iteration_label = cc_new_label(context, "for.iter");
    end_label = cc_new_label(context, "for.end");

    cc_push_control(context, end_label, iteration_label);
    cc_emit_line(context, "  jump %s", condition_label);
    cc_emit_line(context, "%s:", condition_label);

    if (condition_node != NULL && condition_node->kind != CC_AST_EMPTY_STATEMENT) {
        condition = cc_codegen_expression(context, condition_node);
        cc_emit_line(context, "  cjump %s, %s, %s", condition, body_label, end_label);
        free(condition);
    } else {
        cc_emit_line(context, "  jump %s", body_label);
    }

    cc_emit_line(context, "%s:", body_label);
    cc_codegen_statement(context, body);
    cc_emit_line(context, "  jump %s", iteration_label);
    cc_emit_line(context, "%s:", iteration_label);

    if (iteration_node != NULL) {
        char *value;

        value = cc_codegen_expression(context, iteration_node);
        free(value);
    }

    cc_emit_line(context, "  jump %s", condition_label);
    cc_emit_line(context, "%s:", end_label);
    cc_pop_control(context);
    cc_pop_name_scope(context);

    free(condition_label);
    free(body_label);
    free(iteration_label);
    free(end_label);
}

static void cc_codegen_switch_statement(CCCodegenContext *context, const CCAstNode *statement) {
    CCSwitchLayout layout;
    char *switch_value;
    char *dispatch_label;
    char *end_label;
    size_t index;

    if (statement->child_count < 2) {
        cc_add_diagnostic(context, statement->span, "malformed switch statement in code generation");
        return;
    }

    memset(&layout, 0, sizeof(layout));
    cc_collect_switch_layout(context, &layout, statement->children[1]);
    switch_value = cc_codegen_expression(context, statement->children[0]);
    dispatch_label = cc_new_label(context, "switch.dispatch");
    end_label = cc_new_label(context, "switch.end");

    cc_push_control(context, end_label, NULL);
    cc_emit_line(context, "  jump %s", dispatch_label);
    cc_codegen_statement(context, statement->children[1]);
    cc_emit_line(context, "  jump %s", end_label);
    cc_emit_line(context, "%s:", dispatch_label);

    if (layout.case_count == 0) {
        if (layout.default_node != NULL) {
            char *default_label;

            default_label = cc_default_label_name(layout.default_node);
            cc_emit_line(context, "  jump %s", default_label);
            free(default_label);
        } else {
            cc_emit_line(context, "  jump %s", end_label);
        }
    } else {
        for (index = 0; index < layout.case_count; index++) {
            char *next_label;
            char *case_label;
            char *compare_temp;
            char *case_value;
            bool has_next_case;

            has_next_case = index + 1 < layout.case_count;
            if (has_next_case) {
                next_label = cc_new_label(context, "switch.check");
            } else if (layout.default_node != NULL) {
                next_label = cc_default_label_name(layout.default_node);
            } else {
                next_label = cc_duplicate_string(end_label);
            }

            case_label = cc_case_label_name(layout.case_nodes[index]);
            compare_temp = cc_new_temp(context);
            case_value = cc_format_string("%lld", layout.case_values[index]);
            cc_emit_line(context, "  %s = eq %s, %s", compare_temp, switch_value, case_value);
            cc_emit_line(context, "  cjump %s, %s, %s", compare_temp, case_label, next_label);

            if (has_next_case) {
                cc_emit_line(context, "%s:", next_label);
            }

            free(next_label);
            free(case_label);
            free(compare_temp);
            free(case_value);
        }
    }

    cc_emit_line(context, "%s:", end_label);
    cc_pop_control(context);

    cc_free_switch_layout(&layout);
    free(switch_value);
    free(dispatch_label);
    free(end_label);
}

static void cc_codegen_statement(CCCodegenContext *context, const CCAstNode *statement) {
    if (statement == NULL) {
        return;
    }

    switch (statement->kind) {
        case CC_AST_COMPOUND_STATEMENT:
            cc_codegen_compound_statement(context, statement, true);
            break;
        case CC_AST_DECLARATION:
            cc_codegen_declaration(context, statement, false);
            break;
        case CC_AST_IF_STATEMENT:
            if (statement->child_count >= 2) {
                cc_codegen_if_statement(context, statement);
            }
            break;
        case CC_AST_WHILE_STATEMENT:
            if (statement->child_count >= 2) {
                cc_codegen_while_statement(context, statement);
            }
            break;
        case CC_AST_DO_WHILE_STATEMENT:
            cc_codegen_do_while_statement(context, statement);
            break;
        case CC_AST_FOR_STATEMENT:
            cc_codegen_for_statement(context, statement);
            break;
        case CC_AST_SWITCH_STATEMENT:
            cc_codegen_switch_statement(context, statement);
            break;
        case CC_AST_CASE_STATEMENT: {
            char *label_name;

            label_name = cc_case_label_name(statement);
            cc_emit_line(context, "%s:", label_name);
            if (statement->child_count > 1) {
                cc_codegen_statement(context, statement->children[1]);
            }
            free(label_name);
            break;
        }
        case CC_AST_DEFAULT_STATEMENT: {
            char *label_name;

            label_name = cc_default_label_name(statement);
            cc_emit_line(context, "%s:", label_name);
            if (statement->child_count > 0) {
                cc_codegen_statement(context, statement->children[0]);
            }
            free(label_name);
            break;
        }
        case CC_AST_GOTO_STATEMENT: {
            char *label_name;

            label_name = cc_user_label_name(context, statement->text);
            cc_emit_line(context, "  jump %s", label_name);
            free(label_name);
            break;
        }
        case CC_AST_LABEL_STATEMENT: {
            char *label_name;

            label_name = cc_user_label_name(context, statement->text);
            cc_emit_line(context, "%s:", label_name);
            if (statement->child_count > 0) {
                cc_codegen_statement(context, statement->children[0]);
            }
            free(label_name);
            break;
        }
        case CC_AST_RETURN_STATEMENT:
            if (statement->child_count > 0) {
                char *value;

                value = cc_codegen_expression(context, statement->children[0]);
                cc_emit_line(context, "  ret %s", value);
                free(value);
            } else {
                cc_emit_line(context, "  ret");
            }
            break;
        case CC_AST_BREAK_STATEMENT:
            if (cc_current_break_label(context) == NULL) {
                cc_add_diagnostic(context, statement->span, "'break' used outside a loop during code generation");
            } else {
                cc_emit_line(context, "  jump %s", cc_current_break_label(context));
            }
            break;
        case CC_AST_CONTINUE_STATEMENT:
            if (cc_current_continue_label(context) == NULL) {
                cc_add_diagnostic(context, statement->span, "'continue' used outside a loop during code generation");
            } else {
                cc_emit_line(context, "  jump %s", cc_current_continue_label(context));
            }
            break;
        case CC_AST_EXPRESSION_STATEMENT:
            if (statement->child_count > 0) {
                char *value;

                value = cc_codegen_expression(context, statement->children[0]);
                free(value);
            }
            break;
        case CC_AST_EMPTY_STATEMENT:
            break;
        default:
            cc_add_diagnostic(context, statement->span, "unsupported statement in code generation");
            break;
    }
}

static void cc_codegen_function(CCCodegenContext *context, const CCAstNode *function) {
    const CCAstNode *specifiers;
    const CCAstNode *declarator;
    const CCAstNode *body;
    const CCAstNode *parameter_list;
    char *return_type;
    CCStringBuilder signature;
    const char *saved_function_name;
    size_t index;

    if (function->child_count < 3) {
        return;
    }

    specifiers = function->children[0];
    declarator = function->children[1];
    body = function->children[2];
    parameter_list = cc_find_parameter_list(declarator);
    return_type = cc_render_declarator_type(specifiers, declarator);
    memset(&signature, 0, sizeof(signature));
    saved_function_name = context->current_function_name;
    context->current_function_name = function->text;

    cc_builder_append_cstring(&signature, function->text == NULL ? "<anonymous>" : function->text);
    cc_builder_append_char(&signature, '(');
    if (parameter_list != NULL) {
        for (index = 0; index < parameter_list->child_count; index++) {
            const CCAstNode *parameter;
            const CCAstNode *parameter_specifiers;
            const CCAstNode *parameter_declarator;
            char *parameter_type;
            size_t child_index;

            parameter = parameter_list->children[index];
            if (index > 0) {
                cc_builder_append_cstring(&signature, ", ");
            }

            if (parameter->text != NULL) {
                cc_builder_append_cstring(&signature, parameter->text);
                continue;
            }

            parameter_specifiers = NULL;
            parameter_declarator = NULL;
            for (child_index = 0; child_index < parameter->child_count; child_index++) {
                if (parameter->children[child_index]->kind == CC_AST_DECLARATION_SPECIFIERS) {
                    parameter_specifiers = parameter->children[child_index];
                } else if (parameter->children[child_index]->kind == CC_AST_DECLARATOR) {
                    parameter_declarator = parameter->children[child_index];
                }
            }

            parameter_type = cc_render_declarator_type(parameter_specifiers, parameter_declarator);
            if (parameter_declarator != NULL && parameter_declarator->text != NULL) {
                cc_builder_append_cstring(&signature, parameter_declarator->text);
                cc_builder_append_cstring(&signature, ": ");
            }
            cc_builder_append_cstring(&signature, parameter_type);
            free(parameter_type);
        }
    }
    cc_builder_append_char(&signature, ')');

    cc_emit_line(context, "func %s -> %s", signature.data, return_type);
    cc_emit_line(context, "entry:");
    cc_push_name_scope(context);
    if (parameter_list != NULL) {
        for (index = 0; index < parameter_list->child_count; index++) {
            const CCAstNode *parameter;
            const CCAstNode *parameter_declarator;
            size_t child_index;
            char *bound_name;

            parameter = parameter_list->children[index];
            if (parameter->text != NULL) {
                continue;
            }

            parameter_declarator = NULL;
            for (child_index = 0; child_index < parameter->child_count; child_index++) {
                if (parameter->children[child_index]->kind == CC_AST_DECLARATOR) {
                    parameter_declarator = parameter->children[child_index];
                    break;
                }
            }

            if (parameter_declarator == NULL || parameter_declarator->text == NULL) {
                continue;
            }

            bound_name = cc_bind_local_name(context, parameter_declarator->text);
            free(bound_name);
        }
    }
    if (body != NULL && body->kind == CC_AST_COMPOUND_STATEMENT) {
        cc_codegen_compound_statement(context, body, false);
    } else {
        cc_codegen_statement(context, body);
    }
    cc_pop_name_scope(context);
    cc_emit_line(context, "endfunc");
    cc_emit_line(context, "");

    context->current_function_name = saved_function_name;
    free(return_type);
    free(signature.data);
}

void cc_codegen_translation_unit(const CCParseResult *parse_result, CCCodegenResult *result) {
    CCCodegenContext context;
    size_t index;

    memset(&context, 0, sizeof(context));
    memset(result, 0, sizeof(*result));
    context.parse_result = parse_result;

    if (parse_result->translation_unit != NULL) {
        for (index = 0; index < parse_result->translation_unit->child_count; index++) {
            const CCAstNode *child;

            child = parse_result->translation_unit->children[index];
            switch (child->kind) {
                case CC_AST_PREPROCESSOR_LINE:
                    break;
                case CC_AST_DECLARATION:
                    cc_codegen_declaration(&context, child, true);
                    break;
                case CC_AST_FUNCTION_DEFINITION:
                    cc_codegen_function(&context, child);
                    break;
                default:
                    cc_add_diagnostic(&context, child->span, "unsupported top-level declaration in code generation");
                    break;
            }
        }
    }

    result->text = cc_builder_take_string(&context.output);
    result->diagnostics = context.diagnostics;

    free(context.control_stack);
    while (context.name_scope_count > 0) {
        cc_pop_name_scope(&context);
    }
    free(context.name_scopes);
}

void cc_codegen_result_free(CCCodegenResult *result) {
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
}
