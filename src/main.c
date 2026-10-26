#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "buf.h"
#include "bytecode.h"
#include "chunk.h"
#include "compiler.h"
#include "error.h"
#include "parser.h"
#include "vm.h"

#define SPARK_VERSION "0.1.0"

static void print_help(void)
{
    printf("Spark %s - an interpreted programming language with a virtual machine.\n\n",
           SPARK_VERSION);
    printf("Usage: spark [options] [file.spark | file.sparkc]\n\n");
    printf("Options:\n");
    printf("  --compile <file>  compile source to bytecode file (.sparkc)\n");
    printf("  -o <output>       output file for --compile (defaults to <file>.sparkc)\n");
    printf("  --dump            compile and show the bytecode without running\n");
    printf("  --help            show this help\n");
    printf("  --version         show the version\n\n");
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

static int compile_to_file(const char *in_path, const char *out_path)
{
    size_t size = 0;
    char *source = read_file(in_path, &size);
    if (!source) {
        return 74;
    }

    Arena arena;
    Stmt *program = NULL;
    char error[256];
    int error_line = 0;
    int error_column = 0;
    int status = 0;

    arena_init(&arena);
    if (!parse_source(source, &arena, &program, error, sizeof(error), &error_line, &error_column)) {
        error_print(ERROR_SYNTAX, error_line, error_column, source, error);
        status = 65;
    } else {
        ObjFunction *function = compile_ast(program, in_path, source, false, error,
                                            sizeof(error), &error_line, &error_column);
        if (!function) {
            error_print(ERROR_COMPILE, error_line, error_column, source, error);
            status = 65;
        } else {
            char default_out[4096];
            if (!out_path) {
                size_t len = strlen(in_path);
                if (len > 6 && strcmp(in_path + len - 6, ".spark") == 0) {
                    snprintf(default_out, sizeof(default_out), "%.*s.sparkc", (int)(len - 6), in_path);
                } else {
                    snprintf(default_out, sizeof(default_out), "%s.sparkc", in_path);
                }
                out_path = default_out;
            }
            if (!bytecode_save(function, out_path, error, sizeof(error))) {
                fprintf(stderr, "spark: %s\n", error);
                status = 74;
            }
        }
    }

    arena_free(&arena);
    free(source);
    return status;
}

static int run_file(const char *path, bool dump)
{
    if (bytecode_is_bytecode_file(path) ||
        (strlen(path) > 7 && strcmp(path + strlen(path) - 7, ".sparkc") == 0)) {
        char error[256];
        ObjFunction *function = bytecode_load(path, error, sizeof(error));
        if (!function) {
            fprintf(stderr, "spark: %s\n", error);
            return 65;
        }
        if (dump) {
            chunk_disassemble_function(function);
            return 0;
        }
        InterpretResult result = vm_run_function(function, path);
        return result == INTERPRET_OK ? 0 : (result == INTERPRET_COMPILE_ERROR ? 65 : 70);
    }

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
        int error_column = 0;
        int status = 0;

        arena_init(&arena);
        if (!parse_source(source, &arena, &program, error, sizeof(error), &error_line,
                          &error_column)) {
            error_print(ERROR_SYNTAX, error_line, error_column, source, error);
            status = 65;
        } else {
            ObjFunction *function = compile_ast(program, path, source, false, error,
                                                sizeof(error), &error_line, &error_column);

            if (!function) {
                error_print(ERROR_COMPILE, error_line, error_column, source, error);
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
    const char *compile_file = NULL;
    const char *output_file = NULL;
    bool dump = false;
    int script_arg_index = argc;

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
        if (strcmp(argv[i], "--compile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "spark: --compile requires a file argument\n");
                return 64;
            }
            compile_file = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "spark: -o requires a file argument\n");
                return 64;
            }
            output_file = argv[++i];
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "spark: unknown option '%s' (try --help)\n", argv[i]);
            return 64;
        }
        file = argv[i];
        script_arg_index = i;
        break;
    }

    if (compile_file) {
        vm_init();
        int status = compile_to_file(compile_file, output_file);
        vm_free();
        return status;
    }

    if (dump && !file) {
        fprintf(stderr, "spark: --dump needs a file\n");
        return 64;
    }

    vm_init();
    if (file && !dump) {
        vm_set_args(argc - script_arg_index, &argv[script_arg_index]);
    }

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
