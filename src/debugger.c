/* debugger.c - interactive debugger for the Spark language. */
#include "debugger.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "vm.h"

#define MAX_BREAKPOINTS 128

typedef struct {
    char *file;
    int line;
    bool enabled;
    int id;
} Breakpoint;

typedef enum {
    DBG_STOPPED,
    DBG_STEP,
    DBG_NEXT,
    DBG_CONTINUE
} DebugState;

typedef struct {
    bool enabled;
    DebugState state;
    int next_id;
    Breakpoint breakpoints[MAX_BREAKPOINTS];
    int bp_count;
    int last_line;
    int last_frame_depth;
    int cur_line;
    int cur_depth;
    const char *last_file;
} Debugger;

static Debugger dbg;

void debugger_init(void)
{
    dbg.enabled = false;
    dbg.state = DBG_STOPPED;
    dbg.next_id = 1;
    dbg.bp_count = 0;
    dbg.last_line = -1;
    dbg.last_frame_depth = -1;
    dbg.cur_line = -1;
    dbg.cur_depth = -1;
    dbg.last_file = NULL;
}

void debugger_free(void)
{
    for (int i = 0; i < dbg.bp_count; i++) {
        if (dbg.breakpoints[i].file) {
            free(dbg.breakpoints[i].file);
            dbg.breakpoints[i].file = NULL;
        }
    }
    dbg.bp_count = 0;
}

void debugger_enable(bool enable)
{
    dbg.enabled = enable;
}

bool debugger_is_enabled(void)
{
    return dbg.enabled;
}

static void print_source_line(const char *source, int line)
{
    if (!source || line <= 0) {
        return;
    }
    int cur_line = 1;
    const char *p = source;
    while (*p && cur_line < line) {
        if (*p == '\n') {
            cur_line++;
        }
        p++;
    }
    if (cur_line != line) {
        return;
    }
    const char *start = p;
    while (*p && *p != '\n' && *p != '\r') {
        p++;
    }
    int len = (int)(p - start);
    printf("-> %d | %.*s\n", line, len, start);
}

static int add_breakpoint(const char *file, int line)
{
    if (dbg.bp_count >= MAX_BREAKPOINTS) {
        printf("Too many breakpoints (max %d).\n", MAX_BREAKPOINTS);
        return -1;
    }
    Breakpoint *bp = &dbg.breakpoints[dbg.bp_count++];
    bp->id = dbg.next_id++;
    bp->line = line;
    bp->enabled = true;
    bp->file = file ? strdup(file) : NULL;
    return bp->id;
}

static bool delete_breakpoint(int id)
{
    for (int i = 0; i < dbg.bp_count; i++) {
        if (dbg.breakpoints[i].id == id) {
            if (dbg.breakpoints[i].file) {
                free(dbg.breakpoints[i].file);
            }
            for (int j = i; j < dbg.bp_count - 1; j++) {
                dbg.breakpoints[j] = dbg.breakpoints[j + 1];
            }
            dbg.bp_count--;
            return true;
        }
    }
    return false;
}

static void list_breakpoints(void)
{
    if (dbg.bp_count == 0) {
        printf("No breakpoints set.\n");
        return;
    }
    printf("Breakpoints:\n");
    for (int i = 0; i < dbg.bp_count; i++) {
        Breakpoint *bp = &dbg.breakpoints[i];
        printf("  #%d: %s:%d [%s]\n", bp->id,
               bp->file ? bp->file : "<any>", bp->line,
               bp->enabled ? "enabled" : "disabled");
    }
}

static void prompt_loop(const char *file, int line, CallFrame *frame)
{
    char input[256];

    for (;;) {
        printf("(dbg) ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            /* EOF: continue */
            dbg.state = DBG_CONTINUE;
            break;
        }

        /* Trim leading whitespace */
        char *cmd = input;
        while (*cmd && isspace((unsigned char)*cmd)) {
            cmd++;
        }
        /* Trim trailing whitespace */
        char *end = cmd + strlen(cmd);
        while (end > cmd && isspace((unsigned char)*(end - 1))) {
            *(--end) = '\0';
        }

        if (*cmd == '\0') {
            continue;
        }

        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
            printf("Commands:\n");
            printf("  break <[file:]line>  Set a breakpoint (alias: b)\n");
            printf("  delete <id>          Delete a breakpoint (alias: d)\n");
            printf("  list                 List all breakpoints\n");
            printf("  continue             Continue running (alias: c)\n");
            printf("  step                 Step into next line (alias: s)\n");
            printf("  next                 Step over next line (alias: n)\n");
            printf("  locals               Show local variables (alias: l)\n");
            printf("  stack                Show call stack backtrace (alias: bt)\n");
            printf("  help                 Show this help (alias: h)\n");
            printf("  quit                 Quit debugger (alias: q)\n");
            continue;
        }

        if (strncmp(cmd, "break", 5) == 0 || strncmp(cmd, "b ", 2) == 0 || strcmp(cmd, "b") == 0) {
            char *arg = cmd + (strncmp(cmd, "break", 5) == 0 ? 5 : 1);
            while (*arg && isspace((unsigned char)*arg)) {
                arg++;
            }
            if (*arg == '\0') {
                printf("Usage: break <line> or break <file:line>\n");
                continue;
            }
            char *colon = strchr(arg, ':');
            if (colon) {
                *colon = '\0';
                char *target_file = arg;
                int target_line = atoi(colon + 1);
                if (target_line <= 0) {
                    printf("Invalid line number: %s\n", colon + 1);
                    continue;
                }
                int id = add_breakpoint(target_file, target_line);
                if (id > 0) {
                    printf("Breakpoint %d set at %s:%d\n", id, target_file, target_line);
                }
            } else {
                int target_line = atoi(arg);
                if (target_line <= 0) {
                    printf("Invalid line number: %s\n", arg);
                    continue;
                }
                int id = add_breakpoint(NULL, target_line);
                if (id > 0) {
                    printf("Breakpoint %d set at %d\n", id, target_line);
                }
            }
            continue;
        }

        if (strncmp(cmd, "delete", 6) == 0 || strncmp(cmd, "d ", 2) == 0) {
            char *arg = cmd + (cmd[1] == ' ' ? 2 : 6);
            while (*arg && isspace((unsigned char)*arg)) {
                arg++;
            }
            int id = atoi(arg);
            if (id <= 0 || !delete_breakpoint(id)) {
                printf("No breakpoint found with id %d\n", id);
            } else {
                printf("Breakpoint %d deleted\n", id);
            }
            continue;
        }

        if (strcmp(cmd, "list") == 0) {
            list_breakpoints();
            continue;
        }

        if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "c") == 0) {
            dbg.state = DBG_CONTINUE;
            break;
        }

        if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            dbg.state = DBG_STEP;
            dbg.last_line = line;
            dbg.last_frame_depth = vm_frame_count();
            dbg.last_file = file;
            break;
        }

        if (strcmp(cmd, "next") == 0 || strcmp(cmd, "n") == 0) {
            dbg.state = DBG_NEXT;
            dbg.last_line = line;
            dbg.last_frame_depth = vm_frame_count();
            dbg.last_file = file;
            break;
        }

        if (strcmp(cmd, "locals") == 0 || strcmp(cmd, "l") == 0) {
            vm_print_frame_locals(frame);
            continue;
        }

        if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "bt") == 0) {
            vm_print_stack_backtrace();
            continue;
        }

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            exit(0);
        }

        printf("Unknown command '%s'. Type 'help' for available commands.\n", cmd);
    }
}

void debugger_on_start(ObjFunction *function)
{
    if (!dbg.enabled) {
        return;
    }
    const char *file = function->script_path ? function->script_path->chars : "<script>";
    int line = (function->chunk.count > 0 && function->chunk.lines) ? function->chunk.lines[0] : 1;

    printf("Spark Debugger. Type 'help' for commands, 'continue' or 'c' to run.\n");
    printf("Stopped at %s:%d\n", file, line);
    print_source_line(function->source, line);

    dbg.last_line = line;
    dbg.last_frame_depth = 1;
    dbg.cur_line = line;
    dbg.cur_depth = 1;
    dbg.last_file = file;

    prompt_loop(file, line, vm_current_frame());
}

bool debugger_check_stop(const char *file, int line, CallFrame *frame)
{
    if (!dbg.enabled) {
        return true;
    }

    int frame_depth = vm_frame_count();

    /* Clear last_line once execution advances past the stopped location so loops can re-trigger */
    if (dbg.last_line != -1 && (line != dbg.last_line || frame_depth != dbg.last_frame_depth)) {
        if (dbg.state == DBG_CONTINUE) {
            dbg.last_line = -1;
            dbg.last_frame_depth = -1;
        }
    }

    bool should_stop = false;
    int hit_bp_id = 0;

    /* Check breakpoints */
    for (int i = 0; i < dbg.bp_count; i++) {
        Breakpoint *bp = &dbg.breakpoints[i];
        if (bp->enabled && bp->line == line) {
            if (!bp->file || strstr(file, bp->file) != NULL || strcmp(file, bp->file) == 0) {
                if (line != dbg.last_line || frame_depth != dbg.last_frame_depth) {
                    should_stop = true;
                    hit_bp_id = bp->id;
                    break;
                }
            }
        }
    }

    if (!should_stop) {
        if (dbg.state == DBG_STEP) {
            if (line != dbg.last_line || frame_depth != dbg.last_frame_depth) {
                should_stop = true;
            }
        } else if (dbg.state == DBG_NEXT) {
            if ((line != dbg.last_line && frame_depth <= dbg.last_frame_depth) ||
                frame_depth < dbg.last_frame_depth) {
                should_stop = true;
            }
        }
    }

    dbg.cur_line = line;
    dbg.cur_depth = frame_depth;

    if (!should_stop) {
        return true;
    }

    if (hit_bp_id > 0) {
        printf("Breakpoint %d hit at %s:%d\n", hit_bp_id, file, line);
    } else {
        printf("Stopped at %s:%d\n", file, line);
    }

    /* Print source snippet if available */
    CallFrame *cur = frame ? frame : vm_current_frame();
    const char *src = vm_frame_source(cur);
    if (src) {
        print_source_line(src, line);
    }

    dbg.last_line = line;
    dbg.last_frame_depth = frame_depth;
    dbg.last_file = file;

    prompt_loop(file, line, cur);
    return true;
}
