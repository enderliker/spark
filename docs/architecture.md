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
| `net.c` | TCP/UDP sockets, name resolution and the network natives. |
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
  All strings are **interned** in a dynamic, growable hash table (starts at 16k buckets
  and doubles when load factor passes 0.75); two equal strings share the same object,
  allowing O(1) pointer-identity equality checks across the VM and maps.
- `ObjArray`: dynamic array of `Value`.
- `ObjMap`: open-addressing hash table of `MapEntry` (key, value, hash, state). The state is
  `free`, `occupied` or `tombstone`; it caches the key hash to avoid recomputation,
  uses specialized fast probing for object/string keys, and doubles capacity when it fills
  up while counting tombstones to decide when to compact.
- `ObjFunction`: code, arity, min_arity (for default parameters), is_varargs, number of upvalues and name.
- `ObjClosure`: reference to the function plus the array of captured upvalues.
- `ObjUpvalue`: points to a stack variable or to its closed copy.
- `ObjNative`: C function pointer, arity (`-1` = variadic) and name.
- `ObjFile`: GC-managed file stream handle (`FILE *stream`, `is_closed`, `path`), automatically closed upon garbage collection if not closed explicitly.
- `ObjSocket`: GC-managed socket descriptor (`fd`, `is_closed`, `NET_TCP`/`NET_UDP`, a stored `timeout_ms` and a small read buffer used by `recv_line`), closed by `close()`, the finalizer or shutdown.
- `ObjServer`: GC-managed listening descriptor for TCP servers, released the same way.
- `ObjCoroutine`: a green thread: private value stack, snapshot of its frames and try handlers, scheduler links, wait state and result.
- `ObjChannel`: FIFO buffer of `Value` (head, count, capacity, `closed`) plus the queues of blocked senders and receivers.

## String concatenation

`ObjString` is an immutable, interned block: it stores the length, a cached FNV-1a hash
and the bytes inline after the header (`char chars[]`). Equal strings share one object,
so equality and map lookups can start from a pointer comparison. `object_string_concat`
continues the hash fold from the left operand instead of re-hashing the prefix, builds
the bytes in a temporary buffer, and returns an existing interned string when the
content already exists.

Because the value is immutable, `s += x` allocates a new string and copies the whole
accumulator on every step, so building an N-character string one piece at a time costs
O(N^2). That is the trade-off Python and Lua make too, and the language keeps it: a
well-behaved implementation cannot mutate the accumulator in place without proving it
has no other reference.

`string_builder` is the sanctioned way to assemble large strings. `ObjStringBuilder`
owns a `malloc`ed buffer that doubles when full, so an append costs amortized O(1) and
building an N-character string is O(N); `sb_to_string` copies the buffer once through
the interning path, so the result is an ordinary immutable string.

Measured on the development machine (`benchmarks/string_build.spark` and the same loop
written with `+=`), 200,000 single-character appends:

| Idiom | Time |
| --- | --- |
| `sb_append(builder, "x")` | ~6 ms (linear) |
| `s += "x"` | ~2.2 s (quadratic) |

Alternatives that were considered and not taken:

- **Appending in place when the accumulator is "uniquely owned".** It needs escape
  tracking (marking every slot, container and call argument that receives the string),
  and a missed site silently produces wrong output once two variables share a buffer.
- **Rope (cons) strings with lazy flattening.** Appends would be O(1) with no ownership
  check, but every read has to flatten, so `->chars` would have to be routed through a
  helper everywhere, and `s += x; print(s)` stays quadratic anyway.
- **Promoting an accumulator variable to a builder at compile time.** Sound, and it
  would make `+=` itself amortized, but it needs new opcodes, materialization on every
  read and a flag check on global reads (which the numeric hot paths share), so the
  explicit builder buys the same guarantee without touching the existing semantics.

`tests/cases/string_builder_scale.spark` guards the amortized behavior: it measures the
same loop at two sizes and fails when quadrupling the work multiplies the time by more
than about 8 (linear is ~4, quadratic ~16).

## Bytecode

Each `Chunk` contains:

- `code[]`: instruction and operand bytes.
- `lines[]`: line number of each byte (for errors and traces).
- `constants[]`: constant values, indexed with 16 bits.

The VM is a stack machine. The main instructions:

| Group | Instructions |
| --- | --- |
| Constants and stack | `CONSTANT`, `NIL`, `TRUE`, `FALSE`, `POP`, `DUP`, `DUP2` |
| Variables | `GET_LOCAL`, `SET_LOCAL`, `GET_LOCAL_16`, `SET_LOCAL_16`, `GET_GLOBAL`, `DEFINE_GLOBAL`, `DEFINE_GLOBAL_CONST`, `SET_GLOBAL`, `GET_UPVALUE`, `SET_UPVALUE` |
| Collections | `ARRAY n`, `ARRAY_PUSH`, `ARRAY_EXTEND`, `MAP n`, `INDEX`, `INDEX_SET`, `LEN`, `TO_STRING`, `ITER_KEYS` |
| Arithmetic and logic | `ADD`, `SUBTRACT`, `MULTIPLY`, `DIVIDE`, `MODULO`, `INT_DIVIDE`, `BIT_AND`, `BIT_OR`, `BIT_XOR`, `SHL`, `SHR`, `EQUAL`, `GREATER`, `LESS`, `NOT`, `NEGATE`, `BIT_NOT` |
| Superinstructions | `ADD_LOCAL_CONST`, `SUBTRACT_LOCAL_CONST`, `MULTIPLY_LOCAL_CONST`, `DIVIDE_LOCAL_CONST`, `LESS_LOCAL_CONST`, `GREATER_LOCAL_CONST`, `JUMP_IF_NOT_LESS_LOCAL_CONST`, `JUMP_IF_NOT_GREATER_LOCAL_CONST`, `RETURN_LOCAL`, `RETURN_ADD`, `CALL_SELF_SUB_LOCAL_CONST` |
| Control | `JUMP`, `JUMP_IF_FALSE` (peek), `JUMP_IF_FALSE_POP` (consume), `LOOP` (16-bit offsets) |
| Functions | `CALL argc`, `CALL_SPREAD`, `TAIL_CALL argc`, `TAIL_CALL_SPREAD`, `DEFAULT_ARG slot offset`, `CLOSURE const [upvalues]`, `CLOSE_UPVALUE`, `RETURN` |
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
- **Tail-call optimization**: When a `return` returns a function call (including branches of a ternary), the compiler emits `OP_TAIL_CALL` or `OP_TAIL_CALL_SPREAD`. The VM closes open upvalues for the current frame, copies the new arguments into the current frame's slots (`frame->slots`), reinitializes the instruction pointer and constants pool, and executes the target function without allocating a new `CallFrame`.
- **Default parameters**: Functions track `min_arity` alongside `arity`. Calling a function with `argc < arity` pads the stack with `nil` and records `frame->argc`. The function preamble contains `OP_DEFAULT_ARG slot offset` for each parameter with a default; if `frame->argc >= slot`, it skips evaluation, otherwise it compiles the default expression and writes it into the local parameter slot.
- **Varargs and spread**: Functions with `...rest` have `is_varargs = true`. Any arguments beyond `arity` are collected into a newly allocated `ObjArray` placed in slot `arity + 1`. Spread calls (`f(...items)`) or spread array literals (`[...items]`) dynamically build and flatten the argument array via `OP_ARRAY_PUSH` and `OP_ARRAY_EXTEND`, followed by `OP_CALL_SPREAD` or `OP_TAIL_CALL_SPREAD`.
- **Constant folding & dead-code elimination**:
  - The compiler statically evaluates constant expressions involving arithmetic (`+`, `-`, `*`, `/`, `%`), bitwise shifts/operations (`<<`, `>>`, `&`, `|`, `^`), unary operators (`-`, `!`, `~`), comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`), string concatenation, ternary operators (`?:`), and null-coalescing (`??`). Potential runtime errors like division by zero or negative shift amounts safely bypass folding to produce precise runtime exceptions.
  - Constant folding also enables superinstruction fusion when the right-hand operand of an expression folds to a numeric constant.
  - Dead code following unconditional control transfers (`return`, `throw`, `break`, `continue`) is dropped without emitting unreachable instructions. Similarly, `if` and `while` statements with constant boolean conditions emit only the active branch, omitting unreachable blocks and jump instructions entirely.

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
allocation-free instructions that dominate loops and recursion:
constants, locals, global reads and writes (`SET_GLOBAL`), upvalues (`GET_UPVALUE`,
`SET_UPVALUE`), indexing (`INDEX`, `INDEX_SET` on arrays and maps),
numeric arithmetic/comparisons (including pure-float and mixed int/float fast paths),
fused superinstructions, jumps, calls (both matching closures and fixed-arity natives)
and returns. Direct threading (computed goto) is used on compilers that support
labels-as-values (GCC/Clang) to eliminate centralized indirect jump prediction bottlenecks.
Anything else (unhandled natives, allocations, complex string operations, type errors,
undefined globals) jumps to the general path after writing `ip` and `sp` back into the VM,
so the GC and error reporting always see a consistent state. `CALL` uses the hot path for
closures with matching arity and available frames, as well as native functions; `RETURN`
does the same unless it would leave the current `run()` (frame 0 or `base_frame`). The fast
return also skips `close_upvalues` when no upvalue is open. Call frames cache the constant
pool pointer directly to eliminate indirection upon return.
Together with call/return fusions and native fast-dispatch, this keeps both recursive and
iterative loops within the register-threaded dispatch path.

## Stack and frames

The value stack is a dynamically growable buffer (`Value *vm.stack`). It starts at
`STACK_INITIAL_CAPACITY` (2048 values) and automatically doubles on demand up to
`STACK_MAX_LIMIT` (4,194,304 values / 64 MB). When the stack is reallocated, all interior
pointers into the stack (`vm.stack_top`, each `vm.frames[i].slots`, and open upvalues
`upvalue->location`) are safely updated via pointer rebasing (`delta = new_stack - old_stack`).
Recursion depth is bounded by `FRAMES_MAX` (4096 frames), and each function supports up to
`MAX_LOCALS` (1024 local variables), using 16-bit operand instructions (`OP_GET_LOCAL_16` and
`OP_SET_LOCAL_16`) for local slots beyond 255.

A frame stores the closure, the instruction pointer, the base slot, and the cached constant pool:

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
`slots`, pushes the result and resumes the caller frame. There is a maximum of 4096
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
5. The threshold is adaptively recomputed based on the survival ratio: if high (>70% survival), the factor scales to 2.5x to avoid thrashing; if low (<30% survival), the factor contracts to 1.5x to bound memory footprint; otherwise 2.0x, with a 1 MiB minimum. Manual cycles can be triggered via `gc()`.

The GC is paused during compilation (freshly created objects have no root yet) and
around the constructions that allocate several blocks before the object becomes
visible. The value stack grows dynamically, rebasing stack pointers and open upvalues
when reallocation occurs.

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

## Networking

The socket layer lives in `src/net.c` and adds no dependencies. `net_init()` registers
the global natives (`tcp_connect`, `tcp_listen`, `udp_bind`, `net_hostname`) from
`vm_init`, and the VM routes method calls on socket and server receivers to
`net_dispatch_method`.

- **Resource model.** `ObjSocket` and `ObjServer` follow `ObjFile`: they own a file
  descriptor, are closed by `close()`, by `object_free` when the collector finalizes
  them, and on `vm_free`. `type()` reports `"socket"`/`"server"` and using a closed
  handle is a runtime error.
- **Timeouts.** Each operation turns its millisecond timeout into an absolute deadline
  on a monotonic clock and waits with `poll(2)` before the blocking syscall (a timeout
  of `0` waits forever). The handshake runs on a temporarily non-blocking descriptor so
  `connect` honours the deadline, and `send` retries short writes until the buffer is
  drained.
- **Resolution.** `getaddrinfo` resolves names, IPv4 literals and bracketed IPv6
  literals; every address in the result list is tried in turn, so `localhost` works over
  either family. Failures become `NetworkError` / `TimeoutError` with a small, stable set
  of messages.
- **Buffering.** Each TCP socket keeps a read buffer so `recv_line` never consumes
  bytes past the newline it returns; `recv` drains that buffer before reading again.
- **Limits.** Reads are capped at 16 MiB and lines default to 64 KiB.

### Asynchronous I/O (phase 16)

The spike settled on `poll(2)` over `epoll`: the descriptor counts are small, `poll`
keeps the layer dependency-free and portable, and a single call already supports the
mix of readers, writers and deadlines. The integration is:

- **Wait state.** `ObjCoroutine` gains `CORO_WAIT_READ`/`CORO_WAIT_WRITE` kinds plus the
  pending operation (`NetWaitOp`), the awaited socket/server, the requested readiness
  bits and an absolute monotonic deadline. `src/net.c` owns the linked list of waiters.
- **Blocking is the fallback.** Each socket operation first tries a non-blocking
  syscall (sockets are `O_NONBLOCK`). `EWOULDBLOCK` registers the coroutine and returns
  `INTERPRET_BLOCKED`; a partial `send` remembers its progress and resumes where it
  stopped. `tcp_connect` stays deadline-driven on a non-blocking descriptor.
- **Wake path.** `vm_scheduler` calls `net_scheduler_poll(false)` after each coroutine
  switch, so a ready descriptor is not starved while other coroutines run, and
  `net_scheduler_poll(true)` when the run queue empties. The poll builds the descriptor
  set, sleeps until the nearest deadline (no fixed granularity) and, for each ready or
  expired waiter, performs its pending operation or raises the deferred error through
  `coroutine_apply_resume`. Woken coroutines are enqueued by `vm_wake_coroutine`.
- **GC.** The waiter list is a root in `mark_roots`, and `ObjCoroutine` traces its
  awaited socket, server and select array, so a waiting socket survives collection.
- **Guard.** A would-block operation inside a callback or module (`run_depth > 1`)
  raises instead of suspending, exactly like channel waits.
- **Tail calls.** `return <native>(...)` normally becomes `OP_TAIL_CALL`, whose frame
  reuse cannot deliver a resumed result; the compiler therefore compiles a tail return
  whose callee is a global native as an ordinary call.

`tests/cases/net_load.spark` runs 32 concurrent echo clients against one server
coroutine, asserts the descriptor count returns to baseline and checks that a 300 ms
idle wait spends almost no CPU time; `tests/cases/net_async.spark` covers `sock.wait`,
`net_select`, two clients on one server coroutine and the callback guard.
`benchmarks/echo.spark` measures 2000 local round-trips.

## Modules and loading

The VM manages modules through a canonical module table (`vm.modules`).
- `import "path";` emits `OP_IMPORT`.
- Path resolution normalizes paths relative to the current file's directory (stored in `function->script_path`) using `realpath()`, appending `.spark` if needed.
- If a module is currently marked `MODULE_LOADING`, a cyclic import is detected and the import succeeds without re-executing the module body, allowing mutual references.
- Once loaded, `MODULE_LOADED` ensures each file executes at most once across the entire process lifetime.
- Modules share the global namespace, making top-level declarations directly accessible across imported files.

## Exception handling and stack unwinding

Exception handling uses a runtime try-handler stack (`TryHandler vm.try_handlers[TRY_HANDLERS_MAX]`):
- `OP_PUSH_CATCH` and `OP_PUSH_FINALLY` record the handler kind, target IP, current frame index, stack depth, and try nesting depth.
- `OP_POP_TRY` removes the active handler when a try block exits normally.
- `throw <value>;` emits `OP_THROW`. When an exception is thrown:
  1. The VM scans `try_handlers` from top to bottom.
  2. If a catch handler is found in the current frame or an outer frame, intervening call frames are unwound, open upvalues for popped local slots are closed via `close_upvalues`, and execution jumps to the handler target IP with the thrown value pushed.
  3. If a finally handler is encountered during unwinding, the exception value is preserved in `vm.exception_value`, the finally block runs, and `OP_END_FINALLY` resumes unwinding if the exception was not caught inside the finally block.
  4. If no handler exists, the VM halts with a runtime error (`uncaught exception`).

## Object-oriented model and methods

Spark uses a prototype-inspired model with class syntactic sugar:
- Maps act as objects. Method calls `receiver.method(args)` emit `OP_INVOKE` (constant name index + argument count).
- `OP_INVOKE` looks up the closure in the receiver map and calls it, setting `CallFrame.self = receiver`.
- Inside method functions, references to `self` resolve to `OP_SELF`, which retrieves `frame->self`. If `frame->self` is not set (e.g. standalone call), a runtime `NameError` is raised.
- `class Name { init(...) { ... } method() { ... } }` desugars into a constructor function named `Name`. Calling `new Name(...)` or `Name(...)` creates a map instance, attaches the class's method closures, runs `init(...)` with `self` bound, and returns the instance.

## Precompiled bytecode (`.sparkc`)

Programs can be serialized to disk via `spark --compile <file> [-o <out>]`:
- Format starts with 4-byte magic `SPRK` and a 32-bit version number.
- Active global variable names are saved to preserve global slot mappings across modules.
- Recursively serializes the chunk bytecode, line/column tables, and constant values (including nested functions and closures).
- When loaded, bytecode is executed directly via `vm_run_function()` without invoking the lexer or parser.

## Developer tooling architecture

Spark provides native tools without external dependencies:

### Interactive Debugger (`src/debugger.c`, `src/debugger.h`)
- Zero overhead in normal execution: VM computed-goto fast path is unmodified; debug mode routes execution through `cold:` dispatch where instruction/line boundaries check breakpoints, step mode, and next mode.
- Local names are recorded in `ObjFunction.debug_locals` during AST compilation.
- Breakpoints support line numbers and `file:line` filters.
- Interactive prompt supports `break`, `delete`, `list`, `continue`, `step`, `next`, `locals`, `stack`, `help`, and `quit`.

### Formatter (`src/formatter.c`, `src/formatter.h`)
- Syntax validation via `parse_source()`.
- Token scanner preserves comments (`//` and `/*...*/`) and string interpolations while normalizing whitespace.
- Generates canonical 4-space indentation and spacing around operators and punctuation.
- Verified idempotent: formatting twice produces byte-identical output without altering semantics.
- Supports stdout emission or in-place `-w` / `--write`.

### Linter (`src/linter.c`, `src/linter.h`)
- Single-pass AST analysis traversing statements and expressions.
- Checks `unused-variable` on local and function parameter symbols (names starting with `_` ignored).
- Checks `unreachable-code` after block terminators (`return`, `throw`, `break`, `continue`).
- Checks `shadowed-variable` when inner declarations shadow enclosing local/param declarations.
- Returns exit code `1` when warnings exist, `0` when clean.

## Tests

`tests/run_tests.py`:

- runs the programs in `tests/cases` and compares stdout exactly;
- runs the ones in `tests/errors` and checks the exit code and a `stderr` fragment;
- tests the REPL (expressions, persistence, blocks and error recovery);
- tests `--version`, `--help`, `--dump`, `--compile`, `--debug`, `--format`, `--lint` and bytecode execution, and error cases.

There are 208 tests. The suite is also run under `-fsanitize=address,undefined`
(both locally and in CI), and CI builds with `-Werror`, checks the benchmark and
publishes coverage. Coverage can be measured locally with `make coverage`.

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