#include "parser.h"

#include <errno.h>
#include <limits.h>
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
    int error_column;
} Parser;

static Stmt *declaration(Parser *parser);
static Stmt *statement(Parser *parser);
static Expr *expression(Parser *parser);
static Stmt *expression_or_assignment(Parser *parser, bool with_semicolon);

static void fail_at(Parser *parser, Token token, const char *message)
{
    snprintf(parser->error, sizeof(parser->error), "%s", message);
    parser->error_line = token.line;
    parser->error_column = token.column;
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

static Expr *new_expr(Parser *parser, ExprKind kind, int line, int column)
{
    Expr *expr = new_node(parser, sizeof(Expr));

    expr->kind = kind;
    expr->line = line;
    expr->column = column;
    return expr;
}

static Stmt *new_stmt(Parser *parser, StmtKind kind, int line, int column)
{
    Stmt *stmt = new_node(parser, sizeof(Stmt));

    stmt->kind = kind;
    stmt->line = line;
    stmt->column = column;
    return stmt;
}

static void list_append_expr(Parser *parser, ExprList **head, ExprList **tail, Expr *expr, bool is_spread)
{
    ExprList *node = new_node(parser, sizeof(ExprList));

    node->expr = expr;
    node->is_spread = is_spread;
    node->next = NULL;
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

static void list_append_param(Parser *parser, ParamList **head, ParamList **tail, Text name,
                              Expr *default_value, bool is_varargs)
{
    ParamList *node = new_node(parser, sizeof(ParamList));

    node->name = name;
    node->default_value = default_value;
    node->is_varargs = is_varargs;
    node->next = NULL;
    if (*tail) {
        (*tail)->next = node;
    } else {
        *head = node;
    }
    *tail = node;
}

static void parse_params(Parser *parser, ParamList **params, ParamList **tail, int *count, const char *paren_msg)
{
    *params = NULL;
    *tail = NULL;
    *count = 0;
    bool has_default = false;

    consume(parser, TOKEN_LEFT_PAREN, paren_msg);
    if (!check(parser, TOKEN_RIGHT_PAREN)) {
        do {
            bool is_varargs = match(parser, TOKEN_DOT_DOT_DOT);
            Token param = consume(parser, TOKEN_IDENTIFIER,
                                  is_varargs ? "expected the parameter name after '...'"
                                             : "expected the parameter name");

            if (*count == 255) {
                fail(parser, "a function cannot have more than 255 parameters");
            }
            if (is_varargs) {
                if (match(parser, TOKEN_EQUAL)) {
                    fail(parser, "a varargs parameter cannot have a default value");
                }
                list_append_param(parser, params, tail, text_from_token(parser, param), NULL, true);
                (*count)++;
                match(parser, TOKEN_COMMA);
                if (!check(parser, TOKEN_RIGHT_PAREN)) {
                    fail(parser, "a varargs parameter '...' must be the last parameter");
                }
                break;
            }
            Expr *default_value = NULL;
            if (match(parser, TOKEN_EQUAL)) {
                has_default = true;
                default_value = expression(parser);
            } else if (has_default) {
                fail(parser, "a parameter without a default value cannot follow one with a default value");
            }
            list_append_param(parser, params, tail, text_from_token(parser, param), default_value, false);
            (*count)++;
        } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_PAREN));
    }
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the parameters");
}

static Text parse_string_part(Parser *parser, Token token)
{
    char *buffer = new_node(parser, (size_t)token.length + 1);
    int out = 0;

    for (int i = 0; i < token.length; i++) {
        char c = token.start[i];

        if (c == '$' && i + 1 < token.length && token.start[i + 1] == '$') {
            buffer[out++] = '$';
            i++;
            continue;
        }
        if (c != '\\') {
            buffer[out++] = c;
            continue;
        }
        i++;
        if (i >= token.length) {
            fail_at(parser, token, "incomplete escape sequence in the string");
        }
        switch (token.start[i]) {
        case 'n': buffer[out++] = '\n'; break;
        case 't': buffer[out++] = '\t'; break;
        case 'r': buffer[out++] = '\r'; break;
        case '\\': buffer[out++] = '\\'; break;
        case '"': buffer[out++] = '"'; break;
        case '0': buffer[out++] = '\0'; break;
        default: {
            char message[96];

            snprintf(message, sizeof(message), "unknown escape '\\%c'", token.start[i]);
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

static void parse_number_literal(Parser *parser, Token token, Expr *expr)
{
    char *buffer = new_node(parser, (size_t)token.length + 1);
    int length = 0;

    for (int i = 0; i < token.length; i++) {
        if (token.start[i] != '_') {
            buffer[length++] = token.start[i];
        }
    }
    buffer[length] = '\0';

    int base = 10;
    int start = 0;

    if (length >= 2 && buffer[0] == '0') {
        switch (buffer[1]) {
        case 'x':
        case 'X': base = 16; start = 2; break;
        case 'b':
        case 'B': base = 2; start = 2; break;
        case 'o':
        case 'O': base = 8; start = 2; break;
        default: break;
        }
    }

    if (base == 10) {
        bool is_float = false;

        for (int i = 0; i < length; i++) {
            if (buffer[i] == '.' || buffer[i] == 'e' || buffer[i] == 'E') {
                is_float = true;
                break;
            }
        }
        if (!is_float) {
            char *end = NULL;
            long long value;

            errno = 0;
            value = strtoll(buffer, &end, 10);
            if (errno == 0 && end == buffer + length) {
                expr->as.number.is_int = true;
                expr->as.number.integer = (int64_t)value;
                return;
            }
        }
        expr->as.number.is_int = false;
        expr->as.number.number = strtod(buffer, NULL);
        return;
    }

    unsigned long long value = 0;
    unsigned long long digit_limit = (unsigned long long)INT64_MAX;
    double fallback = 0.0;
    bool overflow = false;

    for (int i = start; i < length; i++) {
        char c = buffer[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;

        fallback = fallback * (double)base + (double)digit;
        if (value > (digit_limit - (unsigned long long)digit) / (unsigned long long)base) {
            overflow = true;
        } else {
            value = value * (unsigned long long)base + (unsigned long long)digit;
        }
    }
    if (!overflow) {
        expr->as.number.is_int = true;
        expr->as.number.integer = (int64_t)value;
        return;
    }
    expr->as.number.is_int = false;
    expr->as.number.number = fallback;
}

static InterpPart *append_interp_part(Parser *parser, InterpPart **head, InterpPart **tail)
{
    InterpPart *part = new_node(parser, sizeof(InterpPart));

    if (*tail) {
        (*tail)->next = part;
    } else {
        *head = part;
    }
    *tail = part;
    return part;
}

static Expr *parse_interpolation(Parser *parser, Token first)
{
    Expr *expr = new_expr(parser, EXPR_INTERPOLATION, first.line, first.column);
    InterpPart *head = NULL;
    InterpPart *tail = NULL;
    InterpPart *part = append_interp_part(parser, &head, &tail);

    part->text = parse_string_part(parser, first);

    for (;;) {
        part = append_interp_part(parser, &head, &tail);
        part->is_expression = true;
        part->expr = expression(parser);
        consume(parser, TOKEN_INTERPOLATION_END, "expected '}' when closing the interpolation");

        Token next = parser->current;
        if (next.type == TOKEN_STRING_END) {
            advance(parser);
            part = append_interp_part(parser, &head, &tail);
            part->text = parse_string_part(parser, next);
            break;
        }
        if (next.type != TOKEN_STRING_PART) {
            fail(parser, "expected the rest of the string");
        }
        advance(parser);
        part = append_interp_part(parser, &head, &tail);
        part->text = parse_string_part(parser, next);
    }
    expr->as.interpolation.parts = head;
    return expr;
}

static Expr *primary(Parser *parser)
{
    Token token = parser->current;

    if (match(parser, TOKEN_FALSE)) {
        Expr *expr = new_expr(parser, EXPR_BOOL, token.line, token.column);

        expr->as.boolean = false;
        return expr;
    }
    if (match(parser, TOKEN_TRUE)) {
        Expr *expr = new_expr(parser, EXPR_BOOL, token.line, token.column);

        expr->as.boolean = true;
        return expr;
    }
    if (match(parser, TOKEN_NIL)) {
        return new_expr(parser, EXPR_NIL, token.line, token.column);
    }
    if (match(parser, TOKEN_NUMBER)) {
        Expr *expr = new_expr(parser, EXPR_NUMBER, token.line, token.column);

        parse_number_literal(parser, token, expr);
        return expr;
    }
    if (match(parser, TOKEN_STRING)) {
        Expr *expr = new_expr(parser, EXPR_STRING, token.line, token.column);

        expr->as.string = parse_string_part(parser, token);
        return expr;
    }
    if (match(parser, TOKEN_STRING_PART)) {
        return parse_interpolation(parser, token);
    }
    if (match(parser, TOKEN_IDENTIFIER)) {
        Expr *expr = new_expr(parser, EXPR_VARIABLE, token.line, token.column);

        expr->as.variable = text_from_token(parser, token);
        return expr;
    }
    if (match(parser, TOKEN_LEFT_PAREN)) {
        Expr *inner = expression(parser);

        consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the expression");
        return inner;
    }
    if (match(parser, TOKEN_LEFT_BRACKET)) {
        Expr *array = new_expr(parser, EXPR_ARRAY, token.line, token.column);
        ExprList *head = NULL;
        ExprList *tail = NULL;
        int count = 0;

        if (!check(parser, TOKEN_RIGHT_BRACKET)) {
            do {
                if (count == 255) {
                    fail(parser, "an array literal cannot have more than 255 elements");
                }
                bool is_spread = match(parser, TOKEN_DOT_DOT_DOT);
                list_append_expr(parser, &head, &tail, expression(parser), is_spread);
                count++;
            } while (match(parser, TOKEN_COMMA) && !check(parser, TOKEN_RIGHT_BRACKET));
        }
        consume(parser, TOKEN_RIGHT_BRACKET, "expected ']' when closing the array");
        array->as.array.items = head;
        array->as.array.count = count;
        return array;
    }
    if (match(parser, TOKEN_LEFT_BRACE)) {
        Expr *map = new_expr(parser, EXPR_MAP, token.line, token.column);
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
        Expr *fn = new_expr(parser, EXPR_FUNCTION, token.line, token.column);
        ParamList *params = NULL;
        ParamList *params_tail = NULL;
        int param_count = 0;

        parse_params(parser, &params, &params_tail, &param_count, "expected '(' after 'fn'");

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
            bool is_spread = match(parser, TOKEN_DOT_DOT_DOT);
            list_append_expr(parser, &head, &tail, expression(parser), is_spread);
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
            Expr *call_expr = new_expr(parser, EXPR_CALL, expr->line, expr->column);
            int count = 0;
            ExprList *arguments = parse_arguments(parser, &count);

            call_expr->as.call.callee = expr;
            call_expr->as.call.arguments = arguments;
            call_expr->as.call.argument_count = count;
            expr = call_expr;
        } else if (match(parser, TOKEN_LEFT_BRACKET)) {
            Expr *index = new_expr(parser, EXPR_INDEX, expr->line, parser->previous.column);

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
    if (match(parser, TOKEN_BANG) || match(parser, TOKEN_NOT) || match(parser, TOKEN_MINUS) ||
        match(parser, TOKEN_TILDE)) {
        Token op = parser->previous;
        Expr *expr = new_expr(parser, EXPR_UNARY, op.line, op.column);

        expr->as.unary.op = op.type;
        expr->as.unary.operand = unary(parser);
        return expr;
    }
    return call(parser);
}

static Expr *factor(Parser *parser)
{
    Expr *expr = unary(parser);

    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT) ||
           match(parser, TOKEN_DIV)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

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
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = factor(parser);
        expr = binary;
    }
    return expr;
}

static Expr *shift(Parser *parser)
{
    Expr *expr = term(parser);

    while (match(parser, TOKEN_LESS_LESS) || match(parser, TOKEN_GREATER_GREATER)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = term(parser);
        expr = binary;
    }
    return expr;
}

static Expr *comparison(Parser *parser)
{
    Expr *expr = shift(parser);

    while (match(parser, TOKEN_GREATER) || match(parser, TOKEN_GREATER_EQUAL) ||
           match(parser, TOKEN_LESS) || match(parser, TOKEN_LESS_EQUAL)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = shift(parser);
        expr = binary;
    }
    return expr;
}

static Expr *equality(Parser *parser)
{
    Expr *expr = comparison(parser);

    while (match(parser, TOKEN_EQUAL_EQUAL) || match(parser, TOKEN_BANG_EQUAL)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = comparison(parser);
        expr = binary;
    }
    return expr;
}

static Expr *bitwise_and(Parser *parser)
{
    Expr *expr = equality(parser);

    while (match(parser, TOKEN_AMPERSAND)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = equality(parser);
        expr = binary;
    }
    return expr;
}

static Expr *bitwise_xor(Parser *parser)
{
    Expr *expr = bitwise_and(parser);

    while (match(parser, TOKEN_CARET)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = bitwise_and(parser);
        expr = binary;
    }
    return expr;
}

static Expr *bitwise_or(Parser *parser)
{
    Expr *expr = bitwise_xor(parser);

    while (match(parser, TOKEN_PIPE)) {
        Token op = parser->previous;
        Expr *binary = new_expr(parser, EXPR_BINARY, op.line, op.column);

        binary->as.binary.left = expr;
        binary->as.binary.op = op.type;
        binary->as.binary.right = bitwise_xor(parser);
        expr = binary;
    }
    return expr;
}

static Expr *logic_and(Parser *parser)
{
    Expr *expr = bitwise_or(parser);

    while (match(parser, TOKEN_AND)) {
        Token op = parser->previous;
        Expr *logical = new_expr(parser, EXPR_LOGICAL, op.line, op.column);

        logical->as.logical.left = expr;
        logical->as.logical.op = op.type;
        logical->as.logical.right = bitwise_or(parser);
        expr = logical;
    }
    return expr;
}

static Expr *logic_or(Parser *parser)
{
    Expr *expr = logic_and(parser);

    while (match(parser, TOKEN_OR)) {
        Token op = parser->previous;
        Expr *logical = new_expr(parser, EXPR_LOGICAL, op.line, op.column);

        logical->as.logical.left = expr;
        logical->as.logical.op = op.type;
        logical->as.logical.right = logic_and(parser);
        expr = logical;
    }
    return expr;
}

static Expr *coalesce(Parser *parser)
{
    Expr *expr = logic_or(parser);

    if (match(parser, TOKEN_QUESTION_QUESTION)) {
        Token op = parser->previous;
        Expr *coalesce_expr = new_expr(parser, EXPR_COALESCE, op.line, op.column);

        coalesce_expr->as.coalesce.left = expr;
        coalesce_expr->as.coalesce.right = coalesce(parser);
        expr = coalesce_expr;
    }
    return expr;
}

static Expr *ternary(Parser *parser)
{
    Expr *expr = coalesce(parser);

    if (match(parser, TOKEN_QUESTION)) {
        Token op = parser->previous;
        Expr *conditional = new_expr(parser, EXPR_TERNARY, op.line, op.column);

        conditional->as.ternary.condition = expr;
        conditional->as.ternary.then_branch = expression(parser);
        consume(parser, TOKEN_COLON, "expected ':' in the conditional expression");
        conditional->as.ternary.else_branch = ternary(parser);
        expr = conditional;
    }
    return expr;
}

static Expr *expression(Parser *parser)
{
    return ternary(parser);
}

static Stmt *expression_or_assignment(Parser *parser, bool with_semicolon)
{
    int line = parser->current.line;
    int column = parser->current.column;
    Expr *expr = expression(parser);

    if (match(parser, TOKEN_EQUAL) || match(parser, TOKEN_PLUS_EQUAL) ||
        match(parser, TOKEN_MINUS_EQUAL) || match(parser, TOKEN_STAR_EQUAL) ||
        match(parser, TOKEN_SLASH_EQUAL) || match(parser, TOKEN_AMPERSAND_EQUAL) ||
        match(parser, TOKEN_PIPE_EQUAL) || match(parser, TOKEN_CARET_EQUAL) ||
        match(parser, TOKEN_LESS_LESS_EQUAL) || match(parser, TOKEN_GREATER_GREATER_EQUAL)) {
        Token op = parser->previous;
        Stmt *stmt = new_stmt(parser, STMT_ASSIGN, expr->line, expr->column);

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

    Stmt *stmt = new_stmt(parser, STMT_EXPRESSION, line, column);

    stmt->as.expression = expr;
    if (with_semicolon) {
        consume(parser, TOKEN_SEMICOLON, "expected ';' after the expression");
    }
    return stmt;
}

static Stmt *block(Parser *parser)
{
    Stmt *stmt = new_stmt(parser, STMT_BLOCK, parser->previous.line, parser->previous.column);
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
    Stmt *stmt = new_stmt(parser, STMT_VAR, name.line, name.column);

    stmt->as.var.name = text_from_token(parser, name);
    stmt->as.var.initializer = NULL;
    stmt->as.var.is_const = false;
    if (match(parser, TOKEN_EQUAL)) {
        stmt->as.var.initializer = expression(parser);
    }
    if (with_semicolon) {
        consume(parser, TOKEN_SEMICOLON, "expected ';' after the declaration");
    }
    return stmt;
}

static Stmt *parse_const_declaration(Parser *parser)
{
    Token name = consume(parser, TOKEN_IDENTIFIER, "expected the constant name");
    Stmt *stmt = new_stmt(parser, STMT_VAR, name.line, name.column);

    stmt->as.var.name = text_from_token(parser, name);
    stmt->as.var.initializer = NULL;
    stmt->as.var.is_const = true;
    if (!match(parser, TOKEN_EQUAL)) {
        fail(parser, "a constant must have an initializer");
    }
    stmt->as.var.initializer = expression(parser);
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the declaration");
    return stmt;
}

static Stmt *parse_var_declaration(Parser *parser, bool with_semicolon)
{
    Token name = consume(parser, TOKEN_IDENTIFIER, "expected the variable name");

    return var_declaration_after_name(parser, name, with_semicolon);
}

static Stmt *function_declaration(Parser *parser)
{
    Token name = consume(parser, TOKEN_IDENTIFIER, "expected the function name");
    Stmt *stmt = new_stmt(parser, STMT_FUNCTION, name.line, name.column);
    ParamList *params;
    ParamList *tail;
    int count;

    parse_params(parser, &params, &tail, &count, "expected '(' after the function name");
    stmt->as.function.name = text_from_token(parser, name);
    stmt->as.function.params = params;
    stmt->as.function.param_count = count;
    stmt->as.function.body = statement(parser);
    if (stmt->as.function.body->kind != STMT_BLOCK) {
        fail_at(parser, name, "the body of a function must be enclosed in braces");
    }
    return stmt;
}

static Stmt *if_statement(Parser *parser, int line, int column)
{
    Stmt *stmt = new_stmt(parser, STMT_IF, line, column);

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'if'");
    stmt->as.if_stmt.condition = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
    stmt->as.if_stmt.then_branch = statement(parser);
    stmt->as.if_stmt.else_branch = match(parser, TOKEN_ELSE) ? statement(parser) : NULL;
    return stmt;
}

static Stmt *while_statement(Parser *parser, int line, int column)
{
    Stmt *stmt = new_stmt(parser, STMT_WHILE, line, column);

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'while'");
    stmt->as.while_stmt.condition = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
    stmt->as.while_stmt.body = statement(parser);
    return stmt;
}

static Stmt *do_while_statement(Parser *parser, int line, int column)
{
    Stmt *stmt = new_stmt(parser, STMT_DO_WHILE, line, column);

    stmt->as.do_while.body = statement(parser);
    consume(parser, TOKEN_WHILE, "expected 'while' after the body of 'do'");
    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'while'");
    stmt->as.do_while.condition = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the condition");
    consume(parser, TOKEN_SEMICOLON, "expected ';' after the condition");
    return stmt;
}

static Stmt *switch_statement(Parser *parser, int line, int column)
{
    Stmt *stmt = new_stmt(parser, STMT_SWITCH, line, column);
    SwitchCase *head = NULL;
    SwitchCase *tail = NULL;
    bool has_default = false;

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'switch'");
    stmt->as.switch_stmt.subject = expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "expected ')' after the switch value");
    consume(parser, TOKEN_LEFT_BRACE, "expected '{' after the switch value");

    while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
        SwitchCase *case_node = new_node(parser, sizeof(SwitchCase));
        StmtList *body_tail = NULL;

        if (match(parser, TOKEN_CASE)) {
            case_node->value = expression(parser);
            consume(parser, TOKEN_COLON, "expected ':' after the case value");
        } else if (match(parser, TOKEN_DEFAULT)) {
            if (has_default) {
                fail(parser, "a switch can only have one 'default'");
            }
            has_default = true;
            case_node->value = NULL;
            consume(parser, TOKEN_COLON, "expected ':' after 'default'");
        } else {
            fail(parser, "expected 'case' or 'default' in the switch");
        }

        while (!check(parser, TOKEN_CASE) && !check(parser, TOKEN_DEFAULT) &&
               !check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
            list_append_stmt(parser, &case_node->body, &body_tail, declaration(parser));
        }
        if (tail) {
            tail->next = case_node;
        } else {
            head = case_node;
        }
        tail = case_node;
    }

    consume(parser, TOKEN_RIGHT_BRACE, "expected '}' when closing the switch");
    stmt->as.switch_stmt.cases = head;
    return stmt;
}

static Stmt *for_statement(Parser *parser, int line, int column)
{
    Stmt *stmt = new_stmt(parser, STMT_FOR, line, column);

    consume(parser, TOKEN_LEFT_PAREN, "expected '(' after 'for'");

    if (match(parser, TOKEN_VAR)) {
        Token name = consume(parser, TOKEN_IDENTIFIER, "expected the variable name");

        if (match(parser, TOKEN_IN)) {
            Stmt *for_in = new_stmt(parser, STMT_FOR_IN, line, column);

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

static bool next_is_colon(Parser *parser)
{
    Lexer lookahead;

    if (parser->current.type != TOKEN_IDENTIFIER) {
        return false;
    }
    lookahead = parser->lexer;
    return lexer_next(&lookahead).type == TOKEN_COLON;
}

static Stmt *labelled_loop(Parser *parser, Token label)
{
    int line = parser->current.line;
    int column = parser->current.column;
    Text name = text_from_token(parser, label);

    if (match(parser, TOKEN_WHILE)) {
        Stmt *stmt = while_statement(parser, line, column);

        stmt->as.while_stmt.label = name;
        return stmt;
    }
    if (match(parser, TOKEN_DO)) {
        Stmt *stmt = do_while_statement(parser, line, column);

        stmt->as.do_while.label = name;
        return stmt;
    }
    if (match(parser, TOKEN_FOR)) {
        Stmt *stmt = for_statement(parser, line, column);

        if (stmt->kind == STMT_FOR_IN) {
            stmt->as.for_in.label = name;
        } else {
            stmt->as.for_stmt.label = name;
        }
        return stmt;
    }
    fail(parser, "a label must be followed by a loop");
    return NULL;
}

static Stmt *statement(Parser *parser)
{
    int line = parser->current.line;
    int column = parser->current.column;

    if (next_is_colon(parser)) {
        Token label = parser->current;

        advance(parser);
        consume(parser, TOKEN_COLON, "expected ':' after the label");
        return labelled_loop(parser, label);
    }
    if (match(parser, TOKEN_IF)) {
        return if_statement(parser, line, column);
    }
    if (match(parser, TOKEN_WHILE)) {
        return while_statement(parser, line, column);
    }
    if (match(parser, TOKEN_DO)) {
        return do_while_statement(parser, line, column);
    }
    if (match(parser, TOKEN_SWITCH)) {
        return switch_statement(parser, line, column);
    }
    if (match(parser, TOKEN_FOR)) {
        return for_statement(parser, line, column);
    }
    if (match(parser, TOKEN_RETURN)) {
        Stmt *stmt = new_stmt(parser, STMT_RETURN, line, column);

        stmt->as.ret.value = check(parser, TOKEN_SEMICOLON) ? NULL : expression(parser);
        consume(parser, TOKEN_SEMICOLON, "expected ';' after 'return'");
        return stmt;
    }
    if (match(parser, TOKEN_BREAK)) {
        Stmt *stmt = new_stmt(parser, STMT_BREAK, line, column);

        if (match(parser, TOKEN_IDENTIFIER)) {
            stmt->as.jump.label = text_from_token(parser, parser->previous);
        }
        consume(parser, TOKEN_SEMICOLON, "expected ';' after 'break'");
        return stmt;
    }
    if (match(parser, TOKEN_CONTINUE)) {
        Stmt *stmt = new_stmt(parser, STMT_CONTINUE, line, column);

        if (match(parser, TOKEN_IDENTIFIER)) {
            stmt->as.jump.label = text_from_token(parser, parser->previous);
        }
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
    if (match(parser, TOKEN_CONST)) {
        return parse_const_declaration(parser);
    }
    if (match(parser, TOKEN_FN)) {
        return function_declaration(parser);
    }
    return statement(parser);
}

static Stmt *parse_program(Parser *parser)
{
    Stmt *program = new_stmt(parser, STMT_BLOCK, 1, 1);
    StmtList *head = NULL;
    StmtList *tail = NULL;

    while (!check(parser, TOKEN_EOF)) {
        list_append_stmt(parser, &head, &tail, declaration(parser));
    }
    program->as.block.statements = head;
    return program;
}

bool parse_source(const char *source, Arena *arena, Stmt **out,
                  char *error, size_t error_size, int *error_line, int *error_column)
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
    if (error_column) {
        *error_column = parser.error_column;
    }
    return false;
}
