/* regex.c - Thompson NFA / Pike VM regular expression engine for Spark.
 *
 * The pattern is parsed into a small AST, lowered to a Thompson NFA and run by
 * simulating all threads at once (a Pike VM), so matching time is proportional
 * to the input rather than to a backtracking search. Threads are kept in
 * priority order, which gives Perl/JavaScript leftmost-first semantics
 * (decision Q27): the first alternative that matches wins, greedy quantifiers
 * prefer more input and lazy quantifiers prefer less. Alternatives and
 * quantifiers therefore compile to RE_SPLIT with the preferred branch first.
 *
 * Capture groups ride on the thread state: RE_SAVE records the start or end
 * byte offset of a group, and each thread carries its own capture row, so an
 * optional group that did not participate leaves its slots at -1. The number of
 * groups is capped and the work per operation is bounded by a step budget; both
 * keep a crafted pattern from exhausting memory or time.
 *
 * All offsets exposed to Spark are character offsets. The engine itself works
 * in bytes (classes are byte sets), so results are converted with the same
 * UTF-8 rule `len`/`index_of` use. Case-insensitive matching folds ASCII only,
 * via the C locale's tolower/toupper. */
#include "regex.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "memory.h"
#include "object.h"

#define MAX_REGEX_INST 1024
#define MAX_THREADS 1024
#define MAX_NODES 1024
#define MAX_GROUPS 32
#define MAX_GROUP_NAME 32
#define MAX_REPEAT 10000
#define CAPTURE_SLOTS (2 * (MAX_GROUPS + 1))
#define MAX_STEPS 20000000

typedef enum {
    RE_CHAR,
    RE_ANY,
    RE_CLASS,
    RE_BOL,
    RE_EOL,
    RE_JUMP,
    RE_SPLIT,
    RE_SAVE,
    RE_MATCH
} ReOpcode;

typedef struct {
    uint8_t bits[32];
} CharClass;

static inline void class_set(CharClass *cc, unsigned char c)
{
    cc->bits[c >> 3] |= (uint8_t)(1 << (c & 7));
}

static inline bool class_test(const CharClass *cc, unsigned char c)
{
    return (cc->bits[c >> 3] & (1 << (c & 7))) != 0;
}

typedef struct {
    ReOpcode op;
    char c;
    CharClass cls;
    int target1;
    int target2;
    int slot;
    bool ci;
    bool dotall;
    bool multiline;
} ReInst;

typedef struct {
    ReInst code[MAX_REGEX_INST];
    int count;
    bool anchored;
    int group_count;
    char names[MAX_GROUPS + 1][MAX_GROUP_NAME];
} ReProgram;

static void regex_set_error(char *error, size_t error_size, const char *fmt, ...)
{
    va_list ap;

    if (!error || error_size == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* AST Representation                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    AST_EMPTY,
    AST_CHAR,
    AST_ANY,
    AST_CLASS,
    AST_BOL,
    AST_EOL,
    AST_CAT,
    AST_ALT,
    AST_REPEAT,
    AST_GROUP
} AstType;

typedef struct AstNode AstNode;
struct AstNode {
    AstType type;
    char c;
    CharClass cls;
    AstNode *left;
    AstNode *right;
    int min;
    int max; /* -1 means unbounded */
    bool lazy;
    int group; /* AST_GROUP: capture index, 0 for a non-capturing group */
    bool ci;
    bool dotall;
    bool multiline;
};

typedef struct {
    const char *pattern;
    int index;
    int length;
    AstNode arena[MAX_NODES];
    int node_count;
    ReProgram prog;
    char *error;
    size_t error_size;
    RegexFlags flags;
    int group_count;
    char names[MAX_GROUPS + 1][MAX_GROUP_NAME];
} ReCompiler;

static void re_error(ReCompiler *c, const char *msg)
{
    if (c->error && c->error_size > 0 && c->error[0] == '\0') {
        regex_set_error(c->error, c->error_size, "regex syntax error: %s", msg);
    }
}

static AstNode *alloc_node(ReCompiler *c, AstType type)
{
    AstNode *n;

    if (c->node_count >= MAX_NODES) {
        re_error(c, "pattern too complex");
        return NULL;
    }
    n = &c->arena[c->node_count++];
    memset(n, 0, sizeof(AstNode));
    n->type = type;
    n->max = -1;
    return n;
}

static char re_peek(ReCompiler *c)
{
    if (c->index >= c->length) return '\0';
    return c->pattern[c->index];
}

static char re_advance(ReCompiler *c)
{
    if (c->index >= c->length) return '\0';
    return c->pattern[c->index++];
}

static char re_unescape(char esc)
{
    if (esc == 'n') return '\n';
    if (esc == 't') return '\t';
    if (esc == 'r') return '\r';
    return esc;
}

static void add_shorthand(CharClass *cls, char type)
{
    for (int i = 0; i < 256; i++) {
        unsigned char ch = (unsigned char)i;
        bool match = false;
        switch (type) {
        case 'd': match = isdigit(ch); break;
        case 'D': match = !isdigit(ch); break;
        case 'w': match = isalnum(ch) || ch == '_'; break;
        case 'W': match = !(isalnum(ch) || ch == '_'); break;
        case 's': match = isspace(ch); break;
        case 'S': match = !isspace(ch); break;
        default: break;
        }
        if (match) class_set(cls, ch);
    }
}

/* Forward declarations */
static AstNode *parse_expr(ReCompiler *c);

/* `(?i)`, `(?is)` and `(?-i)` change the flags for the rest of the enclosing
 * group. The caller has consumed the "?" and a ':' / 'P' was not found. */
static bool parse_inline_flags(ReCompiler *c)
{
    bool clearing = false;
    bool any = false;

    while (c->index < c->length) {
        char ch = re_peek(c);

        if (ch == ')') {
            re_advance(c);
            if (!any) {
                re_error(c, "empty flag group");
                return false;
            }
            return true;
        }
        if (ch == '-') {
            if (clearing) {
                re_error(c, "repeated '-' in flag group");
                return false;
            }
            clearing = true;
            re_advance(c);
            continue;
        }
        if (ch == 'i') {
            c->flags.case_insensitive = !clearing;
        } else if (ch == 's') {
            c->flags.dotall = !clearing;
        } else if (ch == 'm') {
            c->flags.multiline = !clearing;
        } else {
            char message[64];

            snprintf(message, sizeof(message), "unknown flag '%c'", ch);
            re_error(c, message);
            return false;
        }
        any = true;
        re_advance(c);
    }
    re_error(c, "unterminated flag group");
    return false;
}

/* Parses a capturing or non-capturing group. The '(' is already consumed. */
static AstNode *parse_group(ReCompiler *c)
{
    RegexFlags saved = c->flags;
    AstNode *inner;
    AstNode *node;
    int group = 0;

    if (re_peek(c) == '?') {
        re_advance(c);
        if (re_peek(c) == ':') {
            re_advance(c);
            inner = parse_expr(c);
            if (re_peek(c) != ')') {
                re_error(c, "unclosed parenthesis");
                return NULL;
            }
            re_advance(c);
            c->flags = saved;
            return inner ? inner : alloc_node(c, AST_EMPTY);
        }
        if (re_peek(c) == 'P') {
            char name[MAX_GROUP_NAME];
            int name_length = 0;

            re_advance(c);
            if (re_peek(c) != '<') {
                re_error(c, "expected '<' after '(?P'");
                return NULL;
            }
            re_advance(c);
            while (c->index < c->length && re_peek(c) != '>') {
                char ch = re_advance(c);

                if (!(isalnum((unsigned char)ch) || ch == '_') ||
                    (name_length == 0 && isdigit((unsigned char)ch))) {
                    re_error(c, "invalid group name");
                    return NULL;
                }
                if (name_length >= MAX_GROUP_NAME - 1) {
                    re_error(c, "group name too long");
                    return NULL;
                }
                name[name_length++] = ch;
            }
            if (re_peek(c) != '>') {
                re_error(c, "unterminated group name");
                return NULL;
            }
            re_advance(c);
            if (name_length == 0) {
                re_error(c, "empty group name");
                return NULL;
            }
            name[name_length] = '\0';
            for (int g = 1; g <= c->group_count; g++) {
                if (strcmp(c->names[g], name) == 0) {
                    re_error(c, "duplicate group name");
                    return NULL;
                }
            }
            if (c->group_count >= MAX_GROUPS) {
                re_error(c, "too many capture groups");
                return NULL;
            }
            group = ++c->group_count;
            snprintf(c->names[group], MAX_GROUP_NAME, "%s", name);
        } else {
            if (!parse_inline_flags(c)) {
                return NULL;
            }
            return alloc_node(c, AST_EMPTY);
        }
    } else {
        if (c->group_count >= MAX_GROUPS) {
            re_error(c, "too many capture groups");
            return NULL;
        }
        group = ++c->group_count;
    }

    inner = parse_expr(c);
    if (re_peek(c) != ')') {
        re_error(c, "unclosed parenthesis");
        return NULL;
    }
    re_advance(c);
    c->flags = saved;
    node = alloc_node(c, AST_GROUP);
    if (node) {
        node->group = group;
        node->left = inner ? inner : alloc_node(c, AST_EMPTY);
    }
    return node;
}

static AstNode *parse_class(ReCompiler *c)
{
    bool negated = false;
    CharClass cls;
    bool first = true;

    re_advance(c); /* '[' */
    if (re_peek(c) == '^') {
        negated = true;
        re_advance(c);
    }
    memset(&cls, 0, sizeof(cls));

    while (c->index < c->length && (first || re_peek(c) != ']')) {
        char cur;

        first = false;
        cur = re_advance(c);
        if (cur == '\\') {
            char esc;

            if (c->index >= c->length) {
                re_error(c, "unclosed character class");
                return NULL;
            }
            esc = re_advance(c);
            if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' ||
                esc == 'S') {
                add_shorthand(&cls, esc);
            } else {
                class_set(&cls, (unsigned char)re_unescape(esc));
            }
        } else if (re_peek(c) == '-' && c->index + 1 < c->length &&
                   c->pattern[c->index + 1] != ']') {
            char end_char;

            re_advance(c); /* '-' */
            end_char = re_advance(c);
            if (end_char == '\\' && c->index < c->length) {
                end_char = re_unescape(re_advance(c));
            }
            if ((unsigned char)cur > (unsigned char)end_char) {
                re_error(c, "invalid range in character class");
                return NULL;
            }
            for (int code = (unsigned char)cur; code <= (unsigned char)end_char; code++) {
                class_set(&cls, (unsigned char)code);
            }
        } else {
            class_set(&cls, (unsigned char)cur);
        }
    }

    if (re_peek(c) != ']') {
        re_error(c, "unclosed character class");
        return NULL;
    }
    re_advance(c);

    if (negated) {
        CharClass neg;

        for (int i = 0; i < 32; i++) {
            neg.bits[i] = (uint8_t)~cls.bits[i];
        }
        cls = neg;
    }

    AstNode *node = alloc_node(c, AST_CLASS);
    if (node) {
        node->cls = cls;
        node->ci = c->flags.case_insensitive;
    }
    return node;
}

static AstNode *parse_atom(ReCompiler *c)
{
    char ch = re_peek(c);

    if (ch == '\0' || ch == '|' || ch == ')') {
        return NULL;
    }
    if (ch == '*' || ch == '+' || ch == '?') {
        re_error(c, "unexpected quantifier");
        return NULL;
    }
    if (ch == '(') {
        re_advance(c);
        return parse_group(c);
    }
    if (ch == '.') {
        AstNode *node;

        re_advance(c);
        node = alloc_node(c, AST_ANY);
        if (node) node->dotall = c->flags.dotall;
        return node;
    }
    if (ch == '^') {
        AstNode *node;

        re_advance(c);
        node = alloc_node(c, AST_BOL);
        if (node) node->multiline = c->flags.multiline;
        return node;
    }
    if (ch == '$') {
        AstNode *node;

        re_advance(c);
        node = alloc_node(c, AST_EOL);
        if (node) node->multiline = c->flags.multiline;
        return node;
    }
    if (ch == '[') {
        return parse_class(c);
    }
    if (ch == '\\') {
        char esc;

        re_advance(c);
        if (c->index >= c->length) {
            re_error(c, "trailing backslash");
            return NULL;
        }
        esc = re_advance(c);
        if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
            AstNode *node;
            CharClass cls;

            memset(&cls, 0, sizeof(cls));
            add_shorthand(&cls, esc);
            node = alloc_node(c, AST_CLASS);
            if (node) {
                node->cls = cls;
                node->ci = c->flags.case_insensitive;
            }
            return node;
        }
        AstNode *node = alloc_node(c, AST_CHAR);
        if (node) {
            node->c = re_unescape(esc);
            node->ci = c->flags.case_insensitive;
        }
        return node;
    }

    re_advance(c);
    AstNode *node = alloc_node(c, AST_CHAR);
    if (node) {
        node->c = ch;
        node->ci = c->flags.case_insensitive;
    }
    return node;
}

/* Parses `{n}`, `{n,}` or `{n,m}` at the cursor. On a malformed form the cursor
 * is restored and false is returned, so the '{' becomes a literal character. */
static bool parse_repetition(ReCompiler *c, int *min, int *max)
{
    int saved = c->index;
    long value = 0;
    bool has_digits = false;
    int minimum;

    re_advance(c); /* '{' */
    while (isdigit((unsigned char)re_peek(c))) {
        value = value * 10 + (re_advance(c) - '0');
        has_digits = true;
        if (value > MAX_REPEAT) {
            re_error(c, "repetition count too large");
            return false;
        }
    }
    if (!has_digits) {
        c->index = saved;
        return false;
    }
    minimum = (int)value;
    if (re_peek(c) == '}') {
        re_advance(c);
        *min = minimum;
        *max = minimum;
        return true;
    }
    if (re_peek(c) != ',') {
        c->index = saved;
        return false;
    }
    re_advance(c);
    if (re_peek(c) == '}') {
        re_advance(c);
        *min = minimum;
        *max = -1;
        return true;
    }
    value = 0;
    has_digits = false;
    while (isdigit((unsigned char)re_peek(c))) {
        value = value * 10 + (re_advance(c) - '0');
        has_digits = true;
        if (value > MAX_REPEAT) {
            re_error(c, "repetition count too large");
            return false;
        }
    }
    if (!has_digits || re_peek(c) != '}') {
        c->index = saved;
        return false;
    }
    re_advance(c);
    *min = minimum;
    *max = (int)value;
    return true;
}

static AstNode *parse_piece(ReCompiler *c)
{
    AstNode *atom = parse_atom(c);
    char q;
    int min = 0;
    int max = -1;
    bool lazy = false;
    AstNode *node;

    if (!atom) {
        return NULL;
    }
    q = re_peek(c);
    if (q == '*') {
        min = 0;
        max = -1;
        re_advance(c);
    } else if (q == '+') {
        min = 1;
        max = -1;
        re_advance(c);
    } else if (q == '?') {
        min = 0;
        max = 1;
        re_advance(c);
    } else if (q == '{') {
        if (!parse_repetition(c, &min, &max)) {
            /* Not a quantifier after all: the '{' is a literal. */
            return atom;
        }
        if (max >= 0 && min > max) {
            re_error(c, "invalid repetition range");
            return NULL;
        }
    } else {
        return atom;
    }
    if (re_peek(c) == '?') {
        lazy = true;
        re_advance(c);
    }
    node = alloc_node(c, AST_REPEAT);
    if (node) {
        node->left = atom;
        node->min = min;
        node->max = max;
        node->lazy = lazy;
    }
    return node;
}

static AstNode *parse_branch(ReCompiler *c)
{
    AstNode *root = NULL;

    while (re_peek(c) != '\0' && re_peek(c) != '|' && re_peek(c) != ')') {
        AstNode *piece = parse_piece(c);

        if (!piece) break;
        if (!root) {
            root = piece;
        } else {
            AstNode *cat = alloc_node(c, AST_CAT);

            if (cat) {
                cat->left = root;
                cat->right = piece;
                root = cat;
            }
        }
    }
    return root ? root : alloc_node(c, AST_EMPTY);
}

static AstNode *parse_expr(ReCompiler *c)
{
    AstNode *left = parse_branch(c);

    if (re_peek(c) == '|') {
        AstNode *right;
        AstNode *alt;

        re_advance(c);
        right = parse_expr(c);
        alt = alloc_node(c, AST_ALT);
        if (alt) {
            alt->left = left;
            alt->right = right;
        }
        return alt ? alt : left;
    }
    return left;
}

/* ------------------------------------------------------------------ */
/* NFA Bytecode Generator                                             */
/* ------------------------------------------------------------------ */

static int emit_inst(ReCompiler *c, ReOpcode op)
{
    int idx;

    if (c->prog.count >= MAX_REGEX_INST - 1) {
        re_error(c, "pattern too complex");
        return -1;
    }
    idx = c->prog.count++;
    memset(&c->prog.code[idx], 0, sizeof(ReInst));
    c->prog.code[idx].op = op;
    return idx;
}

static void emit_node(ReCompiler *c, AstNode *n);

static void emit_star(ReCompiler *c, AstNode *n, bool lazy)
{
    int split = emit_inst(c, RE_SPLIT);
    int start = c->prog.count;
    int jump;
    int end;

    emit_node(c, n);
    jump = emit_inst(c, RE_JUMP);
    end = c->prog.count;
    if (split >= 0) {
        c->prog.code[split].target1 = lazy ? end : start;
        c->prog.code[split].target2 = lazy ? start : end;
    }
    if (jump >= 0) {
        c->prog.code[jump].target1 = split;
    }
}

static void emit_repeat(ReCompiler *c, AstNode *n)
{
    int extra = n->max < 0 ? -1 : n->max - n->min;

    for (int i = 0; i < n->min; i++) {
        emit_node(c, n->left);
    }
    if (extra < 0) {
        emit_star(c, n->left, n->lazy);
        return;
    }
    for (int i = 0; i < extra; i++) {
        int split = emit_inst(c, RE_SPLIT);
        int body = c->prog.count;
        int end;

        emit_node(c, n->left);
        end = c->prog.count;
        if (split >= 0) {
            c->prog.code[split].target1 = n->lazy ? end : body;
            c->prog.code[split].target2 = n->lazy ? body : end;
        }
    }
}

static void emit_node(ReCompiler *c, AstNode *n)
{
    if (!n || n->type == AST_EMPTY) {
        return;
    }
    switch (n->type) {
    case AST_CHAR: {
        int idx = emit_inst(c, RE_CHAR);
        if (idx >= 0) {
            c->prog.code[idx].c = n->c;
            c->prog.code[idx].ci = n->ci;
        }
        break;
    }
    case AST_ANY: {
        int idx = emit_inst(c, RE_ANY);
        if (idx >= 0) c->prog.code[idx].dotall = n->dotall;
        break;
    }
    case AST_CLASS: {
        int idx = emit_inst(c, RE_CLASS);
        if (idx >= 0) {
            c->prog.code[idx].cls = n->cls;
            c->prog.code[idx].ci = n->ci;
        }
        break;
    }
    case AST_BOL: {
        int idx = emit_inst(c, RE_BOL);
        if (idx >= 0) c->prog.code[idx].multiline = n->multiline;
        break;
    }
    case AST_EOL: {
        int idx = emit_inst(c, RE_EOL);
        if (idx >= 0) c->prog.code[idx].multiline = n->multiline;
        break;
    }
    case AST_CAT:
        emit_node(c, n->left);
        emit_node(c, n->right);
        break;
    case AST_ALT: {
        int split = emit_inst(c, RE_SPLIT);
        int left_start = c->prog.count;
        int jump;
        int right_start;
        int end;

        emit_node(c, n->left);
        jump = emit_inst(c, RE_JUMP);
        right_start = c->prog.count;
        emit_node(c, n->right);
        end = c->prog.count;
        if (split >= 0) {
            c->prog.code[split].target1 = left_start;
            c->prog.code[split].target2 = right_start;
        }
        if (jump >= 0) {
            c->prog.code[jump].target1 = end;
        }
        break;
    }
    case AST_REPEAT:
        emit_repeat(c, n);
        break;
    case AST_GROUP: {
        if (n->group > 0) {
            int idx = emit_inst(c, RE_SAVE);

            if (idx >= 0) c->prog.code[idx].slot = 2 * n->group;
        }
        emit_node(c, n->left);
        if (n->group > 0) {
            int idx = emit_inst(c, RE_SAVE);

            if (idx >= 0) c->prog.code[idx].slot = 2 * n->group + 1;
        }
        break;
    }
    case AST_EMPTY:
        break;
    }
}

static bool re_compile(const char *pattern, RegexFlags flags, ReProgram *out, char *error,
                       size_t error_size)
{
    ReCompiler *compiler = malloc(sizeof(ReCompiler));
    AstNode *ast;

    if (!compiler) {
        regex_set_error(error, error_size, "regex: out of memory");
        return false;
    }
    memset(compiler, 0, sizeof(ReCompiler));
    compiler->pattern = pattern;
    compiler->length = (int)strlen(pattern);
    compiler->error = error;
    compiler->error_size = error_size;
    compiler->flags = flags;
    if (error && error_size > 0) error[0] = '\0';

    ast = parse_expr(compiler);
    if (compiler->index < compiler->length &&
        (!compiler->error || compiler->error[0] == '\0')) {
        re_error(compiler, "unexpected trailing character");
    }
    if (compiler->error && compiler->error[0] != '\0') {
        free(compiler);
        return false;
    }

    emit_node(compiler, ast);
    emit_inst(compiler, RE_MATCH);
    if (compiler->error && compiler->error[0] != '\0') {
        free(compiler);
        return false;
    }

    compiler->prog.group_count = compiler->group_count;
    memcpy(compiler->prog.names, compiler->names, sizeof(compiler->prog.names));
    compiler->prog.anchored = compiler->prog.count > 0 &&
                              compiler->prog.code[0].op == RE_BOL &&
                              !compiler->prog.code[0].multiline;
    *out = compiler->prog;
    free(compiler);
    return true;
}

/* ------------------------------------------------------------------ */
/* Pike VM Execution                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int pc;
    int start;
    int caps;
} Thread;

typedef struct {
    Thread threads[MAX_THREADS];
    int count;
    bool visited[MAX_REGEX_INST];
    int (*captures)[CAPTURE_SLOTS];
    int capture_count;
} ThreadList;

typedef struct {
    ThreadList *current;
    ThreadList *next;
    bool tracking;
} RegexSearcher;

typedef struct {
    bool matched;
    int start;
    int end;
    int caps[CAPTURE_SLOTS];
} RegexMatch;

static ThreadList *thread_list_new(bool tracking)
{
    ThreadList *list = malloc(sizeof(ThreadList));

    if (!list) {
        return NULL;
    }
    list->count = 0;
    list->capture_count = 0;
    list->captures = NULL;
    if (tracking) {
        list->captures = malloc(sizeof(int[CAPTURE_SLOTS]) * MAX_THREADS);
        if (!list->captures) {
            free(list);
            return NULL;
        }
    }
    return list;
}

static void thread_list_free(ThreadList *list)
{
    if (list) {
        free(list->captures);
        free(list);
    }
}

static RegexSearcher *searcher_new(bool tracking)
{
    RegexSearcher *searcher = malloc(sizeof(RegexSearcher));

    if (!searcher) {
        return NULL;
    }
    searcher->tracking = tracking;
    searcher->current = thread_list_new(tracking);
    searcher->next = thread_list_new(tracking);
    if (!searcher->current || !searcher->next) {
        thread_list_free(searcher->current);
        thread_list_free(searcher->next);
        free(searcher);
        return NULL;
    }
    return searcher;
}

static void searcher_free(RegexSearcher *searcher)
{
    if (searcher) {
        thread_list_free(searcher->current);
        thread_list_free(searcher->next);
        free(searcher);
    }
}

static void thread_list_reset(ThreadList *list)
{
    list->count = 0;
    list->capture_count = 0;
    memset(list->visited, 0, sizeof(list->visited));
}

static bool add_thread(const ReProgram *prog, ThreadList *list, int pc, int start,
                       const int *captures, const char *text, int pos, int text_len,
                       size_t *steps)
{
    const ReInst *inst;

    if (*steps == 0) {
        return false;
    }
    (*steps)--;
    if (pc < 0 || pc >= prog->count || list->visited[pc]) {
        return true;
    }
    list->visited[pc] = true;

    inst = &prog->code[pc];
    switch (inst->op) {
    case RE_JUMP:
        return add_thread(prog, list, inst->target1, start, captures, text, pos, text_len,
                          steps);
    case RE_SPLIT:
        if (!add_thread(prog, list, inst->target1, start, captures, text, pos, text_len,
                        steps)) {
            return false;
        }
        return add_thread(prog, list, inst->target2, start, captures, text, pos, text_len,
                          steps);
    case RE_SAVE: {
        int saved[CAPTURE_SLOTS];

        if (!list->captures) {
            return add_thread(prog, list, pc + 1, start, captures, text, pos, text_len,
                              steps);
        }
        if (captures) {
            memcpy(saved, captures, sizeof(saved));
        } else {
            for (int i = 0; i < CAPTURE_SLOTS; i++) saved[i] = -1;
        }
        if (inst->slot >= 0 && inst->slot < CAPTURE_SLOTS) {
            saved[inst->slot] = pos;
        }
        return add_thread(prog, list, pc + 1, start, saved, text, pos, text_len, steps);
    }
    case RE_BOL:
        if (pos == 0 || (inst->multiline && pos > 0 && text[pos - 1] == '\n')) {
            return add_thread(prog, list, pc + 1, start, captures, text, pos, text_len,
                              steps);
        }
        return true;
    case RE_EOL:
        if (pos == text_len || (inst->multiline && pos < text_len && text[pos] == '\n')) {
            return add_thread(prog, list, pc + 1, start, captures, text, pos, text_len,
                              steps);
        }
        return true;
    default:
        if (list->count < MAX_THREADS) {
            Thread *thread = &list->threads[list->count++];

            thread->pc = pc;
            thread->start = start;
            thread->caps = -1;
            if (list->captures && list->capture_count < MAX_THREADS) {
                int *row;

                thread->caps = list->capture_count;
                row = list->captures[list->capture_count++];
                if (captures) {
                    memcpy(row, captures, sizeof(int) * CAPTURE_SLOTS);
                } else {
                    for (int i = 0; i < CAPTURE_SLOTS; i++) row[i] = -1;
                }
            }
        }
        return true;
    }
}

static bool inst_matches(const ReInst *inst, unsigned char ch)
{
    switch (inst->op) {
    case RE_CHAR:
        if (inst->c == (char)ch) return true;
        return inst->ci &&
               (tolower((unsigned char)inst->c) == tolower(ch) ||
                tolower((unsigned char)inst->c) == toupper(ch));
    case RE_ANY:
        return inst->dotall || ch != '\n';
    case RE_CLASS:
        if (class_test(&inst->cls, ch)) return true;
        if (inst->ci) {
            return class_test(&inst->cls, (unsigned char)tolower(ch)) ||
                   class_test(&inst->cls, (unsigned char)toupper(ch));
        }
        return false;
    default:
        return false;
    }
}

static bool re_search(RegexSearcher *searcher, const ReProgram *prog, const char *text,
                      int text_len, int from, bool anchored_start, RegexMatch *match,
                      char *error, size_t error_size)
{
    size_t steps = MAX_STEPS;
    ThreadList *current = searcher->current;
    ThreadList *next = searcher->next;
    bool found = false;

    memset(match, 0, sizeof(*match));
    for (int slot = 0; slot < CAPTURE_SLOTS; slot++) {
        match->caps[slot] = -1;
    }

    for (int start = from; start <= text_len; start++) {
        bool candidate = false;
        RegexMatch best;

        memset(&best, 0, sizeof(best));
        for (int slot = 0; slot < CAPTURE_SLOTS; slot++) {
            best.caps[slot] = -1;
        }
        thread_list_reset(current);
        if (!add_thread(prog, current, 0, start, NULL, text, start, text_len, &steps)) {
            regex_set_error(error, error_size, "regex: step limit exceeded");
            return false;
        }

        for (int pos = start; ; pos++) {
            int match_index = -1;

            if (steps == 0) {
                regex_set_error(error, error_size, "regex: step limit exceeded");
                return false;
            }
            steps--;
            for (int t = 0; t < current->count; t++) {
                if (prog->code[current->threads[t].pc].op == RE_MATCH) {
                    match_index = t;
                    break;
                }
            }
            if (match_index >= 0) {
                Thread *thread = &current->threads[match_index];

                candidate = true;
                best.start = thread->start;
                best.end = pos;
                if (searcher->tracking && thread->caps >= 0) {
                    memcpy(best.caps, current->captures[thread->caps], sizeof(best.caps));
                }
                /* Lower-priority threads can no longer win. */
                current->count = match_index;
            }
            if (pos >= text_len || current->count == 0) {
                break;
            }

            thread_list_reset(next);
            for (int t = 0; t < current->count; t++) {
                Thread *thread = &current->threads[t];
                const ReInst *inst = &prog->code[thread->pc];

                if (steps == 0) {
                    regex_set_error(error, error_size, "regex: step limit exceeded");
                    return false;
                }
                steps--;
                if (inst->op == RE_MATCH) {
                    continue;
                }
                if (inst_matches(inst, (unsigned char)text[pos])) {
                    const int *parent =
                        searcher->tracking && thread->caps >= 0
                            ? current->captures[thread->caps]
                            : NULL;

                    if (!add_thread(prog, next, thread->pc + 1, thread->start, parent, text,
                                    pos + 1, text_len, &steps)) {
                        regex_set_error(error, error_size, "regex: step limit exceeded");
                        return false;
                    }
                }
            }
            ThreadList *swap = current;

            current = next;
            next = swap;
        }

        if (candidate) {
            *match = best;
            found = true;
            break;
        }
        if (anchored_start || prog->anchored) {
            break;
        }
    }
    searcher->current = current;
    searcher->next = next;
    return found;
}

/* ------------------------------------------------------------------ */
/* Character offsets                                                  */
/* ------------------------------------------------------------------ */

static int byte_to_char(const char *text, int byte_offset)
{
    int chars = 0;

    for (int i = 0; i < byte_offset; i++) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) {
            chars++;
        }
    }
    return chars;
}

/* The byte offset just past the character that starts at `byte_offset`, used to
 * advance past an empty match without splitting a multibyte sequence. */
static int advance_one_char(const char *text, int length, int byte_offset)
{
    int next = byte_offset + 1;

    while (next < length && ((unsigned char)text[next] & 0xc0) == 0x80) {
        next++;
    }
    return next;
}

static int char_to_byte(const char *text, int length, int char_offset)
{
    int byte = 0;

    for (int i = 0; i < char_offset && byte < length; i++) {
        byte++;
        while (byte < length && ((unsigned char)text[byte] & 0xc0) == 0x80) {
            byte++;
        }
    }
    return byte;
}

/* ------------------------------------------------------------------ */
/* Public operations                                                  */
/* ------------------------------------------------------------------ */

bool regex_parse_flags(const char *flags, RegexFlags *out, char *error, size_t error_size)
{
    memset(out, 0, sizeof(*out));
    if (error && error_size > 0) error[0] = '\0';
    for (const char *p = flags; *p; p++) {
        if (*p == 'i') {
            out->case_insensitive = true;
        } else if (*p == 's') {
            out->dotall = true;
        } else if (*p == 'm') {
            out->multiline = true;
        } else {
            regex_set_error(error, error_size, "invalid regex flags: unknown flag '%c'", *p);
            return false;
        }
    }
    return true;
}

bool regex_match(const char *pattern, const char *text, RegexFlags flags, char *error,
                 size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    RegexMatch match;
    bool found;

    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return false;
    }
    searcher = searcher_new(false);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return false;
    }
    found = re_search(searcher, &prog, text, (int)strlen(text), 0, false, &match, error,
                      error_size);
    searcher_free(searcher);
    return found;
}

ObjArray *regex_find_all(const char *pattern, const char *text, RegexFlags flags, char *error,
                         size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    ObjArray *results;
    int length;
    int pos = 0;

    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return NULL;
    }
    searcher = searcher_new(false);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return NULL;
    }
    length = (int)strlen(text);
    mem_pause_gc();
    results = object_array();
    while (pos <= length) {
        RegexMatch match;

        if (!re_search(searcher, &prog, text, length, pos, false, &match, error, error_size)) {
            if (error && error[0] != '\0') {
                mem_resume_gc();
                searcher_free(searcher);
                return NULL;
            }
            break;
        }
        array_push(results, OBJ_VAL(object_string(text + match.start,
                                                  match.end - match.start)));
        if (match.end > match.start) {
            pos = match.end;
        } else {
            pos = advance_one_char(text, length, match.start);
        }
    }
    mem_resume_gc();
    searcher_free(searcher);
    return results;
}

static ObjMap *span_map(const char *text, int byte_start, int byte_end)
{
    ObjMap *map = object_map();

    map_set(map, OBJ_VAL(object_string("start", 5)),
            INT_VAL(byte_to_char(text, byte_start)));
    map_set(map, OBJ_VAL(object_string("end", 3)), INT_VAL(byte_to_char(text, byte_end)));
    return map;
}

ObjArray *regex_find_index(const char *pattern, const char *text, RegexFlags flags,
                           char *error, size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    ObjArray *results;
    int length;
    int pos = 0;

    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return NULL;
    }
    searcher = searcher_new(false);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return NULL;
    }
    length = (int)strlen(text);
    mem_pause_gc();
    results = object_array();
    while (pos <= length) {
        RegexMatch match;

        if (!re_search(searcher, &prog, text, length, pos, false, &match, error, error_size)) {
            if (error && error[0] != '\0') {
                mem_resume_gc();
                searcher_free(searcher);
                return NULL;
            }
            break;
        }
        array_push(results, OBJ_VAL(span_map(text, match.start, match.end)));
        if (match.end > match.start) {
            pos = match.end;
        } else {
            pos = advance_one_char(text, length, match.start);
        }
    }
    mem_resume_gc();
    searcher_free(searcher);
    return results;
}

bool regex_match_at(const char *pattern, const char *text, int char_start, RegexFlags flags,
                    int *start_char, int *end_char, char *error, size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    RegexMatch match;
    int length;
    int byte_start;
    bool found;

    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return false;
    }
    length = (int)strlen(text);
    byte_start = char_to_byte(text, length, char_start);
    searcher = searcher_new(false);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return false;
    }
    found = re_search(searcher, &prog, text, length, byte_start, true, &match, error,
                      error_size);
    searcher_free(searcher);
    if (found) {
        if (start_char) *start_char = byte_to_char(text, match.start);
        if (end_char) *end_char = byte_to_char(text, match.end);
    }
    return found;
}

/* Appends group `group` of the match to out (or drops it when out is NULL, for
 * validation). Group 0 is the whole match. Returns false on an unknown group. */
static bool append_group(buf_t *out, int group, const ReProgram *prog,
                         const RegexMatch *match, const char *text, char *error,
                         size_t error_size)
{
    int start;
    int end;

    if (group < 0 || group > prog->group_count) {
        regex_set_error(error, error_size, "regex_replace: unknown group '%d'", group);
        return false;
    }
    if (group == 0) {
        start = match->start;
        end = match->end;
    } else {
        start = match->caps[2 * group];
        end = match->caps[2 * group + 1];
    }
    if (out && start >= 0 && end >= start) {
        if (buf_append(out, text + start, (size_t)(end - start)) != 0) {
            return true; /* out of memory is reported by the caller's append checks */
        }
    }
    return true;
}

static bool find_named_group(const ReProgram *prog, const char *name, int *group)
{
    for (int g = 1; g <= prog->group_count; g++) {
        if (strcmp(prog->names[g], name) == 0) {
            *group = g;
            return true;
        }
    }
    return false;
}

static bool expand_replacement(buf_t *out, const char *replacement, const ReProgram *prog,
                               const RegexMatch *match, const char *text, char *error,
                               size_t error_size)
{
    int i = 0;
    int length = (int)strlen(replacement);

    while (i < length) {
        if (replacement[i] != '$') {
            if (out) buf_append(out, replacement + i, 1);
            i++;
            continue;
        }
        if (i + 1 < length && replacement[i + 1] == '$') {
            if (out) buf_append(out, "$", 1);
            i += 2;
            continue;
        }
        if (i + 1 < length && replacement[i + 1] == '{') {
            int close = i + 2;

            while (close < length && replacement[close] != '}') close++;
            if (close >= length) {
                regex_set_error(error, error_size,
                                "regex_replace: unterminated group reference");
                return false;
            }
            if (close == i + 2) {
                regex_set_error(error, error_size, "regex_replace: empty group reference");
                return false;
            }
            int group = -1;
            bool numeric = true;
            for (int k = i + 2; k < close; k++) {
                if (!isdigit((unsigned char)replacement[k])) {
                    numeric = false;
                    break;
                }
            }
            if (numeric) {
                group = 0;
                for (int k = i + 2; k < close; k++) {
                    group = group * 10 + (replacement[k] - '0');
                    if (group > MAX_GROUPS) break;
                }
            } else {
                char name[MAX_GROUP_NAME];

                if (close - (i + 2) >= MAX_GROUP_NAME) {
                    regex_set_error(error, error_size, "regex_replace: group name too long");
                    return false;
                }
                memcpy(name, replacement + i + 2, (size_t)(close - (i + 2)));
                name[close - (i + 2)] = '\0';
                if (!find_named_group(prog, name, &group)) {
                    regex_set_error(error, error_size,
                                    "regex_replace: unknown group '%.*s'",
                                    close - (i + 2), name);
                    return false;
                }
            }
            if (!append_group(out, group, prog, match, text, error, error_size)) {
                return false;
            }
            i = close + 1;
            continue;
        }
        if (i + 1 < length && isdigit((unsigned char)replacement[i + 1])) {
            int group = 0;
            int j = i + 1;

            while (j < length && isdigit((unsigned char)replacement[j])) {
                group = group * 10 + (replacement[j] - '0');
                if (group > MAX_GROUPS) break;
                j++;
            }
            if (!append_group(out, group, prog, match, text, error, error_size)) {
                return false;
            }
            i = j;
            continue;
        }
        if (out) buf_append(out, "$", 1);
        i++;
    }
    return true;
}

ObjString *regex_replace(const char *pattern, const char *replacement, const char *text,
                         RegexFlags flags, char *error, size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    RegexMatch placeholder;
    buf_t out;
    int length;
    int pos = 0;
    ObjString *result;

    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return NULL;
    }
    /* Validate the template once, whether or not the pattern matches anything. */
    memset(&placeholder, 0, sizeof(placeholder));
    if (!expand_replacement(NULL, replacement, &prog, &placeholder, text, error,
                            error_size)) {
        return NULL;
    }
    searcher = searcher_new(true);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return NULL;
    }
    length = (int)strlen(text);
    buf_init(&out);
    while (pos <= length) {
        RegexMatch match;

        if (!re_search(searcher, &prog, text, length, pos, false, &match, error, error_size)) {
            if (error && error[0] != '\0') {
                buf_free(&out);
                searcher_free(searcher);
                return NULL;
            }
            buf_append(&out, text + pos, (size_t)(length - pos));
            break;
        }
        if (match.start > pos) {
            buf_append(&out, text + pos, (size_t)(match.start - pos));
        }
        if (!expand_replacement(&out, replacement, &prog, &match, text, error, error_size)) {
            buf_free(&out);
            searcher_free(searcher);
            return NULL;
        }
        if (match.end > match.start) {
            pos = match.end;
        } else {
            if (match.start < length) {
                int next = advance_one_char(text, length, match.start);

                buf_append(&out, text + match.start, (size_t)(next - match.start));
                pos = next;
            } else {
                break;
            }
        }
    }
    result = object_string(out.data ? out.data : "", (int)out.len);
    buf_free(&out);
    searcher_free(searcher);
    return result;
}

ObjArray *regex_split(const char *pattern, const char *text, int limit, RegexFlags flags,
                      char *error, size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    ObjArray *parts;
    int length;
    int pos = 0;

    if (limit == 0) {
        return object_array();
    }
    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return NULL;
    }
    searcher = searcher_new(false);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return NULL;
    }
    length = (int)strlen(text);
    mem_pause_gc();
    parts = object_array();
    while (limit < 0 || (int)parts->count < limit - 1) {
        int from = pos;
        RegexMatch match;
        bool found = false;

        /* Zero-width matches cannot delimit fields: skip them. */
        while (from <= length) {
            if (!re_search(searcher, &prog, text, length, from, false, &match, error,
                           error_size)) {
                if (error && error[0] != '\0') {
                    mem_resume_gc();
                    searcher_free(searcher);
                    return NULL;
                }
                break;
            }
            if (match.end > match.start) {
                found = true;
                break;
            }
            from = advance_one_char(text, length, match.start);
        }
        if (!found) {
            break;
        }
        array_push(parts, OBJ_VAL(object_string(text + pos, match.start - pos)));
        pos = match.end;
    }
    array_push(parts, OBJ_VAL(object_string(text + pos, length - pos)));
    mem_resume_gc();
    searcher_free(searcher);
    return parts;
}

bool regex_match_groups(const char *pattern, const char *text, RegexFlags flags, ObjMap **result,
                        char *error, size_t error_size)
{
    ReProgram prog;
    RegexSearcher *searcher;
    RegexMatch match;
    ObjMap *map;
    ObjArray *groups;
    ObjMap *named;

    *result = NULL;
    if (!re_compile(pattern, flags, &prog, error, error_size)) {
        return false;
    }
    searcher = searcher_new(true);
    if (!searcher) {
        regex_set_error(error, error_size, "regex: out of memory");
        return false;
    }
    if (!re_search(searcher, &prog, text, (int)strlen(text), 0, false, &match, error,
                   error_size)) {
        searcher_free(searcher);
        return false;
    }
    searcher_free(searcher);

    mem_pause_gc();
    map = object_map();
    map_set(map, OBJ_VAL(object_string("start", 5)),
            INT_VAL(byte_to_char(text, match.start)));
    map_set(map, OBJ_VAL(object_string("end", 3)), INT_VAL(byte_to_char(text, match.end)));
    groups = object_array();
    for (int g = 1; g <= prog.group_count; g++) {
        int start = match.caps[2 * g];
        int end = match.caps[2 * g + 1];

        if (start < 0 || end < start) {
            array_push(groups, NIL_VAL);
        } else {
            ObjMap *group = object_map();

            map_set(group, OBJ_VAL(object_string("start", 5)),
                    INT_VAL(byte_to_char(text, start)));
            map_set(group, OBJ_VAL(object_string("end", 3)),
                    INT_VAL(byte_to_char(text, end)));
            map_set(group, OBJ_VAL(object_string("text", 4)),
                    OBJ_VAL(object_string(text + start, end - start)));
            array_push(groups, OBJ_VAL(group));
        }
    }
    map_set(map, OBJ_VAL(object_string("groups", 6)), OBJ_VAL(groups));
    named = object_map();
    for (int g = 1; g <= prog.group_count; g++) {
        if (prog.names[g][0] == '\0') {
            continue;
        }
        map_set(named, OBJ_VAL(object_string(prog.names[g],
                                             (int)strlen(prog.names[g]))),
                groups->items[g - 1]);
    }
    map_set(map, OBJ_VAL(object_string("named", 5)), OBJ_VAL(named));
    mem_resume_gc();
    *result = map;
    return true;
}