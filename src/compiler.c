#include "compiler.h"

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "vm.h"

#define MAX_LOCALS 1024
#define MAX_UPVALUES 256

typedef struct {
    Text name;
    int depth;
    bool captured;
    bool is_const;
    int debug_index;
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
    bool is_const;
} Upvalue;

typedef enum {
    FUNC_SCRIPT,
    FUNC_FUNCTION,
    FUNC_INITIALIZER,
    FUNC_METHOD
} FunctionType;

typedef struct LoopCtx {
    struct LoopCtx *previous;
    Text label;
    bool is_switch;
    int start;
    int continue_target; /* -1 while it is not known */
    int local_count;
    int *breaks;
    int break_count;
    int break_capacity;
    int *continues;
    int continue_count;
    int continue_capacity;
    int try_depth;
} LoopCtx;

typedef struct ConstGlobal {
    ObjString *name;
    struct ConstGlobal *next;
} ConstGlobal;

typedef struct {
    jmp_buf jump;
    char message[256];
    int line;
    int column;
    bool repl_mode;
    const char *source;
    ObjString *script_path;
    ConstGlobal *const_globals;
} CompileContext;

typedef struct Compiler {
    struct Compiler *enclosing;
    ObjFunction *function;
    FunctionType type;
    Local locals[MAX_LOCALS];
    int local_count;
    Upvalue upvalues[MAX_UPVALUES];
    int upvalue_count;
    int scope_depth;
    int column;
    LoopCtx *loop;
    int try_depth;
    bool has_self;
    CompileContext *ctx;
} Compiler;

static void compile_statement(Compiler *compiler, Stmt *stmt);
static void compile_expression(Compiler *compiler, Expr *expr);
static void compile_tail_return(Compiler *compiler, Expr *expr, int line, int column);
static bool eval_constant_expr(Compiler *compiler, Expr *expr, Value *out_val);
static bool stmt_is_terminator(const Stmt *stmt);

static void compile_error(Compiler *compiler, int line, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(compiler->ctx->message, sizeof(compiler->ctx->message), fmt, ap);
    va_end(ap);
    compiler->ctx->line = line;
    compiler->ctx->column = compiler->column;
    longjmp(compiler->ctx->jump, 1);
}

static bool text_equals(Text a, Text b)
{
    return a.length == b.length && memcmp(a.chars, b.chars, (size_t)a.length) == 0;
}

static ObjString *name_constant(Text name)
{
    return object_string(name.chars, name.length);
}

/* The value of a numeric literal, which may be an integer or a double. */
static Value expr_number_value(const Expr *expr)
{
    if (expr->as.number.is_int) {
        return INT_VAL(expr->as.number.integer);
    }
    return NUMBER_VAL(expr->as.number.number);
}

static void emit_byte(Compiler *compiler, uint8_t byte, int line)
{
    chunk_write(&compiler->function->chunk, byte, line, compiler->column);
}

static void emit_u16(Compiler *compiler, uint16_t value, int line)
{
    emit_byte(compiler, (uint8_t)(value >> 8), line);
    emit_byte(compiler, (uint8_t)(value & 0xff), line);
}

/* Adds a constant and returns its index, without emitting OP_CONSTANT. */
static int add_constant(Compiler *compiler, Value value, int line)
{
    int index = chunk_add_constant(&compiler->function->chunk, value);

    if (index > 0xffff) {
        compile_error(compiler, line, "too many constants in one function");
    }
    return index;
}

static void emit_constant(Compiler *compiler, Value value, int line)
{
    int index = add_constant(compiler, value, line);

    emit_byte(compiler, OP_CONSTANT, line);
    emit_u16(compiler, (uint16_t)index, line);
}

/* Emits the slot index of a global variable. */
static void emit_global_name(Compiler *compiler, ObjString *name, int line)
{
    int slot = vm_global_slot(name);

    if (slot < 0) {
        compile_error(compiler, line, "too many global variables in the program");
    }
    emit_u16(compiler, (uint16_t)slot, line);
}

static int emit_jump(Compiler *compiler, uint8_t instruction, int line)
{
    emit_byte(compiler, instruction, line);
    emit_byte(compiler, 0xff, line);
    emit_byte(compiler, 0xff, line);
    return compiler->function->chunk.count - 2;
}

static int last_line(Compiler *compiler)
{
    int count = compiler->function->chunk.count;

    return count > 0 ? compiler->function->chunk.lines[count - 1] : 1;
}

static void patch_offset(Compiler *compiler, int offset)
{
    int jump = compiler->function->chunk.count - offset - 2;

    if (jump < 0) {
        compile_error(compiler, last_line(compiler), "invalid jump in the bytecode");
    }
    if (jump > 0xffff) {
        compile_error(compiler, last_line(compiler), "block too long");
    }
    compiler->function->chunk.code[offset] = (uint8_t)((jump >> 8) & 0xff);
    compiler->function->chunk.code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void patch_jump(Compiler *compiler, int offset)
{
    patch_offset(compiler, offset);
}

static void emit_loop(Compiler *compiler, int start, int line)
{
    int offset = compiler->function->chunk.count - start + 3;

    if (offset > 0xffff) {
        compile_error(compiler, line, "loop too long");
    }
    emit_byte(compiler, OP_LOOP, line);
    emit_u16(compiler, (uint16_t)offset, line);
}

static void init_compiler(Compiler *compiler, Compiler *enclosing, FunctionType type,
                          CompileContext *ctx)
{
    memset(compiler, 0, sizeof(*compiler));
    compiler->enclosing = enclosing;
    compiler->type = type;
    compiler->ctx = ctx;
    compiler->function = object_function();
    compiler->function->source = ctx->source;
    compiler->function->script_path = ctx->script_path;
    compiler->scope_depth = type != FUNC_SCRIPT ? 1 : 0;

    /* Slot 0 holds the running closure. */
    compiler->locals[0].name.chars = "";
    compiler->locals[0].name.length = 0;
    compiler->locals[0].depth = 0;
    compiler->locals[0].captured = false;
    compiler->locals[0].is_const = false;
    compiler->locals[0].debug_index = -1;
    compiler->local_count = 1;
}

static int function_add_debug_local(ObjFunction *function, int slot, ObjString *name)
{
    if (function->debug_local_count == function->debug_local_capacity) {
        int old_cap = function->debug_local_capacity;
        int new_cap = old_cap < 8 ? 8 : old_cap * 2;
        function->debug_locals = mem_realloc(function->debug_locals,
                                             sizeof(DebugLocal) * (size_t)old_cap,
                                             sizeof(DebugLocal) * (size_t)new_cap);
        function->debug_local_capacity = new_cap;
    }
    int index = function->debug_local_count++;
    function->debug_locals[index].slot = slot;
    function->debug_locals[index].name = name;
    function->debug_locals[index].start_pc = INT_MAX;
    function->debug_locals[index].end_pc = INT_MAX;
    return index;
}

static int add_local(Compiler *compiler, Text name, int line)
{
    if (compiler->local_count == MAX_LOCALS) {
        compile_error(compiler, line, "too many local variables in one function");
    }
    int slot = compiler->local_count;
    Local *local = &compiler->locals[slot];

    local->name = name;
    local->depth = -1;
    local->captured = false;
    local->is_const = false;
    local->debug_index = -1;

    if (name.length > 0 && name.chars[0] != '<' && name.chars[0] != ' ') {
        ObjString *debug_name = name_constant(name);
        local->debug_index = function_add_debug_local(compiler->function, slot, debug_name);
        if (compiler->scope_depth == 0) {
            compiler->function->debug_locals[local->debug_index].start_pc = 0;
        }
    }
    return compiler->local_count++;
}

static int resolve_local(Compiler *compiler, Text name, int line)
{
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local *local = &compiler->locals[i];

        if (text_equals(local->name, name)) {
            if (local->depth == -1) {
                compile_error(compiler, line,
                              "cannot use variable '%.*s' inside its own initializer",
                              name.length, name.chars);
            }
            return i;
        }
    }
    return -1;
}

static int add_upvalue(Compiler *compiler, uint8_t index, bool is_local, bool is_const,
                       int line)
{
    for (int i = 0; i < compiler->upvalue_count; i++) {
        Upvalue *upvalue = &compiler->upvalues[i];

        if (upvalue->index == index && upvalue->is_local == is_local) {
            return i;
        }
    }
    if (compiler->upvalue_count == MAX_UPVALUES) {
        compile_error(compiler, line, "too many variables captured by one function");
    }
    Upvalue *upvalue = &compiler->upvalues[compiler->upvalue_count];

    upvalue->index = index;
    upvalue->is_local = is_local;
    upvalue->is_const = is_const;
    compiler->function->upvalue_count = compiler->upvalue_count + 1;
    return compiler->upvalue_count++;
}

static int resolve_upvalue(Compiler *compiler, Text name, int line)
{
    if (!compiler->enclosing) {
        return -1;
    }
    int local = resolve_local(compiler->enclosing, name, line);
    if (local >= 0) {
        compiler->enclosing->locals[local].captured = true;
        return add_upvalue(compiler, (uint8_t)local, true,
                           compiler->enclosing->locals[local].is_const, line);
    }
    int upvalue = resolve_upvalue(compiler->enclosing, name, line);
    if (upvalue >= 0) {
        return add_upvalue(compiler, (uint8_t)upvalue, false,
                           compiler->enclosing->upvalues[upvalue].is_const, line);
    }
    return -1;
}

static void begin_scope(Compiler *compiler)
{
    compiler->scope_depth++;
}

static void end_scope(Compiler *compiler, int line)
{
    compiler->scope_depth--;
    int current_pc = compiler->function->chunk.count;
    while (compiler->local_count > 0 &&
           compiler->locals[compiler->local_count - 1].depth > compiler->scope_depth) {
        Local *local = &compiler->locals[compiler->local_count - 1];
        if (local->debug_index >= 0 && local->debug_index < compiler->function->debug_local_count) {
            compiler->function->debug_locals[local->debug_index].end_pc = current_pc;
        }
        if (local->captured) {
            emit_byte(compiler, OP_CLOSE_UPVALUE, line);
        } else {
            emit_byte(compiler, OP_POP, line);
        }
        compiler->local_count--;
    }
}

static void mark_local_initialized(Compiler *compiler)
{
    if (compiler->scope_depth > 0 && compiler->local_count > 0) {
        Local *local = &compiler->locals[compiler->local_count - 1];
        local->depth = compiler->scope_depth;
        if (local->debug_index >= 0 && local->debug_index < compiler->function->debug_local_count) {
            compiler->function->debug_locals[local->debug_index].start_pc = compiler->function->chunk.count;
        }
    }
}

typedef enum {
    VAR_LOCAL,
    VAR_UPVALUE,
    VAR_GLOBAL,
    VAR_SELF
} VarKind;

typedef struct {
    VarKind kind;
    int index;
    ObjString *name;
} VarRef;

static VarRef resolve_variable(Compiler *compiler, Text name, int line)
{
    VarRef ref;

    ref.name = NULL;
    int local = resolve_local(compiler, name, line);
    if (local >= 0) {
        ref.kind = VAR_LOCAL;
        ref.index = local;
        return ref;
    }
    int upvalue = resolve_upvalue(compiler, name, line);
    if (upvalue >= 0) {
        ref.kind = VAR_UPVALUE;
        ref.index = upvalue;
        return ref;
    }
    if ((compiler->type == FUNC_FUNCTION || compiler->type == FUNC_INITIALIZER ||
         compiler->type == FUNC_METHOD) &&
        name.length == 4 && memcmp(name.chars, "self", 4) == 0) {
        ref.kind = VAR_SELF;
        ref.index = 0;
        return ref;
    }
    ref.kind = VAR_GLOBAL;
    ref.index = 0;
    ref.name = name_constant(name);
    return ref;
}

static void emit_get_slot(Compiler *compiler, int slot, int line)
{
    if (slot <= 0xff) {
        emit_byte(compiler, OP_GET_LOCAL, line);
        emit_byte(compiler, (uint8_t)slot, line);
    } else {
        emit_byte(compiler, OP_GET_LOCAL_16, line);
        emit_u16(compiler, (uint16_t)slot, line);
    }
}

static void emit_set_slot(Compiler *compiler, int slot, int line)
{
    if (slot <= 0xff) {
        emit_byte(compiler, OP_SET_LOCAL, line);
        emit_byte(compiler, (uint8_t)slot, line);
    } else {
        emit_byte(compiler, OP_SET_LOCAL_16, line);
        emit_u16(compiler, (uint16_t)slot, line);
    }
}

static void emit_get(Compiler *compiler, VarRef ref, int line)
{
    switch (ref.kind) {
    case VAR_LOCAL:
        if (ref.index <= 0xff) {
            emit_byte(compiler, OP_GET_LOCAL, line);
            emit_byte(compiler, (uint8_t)ref.index, line);
        } else {
            emit_byte(compiler, OP_GET_LOCAL_16, line);
            emit_u16(compiler, (uint16_t)ref.index, line);
        }
        break;
    case VAR_UPVALUE:
        emit_byte(compiler, OP_GET_UPVALUE, line);
        emit_byte(compiler, (uint8_t)ref.index, line);
        break;
    case VAR_GLOBAL:
        emit_byte(compiler, OP_GET_GLOBAL, line);
        emit_global_name(compiler, ref.name, line);
        break;
    case VAR_SELF:
        emit_byte(compiler, OP_SELF, line);
        break;
    }
}

static void emit_set(Compiler *compiler, VarRef ref, int line)
{
    switch (ref.kind) {
    case VAR_LOCAL:
        if (ref.index <= 0xff) {
            emit_byte(compiler, OP_SET_LOCAL, line);
            emit_byte(compiler, (uint8_t)ref.index, line);
        } else {
            emit_byte(compiler, OP_SET_LOCAL_16, line);
            emit_u16(compiler, (uint16_t)ref.index, line);
        }
        break;
    case VAR_UPVALUE:
        emit_byte(compiler, OP_SET_UPVALUE, line);
        emit_byte(compiler, (uint8_t)ref.index, line);
        break;
    case VAR_GLOBAL:
        emit_byte(compiler, OP_SET_GLOBAL, line);
        emit_global_name(compiler, ref.name, line);
        break;
    case VAR_SELF:
        compile_error(compiler, line, "cannot assign to 'self'");
        break;
    }
}

static bool global_is_const(Compiler *compiler, ObjString *name)
{
    for (ConstGlobal *entry = compiler->ctx->const_globals; entry; entry = entry->next) {
        if (entry->name == name) {
            return true;
        }
    }
    return vm_global_is_const(name);
}

static void mark_global_const(Compiler *compiler, ObjString *name, int line)
{
    for (ConstGlobal *entry = compiler->ctx->const_globals; entry; entry = entry->next) {
        if (entry->name == name) {
            return;
        }
    }
    ConstGlobal *entry = malloc(sizeof(ConstGlobal));

    if (!entry) {
        compile_error(compiler, line, "out of memory while compiling the constants");
    }
    entry->name = name;
    entry->next = compiler->ctx->const_globals;
    compiler->ctx->const_globals = entry;
}

static void check_mutable(Compiler *compiler, VarRef ref, Text name, int line)
{
    bool is_const = false;

    switch (ref.kind) {
    case VAR_LOCAL: is_const = compiler->locals[ref.index].is_const; break;
    case VAR_UPVALUE: is_const = compiler->upvalues[ref.index].is_const; break;
    case VAR_GLOBAL: is_const = global_is_const(compiler, ref.name); break;
    case VAR_SELF:
        compile_error(compiler, line, "cannot assign to 'self'");
        return;
    }
    if (is_const) {
        compile_error(compiler, line, "cannot assign to constant '%.*s'", name.length,
                      name.chars);
    }
}

static void compile_function_value(Compiler *enclosing, Text name, ParamList *params,
                                   Stmt *body, int line)
{
    Compiler fn;
    int constant = 0;

    init_compiler(&fn, enclosing, FUNC_FUNCTION, enclosing->ctx);
    if (name.length > 0) {
        fn.function->name = object_string(name.chars, name.length);
        fn.locals[0].name = name;
        fn.locals[0].is_const = true;
    }

    fn.function->arity = 0;
    fn.function->min_arity = 0;
    fn.function->is_varargs = false;
    for (ParamList *param = params; param; param = param->next) {
        add_local(&fn, param->name, line);
        if (param->is_varargs) {
            fn.function->is_varargs = true;
        } else {
            fn.function->arity++;
            if (!param->default_value) {
                fn.function->min_arity++;
            }
        }
    }

    int param_slot = 1;
    for (ParamList *param = params; param; param = param->next, param_slot++) {
        if (param->default_value) {
            emit_byte(&fn, OP_DEFAULT_ARG, line);
            emit_byte(&fn, (uint8_t)param_slot, line);
            emit_byte(&fn, 0xff, line);
            emit_byte(&fn, 0xff, line);
            int jump = fn.function->chunk.count - 2;
            compile_expression(&fn, param->default_value);
            emit_byte(&fn, OP_SET_LOCAL, line);
            emit_byte(&fn, (uint8_t)param_slot, line);
            patch_jump(&fn, jump);
        }
        fn.locals[param_slot].depth = fn.scope_depth;
        if (fn.locals[param_slot].debug_index >= 0) {
            fn.function->debug_locals[fn.locals[param_slot].debug_index].start_pc = 0;
        }
    }

    if (body->kind != STMT_BLOCK) {
        compile_error(enclosing, line, "the body of a function must be a block");
    }
    compile_statement(&fn, body);
    emit_byte(&fn, OP_NIL, body->line);
    emit_byte(&fn, OP_RETURN, body->line);

    constant = chunk_add_constant(&enclosing->function->chunk, OBJ_VAL(fn.function));
    if (constant > 0xffff) {
        compile_error(enclosing, line, "too many constants in one function");
    }
    emit_byte(enclosing, OP_CLOSURE, line);
    emit_u16(enclosing, (uint16_t)constant, line);
    for (int i = 0; i < fn.upvalue_count; i++) {
        emit_byte(enclosing, fn.upvalues[i].is_local ? 1 : 0, line);
        emit_byte(enclosing, fn.upvalues[i].index, line);
    }
}

static void compile_method_value(Compiler *enclosing, Text name, ParamList *params, Stmt *body,
                                 bool is_static, int line)
{
    Compiler fn;
    FunctionType type = (!is_static && name.length == 4 && memcmp(name.chars, "init", 4) == 0)
                            ? FUNC_INITIALIZER
                            : FUNC_METHOD;

    init_compiler(&fn, enclosing, type, enclosing->ctx);
    fn.function->name = object_string(name.chars, name.length);
    fn.locals[0].name = name;
    fn.locals[0].is_const = true;

    fn.function->arity = 0;
    fn.function->min_arity = 0;
    fn.function->is_varargs = false;
    for (ParamList *param = params; param; param = param->next) {
        add_local(&fn, param->name, line);
        if (param->is_varargs) {
            fn.function->is_varargs = true;
        } else {
            fn.function->arity++;
            if (!param->default_value) {
                fn.function->min_arity++;
            }
        }
    }

    int self_slot = -1;
    if (!is_static) {
        /* `self` lives in a local slot after the parameters so nested
         * functions can capture it like any other local. The value is pushed
         * once and stays on the stack: that slot is the local. */
        self_slot = add_local(&fn, (Text){"self", 4}, line);
        fn.locals[self_slot].is_const = true;
        fn.has_self = true;
        emit_byte(&fn, OP_SELF, line);
        fn.locals[self_slot].depth = fn.scope_depth;
        if (fn.locals[self_slot].debug_index >= 0) {
            fn.function->debug_locals[fn.locals[self_slot].debug_index].start_pc = 0;
        }
    }

    int param_slot = 1;
    for (ParamList *param = params; param; param = param->next, param_slot++) {
        if (param->default_value) {
            emit_byte(&fn, OP_DEFAULT_ARG, line);
            emit_byte(&fn, (uint8_t)param_slot, line);
            emit_byte(&fn, 0xff, line);
            emit_byte(&fn, 0xff, line);
            int jump = fn.function->chunk.count - 2;
            compile_expression(&fn, param->default_value);
            emit_byte(&fn, OP_SET_LOCAL, line);
            emit_byte(&fn, (uint8_t)param_slot, line);
            patch_jump(&fn, jump);
        }
        fn.locals[param_slot].depth = fn.scope_depth;
        if (fn.locals[param_slot].debug_index >= 0) {
            fn.function->debug_locals[fn.locals[param_slot].debug_index].start_pc = 0;
        }
    }

    if (body->kind != STMT_BLOCK) {
        compile_error(enclosing, line, "the body of a method must be a block");
    }
    compile_statement(&fn, body);
    emit_byte(&fn, type == FUNC_INITIALIZER ? OP_SELF : OP_NIL, body->line);
    emit_byte(&fn, OP_RETURN, body->line);

    int constant = chunk_add_constant(&enclosing->function->chunk, OBJ_VAL(fn.function));
    if (constant > 0xffff) {
        compile_error(enclosing, line, "too many constants in one function");
    }
    emit_byte(enclosing, OP_CLOSURE, line);
    emit_u16(enclosing, (uint16_t)constant, line);
    for (int i = 0; i < fn.upvalue_count; i++) {
        emit_byte(enclosing, fn.upvalues[i].is_local ? 1 : 0, line);
        emit_byte(enclosing, fn.upvalues[i].index, line);
    }
}

/* Compiles the closure that initializes the fields this class declares. It
 * runs on a fresh instance with `self` bound, before `init`. */
static void compile_field_initializer(Compiler *enclosing, ClassField *fields, int line)
{
    Compiler fn;

    init_compiler(&fn, enclosing, FUNC_METHOD, enclosing->ctx);
    fn.function->name = object_string("<fields>", 8);
    fn.function->arity = 0;
    fn.function->min_arity = 0;
    fn.function->is_varargs = false;
    int self_slot = add_local(&fn, (Text){"self", 4}, line);

    fn.locals[self_slot].is_const = true;
    fn.has_self = true;
    emit_byte(&fn, OP_SELF, line);
    fn.locals[self_slot].depth = fn.scope_depth;
    if (fn.locals[self_slot].debug_index >= 0) {
        fn.function->debug_locals[fn.locals[self_slot].debug_index].start_pc = 0;
    }

    for (ClassField *field = fields; field; field = field->next) {
        if (field->is_static) {
            continue;
        }
        emit_byte(&fn, OP_SELF, line);
        int name_index = add_constant(&fn, OBJ_VAL(name_constant(field->name)), line);
        emit_byte(&fn, OP_CONSTANT, line);
        emit_u16(&fn, (uint16_t)name_index, line);
        if (field->initializer) {
            compile_expression(&fn, field->initializer);
        } else {
            emit_byte(&fn, OP_NIL, line);
        }
        emit_byte(&fn, OP_INDEX_SET, line);
    }
    emit_byte(&fn, OP_NIL, line);
    emit_byte(&fn, OP_RETURN, line);

    int constant = chunk_add_constant(&enclosing->function->chunk, OBJ_VAL(fn.function));
    if (constant > 0xffff) {
        compile_error(enclosing, line, "too many constants in one function");
    }
    emit_byte(enclosing, OP_CLOSURE, line);
    emit_u16(enclosing, (uint16_t)constant, line);
    for (int i = 0; i < fn.upvalue_count; i++) {
        emit_byte(enclosing, fn.upvalues[i].is_local ? 1 : 0, line);
        emit_byte(enclosing, fn.upvalues[i].index, line);
    }
}


static int compound_opcode(TokenType op)
{
    switch (op) {
    case TOKEN_PLUS_EQUAL: return OP_ADD;
    case TOKEN_MINUS_EQUAL: return OP_SUBTRACT;
    case TOKEN_STAR_EQUAL: return OP_MULTIPLY;
    case TOKEN_SLASH_EQUAL: return OP_DIVIDE;
    case TOKEN_PERCENT_EQUAL: return OP_MODULO;
    case TOKEN_AMPERSAND_EQUAL: return OP_BIT_AND;
    case TOKEN_PIPE_EQUAL: return OP_BIT_OR;
    case TOKEN_CARET_EQUAL: return OP_BIT_XOR;
    case TOKEN_LESS_LESS_EQUAL: return OP_SHL;
    case TOKEN_GREATER_GREATER_EQUAL: return OP_SHR;
    default: return -1;
    }
}

static int fused_local_const_opcode(TokenType op)
{
    switch (op) {
    case TOKEN_PLUS: return OP_ADD_LOCAL_CONST;
    case TOKEN_MINUS: return OP_SUBTRACT_LOCAL_CONST;
    case TOKEN_STAR: return OP_MULTIPLY_LOCAL_CONST;
    case TOKEN_SLASH: return OP_DIVIDE_LOCAL_CONST;
    case TOKEN_LESS: return OP_LESS_LOCAL_CONST;
    case TOKEN_GREATER: return OP_GREATER_LOCAL_CONST;
    default: return -1;
    }
}

static TokenType compound_to_binary(TokenType op)
{
    switch (op) {
    case TOKEN_PLUS_EQUAL: return TOKEN_PLUS;
    case TOKEN_MINUS_EQUAL: return TOKEN_MINUS;
    case TOKEN_STAR_EQUAL: return TOKEN_STAR;
    case TOKEN_SLASH_EQUAL: return TOKEN_SLASH;
    case TOKEN_PERCENT_EQUAL: return TOKEN_PERCENT;
    default: return TOKEN_EOF;
    }
}

static void compile_discard_assignment(Compiler *compiler, Stmt *stmt, TokenType op,
                                       int line)
{
    Expr *target = stmt->as.assign.target;

    compiler->column = stmt->column;
    if (op == TOKEN_QUESTION_QUESTION_EQUAL) {
        if (target->kind == EXPR_VARIABLE) {
            VarRef ref = resolve_variable(compiler, target->as.variable, line);

            check_mutable(compiler, ref, target->as.variable, line);
            emit_get(compiler, ref, line);
            emit_byte(compiler, OP_DUP, line);
            emit_byte(compiler, OP_NIL, line);
            emit_byte(compiler, OP_EQUAL, line);
            int keep_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
            emit_byte(compiler, OP_POP, line);
            compile_expression(compiler, stmt->as.assign.value);
            compiler->column = stmt->column;
            emit_set(compiler, ref, line);
            int end_jump = emit_jump(compiler, OP_JUMP, line);
            patch_jump(compiler, keep_jump);
            emit_byte(compiler, OP_POP, line);
            patch_jump(compiler, end_jump);
        } else {
            compile_expression(compiler, target->as.index.object);
            compile_expression(compiler, target->as.index.index);
            emit_byte(compiler, OP_DUP2, line);
            emit_byte(compiler, OP_INDEX, line);
            emit_byte(compiler, OP_DUP, line);
            emit_byte(compiler, OP_NIL, line);
            emit_byte(compiler, OP_EQUAL, line);
            int keep_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
            emit_byte(compiler, OP_POP, line);
            compile_expression(compiler, stmt->as.assign.value);
            compiler->column = stmt->column;
            emit_byte(compiler, OP_INDEX_SET, line);
            int end_jump = emit_jump(compiler, OP_JUMP, line);
            patch_jump(compiler, keep_jump);
            emit_byte(compiler, OP_POP, line);
            emit_byte(compiler, OP_POP, line);
            patch_jump(compiler, end_jump);
        }
        return;
    }
    if (target->kind == EXPR_VARIABLE) {
        VarRef ref = resolve_variable(compiler, target->as.variable, line);

        check_mutable(compiler, ref, target->as.variable, line);
        if (op != TOKEN_EQUAL && ref.kind == VAR_LOCAL && stmt->as.assign.value->kind == EXPR_NUMBER) {
            TokenType bin_op = compound_to_binary(op);
            int fused = bin_op != TOKEN_EOF ? fused_local_const_opcode(bin_op) : -1;
            if (fused >= 0) {
                int index = add_constant(compiler, expr_number_value(stmt->as.assign.value), line);
                emit_byte(compiler, (uint8_t)fused, line);
                emit_byte(compiler, (uint8_t)ref.index, line);
                emit_u16(compiler, (uint16_t)index, line);
                compiler->column = stmt->column;
                emit_set(compiler, ref, line);
                return;
            }
        }
        if (op != TOKEN_EQUAL) {
            emit_get(compiler, ref, line);
        }
        compile_expression(compiler, stmt->as.assign.value);
        if (op != TOKEN_EQUAL) {
            compiler->column = stmt->column;
            emit_byte(compiler, (uint8_t)compound_opcode(op), line);
        }
        compiler->column = stmt->column;
        emit_set(compiler, ref, line);
        return;
    }

    compile_expression(compiler, target->as.index.object);
    compile_expression(compiler, target->as.index.index);
    compiler->column = stmt->column;
    if (op != TOKEN_EQUAL) {
        emit_byte(compiler, OP_DUP2, line);
        emit_byte(compiler, OP_INDEX, line);
    }
    compile_expression(compiler, stmt->as.assign.value);
    compiler->column = stmt->column;
    if (op != TOKEN_EQUAL) {
        emit_byte(compiler, (uint8_t)compound_opcode(op), line);
    }
    compiler->column = stmt->column;
    emit_byte(compiler, OP_INDEX_SET, line);
}

static void compile_cleanup_to(Compiler *compiler, int target_local_count, int line)
{
    for (int i = compiler->local_count - 1; i >= target_local_count; i--) {
        if (compiler->locals[i].captured) {
            emit_byte(compiler, OP_CLOSE_UPVALUE, line);
        } else {
            emit_byte(compiler, OP_POP, line);
        }
    }
}

/* Maps a comparison operator to the matching "jump when false" opcode. A zero
 * result means there is no fused form for it. */
static uint8_t comparison_jump_local_const(TokenType op)
{
    switch (op) {
    case TOKEN_LESS: return OP_JUMP_IF_NOT_LESS_LOCAL_CONST;
    case TOKEN_GREATER: return OP_JUMP_IF_NOT_GREATER_LOCAL_CONST;
    case TOKEN_LESS_EQUAL: return OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST;
    case TOKEN_GREATER_EQUAL: return OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST;
    case TOKEN_EQUAL_EQUAL: return OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST;
    case TOKEN_BANG_EQUAL: return OP_JUMP_IF_EQUAL_LOCAL_CONST;
    default: return 0;
    }
}

static uint8_t comparison_jump_local_local(TokenType op)
{
    switch (op) {
    case TOKEN_LESS: return OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL;
    case TOKEN_GREATER: return OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL;
    case TOKEN_LESS_EQUAL: return OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL;
    case TOKEN_GREATER_EQUAL: return OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL;
    case TOKEN_EQUAL_EQUAL: return OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL;
    case TOKEN_BANG_EQUAL: return OP_JUMP_IF_EQUAL_LOCAL_LOCAL;
    default: return 0;
    }
}

static bool is_equality_operator(TokenType op)
{
    return op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL;
}

/* Emits the condition of an if/while/for. If it is "local <op> local" or
 * "local <op> constant" it uses a comparison-and-jump superinstruction;
 * returns the offset to patch. */
static int emit_condition_jump(Compiler *compiler, Expr *condition, int line)
{
    if (condition->kind == EXPR_BINARY && condition->as.binary.left->kind == EXPR_VARIABLE) {
        Expr *right = condition->as.binary.right;
        TokenType op = condition->as.binary.op;
        int slot = resolve_local(compiler, condition->as.binary.left->as.variable, line);

        if (slot >= 0 && slot <= 0xff) {
            if (right->kind == EXPR_VARIABLE) {
                uint8_t branch = comparison_jump_local_local(op);
                int right_slot = resolve_local(compiler, right->as.variable, line);

                if (branch && right_slot >= 0 && right_slot <= 0xff) {
                    emit_byte(compiler, branch, line);
                    emit_byte(compiler, (uint8_t)slot, line);
                    emit_byte(compiler, (uint8_t)right_slot, line);
                    emit_byte(compiler, 0xff, line);
                    emit_byte(compiler, 0xff, line);
                    return compiler->function->chunk.count - 2;
                }
            } else {
                uint8_t branch = comparison_jump_local_const(op);
                Value constant;

                if (branch && eval_constant_expr(compiler, right, &constant) &&
                    (value_is_numeric(constant) || is_equality_operator(op))) {
                    int index = add_constant(compiler, constant, line);

                    emit_byte(compiler, branch, line);
                    emit_byte(compiler, (uint8_t)slot, line);
                    emit_u16(compiler, (uint16_t)index, line);
                    emit_byte(compiler, 0xff, line);
                    emit_byte(compiler, 0xff, line);
                    return compiler->function->chunk.count - 2;
                }
            }
        }
    }
    compile_expression(compiler, condition);
    return emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
}

static void compile_if(Compiler *compiler, Stmt *stmt, int line)
{
    Value cond_val;
    if (eval_constant_expr(compiler, stmt->as.if_stmt.condition, &cond_val)) {
        if (!value_is_falsey(cond_val)) {
            compile_statement(compiler, stmt->as.if_stmt.then_branch);
        } else if (stmt->as.if_stmt.else_branch) {
            compile_statement(compiler, stmt->as.if_stmt.else_branch);
        }
        return;
    }

    int then_jump = emit_condition_jump(compiler, stmt->as.if_stmt.condition, line);

    compile_statement(compiler, stmt->as.if_stmt.then_branch);

    int else_jump = emit_jump(compiler, OP_JUMP, line);
    patch_jump(compiler, then_jump);

    if (stmt->as.if_stmt.else_branch) {
        compile_statement(compiler, stmt->as.if_stmt.else_branch);
    }
    patch_jump(compiler, else_jump);
}

static void compile_for_in(Compiler *compiler, Stmt *stmt, int line)
{
    static const Text source_name = {" iterable_ref", 13};
    static const Text iterable_name = {" iterable", 9};
    static const Text count_name = {" count", 6};
    static const Text index_name = {" index", 6};
    LoopCtx loop;
    int source_slot;
    int iterable_slot;
    int count_slot;
    int index_slot;
    int exit_jump;

    begin_scope(compiler);

    compile_expression(compiler, stmt->as.for_in.iterable);
    source_slot = add_local(compiler, source_name, line);
    mark_local_initialized(compiler);
    emit_byte(compiler, OP_DUP, line);
    emit_byte(compiler, OP_ITER_KEYS, line);
    iterable_slot = add_local(compiler, iterable_name, line);
    mark_local_initialized(compiler);

    emit_byte(compiler, OP_LEN, line);
    count_slot = add_local(compiler, count_name, line);
    mark_local_initialized(compiler);

    emit_constant(compiler, INT_VAL(0), line);
    index_slot = add_local(compiler, index_name, line);
    mark_local_initialized(compiler);

    int loop_start = compiler->function->chunk.count;

    emit_get_slot(compiler, index_slot, line);
    emit_get_slot(compiler, count_slot, line);
    emit_byte(compiler, OP_LESS, line);
    exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);

    begin_scope(compiler);
    if (stmt->as.for_in.has_value_name) {
        emit_get_slot(compiler, source_slot, line);
        emit_get_slot(compiler, iterable_slot, line);
        emit_get_slot(compiler, index_slot, line);
        emit_byte(compiler, OP_ITER_PAIR, line);
        add_local(compiler, stmt->as.for_in.name, line);
        mark_local_initialized(compiler);
        add_local(compiler, stmt->as.for_in.value_name, line);
        mark_local_initialized(compiler);
    } else {
        emit_get_slot(compiler, iterable_slot, line);
        emit_get_slot(compiler, index_slot, line);
        emit_byte(compiler, OP_INDEX, line);
        add_local(compiler, stmt->as.for_in.name, line);
        mark_local_initialized(compiler);
    }

    memset(&loop, 0, sizeof(loop));
    loop.start = loop_start;
    loop.continue_target = -1;
    loop.local_count = compiler->local_count - (stmt->as.for_in.has_value_name ? 2 : 1);
    loop.label = stmt->as.for_in.label;

    LoopCtx *previous_loop = compiler->loop;
    loop.previous = previous_loop;
    loop.try_depth = compiler->try_depth;
    compiler->loop = &loop;
    compile_statement(compiler, stmt->as.for_in.body);
    compiler->loop = previous_loop;

    end_scope(compiler, line);

    loop.continue_target = compiler->function->chunk.count;
    for (int i = 0; i < loop.continue_count; i++) {
        patch_offset(compiler, loop.continues[i]);
    }

    emit_get_slot(compiler, index_slot, line);
    emit_constant(compiler, INT_VAL(1), line);
    emit_byte(compiler, OP_ADD, line);
    emit_set_slot(compiler, index_slot, line);
    emit_loop(compiler, loop_start, line);

    patch_jump(compiler, exit_jump);

    for (int i = 0; i < loop.break_count; i++) {
        patch_offset(compiler, loop.breaks[i]);
    }
    free(loop.breaks);
    free(loop.continues);

    end_scope(compiler, line);
}

static void compile_while(Compiler *compiler, Stmt *stmt, int line)
{
    Value cond_val;
    if (eval_constant_expr(compiler, stmt->as.while_stmt.condition, &cond_val) &&
        value_is_falsey(cond_val)) {
        return;
    }

    int loop_start = compiler->function->chunk.count;
    LoopCtx loop;

    memset(&loop, 0, sizeof(loop));
    loop.start = loop_start;
    loop.continue_target = loop_start;
    loop.local_count = compiler->local_count;
    loop.label = stmt->as.while_stmt.label;

    int exit_jump = emit_condition_jump(compiler, stmt->as.while_stmt.condition, line);

    LoopCtx *previous_loop = compiler->loop;
    loop.previous = previous_loop;
    loop.try_depth = compiler->try_depth;
    compiler->loop = &loop;
    compile_statement(compiler, stmt->as.while_stmt.body);
    compiler->loop = previous_loop;

    emit_loop(compiler, loop_start, line);
    patch_jump(compiler, exit_jump);

    for (int i = 0; i < loop.break_count; i++) {
        patch_offset(compiler, loop.breaks[i]);
    }

    free(loop.breaks);
    free(loop.continues);
}

static void compile_do_while(Compiler *compiler, Stmt *stmt, int line)
{
    int loop_start = compiler->function->chunk.count;
    LoopCtx loop;

    memset(&loop, 0, sizeof(loop));
    loop.start = loop_start;
    loop.continue_target = -1;
    loop.local_count = compiler->local_count;
    loop.label = stmt->as.do_while.label;

    LoopCtx *previous_loop = compiler->loop;
    loop.previous = previous_loop;
    loop.try_depth = compiler->try_depth;
    compiler->loop = &loop;
    compile_statement(compiler, stmt->as.do_while.body);
    compiler->loop = previous_loop;

    loop.continue_target = compiler->function->chunk.count;
    for (int i = 0; i < loop.continue_count; i++) {
        patch_offset(compiler, loop.continues[i]);
    }

    int exit_jump = emit_condition_jump(compiler, stmt->as.do_while.condition, line);

    emit_loop(compiler, loop_start, line);
    patch_jump(compiler, exit_jump);

    for (int i = 0; i < loop.break_count; i++) {
        patch_offset(compiler, loop.breaks[i]);
    }

    free(loop.breaks);
    free(loop.continues);
}

static void compile_for(Compiler *compiler, Stmt *stmt, int line)
{
    LoopCtx loop;
    int exit_jump = -1;
    bool has_condition = stmt->as.for_stmt.condition != NULL;

    begin_scope(compiler);
    if (stmt->as.for_stmt.initializer) {
        compile_statement(compiler, stmt->as.for_stmt.initializer);
    }

    int loop_start = compiler->function->chunk.count;
    if (has_condition) {
        exit_jump = emit_condition_jump(compiler, stmt->as.for_stmt.condition, line);
    }

    memset(&loop, 0, sizeof(loop));
    loop.start = loop_start;
    loop.continue_target = -1;
    loop.local_count = compiler->local_count;
    loop.label = stmt->as.for_stmt.label;

    LoopCtx *previous_loop = compiler->loop;
    loop.previous = previous_loop;
    loop.try_depth = compiler->try_depth;
    compiler->loop = &loop;
    compile_statement(compiler, stmt->as.for_stmt.body);
    compiler->loop = previous_loop;

    loop.continue_target = compiler->function->chunk.count;
    for (int i = 0; i < loop.continue_count; i++) {
        patch_offset(compiler, loop.continues[i]);
    }

    if (stmt->as.for_stmt.step) {
        compile_statement(compiler, stmt->as.for_stmt.step);
    }
    emit_loop(compiler, loop_start, line);

    if (has_condition) {
        patch_jump(compiler, exit_jump);
    }

    for (int i = 0; i < loop.break_count; i++) {
        patch_offset(compiler, loop.breaks[i]);
    }

    end_scope(compiler, line);
    free(loop.breaks);
    free(loop.continues);
}

static void add_break_jump(LoopCtx *loop, int offset)
{
    if (loop->break_count + 1 > loop->break_capacity) {
        loop->break_capacity = loop->break_capacity < 4 ? 4 : loop->break_capacity * 2;
        loop->breaks = realloc(loop->breaks, sizeof(int) * (size_t)loop->break_capacity);
    }
    loop->breaks[loop->break_count++] = offset;
}

static void compile_switch(Compiler *compiler, Stmt *stmt, int line)
{
    static const Text subject_name = {" switch", 7};
    LoopCtx ctx;
    SwitchCase *default_case = NULL;
    int subject_slot;

    begin_scope(compiler);
    compile_expression(compiler, stmt->as.switch_stmt.subject);
    subject_slot = add_local(compiler, subject_name, line);
    mark_local_initialized(compiler);

    memset(&ctx, 0, sizeof(ctx));
    ctx.is_switch = true;
    ctx.local_count = compiler->local_count;
    ctx.previous = compiler->loop;
    ctx.try_depth = compiler->try_depth;
    compiler->loop = &ctx;

    for (SwitchCase *case_node = stmt->as.switch_stmt.cases; case_node;
         case_node = case_node->next) {
        if (!case_node->value) {
            default_case = case_node;
            continue;
        }

        emit_byte(compiler, OP_GET_LOCAL, line);
        emit_byte(compiler, (uint8_t)subject_slot, line);
        compile_expression(compiler, case_node->value);
        emit_byte(compiler, OP_EQUAL, line);
        int next_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);

        begin_scope(compiler);
        for (StmtList *list = case_node->body; list; list = list->next) {
            compile_statement(compiler, list->stmt);
            if (stmt_is_terminator(list->stmt)) {
                break;
            }
        }
        end_scope(compiler, line);

        add_break_jump(&ctx, emit_jump(compiler, OP_JUMP, line));
        patch_jump(compiler, next_jump);
    }

    if (default_case) {
        begin_scope(compiler);
        for (StmtList *list = default_case->body; list; list = list->next) {
            compile_statement(compiler, list->stmt);
            if (stmt_is_terminator(list->stmt)) {
                break;
            }
        }
        end_scope(compiler, line);
    }

    compiler->loop = ctx.previous;
    for (int i = 0; i < ctx.break_count; i++) {
        patch_offset(compiler, ctx.breaks[i]);
    }
    free(ctx.breaks);
    end_scope(compiler, line);
}

static LoopCtx *find_loop(Compiler *compiler, Text label, int line, bool is_continue)
{
    if (label.length == 0) {
        LoopCtx *loop = compiler->loop;

        if (is_continue) {
            while (loop && loop->is_switch) {
                loop = loop->previous;
            }
        }
        if (!loop) {
            compile_error(compiler, line,
                          is_continue ? "'continue' can only be used inside a loop"
                                      : "'break' can only be used inside a loop or switch");
        }
        return loop;
    }
    for (LoopCtx *loop = compiler->loop; loop; loop = loop->previous) {
        if (!loop->is_switch && text_equals(loop->label, label)) {
            return loop;
        }
    }
    compile_error(compiler, line, "no enclosing loop is labelled '%.*s'", label.length,
                  label.chars);
    return NULL;
}

static void compile_break(Compiler *compiler, Text label, int line)
{
    LoopCtx *loop = find_loop(compiler, label, line, false);

    for (int i = 0; i < compiler->try_depth - loop->try_depth; i++) {
        emit_byte(compiler, OP_POP_TRY, line);
    }
    compile_cleanup_to(compiler, loop->local_count, line);
    add_break_jump(loop, emit_jump(compiler, OP_JUMP, line));
}

static void compile_continue(Compiler *compiler, Text label, int line)
{
    LoopCtx *loop = find_loop(compiler, label, line, true);

    for (int i = 0; i < compiler->try_depth - loop->try_depth; i++) {
        emit_byte(compiler, OP_POP_TRY, line);
    }
    compile_cleanup_to(compiler, loop->local_count, line);
    if (loop->continue_target >= 0) {
        emit_loop(compiler, loop->continue_target, line);
    } else {
        int offset = emit_jump(compiler, OP_JUMP, line);

        if (loop->continue_count + 1 > loop->continue_capacity) {
            loop->continue_capacity = loop->continue_capacity < 4 ? 4 : loop->continue_capacity * 2;
            loop->continues = realloc(loop->continues, sizeof(int) * (size_t)loop->continue_capacity);
        }
        loop->continues[loop->continue_count++] = offset;
    }
}

static void compile_try_catch(Compiler *compiler, Stmt *try_block, Text catch_var, Stmt *catch_block, int line)
{
    int catch_jump = emit_jump(compiler, OP_PUSH_CATCH, line);

    compiler->try_depth++;
    compile_statement(compiler, try_block);
    compiler->try_depth--;

    emit_byte(compiler, OP_POP_TRY, line);
    int exit_jump = emit_jump(compiler, OP_JUMP, line);

    patch_jump(compiler, catch_jump);
    begin_scope(compiler);
    if (catch_var.length > 0) {
        add_local(compiler, catch_var, line);
        mark_local_initialized(compiler);
    } else {
        emit_byte(compiler, OP_POP, line);
    }
    compile_statement(compiler, catch_block);
    end_scope(compiler, line);

    patch_jump(compiler, exit_jump);
}

static void compile_try_finally(Compiler *compiler, Stmt *try_block, Stmt *finally_block, int line)
{
    int finally_jump = emit_jump(compiler, OP_PUSH_FINALLY, line);

    compiler->try_depth++;
    compile_statement(compiler, try_block);
    compiler->try_depth--;

    emit_byte(compiler, OP_POP_TRY, line);

    /* Normal completion: push NIL (saved_value) and 0 (action NORMAL) */
    emit_byte(compiler, OP_NIL, line);
    int zero_const = add_constant(compiler, INT_VAL(0), line);
    emit_byte(compiler, OP_CONSTANT, line);
    emit_u16(compiler, (uint16_t)zero_const, line);

    int skip_jump = emit_jump(compiler, OP_JUMP, line);

    patch_jump(compiler, finally_jump);
    patch_jump(compiler, skip_jump);

    compile_statement(compiler, finally_block);
    emit_byte(compiler, OP_END_FINALLY, line);
}

static void compile_try(Compiler *compiler, Stmt *stmt, int line)
{
    if (stmt->as.try_stmt.has_catch && stmt->as.try_stmt.finally_block) {
        Stmt inner_try;
        memset(&inner_try, 0, sizeof(inner_try));
        inner_try.kind = STMT_TRY;
        inner_try.line = stmt->line;
        inner_try.column = stmt->column;
        inner_try.as.try_stmt.try_block = stmt->as.try_stmt.try_block;
        inner_try.as.try_stmt.has_catch = true;
        inner_try.as.try_stmt.catch_var = stmt->as.try_stmt.catch_var;
        inner_try.as.try_stmt.catch_block = stmt->as.try_stmt.catch_block;
        inner_try.as.try_stmt.finally_block = NULL;

        compile_try_finally(compiler, &inner_try, stmt->as.try_stmt.finally_block, line);
    } else if (stmt->as.try_stmt.has_catch) {
        compile_try_catch(compiler, stmt->as.try_stmt.try_block, stmt->as.try_stmt.catch_var,
                          stmt->as.try_stmt.catch_block, line);
    } else if (stmt->as.try_stmt.finally_block) {
        compile_try_finally(compiler, stmt->as.try_stmt.try_block, stmt->as.try_stmt.finally_block, line);
    }
}

static bool is_reserved_type_name(Text name)
{
    static const char *const reserved[] = {
        "number", "string", "bool", "nil", "array", "map",
        "function", "native function", "file", "string_builder", "class", "instance",
        "coroutine", "channel", "socket", "server",
    };

    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        int length = (int)strlen(reserved[i]);

        if (name.length == length && memcmp(name.chars, reserved[i], (size_t)length) == 0) {
            return true;
        }
    }
    return false;
}

static void compile_class(Compiler *compiler, Stmt *stmt, int line)
{
    Text name = stmt->as.class_stmt.name;
    bool is_global = compiler->scope_depth == 0;
    ObjString *class_name = name_constant(name);

    if (is_global && global_is_const(compiler, class_name)) {
        compile_error(compiler, line, "cannot redefine constant '%.*s'", name.length, name.chars);
    }
    if (is_reserved_type_name(name)) {
        compile_error(compiler, line,
                      "class name '%.*s' collides with a built-in type name",
                      name.length, name.chars);
    }

    /* Reject duplicate members early: they would silently shadow each other. */
    for (ClassMethod *m = stmt->as.class_stmt.methods; m; m = m->next) {
        for (ClassMethod *other = m->next; other; other = other->next) {
            if (text_equals(m->name, other->name)) {
                compile_error(compiler, line, "duplicate method '%.*s' in class '%.*s'",
                              m->name.length, m->name.chars, name.length, name.chars);
            }
        }
        for (ClassField *f = stmt->as.class_stmt.fields; f; f = f->next) {
            if (text_equals(m->name, f->name)) {
                compile_error(compiler, line, "member '%.*s' is declared both as a field and a method",
                              m->name.length, m->name.chars);
            }
        }
    }
    for (ClassField *f = stmt->as.class_stmt.fields; f; f = f->next) {
        for (ClassField *other = f->next; other; other = other->next) {
            if (text_equals(f->name, other->name)) {
                compile_error(compiler, line, "duplicate field '%.*s' in class '%.*s'",
                              f->name.length, f->name.chars, name.length, name.chars);
            }
        }
    }

    if (stmt->as.class_stmt.has_superclass) {
        VarRef ref = resolve_variable(compiler, stmt->as.class_stmt.superclass, line);

        emit_get(compiler, ref, line);
    } else {
        emit_byte(compiler, OP_NIL, line);
    }
    int class_index = add_constant(compiler, OBJ_VAL(class_name), line);
    emit_byte(compiler, OP_CLASS, line);
    emit_u16(compiler, (uint16_t)class_index, line);

    bool has_instance_fields = false;
    for (ClassField *field = stmt->as.class_stmt.fields; field; field = field->next) {
        if (!field->is_static) {
            has_instance_fields = true;
            break;
        }
    }
    if (has_instance_fields) {
        compile_field_initializer(compiler, stmt->as.class_stmt.fields, line);
        emit_byte(compiler, OP_CLASS_FIELD_INIT, line);
    }

    for (ClassMethod *m = stmt->as.class_stmt.methods; m; m = m->next) {
        compile_method_value(compiler, m->name, m->params, m->body, m->is_static, line);
        int name_index = add_constant(compiler, OBJ_VAL(name_constant(m->name)), line);
        emit_byte(compiler, OP_METHOD, line);
        emit_u16(compiler, (uint16_t)name_index, line);
        emit_byte(compiler, m->is_static ? 1 : 0, line);
    }

    for (ClassField *field = stmt->as.class_stmt.fields; field; field = field->next) {
        if (!field->is_static) {
            continue;
        }
        emit_byte(compiler, OP_DUP, line);
        int name_index = add_constant(compiler, OBJ_VAL(name_constant(field->name)), line);
        emit_byte(compiler, OP_CONSTANT, line);
        emit_u16(compiler, (uint16_t)name_index, line);
        if (field->initializer) {
            compile_expression(compiler, field->initializer);
        } else {
            emit_byte(compiler, OP_NIL, line);
        }
        emit_byte(compiler, OP_INDEX_SET, line);
    }

    if (is_global) {
        emit_byte(compiler, OP_DEFINE_GLOBAL, line);
        emit_global_name(compiler, class_name, line);
    } else {
        add_local(compiler, name, line);
        mark_local_initialized(compiler);
    }
}

static bool stmt_is_terminator(const Stmt *stmt)
{
    if (!stmt) return false;
    return stmt->kind == STMT_RETURN || stmt->kind == STMT_THROW ||
           stmt->kind == STMT_BREAK || stmt->kind == STMT_CONTINUE;
}

static void compile_statement(Compiler *compiler, Stmt *stmt)
{
    int line = stmt->line;

    compiler->column = stmt->column;
    switch (stmt->kind) {
    case STMT_EXPRESSION:
        compile_expression(compiler, stmt->as.expression);
        emit_byte(compiler, compiler->ctx->repl_mode ? OP_PRINT_REPL : OP_POP, line);
        break;

    case STMT_VAR: {
        bool is_global = compiler->scope_depth == 0;

        if (is_global) {
            ObjString *name = name_constant(stmt->as.var.name);

            if (global_is_const(compiler, name)) {
                compile_error(compiler, line, "cannot redefine constant '%.*s'",
                              stmt->as.var.name.length, stmt->as.var.name.chars);
            }
            if (stmt->as.var.initializer) {
                compile_expression(compiler, stmt->as.var.initializer);
            } else {
                emit_byte(compiler, OP_NIL, line);
            }
            compiler->column = stmt->column;
            if (stmt->as.var.is_const) {
                mark_global_const(compiler, name, line);
                emit_byte(compiler, OP_DEFINE_GLOBAL_CONST, line);
            } else {
                emit_byte(compiler, OP_DEFINE_GLOBAL, line);
            }
            emit_global_name(compiler, name, line);
        } else {
            int slot = add_local(compiler, stmt->as.var.name, line);

            if (stmt->as.var.is_const) {
                compiler->locals[slot].is_const = true;
            }
            if (stmt->as.var.initializer) {
                compile_expression(compiler, stmt->as.var.initializer);
            } else {
                emit_byte(compiler, OP_NIL, line);
            }
            mark_local_initialized(compiler);
        }
        break;
    }

    case STMT_ASSIGN:
        compile_discard_assignment(compiler, stmt, stmt->as.assign.op, line);
        break;

    case STMT_BLOCK:
        if (!stmt->as.block.transparent) {
            begin_scope(compiler);
        }
        for (StmtList *list = stmt->as.block.statements; list; list = list->next) {
            compile_statement(compiler, list->stmt);
            if (stmt_is_terminator(list->stmt)) {
                break;
            }
        }
        if (!stmt->as.block.transparent) {
            end_scope(compiler, line);
        }
        break;

    case STMT_IF:
        compile_if(compiler, stmt, line);
        break;

    case STMT_WHILE:
        compile_while(compiler, stmt, line);
        break;

    case STMT_DO_WHILE:
        compile_do_while(compiler, stmt, line);
        break;

    case STMT_SWITCH:
        compile_switch(compiler, stmt, line);
        break;

    case STMT_FOR:
        compile_for(compiler, stmt, line);
        break;

    case STMT_FOR_IN:
        compile_for_in(compiler, stmt, line);
        break;

    case STMT_FUNCTION: {
        bool is_global = compiler->scope_depth == 0;

        if (is_global) {
            ObjString *name = name_constant(stmt->as.function.name);

            if (global_is_const(compiler, name)) {
                compile_error(compiler, line, "cannot redefine constant '%.*s'",
                              stmt->as.function.name.length, stmt->as.function.name.chars);
            }
            compile_function_value(compiler, stmt->as.function.name, stmt->as.function.params,
                                   stmt->as.function.body, line);
            emit_byte(compiler, OP_DEFINE_GLOBAL, line);
            emit_global_name(compiler, name, line);
        } else {
            add_local(compiler, stmt->as.function.name, line);
            compile_function_value(compiler, stmt->as.function.name, stmt->as.function.params,
                                   stmt->as.function.body, line);
            mark_local_initialized(compiler);
        }
        break;
    }

    case STMT_RETURN:
        if (compiler->type == FUNC_SCRIPT) {
            compile_error(compiler, line, "'return' can only be used inside a function");
        }
        compile_tail_return(compiler, stmt->as.ret.value, line, stmt->column);
        break;

    case STMT_YIELD:
        emit_byte(compiler, OP_YIELD, line);
        break;

    case STMT_BREAK:
        compile_break(compiler, stmt->as.jump.label, line);
        break;

    case STMT_CONTINUE:
        compile_continue(compiler, stmt->as.jump.label, line);
        break;

    case STMT_IMPORT: {
        ObjString *path = name_constant(stmt->as.import_stmt.path);
        int index = add_constant(compiler, OBJ_VAL(path), line);
        emit_byte(compiler, OP_IMPORT, line);
        emit_u16(compiler, (uint16_t)index, line);
        break;
    }

    case STMT_THROW:
        compile_expression(compiler, stmt->as.throw_stmt.value);
        emit_byte(compiler, OP_THROW, line);
        break;

    case STMT_TRY:
        compile_try(compiler, stmt, line);
        break;

    case STMT_CLASS:
        compile_class(compiler, stmt, line);
        break;
    }
}

static bool expr_list_has_spread(ExprList *items)
{
    for (ExprList *item = items; item; item = item->next) {
        if (item->is_spread) {
            return true;
        }
    }
    return false;
}

static void compile_spread_arguments(Compiler *compiler, ExprList *arguments, int count, int line)
{
    if (count == 1 && arguments->is_spread) {
        compile_expression(compiler, arguments->expr);
    } else {
        emit_byte(compiler, OP_ARRAY, line);
        emit_u16(compiler, 0, line);
        for (ExprList *arg = arguments; arg; arg = arg->next) {
            compile_expression(compiler, arg->expr);
            emit_byte(compiler, arg->is_spread ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH, line);
        }
    }
}

static void compile_call_with_arguments(Compiler *compiler, Expr *expr, int line)
{
    if (expr_list_has_spread(expr->as.call.arguments)) {
        compile_spread_arguments(compiler, expr->as.call.arguments, expr->as.call.argument_count,
                                 line);
        compiler->column = expr->column;
        emit_byte(compiler, expr->as.call.is_new ? OP_NEW_SPREAD : OP_CALL_SPREAD, line);
    } else {
        for (ExprList *arg = expr->as.call.arguments; arg; arg = arg->next) {
            compile_expression(compiler, arg->expr);
        }
        compiler->column = expr->column;
        emit_byte(compiler, expr->as.call.is_new ? OP_NEW : OP_CALL, line);
        emit_byte(compiler, (uint8_t)expr->as.call.argument_count, line);
    }
}

static void compile_tail_return(Compiler *compiler, Expr *expr, int line, int column)
{
    if (compiler->type == FUNC_INITIALIZER) {
        if (expr) {
            compile_error(compiler, line, "cannot return a value from a constructor");
        }
        int slot = resolve_local(compiler, (Text){"self", 4}, line);
        emit_byte(compiler, OP_GET_LOCAL, line);
        emit_byte(compiler, (uint8_t)slot, line);
        compiler->column = column;
        emit_byte(compiler, OP_RETURN, line);
        return;
    }
    if (!expr) {
        emit_byte(compiler, OP_NIL, line);
        compiler->column = column;
        emit_byte(compiler, OP_RETURN, line);
        return;
    }
    if (expr->kind == EXPR_CALL && !expr->as.call.is_new &&
        expr->as.call.callee->kind != EXPR_SUPER) {
        compile_expression(compiler, expr->as.call.callee);
        if (expr_list_has_spread(expr->as.call.arguments)) {
            compile_spread_arguments(compiler, expr->as.call.arguments, expr->as.call.argument_count, line);
            compiler->column = expr->column;
            emit_byte(compiler, OP_TAIL_CALL_SPREAD, line);
            return;
        }
        for (ExprList *arg = expr->as.call.arguments; arg; arg = arg->next) {
            compile_expression(compiler, arg->expr);
        }
        compiler->column = expr->column;
        emit_byte(compiler, OP_TAIL_CALL, line);
        emit_byte(compiler, (uint8_t)expr->as.call.argument_count, line);
        return;
    }
    if (expr->kind == EXPR_TERNARY) {
        compile_expression(compiler, expr->as.ternary.condition);
        int then_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
        compile_tail_return(compiler, expr->as.ternary.then_branch, line, column);
        patch_jump(compiler, then_jump);
        compile_tail_return(compiler, expr->as.ternary.else_branch, line, column);
        return;
    }
    if (expr->kind == EXPR_BINARY && expr->as.binary.op == TOKEN_PLUS) {
        compile_expression(compiler, expr->as.binary.left);
        compile_expression(compiler, expr->as.binary.right);
        compiler->column = column;
        emit_byte(compiler, OP_RETURN_ADD, line);
        return;
    }
    if (expr->kind == EXPR_VARIABLE) {
        int slot = resolve_local(compiler, expr->as.variable, line);
        if (slot >= 0 && slot <= 0xff) {
            emit_byte(compiler, OP_RETURN_LOCAL, line);
            emit_byte(compiler, (uint8_t)slot, line);
            return;
        }
    }
    compile_expression(compiler, expr);
    compiler->column = column;
    emit_byte(compiler, OP_RETURN, line);
}







static void compile_binary_operator(Compiler *compiler, TokenType op, int line)
{
    switch (op) {
    case TOKEN_PLUS: emit_byte(compiler, OP_ADD, line); break;
    case TOKEN_MINUS: emit_byte(compiler, OP_SUBTRACT, line); break;
    case TOKEN_STAR: emit_byte(compiler, OP_MULTIPLY, line); break;
    case TOKEN_STAR_STAR: emit_byte(compiler, OP_POW, line); break;
    case TOKEN_IN: emit_byte(compiler, OP_IN, line); break;
    case TOKEN_INSTANCEOF: emit_byte(compiler, OP_INSTANCEOF, line); break;
    case TOKEN_SLASH: emit_byte(compiler, OP_DIVIDE, line); break;
    case TOKEN_PERCENT: emit_byte(compiler, OP_MODULO, line); break;
    case TOKEN_DIV: emit_byte(compiler, OP_INT_DIVIDE, line); break;
    case TOKEN_AMPERSAND: emit_byte(compiler, OP_BIT_AND, line); break;
    case TOKEN_PIPE: emit_byte(compiler, OP_BIT_OR, line); break;
    case TOKEN_CARET: emit_byte(compiler, OP_BIT_XOR, line); break;
    case TOKEN_LESS_LESS: emit_byte(compiler, OP_SHL, line); break;
    case TOKEN_GREATER_GREATER: emit_byte(compiler, OP_SHR, line); break;
    case TOKEN_EQUAL_EQUAL: emit_byte(compiler, OP_EQUAL, line); break;
    case TOKEN_BANG_EQUAL:
        emit_byte(compiler, OP_EQUAL, line);
        emit_byte(compiler, OP_NOT, line);
        break;
    case TOKEN_GREATER: emit_byte(compiler, OP_GREATER, line); break;
    case TOKEN_GREATER_EQUAL: emit_byte(compiler, OP_GREATER_OR_EQUAL, line); break;
    case TOKEN_LESS: emit_byte(compiler, OP_LESS, line); break;
    case TOKEN_LESS_EQUAL: emit_byte(compiler, OP_LESS_OR_EQUAL, line); break;
    default:
        break;
    }
}

static void emit_constant_value(Compiler *compiler, Value val, int line)
{
    if (is_nil(val)) {
        emit_byte(compiler, OP_NIL, line);
    } else if (is_bool(val)) {
        emit_byte(compiler, AS_BOOL(val) ? OP_TRUE : OP_FALSE, line);
    } else {
        emit_constant(compiler, val, line);
    }
}

static bool eval_constant_expr(Compiler *compiler, Expr *expr, Value *out_val)
{
    if (!expr) return false;

    switch (expr->kind) {
    case EXPR_NUMBER:
        *out_val = expr_number_value(expr);
        return true;

    case EXPR_STRING:
        *out_val = OBJ_VAL(object_string(expr->as.string.chars, expr->as.string.length));
        return true;

    case EXPR_BOOL:
        *out_val = BOOL_VAL(expr->as.boolean);
        return true;

    case EXPR_NIL:
        *out_val = NIL_VAL;
        return true;

    case EXPR_UNARY: {
        Value operand;
        if (!eval_constant_expr(compiler, expr->as.unary.operand, &operand)) return false;
        switch (expr->as.unary.op) {
        case TOKEN_BANG:
            *out_val = BOOL_VAL(value_is_falsey(operand));
            return true;
        case TOKEN_MINUS:
            if (is_int(operand)) {
                int64_t val = AS_INT(operand);
                if (val == INT64_MIN) {
                    *out_val = NUMBER_VAL(-(double)INT64_MIN);
                } else {
                    *out_val = INT_VAL(-val);
                }
                return true;
            }
            if (is_number(operand)) {
                *out_val = NUMBER_VAL(-AS_NUMBER(operand));
                return true;
            }
            return false;
        case TOKEN_TILDE:
            if (is_int(operand)) {
                *out_val = INT_VAL(~AS_INT(operand));
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    case EXPR_BINARY: {
        Value left, right;
        if (!eval_constant_expr(compiler, expr->as.binary.left, &left)) return false;
        if (!eval_constant_expr(compiler, expr->as.binary.right, &right)) return false;

        if (is_obj_type(left, OBJ_STRING) && is_obj_type(right, OBJ_STRING)) {
            if (expr->as.binary.op == TOKEN_PLUS) {
                *out_val = OBJ_VAL(object_string_concat(AS_STRING(left), AS_STRING(right)));
                return true;
            }
            return false;
        }

        switch (expr->as.binary.op) {
        case TOKEN_PLUS:
            if (is_int(left) && is_int(right)) {
                int64_t res;
                if (!int_add_overflow(AS_INT(left), AS_INT(right), &res)) {
                    *out_val = INT_VAL(res);
                } else {
                    *out_val = NUMBER_VAL((double)AS_INT(left) + (double)AS_INT(right));
                }
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = NUMBER_VAL(value_as_double(left) + value_as_double(right));
                return true;
            }
            return false;

        case TOKEN_MINUS:
            if (is_int(left) && is_int(right)) {
                int64_t res;
                if (!int_sub_overflow(AS_INT(left), AS_INT(right), &res)) {
                    *out_val = INT_VAL(res);
                } else {
                    *out_val = NUMBER_VAL((double)AS_INT(left) - (double)AS_INT(right));
                }
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = NUMBER_VAL(value_as_double(left) - value_as_double(right));
                return true;
            }
            return false;

        case TOKEN_STAR:
            if (is_int(left) && is_int(right)) {
                int64_t res;
                if (!int_mul_overflow(AS_INT(left), AS_INT(right), &res)) {
                    *out_val = INT_VAL(res);
                } else {
                    *out_val = NUMBER_VAL((double)AS_INT(left) * (double)AS_INT(right));
                }
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = NUMBER_VAL(value_as_double(left) * value_as_double(right));
                return true;
            }
            return false;

        case TOKEN_SLASH:
            if (value_is_numeric(left) && value_is_numeric(right)) {
                double r = value_as_double(right);
                if (r == 0.0) return false;
                *out_val = NUMBER_VAL(value_as_double(left) / r);
                return true;
            }
            return false;

        case TOKEN_DIV:
            if (is_int(left) && is_int(right)) {
                int64_t b = AS_INT(right);
                if (b == 0) return false;
                int64_t a = AS_INT(left);
                if (a == INT64_MIN && b == -1) return false;
                *out_val = INT_VAL(a / b);
                return true;
            }
            return false;

        case TOKEN_PERCENT:
            if (is_int(left) && is_int(right)) {
                int64_t b = AS_INT(right);
                if (b == 0) return false;
                int64_t a = AS_INT(left);
                if (a == INT64_MIN && b == -1) return false;
                *out_val = INT_VAL(a % b);
                return true;
            }
            return false;

        case TOKEN_AMPERSAND:
            if (is_int(left) && is_int(right)) {
                *out_val = INT_VAL(AS_INT(left) & AS_INT(right));
                return true;
            }
            return false;

        case TOKEN_PIPE:
            if (is_int(left) && is_int(right)) {
                *out_val = INT_VAL(AS_INT(left) | AS_INT(right));
                return true;
            }
            return false;

        case TOKEN_CARET:
            if (is_int(left) && is_int(right)) {
                *out_val = INT_VAL(AS_INT(left) ^ AS_INT(right));
                return true;
            }
            return false;

        case TOKEN_LESS_LESS:
            if (is_int(left) && is_int(right)) {
                int64_t shift = AS_INT(right);
                if (shift < 0 || shift >= 64) return false;
                *out_val = INT_VAL((uint64_t)AS_INT(left) << shift);
                return true;
            }
            return false;

        case TOKEN_GREATER_GREATER:
            if (is_int(left) && is_int(right)) {
                int64_t shift = AS_INT(right);
                if (shift < 0 || shift >= 64) return false;
                *out_val = INT_VAL(AS_INT(left) >> shift);
                return true;
            }
            return false;

        case TOKEN_EQUAL_EQUAL:
            *out_val = BOOL_VAL(values_equal(left, right));
            return true;

        case TOKEN_BANG_EQUAL:
            *out_val = BOOL_VAL(!values_equal(left, right));
            return true;

        case TOKEN_LESS:
            if (is_int(left) && is_int(right)) {
                *out_val = BOOL_VAL(AS_INT(left) < AS_INT(right));
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = BOOL_VAL(value_as_double(left) < value_as_double(right));
                return true;
            }
            return false;

        case TOKEN_LESS_EQUAL:
            if (is_int(left) && is_int(right)) {
                *out_val = BOOL_VAL(AS_INT(left) <= AS_INT(right));
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = BOOL_VAL(value_as_double(left) <= value_as_double(right));
                return true;
            }
            return false;

        case TOKEN_GREATER:
            if (is_int(left) && is_int(right)) {
                *out_val = BOOL_VAL(AS_INT(left) > AS_INT(right));
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = BOOL_VAL(value_as_double(left) > value_as_double(right));
                return true;
            }
            return false;

        case TOKEN_GREATER_EQUAL:
            if (is_int(left) && is_int(right)) {
                *out_val = BOOL_VAL(AS_INT(left) >= AS_INT(right));
                return true;
            }
            if (value_is_numeric(left) && value_is_numeric(right)) {
                *out_val = BOOL_VAL(value_as_double(left) >= value_as_double(right));
                return true;
            }
            return false;

        default:
            return false;
        }
    }

    case EXPR_LOGICAL: {
        Value lval;
        if (!eval_constant_expr(compiler, expr->as.logical.left, &lval)) return false;
        if (expr->as.logical.op == TOKEN_AND) {
            if (value_is_falsey(lval)) {
                *out_val = lval;
                return true;
            }
            return eval_constant_expr(compiler, expr->as.logical.right, out_val);
        } else {
            if (!value_is_falsey(lval)) {
                *out_val = lval;
                return true;
            }
            return eval_constant_expr(compiler, expr->as.logical.right, out_val);
        }
    }

    case EXPR_TERNARY: {
        Value cval;
        if (!eval_constant_expr(compiler, expr->as.ternary.condition, &cval)) return false;
        if (!value_is_falsey(cval)) {
            return eval_constant_expr(compiler, expr->as.ternary.then_branch, out_val);
        } else {
            return eval_constant_expr(compiler, expr->as.ternary.else_branch, out_val);
        }
    }

    case EXPR_COALESCE: {
        Value lval;
        if (!eval_constant_expr(compiler, expr->as.coalesce.left, &lval)) return false;
        if (!is_nil(lval)) {
            *out_val = lval;
            return true;
        }
        return eval_constant_expr(compiler, expr->as.coalesce.right, out_val);
    }

    default:
        return false;
    }
}

static void compile_expression(Compiler *compiler, Expr *expr)
{
    int line = expr->line;

    compiler->column = expr->column;

    Value const_val;
    if (eval_constant_expr(compiler, expr, &const_val)) {
        emit_constant_value(compiler, const_val, line);
        return;
    }
    switch (expr->kind) {
    case EXPR_NUMBER:
        emit_constant(compiler, expr_number_value(expr), line);
        break;

    case EXPR_STRING:
        emit_constant(compiler,
                      OBJ_VAL(object_string(expr->as.string.chars, expr->as.string.length)), line);
        break;

    case EXPR_BOOL:
        emit_byte(compiler, expr->as.boolean ? OP_TRUE : OP_FALSE, line);
        break;

    case EXPR_NIL:
        emit_byte(compiler, OP_NIL, line);
        break;

    case EXPR_UNARY:
        compile_expression(compiler, expr->as.unary.operand);
        compiler->column = expr->column;
        if (expr->as.unary.op == TOKEN_MINUS) {
            emit_byte(compiler, OP_NEGATE, line);
        } else if (expr->as.unary.op == TOKEN_TILDE) {
            emit_byte(compiler, OP_BIT_NOT, line);
        } else {
            emit_byte(compiler, OP_NOT, line);
        }
        break;

    case EXPR_BINARY: {
        Expr *left = expr->as.binary.left;
        Expr *right = expr->as.binary.right;

        if (left->kind == EXPR_VARIABLE) {
            Value rval;
            if (eval_constant_expr(compiler, right, &rval) && value_is_numeric(rval)) {
                int fused = fused_local_const_opcode(expr->as.binary.op);

                if (fused >= 0) {
                    int slot = resolve_local(compiler, left->as.variable, line);

                    if (slot >= 0 && slot <= 0xff) {
                        int index = add_constant(compiler, rval, line);

                        emit_byte(compiler, (uint8_t)fused, line);
                        emit_byte(compiler, (uint8_t)slot, line);
                        emit_u16(compiler, (uint16_t)index, line);
                        break;
                    }
                }
            }
        }
        compile_expression(compiler, left);
        compile_expression(compiler, right);
        compiler->column = expr->column;
        compile_binary_operator(compiler, expr->as.binary.op, line);
        break;
    }

    case EXPR_LOGICAL:
        if (expr->as.logical.op == TOKEN_AND) {
            compile_expression(compiler, expr->as.logical.left);
            compiler->column = expr->column;
            int end_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, line);

            emit_byte(compiler, OP_POP, line);
            compile_expression(compiler, expr->as.logical.right);
            patch_jump(compiler, end_jump);
        } else {
            compile_expression(compiler, expr->as.logical.left);
            compiler->column = expr->column;
            int else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, line);
            int end_jump = emit_jump(compiler, OP_JUMP, line);

            patch_jump(compiler, else_jump);
            emit_byte(compiler, OP_POP, line);
            compile_expression(compiler, expr->as.logical.right);
            patch_jump(compiler, end_jump);
        }
        break;

    case EXPR_TERNARY: {
        compile_expression(compiler, expr->as.ternary.condition);
        compiler->column = expr->column;
        int else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);

        compile_expression(compiler, expr->as.ternary.then_branch);
        compiler->column = expr->column;
        int end_jump = emit_jump(compiler, OP_JUMP, line);
        patch_jump(compiler, else_jump);
        compile_expression(compiler, expr->as.ternary.else_branch);
        patch_jump(compiler, end_jump);
        break;
    }

    case EXPR_COALESCE: {
        compile_expression(compiler, expr->as.coalesce.left);
        compiler->column = expr->column;
        emit_byte(compiler, OP_DUP, line);
        emit_byte(compiler, OP_NIL, line);
        emit_byte(compiler, OP_EQUAL, line);
        int not_nil_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);

        emit_byte(compiler, OP_POP, line);
        compile_expression(compiler, expr->as.coalesce.right);
        int end_jump = emit_jump(compiler, OP_JUMP, line);
        patch_jump(compiler, not_nil_jump);
        patch_jump(compiler, end_jump);
        break;
    }

    case EXPR_VARIABLE: {
        VarRef ref = resolve_variable(compiler, expr->as.variable, line);

        emit_get(compiler, ref, line);
        break;
    }

    case EXPR_SUPER:
        compile_error(compiler, line,
                      "'super' can only be used as a call: super(...) or super.method(...)");
        break;

    case EXPR_CALL: {
        /* Superinstruction: self-recursive call with (local - constant) argument */
        if (!expr->as.call.is_new &&
            compiler->type == FUNC_FUNCTION &&
            expr->as.call.argument_count == 1 &&
            !expr->as.call.arguments->is_spread &&
            expr->as.call.callee->kind == EXPR_VARIABLE &&
            resolve_local(compiler, expr->as.call.callee->as.variable, line) == 0) {
            Expr *arg = expr->as.call.arguments->expr;
            if (arg->kind == EXPR_BINARY && arg->as.binary.op == TOKEN_MINUS &&
                arg->as.binary.left->kind == EXPR_VARIABLE &&
                arg->as.binary.right->kind == EXPR_NUMBER) {
                int slot = resolve_local(compiler, arg->as.binary.left->as.variable, line);
                if (slot >= 0) {
                    int index = add_constant(compiler, expr_number_value(arg->as.binary.right), line);
                    compiler->column = expr->column;
                    emit_byte(compiler, OP_CALL_SELF_SUB_LOCAL_CONST, line);
                    emit_byte(compiler, (uint8_t)slot, line);
                    emit_u16(compiler, (uint16_t)index, line);
                    break;
                }
            }
        }

        Expr *callee = expr->as.call.callee;
        if (callee->kind == EXPR_SUPER) {
            if (expr->as.call.is_new) {
                compile_error(compiler, line, "cannot construct an instance through 'super'");
            }
            if (!compiler->has_self) {
                compile_error(compiler, line,
                              "'super' can only be used inside an instance method");
            }
            if (expr_list_has_spread(expr->as.call.arguments)) {
                compile_error(compiler, line,
                              "the spread operator '...' is not supported with 'super'");
            }
            compiler->column = expr->column;
            emit_byte(compiler, OP_SELF, line);
            for (ExprList *arg = expr->as.call.arguments; arg; arg = arg->next) {
                compile_expression(compiler, arg->expr);
            }
            emit_byte(compiler, OP_SUPER, line);
            emit_byte(compiler, (uint8_t)expr->as.call.argument_count, line);
            break;
        }
        if (callee->kind == EXPR_OPTIONAL_CHAIN) {
            if (expr->as.call.is_new) {
                compile_error(compiler, line, "cannot construct through '?.'");
            }
            compiler->column = expr->column;
            if (!callee->as.opt_chain.is_index) {
                /* a?.b(...): short-circuit to nil when the receiver is nil. */
                ObjString *name = name_constant(callee->as.opt_chain.property);
                int name_index = add_constant(compiler, OBJ_VAL(name), line);

                compile_expression(compiler, callee->as.opt_chain.object);
                emit_byte(compiler, OP_DUP, line);
                emit_byte(compiler, OP_NIL, line);
                emit_byte(compiler, OP_EQUAL, line);
                int nil_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
                emit_byte(compiler, OP_POP, line);
                emit_byte(compiler, OP_NIL, line);
                int end_jump = emit_jump(compiler, OP_JUMP, line);

                patch_jump(compiler, nil_jump);
                compiler->column = expr->column;
                if (expr_list_has_spread(expr->as.call.arguments)) {
                    compile_spread_arguments(compiler, expr->as.call.arguments,
                                             expr->as.call.argument_count, line);
                    emit_byte(compiler, OP_INVOKE_SPREAD, line);
                    emit_u16(compiler, (uint16_t)name_index, line);
                } else {
                    for (ExprList *arg = expr->as.call.arguments; arg; arg = arg->next) {
                        compile_expression(compiler, arg->expr);
                    }
                    emit_byte(compiler, OP_INVOKE, line);
                    emit_u16(compiler, (uint16_t)name_index, line);
                    emit_byte(compiler, (uint8_t)expr->as.call.argument_count, line);
                }
                patch_jump(compiler, end_jump);
            } else {
                /* a?.[i](...): short-circuit to nil when the object is nil. */
                compile_expression(compiler, callee->as.opt_chain.object);
                emit_byte(compiler, OP_DUP, line);
                emit_byte(compiler, OP_NIL, line);
                emit_byte(compiler, OP_EQUAL, line);
                int nil_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
                emit_byte(compiler, OP_POP, line);
                emit_byte(compiler, OP_NIL, line);
                int end_jump = emit_jump(compiler, OP_JUMP, line);

                patch_jump(compiler, nil_jump);
                compile_expression(compiler, callee->as.opt_chain.index);
                emit_byte(compiler, OP_INDEX, line);
                compile_call_with_arguments(compiler, expr, line);
                patch_jump(compiler, end_jump);
            }
            break;
        }

        compile_expression(compiler, expr->as.call.callee);
        compile_call_with_arguments(compiler, expr, line);
        break;
    }

    case EXPR_INDEX:
        compile_expression(compiler, expr->as.index.object);
        compile_expression(compiler, expr->as.index.index);
        compiler->column = expr->column;
        emit_byte(compiler, OP_INDEX, line);
        break;

    case EXPR_ARRAY:
        if (expr_list_has_spread(expr->as.array.items)) {
            emit_byte(compiler, OP_ARRAY, line);
            emit_u16(compiler, 0, line);
            for (ExprList *item = expr->as.array.items; item; item = item->next) {
                compile_expression(compiler, item->expr);
                emit_byte(compiler, item->is_spread ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH, line);
            }
        } else {
            for (ExprList *item = expr->as.array.items; item; item = item->next) {
                compile_expression(compiler, item->expr);
            }
            compiler->column = expr->column;
            emit_byte(compiler, OP_ARRAY, line);
            emit_u16(compiler, (uint16_t)expr->as.array.count, line);
        }
        break;

    case EXPR_MAP:
        for (MapPair *pair = expr->as.map.pairs; pair; pair = pair->next) {
            compile_expression(compiler, pair->key);
            compile_expression(compiler, pair->value);
        }
        compiler->column = expr->column;
        emit_byte(compiler, OP_MAP, line);
        emit_u16(compiler, (uint16_t)expr->as.map.count, line);
        break;

    case EXPR_INTERPOLATION: {
        bool have_value = false;

        for (InterpPart *part = expr->as.interpolation.parts; part; part = part->next) {
            if (part->is_expression) {
                compile_expression(compiler, part->expr);
                emit_byte(compiler, OP_TO_STRING, line);
            } else {
                if (part->text.length == 0) {
                    continue;
                }
                emit_constant(compiler,
                              OBJ_VAL(object_string(part->text.chars, part->text.length)), line);
            }
            if (have_value) {
                emit_byte(compiler, OP_ADD, line);
            }
            have_value = true;
        }
        if (!have_value) {
            emit_constant(compiler, OBJ_VAL(object_string("", 0)), line);
        }
        break;
    }

    case EXPR_FUNCTION:
        compile_function_value(compiler, expr->as.function.name, expr->as.function.params,
                               expr->as.function.body, line);
        break;

    case EXPR_METHOD_CALL: {
        if (expr->as.method_call.receiver->kind == EXPR_SUPER) {
            if (compiler->type != FUNC_METHOD && compiler->type != FUNC_INITIALIZER) {
                compile_error(compiler, line,
                              "'super' can only be used inside a method");
            }
            if (expr_list_has_spread(expr->as.method_call.arguments)) {
                compile_error(compiler, line,
                              "the spread operator '...' is not supported with 'super'");
            }
            ObjString *super_name = name_constant(expr->as.method_call.method);
            int super_index = add_constant(compiler, OBJ_VAL(super_name), line);

            compiler->column = expr->column;
            emit_byte(compiler, compiler->has_self ? OP_SELF : OP_NIL, line);
            for (ExprList *arg = expr->as.method_call.arguments; arg; arg = arg->next) {
                compile_expression(compiler, arg->expr);
            }
            emit_byte(compiler, OP_SUPER_INVOKE, line);
            emit_u16(compiler, (uint16_t)super_index, line);
            emit_byte(compiler, (uint8_t)expr->as.method_call.argument_count, line);
            break;
        }
        compile_expression(compiler, expr->as.method_call.receiver);
        if (expr_list_has_spread(expr->as.method_call.arguments)) {
            compile_spread_arguments(compiler, expr->as.method_call.arguments,
                                     expr->as.method_call.argument_count, line);
            ObjString *name = name_constant(expr->as.method_call.method);
            int index = add_constant(compiler, OBJ_VAL(name), line);
            compiler->column = expr->column;
            emit_byte(compiler, OP_INVOKE_SPREAD, line);
            emit_u16(compiler, (uint16_t)index, line);
        } else {
            for (ExprList *arg = expr->as.method_call.arguments; arg; arg = arg->next) {
                compile_expression(compiler, arg->expr);
            }
            ObjString *name = name_constant(expr->as.method_call.method);
            int index = add_constant(compiler, OBJ_VAL(name), line);
            compiler->column = expr->column;
            emit_byte(compiler, OP_INVOKE, line);
            emit_u16(compiler, (uint16_t)index, line);
            emit_byte(compiler, (uint8_t)expr->as.method_call.argument_count, line);
        }
        break;
    }

    case EXPR_SLICE:
        compile_expression(compiler, expr->as.slice.object);
        if (expr->as.slice.start) {
            compile_expression(compiler, expr->as.slice.start);
        } else {
            emit_byte(compiler, OP_NIL, line);
        }
        if (expr->as.slice.end) {
            compile_expression(compiler, expr->as.slice.end);
        } else {
            emit_byte(compiler, OP_NIL, line);
        }
        if (expr->as.slice.step) {
            compile_expression(compiler, expr->as.slice.step);
        } else {
            emit_byte(compiler, OP_NIL, line);
        }
        emit_byte(compiler, OP_SLICE, line);
        break;

    case EXPR_UPDATE: {
        Expr *target = expr->as.update.target;
        bool is_inc = expr->as.update.op == TOKEN_PLUS_PLUS;
        bool is_prefix = expr->as.update.is_prefix;
        if (target->kind == EXPR_VARIABLE) {
            VarRef ref = resolve_variable(compiler, target->as.variable, line);
            emit_get(compiler, ref, line);
            if (!is_prefix) {
                emit_byte(compiler, OP_DUP, line);
            }
            emit_constant(compiler, INT_VAL(1), line);
            emit_byte(compiler, is_inc ? OP_ADD : OP_SUBTRACT, line);
            if (is_prefix) {
                /* SET pops the new value; keep a copy as the result. */
                emit_byte(compiler, OP_DUP, line);
            }
            emit_set(compiler, ref, line);
        } else if (target->kind == EXPR_INDEX) {
            compile_expression(compiler, target->as.index.object);
            compile_expression(compiler, target->as.index.index);
            compiler->column = expr->column;
            emit_byte(compiler, OP_INDEX_INC, line);
            emit_byte(compiler,
                      (uint8_t)((is_inc ? 0 : 1) | (is_prefix ? 2 : 0)), line);
        } else {
            compile_error(compiler, line, "invalid target for '++' or '--'");
        }
        break;
    }

    case EXPR_OPTIONAL_CHAIN: {
        compile_expression(compiler, expr->as.opt_chain.object);
        emit_byte(compiler, OP_DUP, line);
        emit_byte(compiler, OP_NIL, line);
        emit_byte(compiler, OP_EQUAL, line);
        int jump_nil = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
        int jump_end = emit_jump(compiler, OP_JUMP, line);

        patch_jump(compiler, jump_nil);
        if (expr->as.opt_chain.is_index) {
            compile_expression(compiler, expr->as.opt_chain.index);
            emit_byte(compiler, OP_INDEX, line);
        } else {
            ObjString *name = name_constant(expr->as.opt_chain.property);
            emit_constant(compiler, OBJ_VAL(name), line);
            emit_byte(compiler, OP_INDEX, line);
        }
        patch_jump(compiler, jump_end);
        break;
    }
    }
}

static void free_const_globals(CompileContext *ctx)
{
    ConstGlobal *entry = ctx->const_globals;

    while (entry) {
        ConstGlobal *next = entry->next;

        free(entry);
        entry = next;
    }
    ctx->const_globals = NULL;
}

ObjFunction *compile_ast(Stmt *program, const char *name, const char *source, bool repl_mode,
                         char *error, size_t error_size, int *error_line, int *error_column)
{
    CompileContext ctx;
    Compiler compiler;
    ObjFunction *function;

    vm_init();
    memset(&ctx, 0, sizeof(ctx));
    ctx.repl_mode = repl_mode;
    ctx.source = source;
    ctx.script_path = object_string(name, (int)strlen(name));

    mem_pause_gc();
    if (setjmp(ctx.jump) != 0) {
        mem_resume_gc();
        free_const_globals(&ctx);
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", ctx.message);
        }
        if (error_line) {
            *error_line = ctx.line;
        }
        if (error_column) {
            *error_column = ctx.column;
        }
        return NULL;
    }

    init_compiler(&compiler, NULL, FUNC_SCRIPT, &ctx);
    compiler.function->name = object_string(name, (int)strlen(name));

    for (StmtList *list = program->as.block.statements; list; list = list->next) {
        compile_statement(&compiler, list->stmt);
        if (stmt_is_terminator(list->stmt)) {
            break;
        }
    }
    compiler.column = program->column;
    emit_byte(&compiler, OP_NIL, program->line);
    emit_byte(&compiler, OP_RETURN, program->line);

    free_const_globals(&ctx);
    function = compiler.function;
    mem_resume_gc();
    return function;
}
