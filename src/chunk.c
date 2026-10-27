#include "chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "vm.h"

void chunk_init(Chunk *chunk)
{
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->columns = NULL;
    chunk->constants_count = 0;
    chunk->constants_capacity = 0;
    chunk->constants = NULL;
}

void chunk_free(Chunk *chunk)
{
    mem_realloc(chunk->code, sizeof(uint8_t) * (size_t)chunk->capacity, 0);
    mem_realloc(chunk->lines, sizeof(int) * (size_t)chunk->capacity, 0);
    mem_realloc(chunk->columns, sizeof(int) * (size_t)chunk->capacity, 0);
    mem_realloc(chunk->constants, sizeof(Value) * (size_t)chunk->constants_capacity, 0);
    chunk_init(chunk);
}

void chunk_write(Chunk *chunk, uint8_t byte, int line, int column)
{
    if (chunk->count + 1 > chunk->capacity) {
        int capacity = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
        chunk->code = mem_realloc(chunk->code, sizeof(uint8_t) * (size_t)chunk->capacity,
                                  sizeof(uint8_t) * (size_t)capacity);
        chunk->lines = mem_realloc(chunk->lines, sizeof(int) * (size_t)chunk->capacity,
                                   sizeof(int) * (size_t)capacity);
        chunk->columns = mem_realloc(chunk->columns, sizeof(int) * (size_t)chunk->capacity,
                                     sizeof(int) * (size_t)capacity);
        chunk->capacity = capacity;
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->columns[chunk->count] = column;
    chunk->count++;
}

int chunk_add_constant(Chunk *chunk, Value value)
{
    /* Deduplicate constants with the same type and value: repeated numeric or
     * text literals then share one slot, which shrinks both the in-memory
     * constant table and the serialized .sparkc file. Numbers compare by bits
     * so 0.0 and -0.0 keep their distinct representations. */
    for (int i = 0; i < chunk->constants_count; i++) {
        Value existing = chunk->constants[i];

        if (existing.type != value.type) {
            continue;
        }
        if (value.type == VAL_NUMBER) {
            if (memcmp(&existing.as.number, &value.as.number, sizeof(double)) == 0) {
                return i;
            }
            continue;
        }
        if (values_equal(existing, value)) {
            return i;
        }
    }
    if (chunk->constants_count + 1 > chunk->constants_capacity) {
        int capacity = chunk->constants_capacity < 8 ? 8 : chunk->constants_capacity * 2;
        chunk->constants = mem_realloc(chunk->constants,
                                       sizeof(Value) * (size_t)chunk->constants_capacity,
                                       sizeof(Value) * (size_t)capacity);
        chunk->constants_capacity = capacity;
    }
    chunk->constants[chunk->constants_count] = value;
    return chunk->constants_count++;
}

const char *opcode_name(OpCode op)
{
    switch (op) {
    case OP_CONSTANT: return "CONSTANT";
    case OP_NIL: return "NIL";
    case OP_TRUE: return "TRUE";
    case OP_FALSE: return "FALSE";
    case OP_POP: return "POP";
    case OP_DUP: return "DUP";
    case OP_DUP2: return "DUP2";
    case OP_GET_LOCAL: return "GET_LOCAL";
    case OP_SET_LOCAL: return "SET_LOCAL";
    case OP_GET_LOCAL_16: return "GET_LOCAL_16";
    case OP_SET_LOCAL_16: return "SET_LOCAL_16";
    case OP_GET_GLOBAL: return "GET_GLOBAL";
    case OP_DEFINE_GLOBAL: return "DEFINE_GLOBAL";
    case OP_DEFINE_GLOBAL_CONST: return "DEFINE_GLOBAL_CONST";
    case OP_SET_GLOBAL: return "SET_GLOBAL";
    case OP_IMPORT: return "IMPORT";
    case OP_GET_UPVALUE: return "GET_UPVALUE";
    case OP_SET_UPVALUE: return "SET_UPVALUE";
    case OP_INDEX: return "INDEX";
    case OP_INDEX_SET: return "INDEX_SET";
    case OP_INDEX_INC: return "INDEX_INC";
    case OP_ARRAY: return "ARRAY";
    case OP_MAP: return "MAP";
    case OP_ITER_KEYS: return "ITER_KEYS";
    case OP_ITER_PAIR: return "ITER_PAIR";
    case OP_LEN: return "LEN";
    case OP_TO_STRING: return "TO_STRING";
    case OP_EQUAL: return "EQUAL";
    case OP_GREATER: return "GREATER";
    case OP_LESS: return "LESS";
    case OP_GREATER_OR_EQUAL: return "GREATER_OR_EQUAL";
    case OP_LESS_OR_EQUAL: return "LESS_OR_EQUAL";
    case OP_ADD: return "ADD";
    case OP_SUBTRACT: return "SUBTRACT";
    case OP_MULTIPLY: return "MULTIPLY";
    case OP_DIVIDE: return "DIVIDE";
    case OP_MODULO: return "MODULO";
    case OP_INT_DIVIDE: return "INT_DIVIDE";
    case OP_BIT_AND: return "BIT_AND";
    case OP_BIT_OR: return "BIT_OR";
    case OP_BIT_XOR: return "BIT_XOR";
    case OP_SHL: return "SHL";
    case OP_SHR: return "SHR";
    case OP_POW: return "POW";
    case OP_IN: return "IN";
    case OP_INSTANCEOF: return "INSTANCEOF";
    case OP_SLICE: return "SLICE";
    case OP_ADD_LOCAL_CONST: return "ADD_LOCAL_CONST";
    case OP_SUBTRACT_LOCAL_CONST: return "SUBTRACT_LOCAL_CONST";
    case OP_MULTIPLY_LOCAL_CONST: return "MULTIPLY_LOCAL_CONST";
    case OP_DIVIDE_LOCAL_CONST: return "DIVIDE_LOCAL_CONST";
    case OP_LESS_LOCAL_CONST: return "LESS_LOCAL_CONST";
    case OP_GREATER_LOCAL_CONST: return "GREATER_LOCAL_CONST";
    case OP_RETURN_LOCAL: return "RETURN_LOCAL";
    case OP_NOT: return "NOT";
    case OP_NEGATE: return "NEGATE";
    case OP_BIT_NOT: return "BIT_NOT";
    case OP_PRINT_REPL: return "PRINT_REPL";
    case OP_JUMP: return "JUMP";
    case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
    case OP_JUMP_IF_FALSE_POP: return "JUMP_IF_FALSE_POP";
    case OP_JUMP_IF_NOT_LESS_LOCAL_CONST: return "JUMP_IF_NOT_LESS_LC";
    case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST: return "JUMP_IF_NOT_GREATER_LC";
    case OP_LOOP: return "LOOP";
    case OP_DEFAULT_ARG: return "DEFAULT_ARG";
    case OP_ARRAY_PUSH: return "ARRAY_PUSH";
    case OP_ARRAY_EXTEND: return "ARRAY_EXTEND";
    case OP_CALL: return "CALL";
    case OP_NEW: return "NEW";
    case OP_CALL_SPREAD: return "CALL_SPREAD";
    case OP_NEW_SPREAD: return "NEW_SPREAD";
    case OP_TAIL_CALL: return "TAIL_CALL";
    case OP_TAIL_CALL_SPREAD: return "TAIL_CALL_SPREAD";
    case OP_CLOSURE: return "CLOSURE";
    case OP_CLOSE_UPVALUE: return "CLOSE_UPVALUE";
    case OP_RETURN: return "RETURN";
    case OP_RETURN_ADD: return "RETURN_ADD";
    case OP_CALL_SELF_SUB_LOCAL_CONST: return "CALL_SELF_SUB_LC";
    case OP_THROW: return "THROW";
    case OP_PUSH_CATCH: return "PUSH_CATCH";
    case OP_PUSH_FINALLY: return "PUSH_FINALLY";
    case OP_POP_TRY: return "POP_TRY";
    case OP_END_FINALLY: return "END_FINALLY";
    case OP_INVOKE: return "INVOKE";
    case OP_INVOKE_SPREAD: return "INVOKE_SPREAD";
    case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST: return "JUMP_IF_NOT_LE_LC";
    case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST: return "JUMP_IF_NOT_GE_LC";
    case OP_JUMP_IF_EQUAL_LOCAL_CONST: return "JUMP_IF_EQ_LC";
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST: return "JUMP_IF_NE_LC";
    case OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL: return "JUMP_IF_NOT_LESS_LL";
    case OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL: return "JUMP_IF_NOT_GREATER_LL";
    case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL: return "JUMP_IF_NOT_LE_LL";
    case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL: return "JUMP_IF_NOT_GE_LL";
    case OP_JUMP_IF_EQUAL_LOCAL_LOCAL: return "JUMP_IF_EQ_LL";
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL: return "JUMP_IF_NE_LL";
    case OP_SELF: return "SELF";
    }
    return "?";
}

static int simple_instruction(const char *name, int offset)
{
    printf("%-18s\n", name);
    return offset + 1;
}

static int constant_instruction(const Chunk *chunk, const char *name, int offset)
{
    int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];

    printf("%-18s %4d '", name, index);
    value_print_repr(stdout, chunk->constants[index]);
    printf("'\n");
    return offset + 3;
}

static int byte_instruction(const Chunk *chunk, const char *name, int offset)
{
    (void)chunk;
    printf("%-18s %4d\n", name, chunk->code[offset + 1]);
    return offset + 2;
}

static int global_instruction(const Chunk *chunk, const char *name, int offset)
{
    int slot = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];

    printf("%-18s %4d '%s'\n", name, slot, vm_global_slot_name(slot));
    return offset + 3;
}

static int jump_instruction(const Chunk *chunk, const char *name, int sign, int offset)
{
    int jump = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
    printf("%-18s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

int chunk_disassemble_instruction(const Chunk *chunk, int offset)
{
    printf("%04d ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    OpCode op = (OpCode)chunk->code[offset];
    switch (op) {
    case OP_CONSTANT:
        return constant_instruction(chunk, opcode_name(op), offset);
    case OP_IMPORT:
        return constant_instruction(chunk, opcode_name(op), offset);
    case OP_GET_GLOBAL:
    case OP_DEFINE_GLOBAL:
    case OP_DEFINE_GLOBAL_CONST:
    case OP_SET_GLOBAL:
        return global_instruction(chunk, opcode_name(op), offset);
    case OP_CLOSURE: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        ObjFunction *function = AS_FUNCTION(chunk->constants[index]);
        int next = offset + 3;

        printf("%-18s %4d '", "CLOSURE", index);
        value_print_repr(stdout, chunk->constants[index]);
        printf("'\n");
        for (int i = 0; i < function->upvalue_count; i++) {
            printf("%04d    | upvalue %s %d\n", next, chunk->code[next] ? "local" : "external",
                   chunk->code[next + 1]);
            next += 2;
        }
        return next;
    }
    case OP_GET_LOCAL_16:
    case OP_SET_LOCAL_16: {
        int slot = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        printf("%-18s %4d\n", opcode_name(op), slot);
        return offset + 3;
    }
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_UPVALUE:
    case OP_SET_UPVALUE:
    case OP_CALL:
    case OP_NEW:
    case OP_TAIL_CALL:
    case OP_RETURN_LOCAL:
    case OP_INDEX_INC:
        return byte_instruction(chunk, opcode_name(op), offset);
    case OP_ADD_LOCAL_CONST:
    case OP_SUBTRACT_LOCAL_CONST:
    case OP_MULTIPLY_LOCAL_CONST:
    case OP_DIVIDE_LOCAL_CONST:
    case OP_LESS_LOCAL_CONST:
    case OP_GREATER_LOCAL_CONST:
    case OP_CALL_SELF_SUB_LOCAL_CONST: {
        int slot = chunk->code[offset + 1];
        int index = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];

        printf("%-18s %4d '", opcode_name(op), slot);
        value_print_repr(stdout, chunk->constants[index]);
        printf("'\n");
        return offset + 4;
    }
    case OP_JUMP_IF_NOT_LESS_LOCAL_CONST:
    case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST:
    case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST: {
        int slot = chunk->code[offset + 1];
        int index = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
        int jump = (chunk->code[offset + 4] << 8) | chunk->code[offset + 5];

        printf("%-18s %4d '", opcode_name(op), slot);
        value_print_repr(stdout, chunk->constants[index]);
        printf("' -> %d\n", offset + 6 + jump);
        return offset + 6;
    }
    case OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL:
    case OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL:
    case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL:
    case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL:
    case OP_JUMP_IF_EQUAL_LOCAL_LOCAL:
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL: {
        int a = chunk->code[offset + 1];
        int b = chunk->code[offset + 2];
        int jump = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];

        printf("%-18s %4d %4d -> %d\n", opcode_name(op), a, b, offset + 5 + jump);
        return offset + 5;
    }
    case OP_PUSH_CATCH:
    case OP_PUSH_FINALLY:
    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_JUMP_IF_FALSE_POP:
        return jump_instruction(chunk, opcode_name(op), 1, offset);
    case OP_LOOP:
        return jump_instruction(chunk, opcode_name(op), -1, offset);
    case OP_DEFAULT_ARG: {
        int slot = chunk->code[offset + 1];
        int jump = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
        printf("%-18s %4d -> %d\n", opcode_name(op), slot, offset + 4 + jump);
        return offset + 4;
    }
    case OP_ARRAY:
    case OP_MAP: {
        int count = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        printf("%-18s %4d\n", opcode_name(op), count);
        return offset + 3;
    }
    case OP_INVOKE: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        int count = chunk->code[offset + 3];
        printf("%-18s %4d '", opcode_name(op), index);
        value_print_repr(stdout, chunk->constants[index]);
        printf("' (%d args)\n", count);
        return offset + 4;
    }
    case OP_INVOKE_SPREAD: {
        int index = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        printf("%-18s %4d '", opcode_name(op), index);
        value_print_repr(stdout, chunk->constants[index]);
        printf("' (spread)\n");
        return offset + 3;
    }
    default:
        return simple_instruction(opcode_name(op), offset);
    }
}

void chunk_disassemble(const Chunk *chunk, const char *name)
{
    printf("== %s ==\n", name);
    for (int offset = 0; offset < chunk->count;) {
        offset = chunk_disassemble_instruction(chunk, offset);
    }
}

void chunk_disassemble_function(const ObjFunction *function)
{
    const char *name = function->name ? function->name->chars : "<anonymous>";

    chunk_disassemble(&function->chunk, name);
    for (int i = 0; i < function->chunk.constants_count; i++) {
        Value value = function->chunk.constants[i];

        if (is_obj_type(value, OBJ_FUNCTION)) {
            printf("\n");
            chunk_disassemble_function(AS_FUNCTION(value));
        }
    }
}
