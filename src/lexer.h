/* lexer.h - lexical analysis of the Spark language. */
#ifndef SPARK_LEXER_H
#define SPARK_LEXER_H

#include <stdbool.h>

typedef enum {
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
    TOKEN_COMMA, TOKEN_SEMICOLON, TOKEN_COLON,
    TOKEN_QUESTION, TOKEN_QUESTION_QUESTION,

    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL,
    TOKEN_BANG, TOKEN_BANG_EQUAL,
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,
    TOKEN_LESS, TOKEN_LESS_EQUAL,

    TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER,
    TOKEN_STRING_PART, TOKEN_STRING_END, TOKEN_INTERPOLATION_END,

    TOKEN_VAR, TOKEN_CONST, TOKEN_FN,
    TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR, TOKEN_IN,
    TOKEN_DO, TOKEN_SWITCH, TOKEN_CASE, TOKEN_DEFAULT,
    TOKEN_RETURN, TOKEN_BREAK, TOKEN_CONTINUE,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_NIL,
    TOKEN_AND, TOKEN_OR, TOKEN_NOT,

    TOKEN_ERROR, TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int length;
    int line;
    int column;
} Token;

typedef struct {
    const char *source;
    const char *line_start;
    const char *start;
    const char *current;
    int line;
    int interpolation_depth[64];
    int interpolation_count;
    bool resume_string;
    char error[128];
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next(Lexer *lexer);
const char *token_type_name(TokenType type);

#endif
