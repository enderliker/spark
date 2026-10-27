/* formatter.c - canonical code formatter for Spark. */
#include "formatter.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "buf.h"
#include "parser.h"

typedef enum {
    TOK_WORD,           /* identifier or keyword */
    TOK_NUMBER,         /* numeric literal */
    TOK_STRING,         /* string literal */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_SEMICOLON,      /* ; */
    TOK_COMMA,          /* , */
    TOK_COLON,          /* : */
    TOK_DOT,            /* . */
    TOK_DOT_DOT_DOT,    /* ... */
    TOK_OP_BIN,         /* == != <= >= < > + - * / % div & | ^ << >> and or ?? */
    TOK_OP_ASSIGN,      /* = += -= *= /= %= &= |= ^= <<= >>= */
    TOK_OP_UNARY,       /* ! ~ */
    TOK_PLUS_MINUS,     /* + or - (ambiguous between binary and unary) */
    TOK_QUESTION,       /* ? */
    TOK_COMMENT_LINE,   /* // line comment */
    TOK_COMMENT_BLOCK,  /* block comment */
    TOK_EOF
} FmtTokType;

typedef struct {
    FmtTokType type;
    const char *text;
    int length;
    int orig_line;
    int leading_newlines;
} FmtToken;

typedef struct {
    const char *source;
    const char *cur;
    int line;
} FmtScanner;

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static bool is_ident_part(char c)
{
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static FmtToken scan_token(FmtScanner *s)
{
    FmtToken tok;
    memset(&tok, 0, sizeof(tok));
    int newlines = 0;

    for (;;) {
        char c = *s->cur;
        if (c == '\0') {
            tok.type = TOK_EOF;
            tok.text = s->cur;
            tok.length = 0;
            tok.orig_line = s->line;
            tok.leading_newlines = newlines;
            return tok;
        }
        if (c == '\n') {
            newlines++;
            s->line++;
            s->cur++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            s->cur++;
            continue;
        }
        break;
    }

    tok.orig_line = s->line;
    tok.leading_newlines = newlines;
    const char *start = s->cur;

    /* Line comment */
    if (start[0] == '/' && start[1] == '/') {
        s->cur += 2;
        while (*s->cur && *s->cur != '\n') {
            s->cur++;
        }
        tok.type = TOK_COMMENT_LINE;
        tok.text = start;
        tok.length = (int)(s->cur - start);
        return tok;
    }

    /* Block comment */
    if (start[0] == '/' && start[1] == '*') {
        s->cur += 2;
        int depth = 1;
        while (depth > 0 && *s->cur) {
            if (s->cur[0] == '/' && s->cur[1] == '*') {
                depth++;
                s->cur += 2;
            } else if (s->cur[0] == '*' && s->cur[1] == '/') {
                depth--;
                s->cur += 2;
            } else {
                if (*s->cur == '\n') {
                    s->line++;
                }
                s->cur++;
            }
        }
        tok.type = TOK_COMMENT_BLOCK;
        tok.text = start;
        tok.length = (int)(s->cur - start);
        return tok;
    }

    /* Strings (handle escapes and interpolation ${...}) */
    if (*start == '"') {
        s->cur++;
        int brace_depth = 0;
        while (*s->cur) {
            if (*s->cur == '\\' && s->cur[1] != '\0') {
                s->cur += 2;
                continue;
            }
            if (s->cur[0] == '$' && s->cur[1] == '{') {
                brace_depth++;
                s->cur += 2;
                continue;
            }
            if (brace_depth > 0 && *s->cur == '}') {
                brace_depth--;
                s->cur++;
                continue;
            }
            if (*s->cur == '"' && brace_depth == 0) {
                s->cur++;
                break;
            }
            if (*s->cur == '\n') {
                s->line++;
            }
            s->cur++;
        }
        tok.type = TOK_STRING;
        tok.text = start;
        tok.length = (int)(s->cur - start);
        return tok;
    }

    /* Identifiers and keywords */
    if (is_ident_start(*start)) {
        s->cur++;
        while (is_ident_part(*s->cur)) {
            s->cur++;
        }
        tok.text = start;
        tok.length = (int)(s->cur - start);
        if (tok.length == 3 && strncmp(start, "div", 3) == 0) {
            tok.type = TOK_OP_BIN;
        } else if (tok.length == 3 && strncmp(start, "and", 3) == 0) {
            tok.type = TOK_OP_BIN;
        } else if (tok.length == 2 && strncmp(start, "or", 2) == 0) {
            tok.type = TOK_OP_BIN;
        } else if (tok.length == 3 && strncmp(start, "not", 3) == 0) {
            tok.type = TOK_OP_UNARY;
        } else {
            tok.type = TOK_WORD;
        }
        return tok;
    }

    /* Numbers (including 0x, 0b, 0o, exponents, _ separators) */
    if (isdigit((unsigned char)*start) || (*start == '.' && isdigit((unsigned char)start[1]))) {
        bool prefixed = start[0] == '0' && (start[1] == 'x' || start[1] == 'X' ||
                                            start[1] == 'b' || start[1] == 'B' ||
                                            start[1] == 'o' || start[1] == 'O');
        s->cur++;
        while (isalnum((unsigned char)*s->cur) || *s->cur == '_' || *s->cur == '.') {
            if (*s->cur == '.' && s->cur[1] == '.') {
                break; /* part of .. */
            }
            if (!prefixed && (*s->cur == 'e' || *s->cur == 'E') &&
                (s->cur[1] == '+' || s->cur[1] == '-') && isdigit((unsigned char)s->cur[2])) {
                s->cur += 2; /* exponent sign belongs to the literal */
                continue;
            }
            s->cur++;
        }
        tok.type = TOK_NUMBER;
        tok.text = start;
        tok.length = (int)(s->cur - start);
        return tok;
    }

    /* Single / multi-character symbols */
    s->cur++;
    char c = *start;
    char next = *s->cur;

    if (c == '(') { tok.type = TOK_LPAREN; }
    else if (c == ')') { tok.type = TOK_RPAREN; }
    else if (c == '{') { tok.type = TOK_LBRACE; }
    else if (c == '}') { tok.type = TOK_RBRACE; }
    else if (c == '[') { tok.type = TOK_LBRACKET; }
    else if (c == ']') { tok.type = TOK_RBRACKET; }
    else if (c == ';') { tok.type = TOK_SEMICOLON; }
    else if (c == ',') { tok.type = TOK_COMMA; }
    else if (c == ':') { tok.type = TOK_COLON; }
    else if (c == '~') { tok.type = TOK_OP_UNARY; }
    else if (c == '?') {
        if (next == '?') {
            s->cur++;
            tok.type = TOK_OP_BIN; /* ?? */
        } else {
            tok.type = TOK_QUESTION;
        }
    }
    else if (c == '.') {
        if (next == '.' && s->cur[1] == '.') {
            s->cur += 2;
            tok.type = TOK_DOT_DOT_DOT;
        } else {
            tok.type = TOK_DOT;
        }
    }
    else if (c == '+' || c == '-') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_ASSIGN;
        } else {
            tok.type = TOK_PLUS_MINUS;
        }
    }
    else if (c == '*' || c == '/' || c == '%') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_ASSIGN;
        } else {
            tok.type = TOK_OP_BIN;
        }
    }
    else if (c == '=') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_BIN;
        } else {
            tok.type = TOK_OP_ASSIGN;
        }
    }
    else if (c == '!') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_BIN;
        } else {
            tok.type = TOK_OP_UNARY;
        }
    }
    else if (c == '<') {
        if (next == '<') {
            s->cur++;
            if (*s->cur == '=') {
                s->cur++;
                tok.type = TOK_OP_ASSIGN;
            } else {
                tok.type = TOK_OP_BIN;
            }
        } else if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_BIN;
        } else {
            tok.type = TOK_OP_BIN;
        }
    }
    else if (c == '>') {
        if (next == '>') {
            s->cur++;
            if (*s->cur == '=') {
                s->cur++;
                tok.type = TOK_OP_ASSIGN;
            } else {
                tok.type = TOK_OP_BIN;
            }
        } else if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_BIN;
        } else {
            tok.type = TOK_OP_BIN;
        }
    }
    else if (c == '&') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_ASSIGN;
        } else {
            tok.type = TOK_OP_BIN;
        }
    }
    else if (c == '|') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_ASSIGN;
        } else {
            tok.type = TOK_OP_BIN;
        }
    }
    else if (c == '^') {
        if (next == '=') {
            s->cur++;
            tok.type = TOK_OP_ASSIGN;
        } else {
            tok.type = TOK_OP_BIN;
        }
    }
    else {
        tok.type = TOK_WORD;
    }

    tok.text = start;
    tok.length = (int)(s->cur - start);
    return tok;
}

static void buf_char(buf_t *b, char c)
{
    buf_append(b, &c, 1);
}

static void emit_indent(buf_t *b, int indent)
{
    for (int i = 0; i < indent * 4; i++) {
        buf_char(b, ' ');
    }
}

static bool is_control_keyword(const FmtToken *tok)
{
    if (tok->type != TOK_WORD) return false;
    const char *kws[] = {"if", "while", "for", "switch", "catch"};
    for (size_t i = 0; i < sizeof(kws)/sizeof(kws[0]); i++) {
        if (tok->length == (int)strlen(kws[i]) && memcmp(tok->text, kws[i], tok->length) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_keyword_not_expr(const FmtToken *tok)
{
    if (tok->type != TOK_WORD) return false;
    const char *kws[] = {
        "return", "throw", "case", "in", "new", "var", "const", "fn",
        "import", "class", "else", "if", "while", "for", "switch",
        "catch", "finally", "do", "try", "default", "break", "continue"
    };
    for (size_t i = 0; i < sizeof(kws)/sizeof(kws[0]); i++) {
        if (tok->length == (int)strlen(kws[i]) && memcmp(tok->text, kws[i], tok->length) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_binary_left_operand(const FmtToken *tok)
{
    if (!tok) return false;
    if (tok->type == TOK_NUMBER || tok->type == TOK_STRING ||
        tok->type == TOK_RPAREN || tok->type == TOK_RBRACKET) {
        return true;
    }
    if (tok->type == TOK_WORD && !is_keyword_not_expr(tok)) {
        return true;
    }
    return false;
}

static bool token_str_eq(const FmtToken *tok, const char *str)
{
    return tok->length == (int)strlen(str) && memcmp(tok->text, str, tok->length) == 0;
}

static bool brace_contains_semicolon(const FmtToken *tokens, int count, int lbrace_idx)
{
    int depth = 1;
    for (int j = lbrace_idx + 1; j < count - 1; j++) {
        if (tokens[j].type == TOK_LBRACE) {
            depth++;
        } else if (tokens[j].type == TOK_RBRACE) {
            depth--;
            if (depth == 0) return false;
        } else if (tokens[j].type == TOK_SEMICOLON && depth == 1) {
            return true;
        }
    }
    return false;
}

enum FmtCtx {
    CTX_TERNARY,
    CTX_BRACE
};

char *formatter_format(const char *source, const char *path, char *error_buf, size_t error_buf_size)
{
    if (!source) {
        snprintf(error_buf, error_buf_size, "null source");
        return NULL;
    }

    /* 1. Syntax check first */
    Arena arena;
    Stmt *program = NULL;
    int err_line = 0, err_col = 0;
    arena_init(&arena);
    if (!parse_source(source, &arena, &program, error_buf, error_buf_size, &err_line, &err_col)) {
        arena_free(&arena);
        return NULL;
    }
    arena_free(&arena);

    /* 2. Collect all tokens */
    FmtScanner scanner;
    scanner.source = source;
    scanner.cur = source;
    scanner.line = 1;

    FmtToken *tokens = NULL;
    int count = 0;
    int capacity = 0;

    for (;;) {
        FmtToken tok = scan_token(&scanner);
        if (count == capacity) {
            capacity = capacity < 64 ? 64 : capacity * 2;
            tokens = realloc(tokens, (size_t)capacity * sizeof(FmtToken));
        }
        tokens[count++] = tok;
        if (tok.type == TOK_EOF) {
            break;
        }
    }

    /* 3. Emit formatted source */
    buf_t out;
    buf_init(&out);

    int indent = 0;
    bool at_line_start = true;
    int for_paren_depth = -1;
    int paren_depth = 0;
    int ctx_stack[256];
    int ctx_depth = 0;

    for (int i = 0; i < count - 1; i++) {
        FmtToken *tok = &tokens[i];
        FmtToken *prev = (i > 0) ? &tokens[i - 1] : NULL;

        /* Closing brace decreases indent before printing */
        if (tok->type == TOK_RBRACE) {
            if (indent > 0) {
                indent--;
            }
        }

        /* Check if we need to start a new line */
        if (i > 0) {
            bool want_newline = false;
            int blank_lines = (tok->leading_newlines >= 2) ? 1 : 0;

            if (tok->leading_newlines > 0) {
                want_newline = true;
            } else if (tok->type == TOK_COMMENT_LINE && prev && prev->type != TOK_COMMENT_LINE) {
                /* keep a trailing comment on the line it was written on */
                want_newline = false;
            } else if (prev && (prev->type == TOK_SEMICOLON && for_paren_depth == -1)) {
                want_newline = true;
            } else if (prev && prev->type == TOK_LBRACE) {
                if (tok->leading_newlines > 0 || brace_contains_semicolon(tokens, count, i - 1)) {
                    want_newline = true;
                }
            } else if (prev && prev->type == TOK_RBRACE) {
                /* If followed by else, catch, finally, while: stay on same line */
                if (tok->type == TOK_WORD && (token_str_eq(tok, "else") ||
                                              token_str_eq(tok, "catch") ||
                                              token_str_eq(tok, "finally") ||
                                              token_str_eq(tok, "while"))) {
                    want_newline = false;
                } else if (tok->type == TOK_SEMICOLON ||
                           tok->type == TOK_RPAREN ||
                           tok->type == TOK_COMMA ||
                           tok->type == TOK_RBRACKET ||
                           tok->type == TOK_DOT) {
                    want_newline = false;
                } else {
                    want_newline = true;
                }
            } else if (prev && prev->type == TOK_COMMENT_LINE) {
                want_newline = true;
            }

            if (want_newline && !at_line_start) {
                buf_char(&out, '\n');
                if (blank_lines > 0) {
                    buf_char(&out, '\n');
                }
                at_line_start = true;
            }
        }

        /* If at start of line, emit indentation */
        if (at_line_start) {
            if (tok->type != TOK_COMMENT_LINE || tok->orig_line > 0) {
                emit_indent(&out, indent);
            }
            at_line_start = false;
        } else {
            /* Spacing rules on the same line between prev and tok */
            if (prev) {
                bool need_space = false;

                if (tok->type == TOK_COMMENT_LINE) {
                    need_space = true;
                } else if ((prev->type == TOK_WORD || prev->type == TOK_NUMBER) &&
                           (tok->type == TOK_WORD || tok->type == TOK_NUMBER)) {
                    need_space = true;
                } else if (prev->type == TOK_OP_UNARY && token_str_eq(prev, "not")) {
                    need_space = true;
                } else if (prev->type == TOK_RBRACE &&
                           (token_str_eq(tok, "else") || token_str_eq(tok, "catch") ||
                            token_str_eq(tok, "finally") || token_str_eq(tok, "while"))) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && is_control_keyword(prev) && tok->type == TOK_LPAREN) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "case")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "return") && tok->type != TOK_SEMICOLON) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "throw")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "var")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "const")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "fn")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "class")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "new")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "import")) {
                    need_space = true;
                } else if (prev->type == TOK_WORD && token_str_eq(prev, "in")) {
                    need_space = true;
                } else if (tok->type == TOK_WORD && token_str_eq(tok, "in")) {
                    need_space = true;
                } else if (tok->type == TOK_LBRACE &&
                           prev->type != TOK_LPAREN && prev->type != TOK_LBRACKET) {
                    need_space = true;
                } else if (prev->type == TOK_COMMA) {
                    need_space = true;
                } else if (prev->type == TOK_COLON && tok->type != TOK_COLON) {
                    need_space = true;
                } else if (prev->type == TOK_SEMICOLON && for_paren_depth >= 0) {
                    need_space = true;
                } else if (tok->type == TOK_QUESTION || prev->type == TOK_QUESTION) {
                    need_space = true;
                } else if (tok->type == TOK_COLON && ctx_depth > 0 && ctx_stack[ctx_depth - 1] == CTX_TERNARY) {
                    /* ternary colon */
                    need_space = true;
                } else if (tok->type == TOK_OP_ASSIGN || prev->type == TOK_OP_ASSIGN) {
                    need_space = true;
                } else if (tok->type == TOK_OP_BIN || prev->type == TOK_OP_BIN) {
                    need_space = true;
                } else if (tok->type == TOK_PLUS_MINUS) {
                    /* Disambiguate +/- : check if binary */
                    if (is_binary_left_operand(prev)) {
                        need_space = true;
                    }
                } else if (prev->type == TOK_PLUS_MINUS) {
                    if (i >= 2 && is_binary_left_operand(&tokens[i - 2])) {
                        need_space = true;
                    }
                }

                if (need_space) {
                    buf_char(&out, ' ');
                }
            }
        }

        /* Emit token text */
        buf_append(&out, tok->text, (size_t)tok->length);

        /* Update state */
        if (tok->type == TOK_LPAREN) {
            paren_depth++;
            if (prev && token_str_eq(prev, "for")) {
                for_paren_depth = paren_depth;
            }
        } else if (tok->type == TOK_RPAREN) {
            if (paren_depth == for_paren_depth) {
                for_paren_depth = -1;
            }
            paren_depth--;
        } else if (tok->type == TOK_LBRACE) {
            indent++;
            if (ctx_depth < 256) {
                ctx_stack[ctx_depth++] = CTX_BRACE;
            }
        } else if (tok->type == TOK_RBRACE) {
            if (ctx_depth > 0 && ctx_stack[ctx_depth - 1] == CTX_BRACE) {
                ctx_depth--;
            }
        } else if (tok->type == TOK_QUESTION) {
            if (ctx_depth < 256) {
                ctx_stack[ctx_depth++] = CTX_TERNARY;
            }
        } else if (tok->type == TOK_COLON) {
            if (ctx_depth > 0 && ctx_stack[ctx_depth - 1] == CTX_TERNARY) {
                ctx_depth--;
            }
        }
    }

    if (!at_line_start) {
        buf_char(&out, '\n');
    }

    free(tokens);
    (void)path;
    return buf_steal(&out);
}
