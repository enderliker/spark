/* chunk.h - bytecode blocks with constants and line numbers. */
#ifndef SPARK_CHUNK_H
#define SPARK_CHUNK_H

#include "value.h"

typedef struct ObjFunction ObjFunction;

typedef enum {
    OP_CONSTANT,      /* u16: index into the constant table */
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_DUP,           /* duplicates the top value */
    OP_DUP2,          /* duplicates the two top values */
    OP_GET_LOCAL,     /* u8: slot */
    OP_SET_LOCAL,     /* u8: slot */
    OP_GET_GLOBAL,    /* u16: constant with the name */
    OP_DEFINE_GLOBAL, /* u16 */
    OP_DEFINE_GLOBAL_CONST, /* u16; defines the slot and marks it constant */
    OP_SET_GLOBAL,    /* u16 */
    OP_GET_UPVALUE,   /* u8 */
    OP_SET_UPVALUE,   /* u8 */
    OP_INDEX,
    OP_INDEX_SET,
    OP_ARRAY,         /* u16: number of elements */
    OP_MAP,           /* u16: number of key/value pairs */
    OP_ITER_KEYS,     /* turns the iterable into an array of keys/values */
    OP_LEN,           /* length of an array, map or string */
    OP_TO_STRING,     /* converts the top value to a string */
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    /* Superinstructions: left operand in a local, right operand a numeric
     * constant. They avoid two dispatches per operation. */
    OP_ADD_LOCAL_CONST,      /* u8: slot, u16: constant */
    OP_SUBTRACT_LOCAL_CONST, /* u8: slot, u16: constant */
    OP_MULTIPLY_LOCAL_CONST, /* u8: slot, u16: constant */
    OP_DIVIDE_LOCAL_CONST,   /* u8: slot, u16: constant */
    OP_LESS_LOCAL_CONST,     /* u8: slot, u16: constant */
    OP_GREATER_LOCAL_CONST,  /* u8: slot, u16: constant */
    OP_RETURN_LOCAL,         /* u8: slot; returns the local directly */
    OP_NOT,
    OP_NEGATE,
    OP_PRINT_REPL,    /* prints and discards (REPL mode only) */
    OP_JUMP,          /* u16: offset */
    OP_JUMP_IF_FALSE, /* u16; does not consume the condition (used in and/or) */
    OP_JUMP_IF_FALSE_POP, /* u16; consumes the condition (if/while/for) */
    OP_JUMP_IF_NOT_LESS_LOCAL_CONST,    /* u8 slot, u16 const, u16 offset */
    OP_JUMP_IF_NOT_GREATER_LOCAL_CONST, /* u8 slot, u16 const, u16 offset */
    OP_LOOP,          /* u16: backward offset */
    OP_CALL,          /* u8: arguments */
    OP_CLOSURE,       /* u16 constant + (is_local, index) pairs */
    OP_CLOSE_UPVALUE,
    OP_RETURN
} OpCode;

typedef struct {
    int count;
    int capacity;
    uint8_t *code;
    int *lines;
    int *columns;
    int constants_count;
    int constants_capacity;
    Value *constants;
} Chunk;

void chunk_init(Chunk *chunk);
void chunk_free(Chunk *chunk);
void chunk_write(Chunk *chunk, uint8_t byte, int line, int column);
int chunk_add_constant(Chunk *chunk, Value value);
void chunk_disassemble(const Chunk *chunk, const char *name);
void chunk_disassemble_function(const ObjFunction *function);
int chunk_disassemble_instruction(const Chunk *chunk, int offset);

const char *opcode_name(OpCode op);

#endif
