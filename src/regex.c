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

typedef struct {
    const char *pattern;
    int index;
    int length;
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

static int emit(ReCompiler *c, ReOpcode op)
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

/* Forward declarations */
static int compile_expr(ReCompiler *c);

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

static int compile_atom(ReCompiler *c)
{
    char ch = re_peek(c);
    if (ch == '\0' || ch == '|' || ch == ')' || ch == '*' || ch == '+' || ch == '?') {
        return -1;
    }

    if (ch == '(') {
        re_advance(c);
        int start = compile_expr(c);
        if (re_peek(c) != ')') {
            re_error(c, "unclosed parenthesis");
            return -1;
        }
        re_advance(c);
        return start;
    }

    if (ch == '.') {
        re_advance(c);
        return emit(c, RE_ANY);
    }

    if (ch == '^') {
        re_advance(c);
        return emit(c, RE_BOL);
    }

    if (ch == '$') {
        re_advance(c);
        return emit(c, RE_EOL);
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
                for (int code = (unsigned char)cur; code <= (unsigned char)end_char; code++) {
                    class_set(&cls, (unsigned char)code);
                }
            } else {
                class_set(&cls, (unsigned char)cur);
            }
        }

        if (re_peek(c) != ']') {
            re_error(c, "unclosed character class");
            return -1;
        }
        re_advance(c);

        if (negated) {
            CharClass neg;
            for (int i = 0; i < 32; i++) {
                neg.bits[i] = (uint8_t)~cls.bits[i];
            }
            cls = neg;
        }

        int idx = emit(c, RE_CLASS);
        if (idx >= 0) c->prog.code[idx].cls = cls;
        return idx;
    }

    if (ch == '\\') {
        re_advance(c);
        char esc = re_advance(c);
        if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
            CharClass cls;
            memset(&cls, 0, sizeof(cls));
            add_shorthand(&cls, esc);
            int idx = emit(c, RE_CLASS);
            if (idx >= 0) c->prog.code[idx].cls = cls;
            return idx;
        }
        char literal = esc;
        if (esc == 'n') literal = '\n';
        else if (esc == 't') literal = '\t';
        else if (esc == 'r') literal = '\r';
        int idx = emit(c, RE_CHAR);
        if (idx >= 0) c->prog.code[idx].c = literal;
        return idx;
    }

    re_advance(c);
    int idx = emit(c, RE_CHAR);
    if (idx >= 0) c->prog.code[idx].c = ch;
    return idx;
}

static int compile_piece(ReCompiler *c)
{
    int atom_start = c->prog.count;
    int start = compile_atom(c);
    if (start < 0) return -1;

    char q = re_peek(c);
    if (q == '*' || q == '+' || q == '?') {
        re_advance(c);
        int atom_end = c->prog.count;

        if (q == '*') {
            /* split L_atom, L_exit */
            /* code for atom */
            /* jump L_split */
            /* L_exit */
            int split_idx = emit(c, RE_SPLIT);
            int jump_idx = emit(c, RE_JUMP);
            int exit_idx = c->prog.count;

            /* Shift atom to sit between split and jump */
            ReInst atom_copy[MAX_REGEX_INST];
            int atom_len = atom_end - atom_start;
            memcpy(atom_copy, &c->prog.code[atom_start], sizeof(ReInst) * (size_t)atom_len);

            c->prog.code[atom_start].op = RE_SPLIT;
            c->prog.code[atom_start].target1 = atom_start + 1;
            c->prog.code[atom_start].target2 = exit_idx;

            memcpy(&c->prog.code[atom_start + 1], atom_copy, sizeof(ReInst) * (size_t)atom_len);

            c->prog.code[atom_start + 1 + atom_len].op = RE_JUMP;
            c->prog.code[atom_start + 1 + atom_len].target1 = atom_start;

            (void)split_idx;
            (void)jump_idx;
            return atom_start;
        } else if (q == '+') {
            /* atom */
            /* split atom_start, exit */
            int split_idx = emit(c, RE_SPLIT);
            c->prog.code[split_idx].target1 = atom_start;
            c->prog.code[split_idx].target2 = c->prog.count;
            return atom_start;
        } else if (q == '?') {
            /* split atom, exit */
            int exit_idx = c->prog.count + 1;
            ReInst atom_copy[MAX_REGEX_INST];
            int atom_len = atom_end - atom_start;
            memcpy(atom_copy, &c->prog.code[atom_start], sizeof(ReInst) * (size_t)atom_len);

            emit(c, RE_SPLIT); /* space for split */
            c->prog.code[atom_start].op = RE_SPLIT;
            c->prog.code[atom_start].target1 = atom_start + 1;
            c->prog.code[atom_start].target2 = exit_idx;

            memcpy(&c->prog.code[atom_start + 1], atom_copy, sizeof(ReInst) * (size_t)atom_len);
            return atom_start;
        }
    }
    return start;
}

static int compile_branch(ReCompiler *c)
{
    int start = -1;
    while (re_peek(c) != '\0' && re_peek(c) != '|' && re_peek(c) != ')') {
        int piece = compile_piece(c);
        if (piece < 0) break;
        if (start < 0) start = piece;
    }
    return start;
}

static int compile_expr(ReCompiler *c)
{
    int left = compile_branch(c);
    if (re_peek(c) == '|') {
        re_advance(c);
        int jump_left = emit(c, RE_JUMP);
        int right = compile_expr(c);

        int exit_idx = c->prog.count;
        c->prog.code[jump_left].target1 = exit_idx;

        /* Move left & jump into a split */
        int left_len = jump_left - left + 1;
        ReInst copy[MAX_REGEX_INST];
        memcpy(copy, &c->prog.code[left], sizeof(ReInst) * (size_t)left_len);

        emit(c, RE_SPLIT);
        c->prog.code[left].op = RE_SPLIT;
        c->prog.code[left].target1 = left + 1;
        c->prog.code[left].target2 = right + 1;

        memcpy(&c->prog.code[left + 1], copy, sizeof(ReInst) * (size_t)left_len);
        (void)right;
    }
    return left;
}

static bool re_compile(const char *pattern, ReProgram *out, char *error, size_t error_size)
{
    ReCompiler compiler;
    compiler.pattern = pattern;
    compiler.index = 0;
    compiler.length = (int)strlen(pattern);
    compiler.prog.count = 0;
    compiler.prog.anchored = (pattern[0] == '^');
    compiler.error = error;
    compiler.error_size = error_size;
    if (error && error_size > 0) error[0] = '\0';

    compile_expr(&compiler);
    emit(&compiler, RE_MATCH);

    if (compiler.error && compiler.error[0] != '\0') {
        return false;
    }

    *out = compiler.prog;
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
        pos = mend > mstart ? mend : mstart + 1;
    }
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
        pos = mend > mstart ? mend : mstart + 1;
    }

    ObjString *result = object_string(out.data ? out.data : "", (int)out.len);
    buf_free(&out);
    return result;
}
