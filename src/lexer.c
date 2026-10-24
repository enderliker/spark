#include "lexer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void lexer_init(Lexer *lexer, const char *source)
{
    lexer->source = source;
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->error[0] = '\0';
}

static bool is_at_end(const Lexer *lexer)
{
    return *lexer->current == '\0';
}

static char advance(Lexer *lexer)
{
    return *lexer->current++;
}

static char peek(const Lexer *lexer)
{
    return *lexer->current;
}

static char peek_next(const Lexer *lexer)
{
    if (is_at_end(lexer)) {
        return '\0';
    }
    return lexer->current[1];
}

static bool match(Lexer *lexer, char expected)
{
    if (is_at_end(lexer) || *lexer->current != expected) {
        return false;
    }
    lexer->current++;
    return true;
}

static Token make_token(const Lexer *lexer, TokenType type)
{
    Token token;

    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    return token;
}

static Token error_token(Lexer *lexer, const char *message)
{
    Token token;

    snprintf(lexer->error, sizeof(lexer->error), "%s", message);
    token.type = TOKEN_ERROR;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    return token;
}

static void skip_whitespace(Lexer *lexer)
{
    for (;;) {
        char c = peek(lexer);

        switch (c) {
        case ' ':
        case '\t':
        case '\r':
            advance(lexer);
            break;
        case '\n':
            lexer->line++;
            advance(lexer);
            break;
        case '/':
            if (peek_next(lexer) == '/') {
                while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                    advance(lexer);
                }
            } else if (peek_next(lexer) == '*') {
                advance(lexer);
                advance(lexer);
                int depth = 1;
                while (depth > 0 && !is_at_end(lexer)) {
                    if (peek(lexer) == '/' && peek_next(lexer) == '*') {
                        advance(lexer);
                        advance(lexer);
                        depth++;
                    } else if (peek(lexer) == '*' && peek_next(lexer) == '/') {
                        advance(lexer);
                        advance(lexer);
                        depth--;
                    } else {
                        if (peek(lexer) == '\n') {
                            lexer->line++;
                        }
                        advance(lexer);
                    }
                }
            } else {
                return;
            }
            break;
        default:
            return;
        }
    }
}

static bool is_identifier_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static bool is_identifier_part(char c)
{
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static TokenType identifier_type(const Lexer *lexer)
{
    const char *text = lexer->start;
    int length = (int)(lexer->current - lexer->start);

#define IS(word) (length == (int)sizeof(word) - 1 && memcmp(text, word, sizeof(word) - 1) == 0)
    switch (text[0]) {
    case 'a':
        if (IS("and")) return TOKEN_AND;
        break;
    case 'b':
        if (IS("break")) return TOKEN_BREAK;
        break;
    case 'c':
        if (IS("case")) return TOKEN_CASE;
        if (IS("const")) return TOKEN_CONST;
        if (IS("continue")) return TOKEN_CONTINUE;
        break;
    case 'd':
        if (IS("default")) return TOKEN_DEFAULT;
        if (IS("do")) return TOKEN_DO;
        break;
    case 'e':
        if (IS("else")) return TOKEN_ELSE;
        break;
    case 'f':
        if (IS("false")) return TOKEN_FALSE;
        if (IS("fn")) return TOKEN_FN;
        if (IS("for")) return TOKEN_FOR;
        break;
    case 'i':
        if (IS("if")) return TOKEN_IF;
        if (IS("in")) return TOKEN_IN;
        break;
    case 'n':
        if (IS("nil")) return TOKEN_NIL;
        if (IS("not")) return TOKEN_NOT;
        break;
    case 'o':
        if (IS("or")) return TOKEN_OR;
        break;
    case 'r':
        if (IS("return")) return TOKEN_RETURN;
        break;
    case 's':
        if (IS("switch")) return TOKEN_SWITCH;
        break;
    case 't':
        if (IS("true")) return TOKEN_TRUE;
        break;
    case 'v':
        if (IS("var")) return TOKEN_VAR;
        break;
    case 'w':
        if (IS("while")) return TOKEN_WHILE;
        break;
    default:
        break;
    }
#undef IS
    return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer *lexer)
{
    while (is_identifier_part(peek(lexer))) {
        advance(lexer);
    }
    return make_token(lexer, identifier_type(lexer));
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 99;
}

static bool consume_digits(Lexer *lexer, int base, int *digit_count)
{
    bool last_separator = *digit_count == 0;

    while (!is_at_end(lexer)) {
        char c = peek(lexer);

        if (c == '_') {
            if (last_separator) {
                return false;
            }
            last_separator = true;
            advance(lexer);
        } else if (digit_value(c) < base) {
            (*digit_count)++;
            last_separator = false;
            advance(lexer);
        } else {
            break;
        }
    }
    return !last_separator;
}

static Token base_number(Lexer *lexer, int base, const char *prefix)
{
    int digits = 0;

    advance(lexer); /* the base prefix */
    if (!consume_digits(lexer, base, &digits) || digits == 0) {
        char message[96];

        if (is_identifier_part(peek(lexer))) {
            snprintf(message, sizeof(message), "invalid digit '%c' in a base-%d literal",
                     peek(lexer), base);
        } else {
            snprintf(message, sizeof(message), "expected digits after '%s'", prefix);
        }
        return error_token(lexer, message);
    }
    if (is_identifier_part(peek(lexer))) {
        char message[96];

        snprintf(message, sizeof(message), "invalid digit '%c' in a base-%d literal",
                 peek(lexer), base);
        return error_token(lexer, message);
    }
    return make_token(lexer, TOKEN_NUMBER);
}

static Token number(Lexer *lexer)
{
    if (lexer->start[0] == '0') {
        char prefix = peek(lexer);

        if (prefix == 'x' || prefix == 'X') {
            return base_number(lexer, 16, "0x");
        }
        if (prefix == 'b' || prefix == 'B') {
            return base_number(lexer, 2, "0b");
        }
        if (prefix == 'o' || prefix == 'O') {
            return base_number(lexer, 8, "0o");
        }
    }

    int digits = 1;

    if (!consume_digits(lexer, 10, &digits)) {
        return error_token(lexer, "a digit separator must be between digits");
    }
    if (peek(lexer) == '.' && isdigit((unsigned char)peek_next(lexer))) {
        int fraction = 0;

        advance(lexer);
        consume_digits(lexer, 10, &fraction);
    }
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        const char *save = lexer->current;
        int line_save = lexer->line;

        advance(lexer);
        if (peek(lexer) == '+' || peek(lexer) == '-') {
            advance(lexer);
        }
        if (isdigit((unsigned char)peek(lexer))) {
            while (isdigit((unsigned char)peek(lexer))) {
                advance(lexer);
            }
        } else {
            lexer->current = save;
            lexer->line = line_save;
        }
    }
    return make_token(lexer, TOKEN_NUMBER);
}

static Token string_token(Lexer *lexer)
{
    while (!is_at_end(lexer) && peek(lexer) != '"') {
        if (peek(lexer) == '\n') {
            lexer->line++;
        }
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer);
        }
        advance(lexer);
    }
    if (is_at_end(lexer)) {
        return error_token(lexer, "unterminated string (missing closing quote)");
    }
    advance(lexer); /* closing quote */
    return make_token(lexer, TOKEN_STRING);
}

Token lexer_next(Lexer *lexer)
{
    skip_whitespace(lexer);
    lexer->start = lexer->current;

    if (is_at_end(lexer)) {
        return make_token(lexer, TOKEN_EOF);
    }

    char c = advance(lexer);

    if (is_identifier_start(c)) {
        return identifier(lexer);
    }
    if (isdigit((unsigned char)c)) {
        return number(lexer);
    }

    switch (c) {
    case '(': return make_token(lexer, TOKEN_LEFT_PAREN);
    case ')': return make_token(lexer, TOKEN_RIGHT_PAREN);
    case '{': return make_token(lexer, TOKEN_LEFT_BRACE);
    case '}': return make_token(lexer, TOKEN_RIGHT_BRACE);
    case '[': return make_token(lexer, TOKEN_LEFT_BRACKET);
    case ']': return make_token(lexer, TOKEN_RIGHT_BRACKET);
    case ',': return make_token(lexer, TOKEN_COMMA);
    case ';': return make_token(lexer, TOKEN_SEMICOLON);
    case ':': return make_token(lexer, TOKEN_COLON);
    case '?': return make_token(lexer, match(lexer, '?') ? TOKEN_QUESTION_QUESTION : TOKEN_QUESTION);
    case '+': return make_token(lexer, match(lexer, '=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS);
    case '-': return make_token(lexer, match(lexer, '=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS);
    case '*': return make_token(lexer, match(lexer, '=') ? TOKEN_STAR_EQUAL : TOKEN_STAR);
    case '/': return make_token(lexer, match(lexer, '=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH);
    case '%': return make_token(lexer, TOKEN_PERCENT);
    case '!': return make_token(lexer, match(lexer, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
    case '=': return make_token(lexer, match(lexer, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
    case '<': return make_token(lexer, match(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>': return make_token(lexer, match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    case '"': return string_token(lexer);
    default:
        break;
    }

    {
        char message[128];

        snprintf(message, sizeof(message), "unexpected character '%c'", c);
        return error_token(lexer, message);
    }
}

const char *token_type_name(TokenType type)
{
    switch (type) {
    case TOKEN_LEFT_PAREN: return "'('";
    case TOKEN_RIGHT_PAREN: return "')'";
    case TOKEN_LEFT_BRACE: return "'{'";
    case TOKEN_RIGHT_BRACE: return "'}'";
    case TOKEN_LEFT_BRACKET: return "'['";
    case TOKEN_RIGHT_BRACKET: return "']'";
    case TOKEN_COMMA: return "','";
    case TOKEN_SEMICOLON: return "';'";
    case TOKEN_COLON: return "':'";
    case TOKEN_QUESTION: return "'?'";
    case TOKEN_QUESTION_QUESTION: return "\?\?'";
    case TOKEN_PLUS: return "'+'";
    case TOKEN_MINUS: return "'-'";
    case TOKEN_STAR: return "'*'";
    case TOKEN_SLASH: return "'/'";
    case TOKEN_PERCENT: return "'%'";
    case TOKEN_PLUS_EQUAL: return "'+='";
    case TOKEN_MINUS_EQUAL: return "'-='";
    case TOKEN_STAR_EQUAL: return "'*='";
    case TOKEN_SLASH_EQUAL: return "'/='";
    case TOKEN_BANG: return "'!'";
    case TOKEN_BANG_EQUAL: return "'!='";
    case TOKEN_EQUAL: return "'='";
    case TOKEN_EQUAL_EQUAL: return "'=='";
    case TOKEN_GREATER: return "'>'";
    case TOKEN_GREATER_EQUAL: return "'>='";
    case TOKEN_LESS: return "'<'";
    case TOKEN_LESS_EQUAL: return "'<='";
    case TOKEN_IDENTIFIER: return "an identifier";
    case TOKEN_STRING: return "a string";
    case TOKEN_NUMBER: return "a number";
    case TOKEN_VAR: return "'var'";
    case TOKEN_CONST: return "'const'";
    case TOKEN_FN: return "'fn'";
    case TOKEN_IF: return "'if'";
    case TOKEN_ELSE: return "'else'";
    case TOKEN_WHILE: return "'while'";
    case TOKEN_DO: return "'do'";
    case TOKEN_SWITCH: return "'switch'";
    case TOKEN_CASE: return "'case'";
    case TOKEN_DEFAULT: return "'default'";
    case TOKEN_FOR: return "'for'";
    case TOKEN_IN: return "'in'";
    case TOKEN_RETURN: return "'return'";
    case TOKEN_BREAK: return "'break'";
    case TOKEN_CONTINUE: return "'continue'";
    case TOKEN_TRUE: return "'true'";
    case TOKEN_FALSE: return "'false'";
    case TOKEN_NIL: return "'nil'";
    case TOKEN_AND: return "'and'";
    case TOKEN_OR: return "'or'";
    case TOKEN_NOT: return "'not'";
    case TOKEN_ERROR: return "a lexical error";
    case TOKEN_EOF: return "the end of file";
    }
    return "a token";
}
