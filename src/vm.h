/* vm.h - stack virtual machine for Spark. */
#ifndef SPARK_VM_H
#define SPARK_VM_H

#include <stdbool.h>

#include "error.h"
#include "value.h"

typedef struct ObjFunction ObjFunction;
typedef struct CallFrame CallFrame;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

void vm_init(void);
void vm_free(void);
void vm_set_args(int argc, char **argv);
void vm_set_debug_mode(bool debug);
bool vm_get_debug_mode(void);
int vm_frame_count(void);
CallFrame *vm_current_frame(void);
const char *vm_frame_source(CallFrame *frame);
void vm_print_frame_locals(CallFrame *frame);
void vm_print_stack_backtrace(void);

/* Parses, compiles and runs the source code. In REPL mode standalone
 * expressions are printed and global variables are kept between calls. */
InterpretResult vm_interpret(const char *source, const char *script_name, bool repl_mode);

/* Executes a precompiled ObjFunction in the VM. */
InterpretResult vm_run_function(ObjFunction *function, const char *script_name);

void vm_runtime_error(ErrorType type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Stable index of a global variable (created if it does not exist), or -1 if
 * there is no room. The compiler uses the index as the operand of the global
 * access instructions. */
int vm_global_slot(ObjString *name);
const char *vm_global_slot_name(int slot);
bool vm_global_is_const(ObjString *name);
int vm_global_count(void);
ObjString *vm_global_slot_obj_name(int slot);
void vm_register_global_slot(int slot, ObjString *name);
const char *vm_add_source(const char *source);

#endif
