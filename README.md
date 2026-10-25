# Spark

**Spark** is a programming language created from scratch in C11. Source code is
parsed, compiled to its own **bytecode**, and executed on a **stack-based virtual
machine** with a *mark & sweep* garbage collector. It uses no parser generators and
no external libraries: the whole pipeline lives in `src/`.

```spark
fn fib(n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

for (var i = 0; i < 10; i += 1) {
    print("fib(" + str(i) + ") =", fib(i));
}
```

## Features

- Types: `nil`, `bool`, `int`, `number`, `string`, `array`, `map` and `function`.
- 64-bit integers alongside doubles: literals, promotion, `div`, `%`, bitwise
  operators (`&`, `|`, `^`, `~`, `<<`, `>>`) and `int`/`float`/`is_int`.
- Standard library and I/O: script arguments `argv`, environment variables `env`,
  `exit(code)`, `input(prompt)`, one-shot file operations (`read_file`, `write_file`,
  `append_file`, `file_exists`), file handles (`open`, `read`, `read_line`, `write`,
  `close`, `eof`), PRNG (`rand`, `randint`, `srand`), JSON encoding/decoding
  (`to_json`, `from_json`), and linear-time regular expressions (`regex_match`,
  `regex_find`, `regex_replace`).
- Global and local variables with block-lexical scope; `const` declares immutable
  bindings.
- First-class functions, recursion and **closures** captured by reference.
- `if`/`else`, `while`, `do`/`while`, C-style `for`, `for-in` over arrays, maps and
  strings, labelled `break`/`continue`, and `switch`/`case`/`default` with
  auto-break.
- Dynamic arrays with literals, indexing, `push`, `pop` and `len`; maps with
  literals, `map[key]`, `has`, `del`, `keys`, `values` and `len`.
- Text indexing, `substr` by UTF-8 character and interpolation with `${...}`.
- Arithmetic operators, comparisons (also between strings), short-circuited logic
  (`and`, `or`, `not`), the ternary operator and `??`.
- Decimal, hexadecimal, binary and octal literals with `_` digit separators.
- Compilation to bytecode with already-resolved jumps, a line table and a
  disassembler (`--dump`).
- Virtual machine with call frames, upvalues and error messages that include the
  line and a call trace.
- Fused superinstructions for common operations (local/constant arithmetic,
  comparison-and-branch, direct local returns) to cut dispatch overhead.
- Garbage collector with a pause during compilation and an adaptive threshold.
- REPL with multi-line block support and persistent variables.

## Requirements and build

All you need is a C11 compiler, `make` and the C math library. There are no
third-party dependencies.

```bash
make            # builds the ./spark binary
make test       # runs the test suite (needs python3)
```

On **Windows** the recommended path is WSL (Ubuntu): open a WSL terminal, enter the
project directory and run `make` and `./spark`. On Linux and macOS it works natively.

## Usage

```bash
./spark                       # opens the REPL
./spark examples/fibonacci.spark
./spark --dump examples/hola.spark   # shows the bytecode without running
./spark --help
./spark --version
```

The interpreter returns `0` on success, `64` for incorrect CLI usage, `65` for a
syntax or compilation error, `70` for a runtime error and `74` if it cannot read the
file.

In the REPL every standalone expression prints its value and global variables
persist between lines:

```
spark> 1 + 2
3
spark> var greeting = "hello"
spark> greeting + ", world"
hello, world
spark> exit
```

## Quick language guide

```spark
// Line comment
/* Block
   comment */

var name = "world";
print("hello, " + name);

var numbers = [3, 1, 2];
numbers[0] = 30;
push(numbers, 4);
print(numbers, len(numbers));

var person = {"name": "Ada", "age": 36};
person["age"] += 1;
print(person["name"], has(person, "age"));

fn add(a, b) {
    return a + b;
}

var apply = fn(f, value) {
    return f(value);
};
print(apply(fn(x) { return x * x; }, 5));

var total = 0;
for (var i = 1; i <= 5; i += 1) {
    total += i;
}
for (var key in person) {
    print(key, person[key]);
}
while (total > 10) {
    total -= 10;
}
print(total, type(total), str(total));
```

The full reference is in [docs/language.md](docs/language.md) and the internals are in
[docs/architecture.md](docs/architecture.md).

### Native functions

| Function | Description |
| --- | --- |
| `print(...)` | Prints its arguments separated by spaces and a newline. |
| `len(x)` | Number of elements of an array or map, or UTF-8 characters of a string. |
| `push(array, value)` | Appends an element and returns the array. |
| `pop(array)` | Removes and returns the last element. |
| `str(x)` | Converts any value to text. |
| `num(x)` | Converts a numeric string to a number (always a float). |
| `int(x)` / `float(x)` | Convert between integers and floats (or from a numeric string). |
| `is_int(x)` | Whether the value is an integer. |
| `type(x)` | Returns the type name (`nil`, `bool`, `number`, `string`, `array`, `map`, `function`). |
| `clock()` | Seconds since an arbitrary instant (for measuring). |
| `assert(cond, message?)` | Stops the program with an error if `cond` is false. |
| `has(map, key)` | Whether the map contains the key. |
| `del(map, key)` | Deletes the key; returns whether it existed. |
| `keys(map)` | Array with the map's keys. |
| `values(map)` | Array with the map's values. |
| `range(n)` / `range(start, end)` | Array of integers in `[start, end)`. |
| `substr(text, start, count?)` | Substring by UTF-8 characters. |
| `abs(x)` | Absolute value. |
| `floor(x)` / `ceil(x)` / `round(x)` | Rounding. |
| `sqrt(x)` | Square root (error on negative input). |
| `pow(x, y)` | `x` raised to `y`. |
| `min(a, b)` / `max(a, b)` | Smaller/larger of two numbers. |
| `contains(container, value)` | Membership in an array, substring in a string or key in a map. |
| `index_of(container, value)` | First position (elements or UTF-8 characters) or `-1`. |
| `insert(array, index, value)` | Inserts at `index`; returns the array. |
| `remove(array, index)` | Removes and returns the element at `index`. |
| `reverse(array)` / `fill(array, value)` | Reverses / overwrites in place; returns the array. |
| `slice(array, start, end?)` | New array with `[start, end)`. |
| `starts_with(text, prefix)` / `ends_with(text, suffix)` | Prefix/suffix test. |
| `split(text, sep)` / `join(array, sep)` | Text to array and back (empty separator splits into characters). |
| `trim(text)` / `upper(text)` / `lower(text)` | ASCII whitespace trim and ASCII case conversion. |
| `repeat(text, n)` / `replace(text, old, new)` | Repeat and replace all occurrences. |
| `clear(map)` / `merge(dest, src)` / `get(map, key, default?)` | Map helpers. |
| `map(array, fn)` / `filter(array, fn)` | Transform / select with a callback. |
| `reduce(array, fn, initial?)` / `each(array, fn)` | Fold and iterate with a callback. |
| `sort(array)` / `sort_by(array, fn)` | Sort in place (numbers or strings) with an optional comparator. |

## Project layout

```
src/
  main.c        CLI and REPL
  lexer.c       lexical analysis
  parser.c      syntax analysis and AST construction
  ast.c         AST memory arena
  compiler.c    AST -> bytecode
  chunk.c       bytecode blocks, constants and disassembler
  vm.c          virtual machine, GC, natives and errors
  object.c      dynamic values, interned strings, arrays and maps
  table.c       hash table of global names (name -> slot)
  memory.c      memory accounting and GC trigger
  value.h       value representation
  buf.c         auxiliary dynamic buffer
examples/       example programs
tests/          automated tests and fixtures
benchmarks/     the fib(27) benchmark
```

## How it works

1. **Lexer**: turns the text into tokens with a line number.
2. **Parser**: builds an AST with recursive descent and precedences; nodes are
   allocated in an arena that is freed all at once when compilation finishes.
3. **Compiler**: walks the AST and emits bytecode with local slots, upvalue
   resolution at compile time and jump patching.
4. **VM**: executes the bytecode on a value stack with call frames, closures,
   natives and a *mark & sweep* GC.

The bytecode uses a 16-bit constant index and a line entry per instruction. For
example, `print(1 + 2);` compiles to:

```
== program ==
0000    1 GET_GLOBAL            0 'print'
0003    | CONSTANT              0 '1'
0006    | CONSTANT              1 '2'
0009    | ADD
0010    | CALL                  1
0012    | POP
0013    | NIL
0014    | RETURN
```

## Tests

`tests/run_tests.py` runs valid programs comparing exact output, error cases checking
the exit code and a `stderr` fragment, REPL sessions and the CLI options. There are
103 tests, plus the `-fsanitize=address,undefined` build used during development.

## Benchmark

`benchmarks/run.py` runs `benchmarks/fib.spark` (fib(27)) several times and reports
the minimum and median. The VM uses fused superinstructions and hot/cold dispatch to
keep the minimum around 4.6 ms on the development machine.

## Known limits of this version

- No modules or `import`: each file is a complete program.
- Integers are 64-bit and floats are IEEE doubles; mixed arithmetic and integer
  overflow promote to a float.
- At most 255 parameters and arguments per function, 256 local variables per
  function and 256 nested call frames.
- Assignments are statements (`x = 1;`), not expressions.
- Closures capture the *variable*, not a copy: closures created inside a loop body
  capture a new variable per iteration, but the `for` variable (`var i`) is shared
  across iterations.
- No user-defined exceptions, threads or file I/O.

## Ideas for the future

`import`, native modules for files/networking, and a native code generator from the
bytecode.