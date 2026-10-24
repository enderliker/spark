#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "buf.h"
#include "chunk.h"
#include "compiler.h"
#include "parser.h"
#include "vm.h"

#define SPARK_VERSION "0.1.0"

static void print_help(void)
{
    printf("Spark %s - an interpreted programming language with a virtual machine.\n\n",
           SPARK_VERSION);
    printf("Usage: spark [options] [file.spark]\n\n");
    printf("Options:\n");
    printf("  --dump      compile and show the bytecode without running\n");
    printf("  --help      show this help\n");
    printf("  --version   show the version\n\n");
    printf("Without a file an interactive REPL opens. Type 'salir' to exit.\n");
}

static char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    buf_t contents;

    if (!file) {
        fprintf(stderr, "spark: could not open '%s'\n", path);
        return NULL;
    }

    buf_init(&contents);
    char chunk[8192];
    size_t read_count;

    while ((read_count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (buf_append(&contents, chunk, read_count) != 0) {
            buf_free(&contents);
            fclose(file);
            fprintf(stderr, "spark: file too large\n");
            return NULL;
        }
    }
    if (ferror(file)) {
        fprintf(stderr, "spark: error reading '%s'\n", path);
        buf_free(&contents);
        fclose(file);
        return NULL;
    }
    fclose(file);

    *out_size = contents.len;
    return buf_steal(&contents);
}

static bool source_is_incomplete(const char *source)
{
    int braces = 0;
    bool in_string = false;
    bool escaped = false;
    bool in_line_comment = false;
    bool in_block_comment = false;

    for (const char *p = source; *p; p++) {
        char c = *p;

        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
            }
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && p[1] == '/') {
                in_block_comment = false;
                p++;
            }
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '/' && p[1] == '/') {
            in_line_comment = true;
            p++;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            in_block_comment = true;
            p++;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            braces++;
        } else if (c == '}') {
            braces--;
        }
    }
    return braces > 0 || in_string || in_block_comment;
}

static int run_file(const char *path, bool dump)
{
    size_t size = 0;
    char *source = read_file(path, &size);

    if (!source) {
        return 74;
    }

    if (dump) {
        Arena arena;
        Stmt *program = NULL;
        char error[256];
        int error_line = 0;
        int status = 0;

        arena_init(&arena);
        if (!parse_source(source, &arena, &program, error, sizeof(error), &error_line)) {
            fprintf(stderr, "syntax error on line %d: %s\n", error_line, error);
            status = 65;
        } else {
            ObjFunction *function = compile_ast(program, path, false, error, sizeof(error),
                                                &error_line);

            if (!function) {
                fprintf(stderr, "compilation error on line %d: %s\n", error_line, error);
                status = 65;
            } else {
                chunk_disassemble_function(function);
            }
        }
        arena_free(&arena);
        free(source);
        return status;
    }

    InterpretResult result = vm_interpret(source, path, false);

    free(source);
    return result == INTERPRET_OK ? 0 : (result == INTERPRET_COMPILE_ERROR ? 65 : 70);
}

static void repl(void)
{
    char *line = NULL;
    size_t capacity = 0;
    buf_t pending;

    printf("Spark %s — type code and press Enter. 'salir' or Ctrl+D to exit.\n",
           SPARK_VERSION);
    buf_init(&pending);

    for (;;) {
        fputs(pending.len > 0 ? "... " : "spark> ", stdout);
        fflush(stdout);

        ssize_t length = getline(&line, &capacity, stdin);
        if (length < 0) {
            break;
        }

        if (pending.len == 0) {
            const char *start = line;
            size_t trimmed_len;

            while (*start == ' ' || *start == '\t') {
                start++;
            }
            trimmed_len = strlen(start);
            while (trimmed_len > 0 && (start[trimmed_len - 1] == '\n' ||
                                       start[trimmed_len - 1] == '\r' ||
                                       start[trimmed_len - 1] == ' ')) {
                trimmed_len--;
            }
            if ((trimmed_len == 5 && strncmp(start, "salir", 5) == 0) ||
                (trimmed_len == 4 && strncmp(start, "exit", 4) == 0) ||
                (trimmed_len == 4 && strncmp(start, "quit", 4) == 0)) {
                break;
            }
            if (trimmed_len == 0) {
                continue;
            }
        }

        buf_append(&pending, line, (size_t)length);
        if (source_is_incomplete(pending.data)) {
            continue;
        }

        vm_interpret(pending.data, "<repl>", true);
        buf_clear(&pending);
    }

    free(line);
    buf_free(&pending);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    bool dump = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("Spark %s\n", SPARK_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--dump") == 0) {
            dump = true;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "spark: unknown option '%s' (try --help)\n", argv[i]);
            return 64;
        }
        if (file) {
            fprintf(stderr, "spark: only one file can be run at a time\n");
            return 64;
        }
        file = argv[i];
    }

    if (dump && !file) {
        fprintf(stderr, "spark: --dump needs a file\n");
        return 64;
    }

    vm_init();

    int status;
    if (file) {
        status = run_file(file, dump);
    } else {
        repl();
        status = 0;
    }

    vm_free();
    return status;
}
