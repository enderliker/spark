/* ast.h - abstract syntax tree and memory arena. */
#ifndef SPARK_AST_H
#define SPARK_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lexer.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
    const char *chars;
    int length;
} Text;

typedef struct ExprList {
    Expr *expr;
    bool is_spread;
    struct ExprList *next;
} ExprList;

typedef struct StmtList {
    Stmt *stmt;
    struct StmtList *next;
} StmtList;

typedef struct ParamList {
    Text name;
    Expr *default_value; /* NULL if no default */
    bool is_varargs;
    struct ParamList *next;
} ParamList;

typedef struct MapPair {
    Expr *key;
    Expr *value;
    struct MapPair *next;
} MapPair;

typedef struct SwitchCase {
    Expr *value; /* NULL for default */
    StmtList *body;
    struct SwitchCase *next;
} SwitchCase;

typedef struct InterpPart {
    bool is_expression;
    Text text;
    Expr *expr;
    struct InterpPart *next;
} InterpPart;

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
    EXPR_INTERPOLATION,
    EXPR_FUNCTION,
    EXPR_METHOD_CALL,
    EXPR_SLICE,
    EXPR_UPDATE,
    EXPR_OPTIONAL_CHAIN
} ExprKind;

struct Expr {
    ExprKind kind;
    int line;
    int column;
    union {
        struct {
            bool is_int;
            int64_t integer;
            double number;
        } number;
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
            bool is_new;
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
            InterpPart *parts;
        } interpolation;
        struct {
            Text name;
            ParamList *params;
            int param_count;
            Stmt *body;
        } function;
        struct {
            Expr *receiver;
            Text method;
            ExprList *arguments;
            int argument_count;
        } method_call;
        struct {
            Expr *object;
            Expr *start;
            Expr *end;
            Expr *step;
        } slice;
        struct {
            Expr *target;
            TokenType op;
            bool is_prefix;
        } update;
        struct {
            Expr *object;
            Text property;
            bool is_index;
            Expr *index;
        } opt_chain;
    } as;
};

typedef enum {
    STMT_EXPRESSION,
    STMT_VAR,
    STMT_ASSIGN,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_SWITCH,
    STMT_FOR,
    STMT_FOR_IN,
    STMT_FUNCTION,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_IMPORT,
    STMT_THROW,
    STMT_TRY,
    STMT_CLASS
} StmtKind;

typedef struct ClassField {
    Text name;
    Expr *initializer;
    struct ClassField *next;
} ClassField;

typedef struct ClassMethod {
    Text name;
    ParamList *params;
    int param_count;
    Stmt *body;
    bool is_init;
    bool is_static;
    struct ClassMethod *next;
} ClassMethod;

struct Stmt {
    StmtKind kind;
    int line;
    int column;
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
            bool transparent; /* destructuring desugaring: keep the enclosing scope */
        } block;
        struct {
            Expr *condition;
            Stmt *then_branch;
            Stmt *else_branch;
        } if_stmt;
        struct {
            Text label;
            Expr *condition;
            Stmt *body;
        } while_stmt;
        struct {
            Text label;
            Stmt *body;
            Expr *condition;
        } do_while;
        struct {
            Expr *subject;
            SwitchCase *cases;
        } switch_stmt;
        struct {
            Text label;
            Stmt *initializer;
            Expr *condition;
            Stmt *step;
            Stmt *body;
        } for_stmt;
        struct {
            Text label;
            Text name;
            bool has_value_name;
            Text value_name;
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
        struct {
            Text label;
        } jump;
        struct {
            Text path;
        } import_stmt;
        struct {
            Expr *value;
        } throw_stmt;
        struct {
            Stmt *try_block;
            bool has_catch;
            Text catch_var;
            Stmt *catch_block;
            Stmt *finally_block;
        } try_stmt;
        struct {
            Text name;
            bool has_superclass;
            Text superclass;
            ClassField *fields;
            ClassMethod *methods;
        } class_stmt;
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
