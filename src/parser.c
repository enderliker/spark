#include "parser.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    Arena *arena;
    jmp_buf jump;
    char error[256];
    int error_line;
} Parser;

static Stmt *declaration(Parser *parser);
static Stmt *statement(Parser *parser);
static Expr *expression(Parser *parser);
static Stmt *expression_or_assignment(Parser *parser, bool with_semicolon);

static void fail_at(Parser *parser, Token token, const char *message)
{
    snprintf(parser->error, sizeof(parser->error), "%s", message);
    parser->error_line = token.line;
    longjmp(parser->jump, 1);
}

static void fail(Parser *parser, const char *message)
{
    fail_at(parser, parser->current, message);
}

static void advance(Parser *parser)
{
    parser->previous = parser->current;

    for (;;) {
        parser->current = lexer_next(&parser->lexer);
        if (parser->current.type != TOKEN_ERROR) {
            break;
        }
        fail_at(parser, parser->current, parser->lexer.error);
    }
}

static bool check(const Parser *parser, TokenType type)
{
    return parser->current.type == type;
}

static bool match(Parser *parser, TokenType type)
{
    if (!check(parser, type)) {
        return false;
    }
    advance(parser);
    return true;
}

static Token consume(Parser *parser, TokenType type, const char *message)
{
    if (check(parser, type)) {
        Token token = parser->current;

        advance(parser);
        return token;
    }
    fail(parser, message);
    return parser->current; /* unreachable */
}

static Text text_from_token(Parser *parser, Token token)
{
    Text text;

    text.chars = arena_strndup(parser->arena, token.start, token.length);
    text.length = token.length;
    return text;
}

static void *new_node(Parser *parser, size_t size)
{
    return arena_alloc(parser->arena, size);
}

static Expr *new_expr(Parser *parser, ExprKind kind, int line)
{
    Expr *expr = new_node(parser, sizeof(Expr));

    expr->kind = kind;
    expr->line = line;
    return expr;
}

static Stmt *new_stmt(Parser *parser, StmtKind kind, int line)
{
    Stmt *stmt = new_node(parser, sizeof(Stmt));

    stmt->kind = kind;
    stmt->line = line;
    return stmt;
}

static void list_append_expr(Parser *parser, ExprList **head, ExprList **tail, Expr *expr)
{
    ExprList *node = new_node(parser, sizeof(ExprList));

    node->expr = expr;
    if (*tail) {
        (*tail)->next = node;
    } else {
        *head = node;
    }
    *tail = node;
}

static void list_append_stmt(Parser *parser, StmtList **head, StmtList **tail, Stmt *stmt)
{
    StmtList *node = new_node(parser, sizeof(StmtList));

    node->stmt = stmt;
    if (*tail) {
        (*tail)->next = node;
    } else {
        *head = node;
    }
    *tail = node;
}

static void list_append_param(Parser *parser, ParamList **head, ParamList **tail, Text name)
{
    ParamList *node = new_node(parser, sizeof(ParamList));

    node->name = name;
    if (*tail) {
        (*tail)->next = node;
    } else {
        *head = node;
    }
    *tail = node;
}

static Text parse_string_literal(Parser *parser, Token token)
{
    int raw_length = token.length - 2;
    const char *raw = token.start + 1;
    char *buffer = new_node(parser, (size_t)raw_length + 1);
    int out = 0;

    for (int i = 0; i < raw_length; i++) {
        char c = raw[i];

        if (c != '\\') {
            buffer[out++] = c;
            continue;
        }
        i++;
        if (i >= raw_length) {
            fail_at(parser, token, "incomplete escape sequence in the string");
        }
        switch (raw[i]) {
        case 'n': buffer[out++] = '\n'; break;
        case 't': buffer[out++] = '\t'; break;
        case 'r': buffer[out++] = '\r'; break;
        case '\\': buffer[out++] = '\\'; break;
        case '"': buffer[out++] = '"'; break;
        case '0': buffer[out++] = '\0'; break;
        default: {
            char message[96];

            snprintf(message, sizeof(message), "unknown escape '\\%c'", raw[i]);
            fail_at(parser, token, message);
            break;
        }
        }
    }
    buffer[out] = '\0';

    Text text;
    text.chars = buffer;
    text.length = out;
    return text;
}

static double parse_number_literal(Parser *parser, Token token)
{
    char buffer[64];

    if (token.length >= (int)sizeof(buffer)) {
        fail_at(parser, token, "number too long");
    }
    memcpy(buffer, token.start, (size_t)token.length);
    buffer[token.length] = '\0';
    return strtod(buffer, NULL);
}

static Expr *primary(Parser *parser)
{
    Token token = parser->current;

    if (match(parser, TOKEN_FALSE)) {
        Expr *expr = new_expr(parser, EXPR_BOOL, token.line);

        expr->as.boolean = false;
        return expr;
    }
    if (match(parser, TOKEN_TRUE)) {
        Expr *expr = new_expr(parser, EXPR_BOOL, token.line);

        expr->as.boolean = true;
        return expr;
    }
    if (match(parser, TOKEN_NIL)) {
        return new_expr(parser, EXPR_NIL, token.line);
    }
    if (match(parser, TOKEN_NUMBER)) {
        Expr *expr = new_expr(parser, EXPR_NUMBER, token.line);

        expr->as.number = parse_number_literal(parser, token);
        return expr;
    }
    if (match(parser, TOKEN_STRING)) {
        Expr *expr = new_expr(parser, EXPR_STRING, token.line);

        expr->as.string = parse_string_literal(parser, token);
        return expr;
    }
    if (match(parser, TOKEN_IDENTIFIER)) {
        Expr *expr = new_expr(parser, EXPR_VARIABLE, token.line);

        expr->as.variable = text_from_token(parser, token);
        return expr;
    }
    if (match(parser, TOKEN_LEFT_PAREN)) {
        Expr *inner = expression(parser);

        consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the expression");
        return inner;
    }
    if (match(parser, TOKEN_LEFT_BRACKET)) {
        Expr *array = new_expr(parser, EXPR_ARRAY, token.line);
        ExprList *head = NULL;
        ExprList *tail = NULL;
        int count = 0;

        if (!check(parser, TOKEN_RIGHT_BRACKET)) {
            do {
                if (count == 255) {
                    fail(parser, "an array literal cannot have more than 255 elements");
                }
                list_append_expr(parser, &head, &tail, expression(parser));
                count++;
            } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_BRACKET));
        }
        consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' when closing the array");
        array->as.array.items = head;
        array->as.array.count = count;
        return array;
    }
    if (match(parser, TOKEN_LEFT_BRACE)) {
        Expr *map = new_expr(parser, EXPR_MAP, token.line);
        MapPair *head = NULL;
        MapPair *tail = NULL;
        int count = 0;

        if (!check(parser, TOKEN_RIGHT_BRACE)) {
            do {
                if (count == 255) {
                    fail(parser, "a map literal cannot have more than 255 entries");
                }
                MapPair *pair = new_node(parser, sizeof(MapPair));

                pair->key = expression(parser);
                consume(parser, TOKEN_COLON,
                        "expected ':' between the map key and value");
                pair->value = expression(parser);
                if (tail) {
                    tail->next = pair;
                } else {
                    head = pair;
                }
                tail = pair;
                count++;
            } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_BRACE));
        }
        consume(parser, TOKEN_RIGHT_BRACE, "expected '}' when closing the map");
        map->as.map.pairs = head;
        map->as.map.count = count;
        return map;
    }
    if (match(parser, TOKEN_FN)) {
        Expr *fn = new_expr(parser, EXPR_FUNCTION, token.line);
        ParamList *params = NULL;
        ParamList *params_tail = NULL;
        int param_count = 0;

        consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'fn'");
        if (!check(parser, TOKEN_RIGHT_PAREN)) {
            do {
                Token param = consume(parser, TOKEN_IDENTIFIER, "expected the parameter name");

                if (param_count == 255) {
                    fail(parser, "a function cannot have more than 255 parameters");
                }
                list_append_param(parser, &params, &params_tail, text_from_token(parser, param));
                param_count++;
            } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_PAREN));
        }
        consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the parameters");

        fn->as.function.name.chars = "";
        fn->as.function.name.length = 0;
        fn->as.function.params = params;
        fn->as.function.param_count = param_count;
        fn->as.function.body = statement(parser);
        if (fn->as.function.body->kind != STMT_BLOCK) {
            fail_at(parser, token, "the body of a function must be enclosed in braces");
        }
        return fn;
    }

    fail(parser, "expected an expression");
    return NULL;
}

static ExprList *parse_arguments(Parser *parser, int *count)
{
    ExprList *head = NULL;
    ExprList *tail = NULL;

    *count = 0;
    if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            if (*count == 255) {
                fail(parser, "a call cannot have more than 255 arguments");
            }
            list_append_expr(parser, &head, &tail, expression(parser));
            (*count)++;
        } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_PAREN));
    }
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the arguments");
    return head;
}

static Expr *call(Parser *parser)
{
    Expr *expr = primary(parser);

    for (;;) {
        if (match(parser, TOKEN_LEFT_PAREN)) {
            Expr *call_expr = new_expr(parser, EXPR_CALL, expr->line);
            int count = 0;
            ExprList *arguments = parse_arguments(parser, &count);

            call_expr->as.call.callee = expr;
            call_expr->as.call.arguments = arguments;
            call_expr->as.call.argument_count = count;
            expr = call_expr;
        } else if (match(parser, TOKEN_LEFT_BRACKET)) {
            Expr *index = new_expr(parser, EXPR_INDEX, expr->line);

            index->as.index.object = expr;
            index->as.index.index = expression(parser);
            consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' after the index");
            expr = index;
        } else {
            break;
        }
    }
    return expr;
}

static Expr *unary(Parser *parser)
{
    if (match(parser, TOKEN_BANG) || match(parser, TOKEN_NOT) || match(parser, TOKEN_MINUS)) {
        Token op = parser->previous;
        Expr *expr = new_expr(parser, EXPR_UNARY, op.line);

        expr->as.unary.op = op.type;
        expr->as.unary.operand = unary(parser);
        return expr;
    }
    return call(parser);
}

static Expr *factor(Parser *parser)
{
    Expr *expr = unary(parser);

    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = unary(parser);
        expr = binary;
    }
    return expr;
}

static Expr *term(Parser *parser)
{
    Expr *expr = factor(parser);

    while (match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = factor(parser);
        expr = binary;
    }
    return expr;
}

static Expr *comparison(Parser *parser)
{
    Expr *expr = term(parser);

    while (match(parser, TOKEN_GREATER) || match(parser, TOKEN_GREATER_EQUAL) ||
           match(parser, TOKEN_LESS) || match(parser, TOKEN_LESS_EQUAL)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = term(parser);
        expr = binary;
    }
    return expr;
}

static Expr *equality(Parser *parser)
{
    Expr *expr = comparison(parser);

    while (match(parser, TOKEN_EQUAL_EQUAL) || match(parser, TOKEN_BANG_EQUAL)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = comparison(parser);
        expr = binary;
    }
    return expr;
}

static Expr *logic_and(Parser *parser)
{
    Expr *expr = equality(parser);

    while (match(parser, TOKEN_AND)) {
        Token op = parser->previous;
        Expr *logical = new_expr(parser, EXPR_LOGICAL, op.line);

        logical->as.logical.left = expr;
        logical->as.logical.op = op.type;
        logical->as.logical.right = equality(parser);
        expr = logical;
    }
    return expr;
}

static Expr *logic_or(Parser *parser)
{
    Expr *expr = logic_and(parser);

    while (match(parser, TOKEN_OR)) {
        Token op = parser->previous;
        Expr *logical = new_expr(parser, EXPR_LOGICAL, op.line);

        logical->as.logical.left = expr;
        logical->as.logical.op = op.type;
        logical->as.logical.right = logic_and(parser);
        expr = logical;
    }
    return expr;
}

static Expr *expression(Parser *parser)
{
    return logic_or(parser);
}

static Stmt *expression_or_assignment(Parser *parser, bool with_semicolon)
{
    int line = parser->current.line;
    Expr *expr = expression(parser);

    if (match(parser, TOKEN_EQUAL) || match(parser, TOKEN_PLUS_EQUAL) ||
        match(parser, TOKEN_MINUS_EQUAL) || match(parser, TOKEN_STAR_EQUAL) ||
        match(parser, TOKEN_SLASH_EQUAL)) {
        Token op = parser->previous;
        Stmt *stmt = new_stmt(parser, STMT_ASSIGN, line);

        if (expr->kind != EXPR_VARIABLE && expr->kind != EXPR_INDEX) {
            fail_at(parser, op, "can only assign to a variable or an array element");
        }
        stmt->as.assign.target = expr;
        stmt->as.assign.op = op.type;
        stmt->as.assign.value = expression(parser);
        if (with_semicolon) {
            consume(parser, TOKEN_SEMICOLON, "expected ';' after the assignment");
        }
        return stmt;
    }

    Stmt *stmt = new_stmt(parser, STMT_EXPRESSION, line);

    stmt->as.expression = expr;
    if (with_semicolon) {
        consume(parser, TOKEN_SEMICOLON, "expected ';' after the expression");
    }
    return stmt;
}

static Stmt *block(Parser *parser)
{
    Stmt *stmt = new_stmt(parser, STMT_BLOCK, parser->previous.line);
    StmtList *head = NULL;
    StmtList *tail = NULL;

    while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
        list_append_stmt(parser, &head, &tail, declaration(parser));
    }
    consume(parser, TOKEN_RIGHT_BRACE, "expected '}' when closing the block");
    stmt->as.block.statements = head;
    return stmt;
}

static Stmt *var_declaration_after_name(Parser *parser, Token name, bool with_semicolon)
{
    Stmt *stmt = new_stmt(parser, STMT_VAR, name.line);

    stmt->as.var.name = text_from_token(parser, name);
    stmt->as.var.initializer = NULL;
    if (match(parser, TOKEN_EQUAL)) {
        stmt->as.var.initializer = expression(parser);
    }
    if (with_semicolon) {
        consume(parser, TOKEN_SEMICOLON, "expected ';' after the declaration");
    }
    return stmt;
}

static Stmt *parse_var_declaration(Parser *parser, bool with_semicolon)
{
    Token name = consume(parser, TOKEN_IDENTIFIER, "expected the variable name");

    return var_declaration_after_name(parser, name, with_semicolon);
}

static void parse_params(Parser *parser, ParamList **params, ParamList **tail, int *count)
{
    *params = NULL;
    *tail = NULL;
    *count = 0;

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after the function name");
    if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            Token param = consume(parser, TOKEN_IDENTIFIER, "expected the parameter name");

            if (*count == 255) {
                fail(parser, "a function cannot have more than 255 parameters");
            }
            list_append_param(parser, params, tail, text_from_token(parser, param));
            (*count)++;
        } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_PAREN));
    }
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the parameters");
}

static Stmt *function_declaration(Parser *parser)
{
    Token name = consume(parser, TOKEN_IDENTIFIER, "expected the function name");
    Stmt *stmt = new_stmt(parser, STMT_FUNCTION, name.line);
    ParamList *params;
    ParamList *tail;
    int count;

    parse_params(parser, &params, &tail, &count);
    stmt->as.function.name = text_from_token(parser, name);
    stmt->as.function.params = params;
    stmt->as.function.param_count = count;
    stmt->as.function.body = statement(parser);
    if (stmt->as.function.body->kind != STMT_BLOCK) {
        fail_at(parser, name, "the body of a function must be enclosed in braces");
    }
    return stmt;
}

static Stmt *if_statement(Parser *parser, int line)
{
    Stmt *stmt = new_stmt(parser, STMT_IF, line);

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'if'");
    stmt->as.if_stmt.condition = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
    stmt->as.if_stmt.then_branch = statement(parser);
    stmt->as.if_stmt.else_branch = match(parser, TOKEN_ELSE) ? statement(parser) : NULL;
    return stmt;
}

static Stmt *while_statement(Parser *parser, int line)
{
    Stmt *stmt = new_stmt(parser, STMT_WHILE, line);

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'while'");
    stmt->as.while_stmt.condition = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
    stmt->as.while_stmt.body = statement(parser);
    return stmt;
}

static Stmt *for_statement(Parser *parser, int line)
{
    Stmt *stmt = new_stmt(parser, STMT_FOR, line);

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'for'");

    if (match(parser, TOKEN_VAR)) {
        Token name = consume(parser, TOKEN_IDENTIFIER, "expected the variable name");

        if (match(parser, TOKEN_IN)) {
            Stmt *for_in = new_stmt(parser, STMT_FOR_IN, line);

            for_in->as.for_in.name = text_from_token(parser, name);
            for_in->as.for_in.iterable = expression(parser);
            consume(parser, TOKEN_RIGHT_PAREN, "expected ')' when closing the for");
            for_in->as.for_in.body = statement(parser);
            return for_in;
        }
        stmt->as.for_stmt.initializer = var_declaration_after_name(parser, name, false);
    } else if (!check(parser, TOKEN_SEMICOLON)) {
        stmt->as.for_stmt.initializer = expression_or_assignment(parser, false);
    } else {
        stmt->as.for_stmt.initializer = NULL;
    }
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the for initializer");

    stmt->as.for_stmt.condition = check(parser, TOKEN_SEMICOLON) ? NULL : expression(parser);
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the for condition");

    stmt->as.for_stmt.step = check(parser, TOKEN_RIGHT_PAREN)
                                 ? NULL
                                 : expression_or_assignment(parser, false);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' when closing the for");
    stmt->as.for_stmt.body = statement(parser);
    return stmt;
}

static Stmt *statement(Parser *parser)
{
    int line = parser->current.line;

    if (match(parser, TOKEN_IF)) {
        return if_statement(parser, line);
    }
    if (match(parser, TOKEN_WHILE)) {
        return while_statement(parser, line);
    }
    if (match(parser, TOKEN_FOR)) {
        return for_statement(parser, line);
    }
    if (match(parser, TOKEN_RETURN)) {
        Stmt *stmt = new_stmt(parser, STMT_RETURN, line);

        stmt->as.ret.value = check(parser, TOKEN_SEMICOLON) ? NULL : expression(parser);
        consume(parser, TOKEN_SEMICOLON, "expected ';' after 'return'");
        return stmt;
    }
    if (match(parser, TOKEN_BREAK)) {
        Stmt *stmt = new_stmt(parser, STMT_BREAK, line);

        consume(parser, TOKEN_SEMICOLON, "expected ';' after 'break'");
        return stmt;
    }
    if (match(parser, TOKEN_CONTINUE)) {
        Stmt *stmt = new_stmt(parser, STMT_CONTINUE, line);

        consume(parser, TOKEN_SEMICOLON, "expected ';' after 'continue'");
        return stmt;
    }
    if (match(parser, TOKEN_LEFT_BRACE)) {
        return block(parser);
    }
    return expression_or_assignment(parser, true);
}

static Stmt *declaration(Parser *parser)
{
    if (match(parser, TOKEN_VAR)) {
        return parse_var_declaration(parser, true);
    }
    if (match(parser, TOKEN_FN)) {
        return function_declaration(parser);
    }
    return statement(parser);
}

static Stmt *parse_program(Parser *parser)
{
    Stmt *program = new_stmt(parser, STMT_BLOCK, 1);
    StmtList *head = NULL;
    StmtList *tail = NULL;

    while (!check(parser, TOKEN_EOF)) {
        list_append_stmt(parser, &head, &tail, declaration(parser));
    }
    program->as.block.statements = head;
    return program;
}

bool parse_source(const char *source, Arena *arena, Stmt **out,
                  char *error, size_t error_size, int *error_line)
{
    Parser parser;

    memset(&parser, 0, sizeof(parser));
    parser.arena = arena;
    lexer_init(&parser.lexer, source);

    if (setjmp(parser.jump) == 0) {
        advance(&parser);
        *out = parse_program(&parser);
        return true;
    }

    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", parser.error);
    }
    if (error_line) {
        *error_line = parser.error_line;
    }
    return false;
}
