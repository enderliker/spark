/* vm.h - stack virtual machine for Chispa. */
#ifndef CHISPA_VM_H
#define CHISPA_VM_H

#include <stdbool.h>

#include "value.h"

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

void vm_init(void);
void vm_free(void);

/* Parses, compiles and runs the source code. In REPL mode standalone
 * expressions are printed and global variables are kept between calls. */
InterpretResult vm_interpret(const char *source, const char *script_name, bool repl_mode);

void vm_runtime_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Stable index of a global variable (created if it does not exist), or -1 if
 * there is no room. The compiler uses the index as the operand of the global
 * access instructions. */
int vm_global_slot(ObjString *name);
const char *vm_global_slot_name(int slot);

#endif
