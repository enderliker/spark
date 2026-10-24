/* ast.h - abstract syntax tree and memory arena. */
#ifndef SPARK_AST_H
#define SPARK_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "lexer.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
    const char *chars;
    int length;
} Text;

typedef struct ExprList {
    Expr *expr;
    struct ExprList *next;
} ExprList;

typedef struct StmtList {
    Stmt *stmt;
    struct StmtList *next;
} StmtList;

typedef struct ParamList {
    Text name;
    struct ParamList *next;
} ParamList;

typedef struct MapPair {
    Expr *key;
    Expr *value;
    struct MapPair *next;
} MapPair;

typedef enum {
    EXPR_NUMBER,
    EXPR_STRING,
    EXPR_BOOL,
    EXPR_NIL,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_LOGICAL,
    EXPR_TERNARY,
    EXPR_COALESCE,
    EXPR_VARIABLE,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_ARRAY,
    EXPR_MAP,
    EXPR_FUNCTION
} ExprKind;

struct Expr {
    ExprKind kind;
    int line;
    union {
        double number;
        Text string;
        bool boolean;
        struct {
            TokenType op;
            Expr *operand;
        } unary;
        struct {
            Expr *left;
            TokenType op;
            Expr *right;
        } binary;
        struct {
            Expr *left;
            TokenType op;
            Expr *right;
        } logical;
        struct {
            Expr *condition;
            Expr *then_branch;
            Expr *else_branch;
        } ternary;
        struct {
            Expr *left;
            Expr *right;
        } coalesce;
        Text variable;
        struct {
            Expr *callee;
            ExprList *arguments;
            int argument_count;
        } call;
        struct {
            Expr *object;
            Expr *index;
        } index;
        struct {
            ExprList *items;
            int count;
        } array;
        struct {
            MapPair *pairs;
            int count;
        } map;
        struct {
            Text name;
            ParamList *params;
            int param_count;
            Stmt *body;
        } function;
    } as;
};

typedef enum {
    STMT_EXPRESSION,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_FOR_IN,
    STMT_FUNCTION,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE
} StmtKind;

struct Stmt {
    StmtKind kind;
    int line;
    union {
        Expr *expression;
        struct {
            Text name;
            Expr *initializer;
            bool is_const;
        } var;
        struct {
            Expr *target;
            TokenType op; /* TOKEN_EQUAL or a compound operator */
            Expr *value;
        } assign;
        struct {
            StmtList *statements;
        } block;
        struct {
            Expr *condition;
            Stmt *then_branch;
            Stmt *else_branch;
        } if_stmt;
        struct {
            Expr *condition;
            Stmt *body;
        } while_stmt;
        struct {
            Stmt *initializer;
            Expr *condition;
            Stmt *step;
            Stmt *body;
        } for_stmt;
        struct {
            Text name;
            Expr *iterable;
            Stmt *body;
        } for_in;
        struct {
            Text name;
            ParamList *params;
            int param_count;
            Stmt *body;
        } function;
        struct {
            Expr *value;
        } ret;
    } as;
};

/* Arena: the whole AST is freed at once when compilation finishes. */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t capacity;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

void arena_init(Arena *arena);
void *arena_alloc(Arena *arena, size_t size);
char *arena_strndup(Arena *arena, const char *chars, int length);
void arena_free(Arena *arena);

#endif
