/* vm.h - stack virtual machine for Spark. */
#ifndef SPARK_VM_H
#define SPARK_VM_H

#include <stdbool.h>

#include "error.h"
#include "value.h"

typedef struct ObjFunction ObjFunction;
typedef struct CallFrame CallFrame;
struct ObjCoroutine;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_YIELD,
    INTERPRET_BLOCKED
} InterpretResult;

void vm_init(void);
void vm_free(void);
void vm_set_args(int argc, char **argv);
void vm_set_debug_mode(bool debug);
bool vm_get_debug_mode(void);
/* Registers a global native. `function` has the NativeFn signature from
 * object.h; spelled out here so vm.h does not depend on object.h. */
void vm_define_native(const char *name, Value (*function)(int arg_count, Value *args),
                      int arity);

/* Scheduler hooks used by the network layer: wake a blocked coroutine (or mark
 * an object while tracing network waiters). */
void vm_wake_coroutine(struct ObjCoroutine *coroutine, Value result, const char *error);
void vm_mark_object(Obj *object);
struct ObjCoroutine *vm_current_coroutine(void);
int vm_run_depth(void);
bool vm_had_error(void);
void vm_block_native(void);
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
bool vm_name_is_native(const char *chars, int length);
int vm_global_count(void);
ObjString *vm_global_slot_obj_name(int slot);
void vm_register_global_slot(int slot, ObjString *name);
const char *vm_add_source(const char *source);

#endif
