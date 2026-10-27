/* debugger.h - interactive debugger for the Spark language. */
#ifndef SPARK_DEBUGGER_H
#define SPARK_DEBUGGER_H

#include <stdbool.h>

typedef struct ObjFunction ObjFunction;
typedef struct CallFrame CallFrame;

void debugger_init(void);
void debugger_free(void);
void debugger_enable(bool enable);
bool debugger_is_enabled(void);

/* Called at program start */
void debugger_on_start(ObjFunction *function);

/* Called before each instruction in debug mode.
 * Returns false if user entered 'quit'. */
bool debugger_check_stop(const char *file, int line, CallFrame *frame);

#endif
