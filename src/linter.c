/* linter.c - static analysis linter for the Spark language. */
#include "linter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "parser.h"

typedef struct LintSymbol {
    Text name;
    int line;
    int column;
    bool used;
    bool is_param;
    struct LintSymbol *next;
} LintSymbol;

typedef struct LintScope {
    struct LintScope *parent;
    LintSymbol *symbols;
    int depth;
    bool is_function;
} LintScope;

typedef struct {
    LintScope *scope;
    const char *path;
    const char *source;
    int warnings;
    int function_depth;
} LinterContext;

static bool text_eq(Text a, Text b)
{
    return a.length == b.length && memcmp(a.chars, b.chars, (size_t)a.length) == 0;
}

static void push_scope(LinterContext *ctx, bool is_function)
{
    LintScope *scope = malloc(sizeof(LintScope));
    scope->parent = ctx->scope;
    scope->symbols = NULL;
    scope->depth = ctx->scope ? ctx->scope->depth + 1 : 0;
    scope->is_function = is_function;
    ctx->scope = scope;
    if (is_function) {
        ctx->function_depth++;
    }
}

static void pop_scope(LinterContext *ctx)
{
    LintScope *scope = ctx->scope;
    if (!scope) {
        return;
    }

    /* Check for unused variables in local/function scopes */
    if (scope->depth > 0 || ctx->function_depth > 0) {
        for (LintSymbol *sym = scope->symbols; sym; sym = sym->next) {
            /* Variables starting with '_' are intentionally unused */
            if (!sym->used && sym->name.length > 0 && sym->name.chars[0] != '_') {
                ctx->warnings++;
                printf("%s:%d:%d: warning [unused-variable]: variable '%.*s' is declared but never used\n",
                       ctx->path, sym->line, sym->column, sym->name.length, sym->name.chars);
            }
        }
    }

    if (scope->is_function) {
        ctx->function_depth--;
    }

    LintSymbol *sym = scope->symbols;
    while (sym) {
        LintSymbol *next = sym->next;
        free(sym);
        sym = next;
    }

    ctx->scope = scope->parent;
    free(scope);
}

static void declare_symbol(LinterContext *ctx, Text name, int line, int column, bool is_param)
{
    if (name.length == 0 || name.chars[0] == '<') {
        return;
    }

    /* Check for shadowing against enclosing local/param scopes */
    for (LintScope *s = ctx->scope->parent; s; s = s->parent) {
        /* Only check shadowing within local scopes and parameters */
        if (s->depth == 0 && !s->is_function) {
            break;
        }
        for (LintSymbol *sym = s->symbols; sym; sym = sym->next) {
            if (text_eq(sym->name, name)) {
                ctx->warnings++;
                printf("%s:%d:%d: warning [shadowed-variable]: variable '%.*s' shadows variable declared at line %d\n",
                       ctx->path, line, column, name.length, name.chars, sym->line);
                goto added;
            }
        }
    }

added:;
    LintSymbol *sym = malloc(sizeof(LintSymbol));
    sym->name = name;
    sym->line = line;
    sym->column = column;
    sym->used = false;
    sym->is_param = is_param;
    sym->next = ctx->scope->symbols;
    ctx->scope->symbols = sym;
}

static void use_variable(LinterContext *ctx, Text name)
{
    for (LintScope *s = ctx->scope; s; s = s->parent) {
        for (LintSymbol *sym = s->symbols; sym; sym = sym->next) {
            if (text_eq(sym->name, name)) {
                sym->used = true;
                return;
            }
        }
    }
}

static bool is_terminator(const Stmt *stmt)
{
    if (!stmt) return false;
    return stmt->kind == STMT_RETURN || stmt->kind == STMT_THROW ||
           stmt->kind == STMT_BREAK || stmt->kind == STMT_CONTINUE;
}

static const char *terminator_desc(const Stmt *stmt)
{
    if (!stmt) return "statement";
    switch (stmt->kind) {
    case STMT_RETURN: return "return";
    case STMT_THROW: return "throw";
    case STMT_BREAK: return "break";
    case STMT_CONTINUE: return "continue";
    default: return "statement";
    }
}

static void lint_expression(LinterContext *ctx, Expr *expr);
static void lint_statement(LinterContext *ctx, Stmt *stmt);

static void lint_expression(LinterContext *ctx, Expr *expr)
{
    if (!expr) return;

    switch (expr->kind) {
    case EXPR_VARIABLE:
        use_variable(ctx, expr->as.variable);
        break;

    case EXPR_UNARY:
        lint_expression(ctx, expr->as.unary.operand);
        break;

    case EXPR_BINARY:
        lint_expression(ctx, expr->as.binary.left);
        lint_expression(ctx, expr->as.binary.right);
        break;

    case EXPR_LOGICAL:
        lint_expression(ctx, expr->as.logical.left);
        lint_expression(ctx, expr->as.logical.right);
        break;

    case EXPR_TERNARY:
        lint_expression(ctx, expr->as.ternary.condition);
        lint_expression(ctx, expr->as.ternary.then_branch);
        lint_expression(ctx, expr->as.ternary.else_branch);
        break;

    case EXPR_COALESCE:
        lint_expression(ctx, expr->as.coalesce.left);
        lint_expression(ctx, expr->as.coalesce.right);
        break;

    case EXPR_CALL:
        lint_expression(ctx, expr->as.call.callee);
        for (ExprList *arg = expr->as.call.arguments; arg; arg = arg->next) {
            lint_expression(ctx, arg->expr);
        }
        break;

    case EXPR_METHOD_CALL:
        lint_expression(ctx, expr->as.method_call.receiver);
        for (ExprList *arg = expr->as.method_call.arguments; arg; arg = arg->next) {
            lint_expression(ctx, arg->expr);
        }
        break;

    case EXPR_INDEX:
        lint_expression(ctx, expr->as.index.object);
        lint_expression(ctx, expr->as.index.index);
        break;

    case EXPR_ARRAY:
        for (ExprList *item = expr->as.array.items; item; item = item->next) {
            lint_expression(ctx, item->expr);
        }
        break;

    case EXPR_MAP:
        for (MapPair *pair = expr->as.map.pairs; pair; pair = pair->next) {
            lint_expression(ctx, pair->key);
            lint_expression(ctx, pair->value);
        }
        break;

    case EXPR_INTERPOLATION:
        for (InterpPart *part = expr->as.interpolation.parts; part; part = part->next) {
            if (part->is_expression) {
                lint_expression(ctx, part->expr);
            }
        }
        break;

    case EXPR_FUNCTION:
        push_scope(ctx, true);
        for (ParamList *param = expr->as.function.params; param; param = param->next) {
            if (param->default_value) {
                lint_expression(ctx, param->default_value);
            }
            declare_symbol(ctx, param->name, expr->line, expr->column, true);
        }
        lint_statement(ctx, expr->as.function.body);
        pop_scope(ctx);
        break;

    default:
        break;
    }
}

static void lint_statement(LinterContext *ctx, Stmt *stmt)
{
    if (!stmt) return;

    switch (stmt->kind) {
    case STMT_EXPRESSION:
        lint_expression(ctx, stmt->as.expression);
        break;

    case STMT_VAR:
        if (stmt->as.var.initializer) {
            lint_expression(ctx, stmt->as.var.initializer);
        }
        declare_symbol(ctx, stmt->as.var.name, stmt->line, stmt->column, false);
        break;

    case STMT_ASSIGN:
        lint_expression(ctx, stmt->as.assign.target);
        lint_expression(ctx, stmt->as.assign.value);
        break;

    case STMT_BLOCK: {
        push_scope(ctx, false);
        bool terminated = false;
        const char *t_desc = NULL;
        bool reported_dead = false;

        for (StmtList *item = stmt->as.block.statements; item; item = item->next) {
            if (terminated && !reported_dead) {
                ctx->warnings++;
                printf("%s:%d:%d: warning [unreachable-code]: unreachable code after '%s'\n",
                       ctx->path, item->stmt->line, item->stmt->column, t_desc);
                reported_dead = true;
            }
            lint_statement(ctx, item->stmt);
            if (is_terminator(item->stmt)) {
                terminated = true;
                t_desc = terminator_desc(item->stmt);
            }
        }
        pop_scope(ctx);
        break;
    }

    case STMT_IF:
        lint_expression(ctx, stmt->as.if_stmt.condition);
        lint_statement(ctx, stmt->as.if_stmt.then_branch);
        if (stmt->as.if_stmt.else_branch) {
            lint_statement(ctx, stmt->as.if_stmt.else_branch);
        }
        break;

    case STMT_WHILE:
        lint_expression(ctx, stmt->as.while_stmt.condition);
        lint_statement(ctx, stmt->as.while_stmt.body);
        break;

    case STMT_DO_WHILE:
        lint_statement(ctx, stmt->as.do_while.body);
        lint_expression(ctx, stmt->as.do_while.condition);
        break;

    case STMT_FOR:
        push_scope(ctx, false);
        if (stmt->as.for_stmt.initializer) {
            lint_statement(ctx, stmt->as.for_stmt.initializer);
        }
        if (stmt->as.for_stmt.condition) {
            lint_expression(ctx, stmt->as.for_stmt.condition);
        }
        if (stmt->as.for_stmt.step) {
            lint_statement(ctx, stmt->as.for_stmt.step);
        }
        lint_statement(ctx, stmt->as.for_stmt.body);
        pop_scope(ctx);
        break;

    case STMT_FOR_IN:
        lint_expression(ctx, stmt->as.for_in.iterable);
        push_scope(ctx, false);
        declare_symbol(ctx, stmt->as.for_in.name, stmt->line, stmt->column, false);
        lint_statement(ctx, stmt->as.for_in.body);
        pop_scope(ctx);
        break;

    case STMT_SWITCH:
        lint_expression(ctx, stmt->as.switch_stmt.subject);
        for (SwitchCase *sc = stmt->as.switch_stmt.cases; sc; sc = sc->next) {
            push_scope(ctx, false);
            if (sc->value) {
                lint_expression(ctx, sc->value);
            }
            bool terminated = false;
            const char *t_desc = NULL;
            bool reported_dead = false;

            for (StmtList *item = sc->body; item; item = item->next) {
                if (terminated && !reported_dead) {
                    ctx->warnings++;
                    printf("%s:%d:%d: warning [unreachable-code]: unreachable code after '%s'\n",
                           ctx->path, item->stmt->line, item->stmt->column, t_desc);
                    reported_dead = true;
                }
                lint_statement(ctx, item->stmt);
                if (is_terminator(item->stmt)) {
                    terminated = true;
                    t_desc = terminator_desc(item->stmt);
                }
            }
            pop_scope(ctx);
        }
        break;

    case STMT_FUNCTION:
        declare_symbol(ctx, stmt->as.function.name, stmt->line, stmt->column, false);
        push_scope(ctx, true);
        for (ParamList *param = stmt->as.function.params; param; param = param->next) {
            if (param->default_value) {
                lint_expression(ctx, param->default_value);
            }
            declare_symbol(ctx, param->name, stmt->line, stmt->column, true);
        }
        lint_statement(ctx, stmt->as.function.body);
        pop_scope(ctx);
        break;

    case STMT_RETURN:
        if (stmt->as.ret.value) {
            lint_expression(ctx, stmt->as.ret.value);
        }
        break;

    case STMT_THROW:
        if (stmt->as.throw_stmt.value) {
            lint_expression(ctx, stmt->as.throw_stmt.value);
        }
        break;

    case STMT_TRY:
        lint_statement(ctx, stmt->as.try_stmt.try_block);
        if (stmt->as.try_stmt.has_catch) {
            push_scope(ctx, false);
            declare_symbol(ctx, stmt->as.try_stmt.catch_var, stmt->line, stmt->column, false);
            lint_statement(ctx, stmt->as.try_stmt.catch_block);
            pop_scope(ctx);
        }
        if (stmt->as.try_stmt.finally_block) {
            lint_statement(ctx, stmt->as.try_stmt.finally_block);
        }
        break;

    case STMT_CLASS:
        declare_symbol(ctx, stmt->as.class_stmt.name, stmt->line, stmt->column, false);
        for (ClassMethod *m = stmt->as.class_stmt.methods; m; m = m->next) {
            push_scope(ctx, true);
            declare_symbol(ctx, (Text){"self", 4}, stmt->line, stmt->column, true);
            use_variable(ctx, (Text){"self", 4});
            for (ParamList *param = m->params; param; param = param->next) {
                declare_symbol(ctx, param->name, stmt->line, stmt->column, true);
            }
            lint_statement(ctx, m->body);
            pop_scope(ctx);
        }
        break;

    default:
        break;
    }
}

int linter_lint(const char *source, const char *path)
{
    Arena arena;
    Stmt *program = NULL;
    char error[256];
    int err_line = 0, err_col = 0;

    arena_init(&arena);
    if (!parse_source(source, &arena, &program, error, sizeof(error), &err_line, &err_col)) {
        fprintf(stderr, "%s:%d:%d: syntax error: %s\n", path, err_line, err_col, error);
        arena_free(&arena);
        return -1;
    }

    LinterContext ctx;
    ctx.scope = NULL;
    ctx.path = path;
    ctx.source = source;
    ctx.warnings = 0;
    ctx.function_depth = 0;

    /* Push root scope */
    push_scope(&ctx, false);

    for (StmtList *item = program->as.block.statements; item; item = item->next) {
        lint_statement(&ctx, item->stmt);
    }

    pop_scope(&ctx);
    arena_free(&arena);

    return ctx.warnings;
}
