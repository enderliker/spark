#include "compiler.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "vm.h"

#define MAX_LOCALS 256
#define MAX_UPVALUES 256

typedef struct {
    Text name;
    int depth;
    bool captured;
    bool is_const;
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
    bool is_const;
} Upvalue;

typedef enum {
    FUNC_SCRIPT,
    FUNC_FUNCTION
} FunctionType;

typedef struct LoopCtx {
    int start;
    int continue_target; /* -1 while it is not known */
    int local_count;
    int *breaks;
    int break_count;
    int break_capacity;
    int *continues;
    int continue_count;
    int continue_capacity;
} LoopCtx;

typedef struct ConstGlobal {
    ObjString *name;
    struct ConstGlobal *next;
} ConstGlobal;

typedef struct {
    jmp_buf jump;
    char message[256];
    int line;
    bool repl_mode;
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
    LoopCtx *loop;
    CompileContext *ctx;
} Compiler;

static void compile_statement(Compiler *compiler, Stmt *stmt);
static void compile_expression(Compiler *compiler, Expr *expr);

static void compile_error(Compiler *compiler, int line, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(compiler->ctx->message, sizeof(compiler->ctx->message), fmt, ap);
    va_end(ap);
    compiler->ctx->line = line;
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

static void emit_byte(Compiler *compiler, uint8_t byte, int line)
{
    chunk_write(&compiler->function->chunk, byte, line);
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
    compiler->scope_depth = type == FUNC_FUNCTION ? 1 : 0;

    /* Slot 0 holds the running closure. */
    compiler->locals[0].name.chars = "";
    compiler->locals[0].name.length = 0;
    compiler->locals[0].depth = 0;
    compiler->locals[0].captured = false;
    compiler->local_count = 1;
}

static int add_local(Compiler *compiler, Text name, int line)
{
    if (compiler->local_count == MAX_LOCALS) {
        compile_error(compiler, line, "too many local variables in one function");
    }
    Local *local = &compiler->locals[compiler->local_count];

    local->name = name;
    local->depth = -1;
    local->captured = false;
    local->is_const = false;
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
    while (compiler->local_count > 0 &&
           compiler->locals[compiler->local_count - 1].depth > compiler->scope_depth) {
        if (compiler->locals[compiler->local_count - 1].captured) {
            emit_byte(compiler, OP_CLOSE_UPVALUE, line);
        } else {
            emit_byte(compiler, OP_POP, line);
        }
        compiler->local_count--;
    }
}

static void mark_local_initialized(Compiler *compiler)
{
    if (compiler->scope_depth > 0) {
        compiler->locals[compiler->local_count - 1].depth = compiler->scope_depth;
    }
}

typedef enum {
    VAR_LOCAL,
    VAR_UPVALUE,
    VAR_GLOBAL
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
    ref.kind = VAR_GLOBAL;
    ref.index = 0;
    ref.name = name_constant(name);
    return ref;
}

static void emit_get(Compiler *compiler, VarRef ref, int line)
{
    switch (ref.kind) {
    case VAR_LOCAL:
        emit_byte(compiler, OP_GET_LOCAL, line);
        emit_byte(compiler, (uint8_t)ref.index, line);
        break;
    case VAR_UPVALUE:
        emit_byte(compiler, OP_GET_UPVALUE, line);
        emit_byte(compiler, (uint8_t)ref.index, line);
        break;
    case VAR_GLOBAL:
        emit_byte(compiler, OP_GET_GLOBAL, line);
        emit_global_name(compiler, ref.name, line);
        break;
    }
}

static void emit_set(Compiler *compiler, VarRef ref, int line)
{
    switch (ref.kind) {
    case VAR_LOCAL:
        emit_byte(compiler, OP_SET_LOCAL, line);
        emit_byte(compiler, (uint8_t)ref.index, line);
        break;
    case VAR_UPVALUE:
        emit_byte(compiler, OP_SET_UPVALUE, line);
        emit_byte(compiler, (uint8_t)ref.index, line);
        break;
    case VAR_GLOBAL:
        emit_byte(compiler, OP_SET_GLOBAL, line);
        emit_global_name(compiler, ref.name, line);
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
    }

    for (ParamList *param = params; param; param = param->next) {
        add_local(&fn, param->name, line);
        mark_local_initialized(&fn);
        fn.function->arity++;
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

static void compile_discard_assignment(Compiler *compiler, Stmt *stmt, TokenType op,
                                       int line)
{
    Expr *target = stmt->as.assign.target;

    if (target->kind == EXPR_VARIABLE) {
        VarRef ref = resolve_variable(compiler, target->as.variable, line);

        check_mutable(compiler, ref, target->as.variable, line);
        if (op != TOKEN_EQUAL) {
            emit_get(compiler, ref, line);
        }
        compile_expression(compiler, stmt->as.assign.value);
        if (op != TOKEN_EQUAL) {
            switch (op) {
            case TOKEN_PLUS_EQUAL: emit_byte(compiler, OP_ADD, line); break;
            case TOKEN_MINUS_EQUAL: emit_byte(compiler, OP_SUBTRACT, line); break;
            case TOKEN_STAR_EQUAL: emit_byte(compiler, OP_MULTIPLY, line); break;
            case TOKEN_SLASH_EQUAL: emit_byte(compiler, OP_DIVIDE, line); break;
            default: break;
            }
        }
        emit_set(compiler, ref, line);
        return;
    }

    compile_expression(compiler, target->as.index.object);
    compile_expression(compiler, target->as.index.index);
    if (op != TOKEN_EQUAL) {
        emit_byte(compiler, OP_DUP2, line);
        emit_byte(compiler, OP_INDEX, line);
    }
    compile_expression(compiler, stmt->as.assign.value);
    if (op != TOKEN_EQUAL) {
        switch (op) {
        case TOKEN_PLUS_EQUAL: emit_byte(compiler, OP_ADD, line); break;
        case TOKEN_MINUS_EQUAL: emit_byte(compiler, OP_SUBTRACT, line); break;
        case TOKEN_STAR_EQUAL: emit_byte(compiler, OP_MULTIPLY, line); break;
        case TOKEN_SLASH_EQUAL: emit_byte(compiler, OP_DIVIDE, line); break;
        default: break;
        }
    }
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

/* Emits the condition of an if/while/for. If it is "local <op> constant" it
 * uses a comparison-and-jump superinstruction; returns the offset to patch. */
static int emit_condition_jump(Compiler *compiler, Expr *condition, int line)
{
    if (condition->kind == EXPR_BINARY && condition->as.binary.left->kind == EXPR_VARIABLE &&
        condition->as.binary.right->kind == EXPR_NUMBER) {
        TokenType op = condition->as.binary.op;
        uint8_t branch = 0;

        if (op == TOKEN_LESS) {
            branch = OP_JUMP_IF_NOT_LESS_LOCAL_CONST;
        } else if (op == TOKEN_GREATER) {
            branch = OP_JUMP_IF_NOT_GREATER_LOCAL_CONST;
        }
        if (branch) {
            int slot = resolve_local(compiler, condition->as.binary.left->as.variable, line);

            if (slot >= 0) {
                int index =
                    add_constant(compiler, NUMBER_VAL(condition->as.binary.right->as.number), line);

                emit_byte(compiler, branch, line);
                emit_byte(compiler, (uint8_t)slot, line);
                emit_u16(compiler, (uint16_t)index, line);
                emit_byte(compiler, 0xff, line);
                emit_byte(compiler, 0xff, line);
                return compiler->function->chunk.count - 2;
            }
        }
    }
    compile_expression(compiler, condition);
    return emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);
}

static void compile_if(Compiler *compiler, Stmt *stmt, int line)
{
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
    static const Text iterable_name = {" iterable", 9};
    static const Text count_name = {" count", 6};
    static const Text index_name = {" index", 6};
    LoopCtx loop;
    int iterable_slot;
    int count_slot;
    int index_slot;
    int exit_jump;

    begin_scope(compiler);

    compile_expression(compiler, stmt->as.for_in.iterable);
    emit_byte(compiler, OP_ITER_KEYS, line);
    iterable_slot = add_local(compiler, iterable_name, line);
    mark_local_initialized(compiler);

    emit_byte(compiler, OP_LEN, line);
    count_slot = add_local(compiler, count_name, line);
    mark_local_initialized(compiler);

    emit_constant(compiler, NUMBER_VAL(0), line);
    index_slot = add_local(compiler, index_name, line);
    mark_local_initialized(compiler);

    int loop_start = compiler->function->chunk.count;

    emit_byte(compiler, OP_GET_LOCAL, line);
    emit_byte(compiler, (uint8_t)index_slot, line);
    emit_byte(compiler, OP_GET_LOCAL, line);
    emit_byte(compiler, (uint8_t)count_slot, line);
    emit_byte(compiler, OP_LESS, line);
    exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE_POP, line);

    emit_byte(compiler, OP_GET_LOCAL, line);
    emit_byte(compiler, (uint8_t)iterable_slot, line);
    emit_byte(compiler, OP_GET_LOCAL, line);
    emit_byte(compiler, (uint8_t)index_slot, line);
    emit_byte(compiler, OP_INDEX, line);

    begin_scope(compiler);
    add_local(compiler, stmt->as.for_in.name, line);
    mark_local_initialized(compiler);

    memset(&loop, 0, sizeof(loop));
    loop.start = loop_start;
    loop.continue_target = -1;
    loop.local_count = compiler->local_count - 1;

    LoopCtx *previous_loop = compiler->loop;
    compiler->loop = &loop;
    compile_statement(compiler, stmt->as.for_in.body);
    compiler->loop = previous_loop;

    end_scope(compiler, line);

    loop.continue_target = compiler->function->chunk.count;
    for (int i = 0; i < loop.continue_count; i++) {
        patch_offset(compiler, loop.continues[i]);
    }

    emit_byte(compiler, OP_GET_LOCAL, line);
    emit_byte(compiler, (uint8_t)index_slot, line);
    emit_constant(compiler, NUMBER_VAL(1), line);
    emit_byte(compiler, OP_ADD, line);
    emit_byte(compiler, OP_SET_LOCAL, line);
    emit_byte(compiler, (uint8_t)index_slot, line);
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
    int loop_start = compiler->function->chunk.count;
    LoopCtx loop;

    memset(&loop, 0, sizeof(loop));
    loop.start = loop_start;
    loop.continue_target = loop_start;
    loop.local_count = compiler->local_count;

    int exit_jump = emit_condition_jump(compiler, stmt->as.while_stmt.condition, line);

    LoopCtx *previous_loop = compiler->loop;
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

    LoopCtx *previous_loop = compiler->loop;
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

static void compile_break(Compiler *compiler, int line)
{
    if (!compiler->loop) {
        compile_error(compiler, line, "'break' can only be used inside a loop");
    }
    LoopCtx *loop = compiler->loop;

    compile_cleanup_to(compiler, loop->local_count, line);
    int offset = emit_jump(compiler, OP_JUMP, line);

    if (loop->break_count + 1 > loop->break_capacity) {
        loop->break_capacity = loop->break_capacity < 4 ? 4 : loop->break_capacity * 2;
        loop->breaks = realloc(loop->breaks, sizeof(int) * (size_t)loop->break_capacity);
    }
    loop->breaks[loop->break_count++] = offset;
}

static void compile_continue(Compiler *compiler, int line)
{
    if (!compiler->loop) {
        compile_error(compiler, line, "'continue' can only be used inside a loop");
    }
    LoopCtx *loop = compiler->loop;

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

static void compile_statement(Compiler *compiler, Stmt *stmt)
{
    int line = stmt->line;

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
            if (stmt->as.var.is_const) {
                mark_global_const(compiler, name, line);
            }
            emit_byte(compiler, OP_DEFINE_GLOBAL, line);
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
        begin_scope(compiler);
        for (StmtList *list = stmt->as.block.statements; list; list = list->next) {
            compile_statement(compiler, list->stmt);
        }
        end_scope(compiler, line);
        break;

    case STMT_IF:
        compile_if(compiler, stmt, line);
        break;

    case STMT_WHILE:
        compile_while(compiler, stmt, line);
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
            compile_function_value(compiler, stmt->as.function.name, stmt->as.function.params,
                                   stmt->as.function.body, line);
            ObjString *name = name_constant(stmt->as.function.name);

            emit_byte(compiler, OP_DEFINE_GLOBAL, line);
            emit_global_name(compiler, name, line);
        } else {
            add_local(compiler, stmt->as.function.name, line);
            mark_local_initialized(compiler);
            compile_function_value(compiler, stmt->as.function.name, stmt->as.function.params,
                                   stmt->as.function.body, line);
        }
        break;
    }

    case STMT_RETURN:
        if (compiler->type == FUNC_SCRIPT) {
            compile_error(compiler, line, "'return' can only be used inside a function");
        }
        if (stmt->as.ret.value && stmt->as.ret.value->kind == EXPR_VARIABLE) {
            int slot = resolve_local(compiler, stmt->as.ret.value->as.variable, line);

            if (slot >= 0) {
                emit_byte(compiler, OP_RETURN_LOCAL, line);
                emit_byte(compiler, (uint8_t)slot, line);
                break;
            }
        }
        if (stmt->as.ret.value) {
            compile_expression(compiler, stmt->as.ret.value);
        } else {
            emit_byte(compiler, OP_NIL, line);
        }
        emit_byte(compiler, OP_RETURN, line);
        break;

    case STMT_BREAK:
        compile_break(compiler, line);
        break;

    case STMT_CONTINUE:
        compile_continue(compiler, line);
        break;
    }
}

/* Returns the superinstruction for "local <op> constant", or -1 if it does not apply. */
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





static void compile_binary_operator(Compiler *compiler, TokenType op, int line)
{
    switch (op) {
    case TOKEN_PLUS: emit_byte(compiler, OP_ADD, line); break;
    case TOKEN_MINUS: emit_byte(compiler, OP_SUBTRACT, line); break;
    case TOKEN_STAR: emit_byte(compiler, OP_MULTIPLY, line); break;
    case TOKEN_SLASH: emit_byte(compiler, OP_DIVIDE, line); break;
    case TOKEN_PERCENT: emit_byte(compiler, OP_MODULO, line); break;
    case TOKEN_EQUAL_EQUAL: emit_byte(compiler, OP_EQUAL, line); break;
    case TOKEN_BANG_EQUAL:
        emit_byte(compiler, OP_EQUAL, line);
        emit_byte(compiler, OP_NOT, line);
        break;
    case TOKEN_GREATER: emit_byte(compiler, OP_GREATER, line); break;
    case TOKEN_GREATER_EQUAL:
        emit_byte(compiler, OP_LESS, line);
        emit_byte(compiler, OP_NOT, line);
        break;
    case TOKEN_LESS: emit_byte(compiler, OP_LESS, line); break;
    case TOKEN_LESS_EQUAL:
        emit_byte(compiler, OP_GREATER, line);
        emit_byte(compiler, OP_NOT, line);
        break;
    default:
        break;
    }
}

static void compile_expression(Compiler *compiler, Expr *expr)
{
    int line = expr->line;

    switch (expr->kind) {
    case EXPR_NUMBER:
        emit_constant(compiler, NUMBER_VAL(expr->as.number), line);
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
        emit_byte(compiler, expr->as.unary.op == TOKEN_MINUS ? OP_NEGATE : OP_NOT, line);
        break;

    case EXPR_BINARY: {
        Expr *left = expr->as.binary.left;
        Expr *right = expr->as.binary.right;

        if (left->kind == EXPR_VARIABLE && right->kind == EXPR_NUMBER) {
            int fused = fused_local_const_opcode(expr->as.binary.op);

            if (fused >= 0) {
                int slot = resolve_local(compiler, left->as.variable, line);

                if (slot >= 0) {
                    int index = add_constant(compiler, NUMBER_VAL(right->as.number), line);

                    emit_byte(compiler, (uint8_t)fused, line);
                    emit_byte(compiler, (uint8_t)slot, line);
                    emit_u16(compiler, (uint16_t)index, line);
                    break;
                }
            }
        }
        compile_expression(compiler, left);
        compile_expression(compiler, right);
        compile_binary_operator(compiler, expr->as.binary.op, line);
        break;
    }

    case EXPR_LOGICAL:
        if (expr->as.logical.op == TOKEN_AND) {
            compile_expression(compiler, expr->as.logical.left);
            int end_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, line);

            emit_byte(compiler, OP_POP, line);
            compile_expression(compiler, expr->as.logical.right);
            patch_jump(compiler, end_jump);
        } else {
            compile_expression(compiler, expr->as.logical.left);
            int else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, line);
            int end_jump = emit_jump(compiler, OP_JUMP, line);

            patch_jump(compiler, else_jump);
            emit_byte(compiler, OP_POP, line);
            compile_expression(compiler, expr->as.logical.right);
            patch_jump(compiler, end_jump);
        }
        break;

    case EXPR_VARIABLE: {
        VarRef ref = resolve_variable(compiler, expr->as.variable, line);

        emit_get(compiler, ref, line);
        break;
    }

    case EXPR_CALL:
        compile_expression(compiler, expr->as.call.callee);
        for (ExprList *arg = expr->as.call.arguments; arg; arg = arg->next) {
            compile_expression(compiler, arg->expr);
        }
        emit_byte(compiler, OP_CALL, line);
        emit_byte(compiler, (uint8_t)expr->as.call.argument_count, line);
        break;

    case EXPR_INDEX:
        compile_expression(compiler, expr->as.index.object);
        compile_expression(compiler, expr->as.index.index);
        emit_byte(compiler, OP_INDEX, line);
        break;

    case EXPR_ARRAY:
        for (ExprList *item = expr->as.array.items; item; item = item->next) {
            compile_expression(compiler, item->expr);
        }
        emit_byte(compiler, OP_ARRAY, line);
        emit_u16(compiler, (uint16_t)expr->as.array.count, line);
        break;

    case EXPR_MAP:
        for (MapPair *pair = expr->as.map.pairs; pair; pair = pair->next) {
            compile_expression(compiler, pair->key);
            compile_expression(compiler, pair->value);
        }
        emit_byte(compiler, OP_MAP, line);
        emit_u16(compiler, (uint16_t)expr->as.map.count, line);
        break;

    case EXPR_FUNCTION:
        compile_function_value(compiler, expr->as.function.name, expr->as.function.params,
                               expr->as.function.body, line);
        break;
    }
}

static void apply_const_globals(CompileContext *ctx)
{
    for (ConstGlobal *entry = ctx->const_globals; entry; entry = entry->next) {
        int slot = vm_global_slot(entry->name);

        if (slot >= 0) {
            vm_mark_global_const(slot);
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

ObjFunction *compile_ast(Stmt *program, const char *name, bool repl_mode,
                         char *error, size_t error_size, int *error_line)
{
    CompileContext ctx;
    Compiler compiler;
    ObjFunction *function;

    vm_init();
    memset(&ctx, 0, sizeof(ctx));
    ctx.repl_mode = repl_mode;

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
        return NULL;
    }

    init_compiler(&compiler, NULL, FUNC_SCRIPT, &ctx);
    compiler.function->name = object_string(name, (int)strlen(name));

    for (StmtList *list = program->as.block.statements; list; list = list->next) {
        compile_statement(&compiler, list->stmt);
    }
    emit_byte(&compiler, OP_NIL, program->line);
    emit_byte(&compiler, OP_RETURN, program->line);

    apply_const_globals(&ctx);
    free_const_globals(&ctx);
    function = compiler.function;
    mem_resume_gc();
    return function;
}
