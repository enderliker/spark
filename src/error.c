#include "error.h"

const char *error_type_name(ErrorType type)
{
    switch (type) {
    case ERROR_SYNTAX: return "SyntaxError";
    case ERROR_COMPILE: return "CompileError";
    case ERROR_TYPE: return "TypeError";
    case ERROR_NAME: return "NameError";
    case ERROR_CONST: return "ConstError";
    case ERROR_ZERO_DIVISION: return "ZeroDivisionError";
    case ERROR_INDEX: return "IndexError";
    case ERROR_VALUE: return "ValueError";
    case ERROR_ARITY: return "ArityError";
    case ERROR_ASSERT: return "AssertionError";
    case ERROR_RECURSION: return "RecursionError";
    case ERROR_MEMORY: return "MemoryError";
    case ERROR_RUNTIME: return "RuntimeError";
    }
    return "Error";
}