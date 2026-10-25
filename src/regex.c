/* regex.c - Thompson NFA / Pike VM regular expression engine for Spark. */
#include "regex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "memory.h"
#include "object.h"

#define MAX_REGEX_INST 1024
#define MAX_THREADS 1024
#define MAX_NODES 1024

typedef enum {
    RE_CHAR,
    RE_ANY,
    RE_CLASS,
    RE_BOL,
    RE_EOL,
    RE_JUMP,
    RE_SPLIT,
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
} ReInst;

typedef struct {
    ReInst code[MAX_REGEX_INST];
    int count;
    bool anchored;
} ReProgram;

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
    AST_STAR,
    AST_PLUS,
    AST_OPT
} AstType;

typedef struct AstNode AstNode;
struct AstNode {
    AstType type;
    char c;
    CharClass cls;
    AstNode *left;
    AstNode *right;
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
} ReCompiler;

static void re_error(ReCompiler *c, const char *msg)
{
    if (c->error && c->error_size > 0 && c->error[0] == '\0') {
        snprintf(c->error, c->error_size, "regex syntax error: %s", msg);
    }
}

static AstNode *alloc_node(ReCompiler *c, AstType type)
{
    if (c->node_count >= MAX_NODES) {
        re_error(c, "pattern too complex");
        return NULL;
    }
    AstNode *n = &c->arena[c->node_count++];
    memset(n, 0, sizeof(AstNode));
    n->type = type;
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
        AstNode *inner = parse_expr(c);
        if (re_peek(c) != ')') {
            re_error(c, "unclosed parenthesis");
            return NULL;
        }
        re_advance(c);
        return inner ? inner : alloc_node(c, AST_EMPTY);
    }

    if (ch == '.') {
        re_advance(c);
        return alloc_node(c, AST_ANY);
    }

    if (ch == '^') {
        re_advance(c);
        return alloc_node(c, AST_BOL);
    }

    if (ch == '$') {
        re_advance(c);
        return alloc_node(c, AST_EOL);
    }

    if (ch == '[') {
        re_advance(c);
        bool negated = false;
        if (re_peek(c) == '^') {
            negated = true;
            re_advance(c);
        }

        CharClass cls;
        memset(&cls, 0, sizeof(cls));
        bool first = true;

        while (c->index < c->length && (first || re_peek(c) != ']')) {
            first = false;
            char cur = re_advance(c);
            if (cur == '\\') {
                if (c->index >= c->length) {
                    re_error(c, "unclosed character class");
                    return NULL;
                }
                char esc = re_advance(c);
                if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
                    add_shorthand(&cls, esc);
                } else if (esc == 'n') class_set(&cls, '\n');
                else if (esc == 't') class_set(&cls, '\t');
                else if (esc == 'r') class_set(&cls, '\r');
                else class_set(&cls, (unsigned char)esc);
            } else if (re_peek(c) == '-' && c->index + 1 < c->length && c->pattern[c->index + 1] != ']') {
                re_advance(c); /* consume '-' */
                char end_char = re_advance(c);
                if (end_char == '\\' && c->index < c->length) {
                    end_char = re_advance(c);
                    if (end_char == 'n') end_char = '\n';
                    else if (end_char == 't') end_char = '\t';
                    else if (end_char == 'r') end_char = '\r';
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
        if (node) node->cls = cls;
        return node;
    }

    if (ch == '\\') {
        re_advance(c);
        if (c->index >= c->length) {
            re_error(c, "trailing backslash");
            return NULL;
        }
        char esc = re_advance(c);
        if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
            CharClass cls;
            memset(&cls, 0, sizeof(cls));
            add_shorthand(&cls, esc);
            AstNode *node = alloc_node(c, AST_CLASS);
            if (node) node->cls = cls;
            return node;
        }
        char literal = esc;
        if (esc == 'n') literal = '\n';
        else if (esc == 't') literal = '\t';
        else if (esc == 'r') literal = '\r';
        AstNode *node = alloc_node(c, AST_CHAR);
        if (node) node->c = literal;
        return node;
    }

    re_advance(c);
    AstNode *node = alloc_node(c, AST_CHAR);
    if (node) node->c = ch;
    return node;
}

static AstNode *parse_piece(ReCompiler *c)
{
    AstNode *atom = parse_atom(c);
    if (!atom) return NULL;

    char q = re_peek(c);
    if (q == '*' || q == '+' || q == '?') {
        re_advance(c);
        AstType type = (q == '*') ? AST_STAR : ((q == '+') ? AST_PLUS : AST_OPT);
        AstNode *node = alloc_node(c, type);
        if (node) node->left = atom;
        return node;
    }
    return atom;
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
        re_advance(c);
        AstNode *right = parse_expr(c);
        AstNode *alt = alloc_node(c, AST_ALT);
        if (alt) {
            alt->left = left;
            alt->right = right;
            return alt;
        }
    }
    return left;
}

/* ------------------------------------------------------------------ */
/* NFA Bytecode Generator                                             */
/* ------------------------------------------------------------------ */

static int emit_inst(ReCompiler *c, ReOpcode op)
{
    if (c->prog.count >= MAX_REGEX_INST - 1) {
        re_error(c, "pattern too complex");
        return -1;
    }
    int idx = c->prog.count++;
    memset(&c->prog.code[idx], 0, sizeof(ReInst));
    c->prog.code[idx].op = op;
    return idx;
}

static void emit_node(ReCompiler *c, AstNode *n)
{
    if (!n || n->type == AST_EMPTY) {
        return;
    }

    switch (n->type) {
    case AST_CHAR: {
        int idx = emit_inst(c, RE_CHAR);
        if (idx >= 0) c->prog.code[idx].c = n->c;
        break;
    }
    case AST_ANY:
        emit_inst(c, RE_ANY);
        break;
    case AST_CLASS: {
        int idx = emit_inst(c, RE_CLASS);
        if (idx >= 0) c->prog.code[idx].cls = n->cls;
        break;
    }
    case AST_BOL:
        emit_inst(c, RE_BOL);
        break;
    case AST_EOL:
        emit_inst(c, RE_EOL);
        break;
    case AST_CAT:
        emit_node(c, n->left);
        emit_node(c, n->right);
        break;
    case AST_ALT: {
        int split = emit_inst(c, RE_SPLIT);
        int left_start = c->prog.count;
        emit_node(c, n->left);
        int jump = emit_inst(c, RE_JUMP);
        int right_start = c->prog.count;
        emit_node(c, n->right);
        int end = c->prog.count;

        if (split >= 0) {
            c->prog.code[split].target1 = left_start;
            c->prog.code[split].target2 = right_start;
        }
        if (jump >= 0) {
            c->prog.code[jump].target1 = end;
        }
        break;
    }
    case AST_STAR: {
        int split = emit_inst(c, RE_SPLIT);
        int start = c->prog.count;
        emit_node(c, n->left);
        int jump = emit_inst(c, RE_JUMP);
        int end = c->prog.count;

        if (split >= 0) {
            c->prog.code[split].target1 = start;
            c->prog.code[split].target2 = end;
        }
        if (jump >= 0) {
            c->prog.code[jump].target1 = split;
        }
        break;
    }
    case AST_PLUS: {
        int start = c->prog.count;
        emit_node(c, n->left);
        int split = emit_inst(c, RE_SPLIT);
        if (split >= 0) {
            c->prog.code[split].target1 = start;
            c->prog.code[split].target2 = c->prog.count;
        }
        break;
    }
    case AST_OPT: {
        int split = emit_inst(c, RE_SPLIT);
        int start = c->prog.count;
        emit_node(c, n->left);
        int end = c->prog.count;

        if (split >= 0) {
            c->prog.code[split].target1 = start;
            c->prog.code[split].target2 = end;
        }
        break;
    }
    case AST_EMPTY:
        break;
    }
}

static bool re_compile(const char *pattern, ReProgram *out, char *error, size_t error_size)
{
    ReCompiler *compiler = malloc(sizeof(ReCompiler));
    if (!compiler) {
        if (error && error_size > 0) snprintf(error, error_size, "out of memory");
        return false;
    }
    compiler->pattern = pattern;
    compiler->index = 0;
    compiler->length = (int)strlen(pattern);
    compiler->node_count = 0;
    compiler->prog.count = 0;
    compiler->prog.anchored = (pattern[0] == '^');
    compiler->error = error;
    compiler->error_size = error_size;
    if (error && error_size > 0) error[0] = '\0';

    AstNode *ast = parse_expr(compiler);
    if (compiler->index < compiler->length && (!compiler->error || compiler->error[0] == '\0')) {
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
} Thread;

typedef struct {
    Thread threads[MAX_THREADS];
    int count;
    bool visited[MAX_REGEX_INST];
} ThreadList;

static void add_thread(const ReProgram *prog, ThreadList *list, int pc, int start,
                       const char *text, int pos, int text_len)
{
    if (pc < 0 || pc >= prog->count || list->visited[pc]) {
        return;
    }
    list->visited[pc] = true;

    const ReInst *inst = &prog->code[pc];
    switch (inst->op) {
    case RE_JUMP:
        add_thread(prog, list, inst->target1, start, text, pos, text_len);
        break;
    case RE_SPLIT:
        add_thread(prog, list, inst->target1, start, text, pos, text_len);
        add_thread(prog, list, inst->target2, start, text, pos, text_len);
        break;
    case RE_BOL:
        if (pos == 0 || (pos > 0 && text[pos - 1] == '\n')) {
            add_thread(prog, list, pc + 1, start, text, pos, text_len);
        }
        break;
    case RE_EOL:
        if (pos == text_len || (pos < text_len && text[pos] == '\n')) {
            add_thread(prog, list, pc + 1, start, text, pos, text_len);
        }
        break;
    default:
        if (list->count < MAX_THREADS) {
            list->threads[list->count].pc = pc;
            list->threads[list->count].start = start;
            list->count++;
        }
        break;
    }
}

static bool re_search_prog(const ReProgram *prog, const char *text, int text_len,
                           int from_pos, int *match_start, int *match_end)
{
    ThreadList current;
    ThreadList next;

    for (int i = from_pos; i <= text_len; i++) {
        current.count = 0;
        memset(current.visited, 0, sizeof(current.visited));

        add_thread(prog, &current, 0, i, text, i, text_len);

        int best_start = -1;
        int best_end = -1;

        for (int pos = i; pos <= text_len; pos++) {
            if (current.count == 0) break;

            next.count = 0;
            memset(next.visited, 0, sizeof(next.visited));

            for (int t = 0; t < current.count; t++) {
                int pc = current.threads[t].pc;
                int start = current.threads[t].start;
                const ReInst *inst = &prog->code[pc];

                if (inst->op == RE_MATCH) {
                    if (best_start < 0 || (pos - start > best_end - best_start)) {
                        best_start = start;
                        best_end = pos;
                    }
                    continue;
                }

                if (pos < text_len) {
                    unsigned char ch = (unsigned char)text[pos];
                    bool matched = false;
                    if (inst->op == RE_CHAR && inst->c == (char)ch) matched = true;
                    else if (inst->op == RE_ANY && ch != '\n') matched = true;
                    else if (inst->op == RE_CLASS && class_test(&inst->cls, ch)) matched = true;

                    if (matched) {
                        add_thread(prog, &next, pc + 1, start, text, pos + 1, text_len);
                    }
                }
            }

            current = next;
        }

        if (best_start >= 0) {
            if (match_start) *match_start = best_start;
            if (match_end) *match_end = best_end;
            return true;
        }

        if (prog->anchored) {
            break;
        }
    }
    return false;
}

bool regex_match(const char *pattern, const char *text, char *error, size_t error_size)
{
    ReProgram prog;
    if (!re_compile(pattern, &prog, error, error_size)) {
        return false;
    }
    int len = (int)strlen(text);
    int mstart = 0, mend = 0;
    return re_search_prog(&prog, text, len, 0, &mstart, &mend);
}

ObjArray *regex_find_all(const char *pattern, const char *text, char *error, size_t error_size)
{
    ReProgram prog;
    if (!re_compile(pattern, &prog, error, error_size)) {
        return NULL;
    }

    mem_pause_gc();
    ObjArray *results = object_array();
    int len = (int)strlen(text);
    int pos = 0;

    while (pos <= len) {
        int mstart = 0, mend = 0;
        if (!re_search_prog(&prog, text, len, pos, &mstart, &mend)) {
            break;
        }
        int match_len = mend - mstart;
        ObjString *s = object_string(text + mstart, match_len);
        array_push(results, OBJ_VAL(s));
        if (mend > mstart) {
            pos = mend;
        } else {
            pos = mstart + 1;
        }
    }
    mem_resume_gc();
    return results;
}

ObjString *regex_replace(const char *pattern, const char *replacement, const char *text,
                         char *error, size_t error_size)
{
    ReProgram prog;
    if (!re_compile(pattern, &prog, error, error_size)) {
        return NULL;
    }

    buf_t out;
    buf_init(&out);

    int len = (int)strlen(text);
    int repl_len = (int)strlen(replacement);
    int pos = 0;

    while (pos <= len) {
        int mstart = 0, mend = 0;
        if (!re_search_prog(&prog, text, len, pos, &mstart, &mend)) {
            buf_append(&out, text + pos, (size_t)(len - pos));
            break;
        }

        if (mstart > pos) {
            buf_append(&out, text + pos, (size_t)(mstart - pos));
        }
        buf_append(&out, replacement, (size_t)repl_len);
        if (mend > mstart) {
            pos = mend;
        } else {
            if (mstart < len) {
                buf_append(&out, text + mstart, 1);
                pos = mstart + 1;
            } else {
                break;
            }
        }
    }

    ObjString *result = object_string(out.data ? out.data : "", (int)out.len);
    buf_free(&out);
    return result;
}
