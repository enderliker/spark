# Spark architecture

Spark is a bytecode interpreter written in C11 with no external dependencies. The
path of an execution is:

```
.spark source
   │  lexer.c          tokens with line numbers
   ▼
parser.c  ──►  AST in an arena (ast.c)
   │  compiler.c       variable resolution and emission
   ▼
Chunk (bytecode + constants + lines)
   │  vm.c             value stack, frames and closures
   ▼
result / error
```

## Modules

| File | Responsibility |
| --- | --- |
| `lexer.c` | Tokens, keywords, numbers, strings with escapes and comments. |
| `parser.c` | Recursive descent, precedences, AST nodes and recovery via `longjmp` (first error). |
| `ast.c` | Arena with 8 KiB blocks; the whole AST is freed at once. |
| `compiler.c` | Walks the AST, manages scopes, locals, upvalues and emits bytecode. |
| `chunk.c` | Code, lines and constants arrays; recursive disassembler. |
| `vm.c` | Stack, frames, closures, natives, errors and the garbage collector. |
| `object.c` | Dynamic objects, string interning, arrays, maps and value formatting. |
| `table.c` | Hash table used for the global `name -> slot` map (open addressing, tombstones). |
| `memory.c` | Byte accounting and the GC threshold. |
| `value.h` | `Value` (tag + union) and the `Obj` header. |
| `main.c` | CLI, file reading and REPL. |

## Values and objects

`Value` is a tag-and-union structure:

```c
typedef struct {
    ValueType type;              /* VAL_NIL, VAL_BOOL, VAL_NUMBER, VAL_INT, VAL_OBJ */
    union { bool boolean; double number; int64_t integer; Obj *obj; } as;
} Value;
```

There are two numeric tags: `VAL_NUMBER` (`double`) and `VAL_INT` (`int64_t`).
Arithmetic and comparisons accept both by promoting the integer to a double, but two
integers stay exact. `value.h` has the overflow-checked `int_add_overflow`,
`int_sub_overflow` and `int_mul_overflow` helpers: when a result does not fit in
`int64_t` the operation falls back to a double, so C signed overflow never happens.
Equality (`values_equal`) treats an integer and a whole double in range as equal
(`1 == 1.0`), and `hash_value` canonicalizes a whole double to its integer so `map[1]`
and `map[1.0]` are the same key. In the VM, `value_is_numeric`/`value_as_double` and the
`numeric_less`/`numeric_greater` helpers keep the hot paths free of type branching.

Every dynamic value shares the `Obj` header:

```c
struct Obj {
    ObjType type;
    bool marked;      /* GC mark */
    Obj *next;        /* linked list of all objects */
};
```

Concrete types:

- `ObjString`: FNV-1a hash, length and a NUL-terminated string in a flexible array.
  All strings are **interned** in a 1024-bucket hash table; two equal strings share the
  same object.
- `ObjArray`: dynamic array of `Value`.
- `ObjMap`: open-addressing hash table of `MapEntry` (key, value, state). The state is
  `free`, `occupied` or `tombstone`; it doubles capacity when it fills up and counts
  tombstones to decide when to compact.
- `ObjFunction`: code, arity, number of upvalues and name.
- `ObjClosure`: reference to the function plus the array of captured upvalues.
- `ObjUpvalue`: points to a stack variable or to its closed copy.
- `ObjNative`: C function pointer, arity (`-1` = variadic) and name.

## Bytecode

Each `Chunk` contains:

- `code[]`: instruction and operand bytes.
- `lines[]`: line number of each byte (for errors and traces).
- `constants[]`: constant values, indexed with 16 bits.

The VM is a stack machine. The main instructions:

| Group | Instructions |
| --- | --- |
| Constants and stack | `CONSTANT`, `NIL`, `TRUE`, `FALSE`, `POP`, `DUP`, `DUP2` |
| Variables | `GET_LOCAL`, `SET_LOCAL`, `GET_GLOBAL`, `DEFINE_GLOBAL`, `DEFINE_GLOBAL_CONST`, `SET_GLOBAL`, `GET_UPVALUE`, `SET_UPVALUE` |
| Collections | `ARRAY n`, `MAP n`, `INDEX`, `INDEX_SET`, `LEN`, `TO_STRING`, `ITER_KEYS` |
| Arithmetic and logic | `ADD`, `SUBTRACT`, `MULTIPLY`, `DIVIDE`, `MODULO`, `INT_DIVIDE`, `BIT_AND`, `BIT_OR`, `BIT_XOR`, `SHL`, `SHR`, `EQUAL`, `GREATER`, `LESS`, `NOT`, `NEGATE`, `BIT_NOT` |
| Superinstructions | `ADD_LOCAL_CONST`, `SUBTRACT_LOCAL_CONST`, `MULTIPLY_LOCAL_CONST`, `DIVIDE_LOCAL_CONST`, `LESS_LOCAL_CONST`, `GREATER_LOCAL_CONST`, `JUMP_IF_NOT_LESS_LOCAL_CONST`, `JUMP_IF_NOT_GREATER_LOCAL_CONST`, `RETURN_LOCAL` |
| Control | `JUMP`, `JUMP_IF_FALSE` (peek), `JUMP_IF_FALSE_POP` (consume), `LOOP` (16-bit offsets) |
| Functions | `CALL argc`, `CLOSURE const [upvalues]`, `CLOSE_UPVALUE`, `RETURN` |
| REPL | `PRINT_REPL` |

`JUMP_IF_FALSE` peeks at the top without popping (used by `and`/`or`);
`JUMP_IF_FALSE_POP` consumes the condition and is used by `if`/`while`/`for`, which
avoids an extra `POP` per condition. Jumps are emitted with a `0xffff` marker and
patched once the target is known. `OP_CLOSURE` is followed by an
`(is_local, index)` pair per upvalue.

`OP_ITER_KEYS` normalizes a `for-in` iterable into an array: arrays are left as-is,
maps become an array of keys and strings become an array of one-character strings.
`OP_LEN` leaves the value and pushes its length, which `for-in` uses to initialize its
counter.

### Compiler design

- Each function has its own `Compiler` linked to the previous one, so lexical captures
  are resolved in a single pass.
- Scopes use `scope_depth` and a stack of locals; closing a scope emits `POP` or
  `CLOSE_UPVALUE`.
- Non-local variables are resolved as globals by name at compile time:
  `vm_global_slot(name)` returns a stable 16-bit slot (creating it if needed). The
  `global_names` hash table only maps names to slots, and it persists across REPL
  lines so globals can be reused. At runtime, `GET_GLOBAL`, `SET_GLOBAL`,
  `DEFINE_GLOBAL` and `DEFINE_GLOBAL_CONST` index `vm.global_slots` directly, with no
  hashing.
- Loops keep a list of pending `break` and `continue` jumps to patch. Leaving a block
  with `break` emits the necessary stack cleanups. The loop context chain also holds
  labels, so `break outer`/`continue outer` can target an enclosing loop.
- `switch` evaluates its subject once into a hidden local. Each case has its own
  scope, cases do not fall through, and the hidden local lets `break`, `continue` and
  labelled jumps leave the switch with a balanced stack.
- String interpolation compiles each `${...}` expression, converts it with
  `TO_STRING` and concatenates the parts with `ADD`.
- `const` is tracked per local/upvalue and in the compile context for globals. A
  global constant is defined with `DEFINE_GLOBAL_CONST`, which marks the slot when the
  declaration executes; a failed initializer therefore does not reserve the name. A
  function compiled before a later `const` still fails at runtime through
  `SET_GLOBAL`.
- Compound assignments reuse `DUP2` + `INDEX` so the array and the index are not
  evaluated twice.

### Superinstructions

To cut dispatch overhead (the main cost of a bytecode VM), the compiler folds common
patterns into single instructions whenever the left operand is a local and the right
one is a numeric constant:

- `local +|-|*|/ const` compiles to `*_LOCAL_CONST`.
- `local <|> const` compiles to `LESS_LOCAL_CONST` / `GREATER_LOCAL_CONST`.
- An `if`/`while`/`for` whose condition is `local <|> const` compiles directly to
  `JUMP_IF_NOT_LESS_LOCAL_CONST` / `JUMP_IF_NOT_GREATER_LOCAL_CONST`, which compares
  and branches without pushing a boolean.
- `return local;` compiles to `RETURN_LOCAL`.
- `return a + b;` compiles to `RETURN_ADD`, fusing addition and return.
- `self(local - const)` compiles to `CALL_SELF_SUB_LOCAL_CONST`, avoiding stack moves and global lookups during self-recursive calls.

All of them reuse the same helpers (`compare_values`, `arithmetic_values`,
`add_values`) as the general operations, so error messages and semantics are identical.
The `fib(27)` benchmark went from ~22 ms to ~13 ms with these fusions alone.

### Hot/cold dispatch and direct threading

The execution loop keeps `ip`, the stack top, the current frame's slots and its
constant pool in local variables. A hot path handles the error-free,
allocation-free instructions that dominate numeric loops and recursion:
constants, locals, globals, numeric arithmetic/comparisons, fused
superinstructions, jumps, closure calls and returns. Direct threading (computed goto)
is used on compilers that support labels-as-values (GCC/Clang) to eliminate centralized
indirect jump prediction bottlenecks. Anything else (natives,
allocations, string operations, type errors, undefined globals) jumps to the
general path after writing `ip` and `sp` back into the VM, so the GC and error
reporting always see a consistent state. `CALL` uses the hot path only for a
closure with matching arity and an available frame; `RETURN` does the same
unless it would leave the current `run()` (frame 0 or `base_frame`). The fast
return also skips `close_upvalues` when no upvalue is open. Call frames cache
the constant pool pointer directly to eliminate indirection upon return.
Together with call/return fusions, this brought the `fib(27)` benchmark to
~4.6 ms minimum on the development machine.

## Stack and frames

The value stack is a fixed array of `STACK_MAX` (131072) `Value`. A frame stores the
closure, the instruction pointer, the base slot and the cached constant pool:

```c
typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
    Value *constants;
} CallFrame;
```

For a call to `fn f(a, b)` the stack looks like:

```
... callee  arg1  arg2 | slot0=closure  slot1=a  slot2=b  ...
                        └─ frame->slots
```

On return, `OP_RETURN` closes the frame's open upvalues, discards the stack down to
`slots`, pushes the result and resumes the caller frame. There is a maximum of 256
frames.

### Re-entrant calls from natives

Higher-order natives (`map`, `filter`, `reduce`, `each`, `sort_by`) need to call a
Spark function from C. The helper `call_callback(callee, arg_count, &result)` expects
the arguments already pushed on the value stack and runs the callee to completion:

- The interpreter loop takes the frame index it must stop at (`run(base_frame)`).
  `OP_RETURN`/`OP_RETURN_LOCAL` return early once they pop back to `base_frame`, leaving
  the result on top of the stack instead of resuming an outer frame.
- For a closure, `call_callback` shifts the arguments up one slot to insert the callee,
  records the current frame count as `base_frame`, calls it and runs a nested loop.
- For a native, it calls the function directly and restores the stack.

A native must therefore finish with the stack balanced up to its arguments; the caller
(`call_value`) snapshots the argument base before the call and restores it afterwards,
so any temporary values a native pushed are discarded. Because callbacks can allocate,
a native that keeps an intermediate value across a callback must leave it on the value
stack so the GC can see it.

## Upvalues

An open upvalue points directly at the stack (`location`). The VM keeps a list sorted
by descending address; `capture_upvalue` reuses the existing upvalue if the variable
was already captured. When the scope closes (`OP_CLOSE_UPVALUE`, end of block or
`return`) the value is copied to `closed` and `location` points there, so the closure
keeps working after the frame disappears.

## Garbage collector

It is a non-compacting *mark & sweep*:

1. Roots are marked: the value stack, the frame closures, the global slots (and their
   names), the `global_names` table and the open upvalues.
2. An iterative traversal with a gray stack marks the children (function constants,
   array elements, map entries, upvalues, names).
3. Before sweeping, unmarked strings are removed from the interning table.
4. `sweep` walks the object list, unlinks the unmarked ones and calls `object_free`.
5. The threshold is recomputed as twice the live size, with a 1 MiB minimum.

The GC is paused during compilation (freshly created objects have no root yet) and
around the constructions that allocate several blocks before the object becomes
visible. The stack does not move, so pointers to slots are stable.

A fresh object is only protected while it is reachable from a root. Code that creates
an object and then hands it to a container (`array_push`, `map_set`) must root it
first, because growing the container can trigger a collection. Natives use
`push`/`pop` around those values, and `array_push_new_string` centralizes the pattern
for strings. The same rule applies to a callback result that is stored after
`call_callback` returns.

## Errors

- Every diagnostic has an `ErrorType` category (a name such as `TypeError`) shared by
  the parser, the compiler and the VM, so a future try/catch can dispatch on it.
- The parser and the compiler use `setjmp`/`longjmp`: they abort on the first error and
  return the message, line and column.
- Tokens, AST nodes and bytecode instructions carry a source column, so an error can
  print the offending line and a caret under it. Every function keeps a borrowed
  pointer to its source text, which the VM owns for the whole session.
- Runtime errors print the category, the line, the source snippet and a trace of the
  frames, from the innermost to the script.
- After a runtime error the VM closes the open upvalues, clears the stack and frames
  and leaves the globals intact; the REPL can continue.
- The REPL compiles in `repl_mode`, which turns each expression statement into
  `PRINT_REPL` to show its value.

## Tests

`tests/run_tests.py`:

- runs the programs in `tests/cases` and compares stdout exactly;
- runs the ones in `tests/errors` and checks the exit code and a `stderr` fragment;
- tests the REPL (expressions, persistence, blocks and error recovery);
- tests `--version`, `--help`, `--dump` and a missing file.

There are 103 tests. During development the project was also compiled with
`-fsanitize=address,undefined` and the whole suite ran with no errors or leaks.

`benchmarks/run.py` runs `benchmarks/fib.spark` (fib(27)) seven times and reports the
minimum and median.

## How to extend the language

### Add a native

1. Write `static Value my_native(int argc, Value *args)` in `vm.c`; return a `Value`
   (use `NIL_VAL` if there is no result).
2. Before allocating new objects, make sure the arguments are on the stack (they
   already are) and push the result before releasing anything.
3. Call `define_native("my_native", my_native, arity)` in `vm_init`; use `-1` for a
   variadic arity and validate the arguments by hand.
4. To raise an error, call `vm_runtime_error("...")` and return `NIL_VAL`.

### Add an instruction

1. Add the opcode to the enum in `chunk.h` and to `opcode_name`/the disassembler.
2. Emit it from `compiler.c` with `emit_byte` and, if it has operands, `emit_u16`.
3. Implement its case in the `switch` of `run()` in `vm.c`, keeping the stack balanced.
4. Update the opcode documentation and add a test.