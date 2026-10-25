# Spark language reference

This guide describes version 0.1.0. Spark is an interpreted language: it is first
compiled to bytecode and then run on a stack-based virtual machine.

## 1. Lexicon

### Comments

```spark
// until the end of the line
/* block
   nestable */
```

### Literals

```spark
nil
true
false
42
3.14
1.5e3
"text with \"quotes\", \\ and \n"
[1, "two", true]
{"key": 1, "other": [2, 3]}
```

Strings support the escapes `\n`, `\t`, `\r`, `\\`, `\"` and `\0`. Any other escape
is a syntax error.

Strings can interpolate expressions with `${...}`. The expression is converted to
text and concatenated with the surrounding literal; any expression is allowed. Use
`$$` for a literal dollar sign:

```spark
var name = "Spark";
print("hello ${name}, x=${1 + 2}");   // hello Spark, x=3
print("cost: $$5");                    // cost: $5
print("${ "nested ${name}" }");        // nested Spark
```

### Identifiers

They start with a letter (including `á`, `ñ`, etc.), `_` or a valid UTF-8
continuation byte; they continue with letters, digits and `_`. The keywords are:

```
var const fn if else while for in return break continue do switch case default
true false nil and or not
```

## 2. Types

| Type | Examples | Truthy/falsey |
| --- | --- | --- |
| `nil` | `nil` | falsey |
| `bool` | `true`, `false` | by value |
| `int` | `42`, `-7`, `0xFF` | always truthy (including `0`) |
| `number` | `3.14`, `1.5e3` | always truthy (including `0`) |
| `string` | `"hello"` | always truthy (including `""`) |
| `array` | `[1, 2]` | always truthy |
| `map` | `{"a": 1}` | always truthy |
| `function` | `fn(x) { return x; }`, `print` | always truthy |

There are two numeric types. An **integer** is a signed 64-bit value; a **number** is
a double-precision float. `type()` reports `"number"` for both, and `is_int(x)` tells
them apart. `int(x)` and `float(x)` convert between them.

```spark
print(type(1), type(1.0), is_int(1), is_int(1.0));   // number number true false
print(1 + 2, 10 / 4, 10 div 4);                      // 3 2.5 2
```

- A literal **without** a `.` or an exponent is an integer (`42`, `0xFF`, `1_000`); one
  with a `.` or an exponent is a float (`42.0`, `1e3`).
- Arithmetic between two integers stays an integer. Mixing an integer with a float
  promotes both to float, and an integer result that would not fit in 64 bits also
  promotes to float, so there is never a silent signed overflow:

```spark
print(9223372036854775807);        //  9223372036854775807
print(9223372036854775807 + 1);    //  9.22337203685478e+18  (promoted to a float)
```

- `1` and `1.0` compare equal with `==`, and `map[1]` and `map[1.0]` use the same key.
- Literals can be decimal (`42`, `3.5`, `1e3`), hexadecimal (`0xff`), binary (`0b1010`)
  or octal (`0o17`). Underscores may separate digits (`1_000_000`, `0xFF_FF`, `1e1_0`);
  a separator must sit between two digits.

Arrays and maps can hold any value, including other arrays and functions. Functions,
arrays and maps are compared by identity; strings by content. Integers use the same
equality rules as numbers (`1 == 1.0`, but `2^53 + 1 != 2^53` as a float).

## 3. Operators

From highest to lowest precedence:

| Operators | Associativity | Description |
| --- | --- | --- |
| `()` `[]` | left | call and indexing |
| `!` `not` `-` `~` | right | logical, arithmetic and bitwise negation |
| `*` `/` `%` `div` | left | multiplication, division, modulo and integer division |
| `+` `-` | left | addition/concatenation and subtraction |
| `<<` `>>` | left | bit shifts |
| `<` `<=` `>` `>=` | left | comparison of numbers or strings |
| `==` `!=` | left | equality |
| `&` | left | bitwise AND |
| `^` | left | bitwise XOR |
| `|` | left | bitwise OR |
| `and` | left | short-circuited conjunction |
| `or` | left | short-circuited disjunction |
| `??` | right | null-coalescing |
| `?:` | right | conditional expression |

`+` adds two numbers or concatenates two strings; it does not mix types. `/` always
performs floating-point division; `div` is truncating integer division and requires two
integers (`7 div 2` is `3`, `-7 div 2` is `-3`). `%` is the remainder (`-7 % 2` is
`-1`). Dividing or taking the remainder by zero is a runtime error. `and` and `or`
return one of their operands:

```spark
print(nil or "default");   // default
print(1 and 2);            // 2
print(false and 1);        // false
```

The bitwise operators `&`, `|`, `^`, `~`, `<<` and `>>` work on integers only; any
other operand is a runtime error. `>>` is an arithmetic shift (it keeps the sign) and
the shift count must be between `0` and `63`. They also have compound-assignment forms
(`x &= y`, `x |= y`, `x ^= y`, `x <<= y`, `x >>= y`):

```spark
print(5 & 3, 5 | 3, 5 ^ 3, ~5);   // 1 7 6 -6
print(1 << 4, -16 >> 2);          // 16 -4
print(1 << 2 + 3);                // 32  (shift binds looser than +)
print(2 + 3 & 1);                 // 1   (bitwise AND binds looser than +)
```

The conditional operator evaluates only the selected branch and is
right-associative:

```spark
print(age >= 18 ? "adult" : "minor");
print(a ? b : c ? d : e);   // a ? b : (c ? d : e)
```

`??` returns its left operand unless it is `nil`, in which case it evaluates and
returns the right one. Only `nil` triggers the fallback; `false`, `0` and `""` are
returned unchanged. It is right-associative and binds tighter than `?:`:

```spark
print(value ?? "default");
print(nil ?? nil ?? "third");   // third
```

## 4. Variables and scope

```spark
var global = 1;

{
    var local = 2;
    print(global + local);
}
```

- `var name;` creates the variable with `nil`.
- `const name = value;` creates an immutable binding. The initializer is required
  and redefining the name is an error. Reassignment (directly or with a compound
  operator) is a compile error when the declaration is already visible; inside a
  function compiled before the constant, it is a runtime error. The value itself is
  not frozen: a `const` array or map can still be mutated.
- At the top level of a program, variables are global; inside a block or function they
  are local.
- Blocks create a new scope; an outer variable can be shadowed by one with the same
  name.
- Reading a local variable inside its own initializer is a compile error; a
  nonexistent global gives a runtime error.

### Assignments

Assignments are **statements**, not expressions:

```spark
x = 10;
x += 1;      // x = x + 1
x -= 2;
x *= 3;
x /= 2;
bits &= mask;   // also |= ^= <<= >>=
array[0] = 5;
array[1] += 10;
map["key"] = 42;
map["key"] += 1;
```

The left-hand side must be a variable or an indexing. `print(x = 1)` is a syntax
error.

## 5. Control flow

```spark
if (condition) {
    ...
} else if (other) {
    ...
} else {
    ...
}

while (condition) {
    ...
}

do {
    ...
} while (condition);   // the body runs at least once

for (var i = 0; i < 10; i += 1) {
    if (i == 3) { continue; }
    if (i == 8) { break; }
}

for (;;) {           // all three parts are optional
    break;
}
```

`for-in` iterates over the elements of an array, the keys of a map or the characters
of a string:

```spark
for (var value in [10, 20, 30]) {
    print(value);
}

for (var key in {"a": 1, "b": 2}) {
    print(key);
}

for (var character in "spark") {
    print(character);
}
```

### switch

```spark
switch (value) {
    case 1:
        print("one");
    case 2:
        print("two");
    default:
        print("other");
}
```

- The subject is evaluated once and compared with each `case` value using `==`.
- Case values are expressions evaluated in order until one matches.
- Cases do **not** fall through: only the matching body runs and it ends
  automatically. An empty case body does nothing.
- `default` may appear anywhere and runs only when no case matches.
- `break` exits the switch; `continue` targets the enclosing loop. Use a loop label
  to break out of an outer loop.

### Labelled loops

`while`, `do/while`, `for` and `for-in` accept a label. `break label` and
`continue label` target that loop instead of the innermost one:

```spark
outer: for (var i = 0; i < 3; i += 1) {
    for (var j = 0; j < 3; j += 1) {
        if (j == 1) { continue outer; }
        if (i == 2) { break outer; }
    }
}
```

The label must be followed by a loop, and the loop it names must be enclosing the
`break`/`continue`; otherwise it is a compile error.

Any value can be used as a condition: `nil` and `false` are falsey; everything else is
truthy. `break` may appear inside a loop or a `switch`; `continue` only inside a loop;
`return` only inside a function.

## 6. Functions and closures

```spark
fn add(a, b) {
    return a + b;
}

var double = fn(x) { return x * 2; };
print(add(2, 3), double(4));

fn make_counter() {
    var total = 0;
    return fn(increment) {
        total += increment;
        return total;
    };
}

var counter = make_counter();
print(counter(5), counter(3));   // 5 8
```

- Functions are values: they can be stored in variables, passed as arguments and
  returned.
- A function without `return` returns `nil`.
- The number of arguments must exactly match the arity; otherwise it is a runtime
  error.
- Closures capture **variables** by reference, so two closures created in the same
  call share the state of the captured variable.
- The variable of a `for (var i = ...)` is a single one for the whole loop. Variables
  declared inside the body are new each iteration:

```spark
var fs = [];
for (var i = 0; i < 3; i += 1) {
    var value = i;
    push(fs, fn() { return value; });
}
print(fs[0](), fs[1](), fs[2]());   // 0 1 2
```

## 7. Arrays

```spark
var a = [1, 2, 3];
print(len(a), a[0], a[2]);
a[1] = 20;
a[0] += 10;
push(a, 4);
print(a, pop(a));
```

- Indexes must be non-negative integers within range.
- Arrays are passed by reference (they are objects).
- `len` counts elements; on strings it counts UTF-8 characters.
- The array helpers `contains`, `index_of`, `insert`, `remove`, `reverse`, `fill`,
  `slice`, `sort`, `sort_by`, `map`, `filter`, `reduce` and `each` are documented in
  section 10.

## 8. Maps

```spark
var person = {"name": "Ada", "age": 36};
print(person["name"], len(person));
person["age"] += 1;
person["city"] = "London";

print(has(person, "age"), has(person, "height"));
print(person["missing"]);          // nil for a missing key
print(del(person, "age"));         // true
print(keys(person), values(person));
```

- Keys can be any value: numbers, strings, arrays, maps, functions, etc. Values are
  compared with the same equality rules as `==` (identity for objects, content for
  strings).
- Reading a missing key returns `nil`; it is not an error.
- `has`, `del`, `keys`, `values` and `len` operate on maps.
- `get(map, key, default?)`, `clear(map)`, `merge(dest, src)` and
  `contains(map, key)` are documented in section 10.
- Maps are passed by reference.

## 9. Strings

Strings are UTF-8 and immutable. Indexing returns a one-character string; `substr`
returns a substring. Positions are counted in characters, not bytes.

```spark
print("spark"[0], "spark"[5]);   // c a
print(substr("abcdef", 1, 3));     // bcd
print(substr("program", 5));       // am
print(len("ñandú"));               // 5
print(split("a,b,c", ","));        // ["a", "b", "c"]
print(upper("hola"), trim("  x  "));
```

- A negative, non-integer or out-of-range index is a runtime error.
- `substr(text, start, count?)` starts at `start` and takes `count` characters (or the
  rest if omitted). Negative `start`/`count` are errors; `start` beyond the end
  returns `""`.
- `split`, `join`, `trim`, `upper`, `lower`, `repeat`, `replace`, `starts_with`,
  `ends_with`, `contains` and `index_of` are documented in section 10. `upper` and
  `lower` only change ASCII letters; `trim` only removes ASCII whitespace. Other
  characters are left untouched.

## 10. Natives

| Call | Result |
| --- | --- |
| `print(a, b, ...)` | Prints the values separated by a space. |
| `len(x)` | Length of an array, a map or a string. |
| `push(array, value)` | Appends to the end and returns the array. |
| `pop(array)` | Removes and returns the last element; error if empty. |
| `str(x)` | Text representing the value. |
| `num(x)` | Number from a numeric string (always a float). |
| `int(x)` | Integer from a number or numeric string; floats truncate toward zero. |
| `float(x)` | Float from an integer or numeric string. |
| `is_int(x)` | `true` when the value is an integer. |
| `type(x)` | `"nil"`, `"bool"`, `"number"`, `"string"`, `"array"`, `"map"` or `"function"`. |
| `clock()` | Monotonic clock seconds, for measuring. |
| `assert(cond, message?)` | Runtime error if `cond` is falsey. |
| `has(map, key)` | Whether the map contains the key. |
| `del(map, key)` | Deletes the key and returns whether it existed. |
| `keys(map)` | Array with the map's keys. |
| `values(map)` | Array with the map's values. |
| `range(n)` / `range(start, end)` | Array of integers in `[start, end)`. |
| `substr(text, start, count?)` | Substring by UTF-8 characters. |
| `contains(container, value)` | `true` if an array has the value, a string has the substring, or a map has the key. |
| `index_of(container, value)` | First index (in array elements or UTF-8 characters) or `-1`. |
| `insert(array, index, value)` | Inserts at `index`; returns the array. |
| `remove(array, index)` | Removes the element at `index` and returns it. |
| `reverse(array)` | Reverses the array in place; returns it. |
| `fill(array, value)` | Sets every element to `value`; returns the array. |
| `slice(array, start, end?)` | New array with `[start, end)`; indexes are clamped, negatives are errors. |
| `starts_with(text, prefix)` / `ends_with(text, suffix)` | Prefix/suffix test. |
| `split(text, sep)` | Array of parts; an empty separator splits into characters. |
| `join(array, sep)` | Text joining the stringified elements. |
| `trim(text)` | Removes leading and trailing ASCII whitespace. |
| `upper(text)` / `lower(text)` | ASCII upper/lower case. |
| `repeat(text, n)` | `text` repeated `n` times. |
| `replace(text, old, new)` | Replaces every occurrence of `old`. |
| `clear(map)` | Empties the map; returns it. |
| `merge(dest, src)` | Copies `src` entries into `dest` (src wins); returns `dest`. |
| `get(map, key, default?)` | Value for `key`, or the default (or `nil`) when absent. |
| `map(array, fn)` / `filter(array, fn)` | New array with `fn(x)` applied / kept when truthy. |
| `reduce(array, fn, initial?)` | Left fold with `fn(acc, x)`; without `initial`, uses the first element. |
| `each(array, fn)` | Calls `fn(x)` for every element; returns `nil`. |
| `sort(array)` | Sorts numbers or strings in place; mixing types is an error. |
| `sort_by(array, fn)` | Sorts in place with `fn(x, y)` returning whether `x` goes before `y`. |
| `abs(x)` | Absolute value; an integer stays an integer. |
| `floor(x)` / `ceil(x)` / `round(x)` | Rounding; an integer input is returned unchanged. |
| `sqrt(x)` | Square root (error on negative input). |
| `pow(x, y)` | `x` raised to `y`. |
| `min(a, b)` / `max(a, b)` | Smaller/larger of two numbers; returns the chosen operand. |
| `exit(code?)` | Flushes stdout/stderr and terminates the program with the given exit code (default 0). |
| `env(name?)` | Value of an environment variable as string or `nil`; without arguments, returns a map of all variables. |
| `input(prompt?)` | Reads a line from standard input (without trailing newline); returns `nil` on EOF. |
| `file_exists(path)` | Whether the file exists at `path`. |
| `read_file(path)` | Reads entire file contents as a string. |
| `write_file(path, data)` | Writes string data to file (truncating existing contents). |
| `append_file(path, data)` | Appends string data to file. |
| `open(path, mode?)` | Opens file and returns a file handle object (default mode `"r"`). |
| `read_line(file)` | Reads next line from file handle; returns `nil` on EOF. |
| `read(file, bytes?)` | Reads up to `bytes` from file handle, or until EOF if omitted. |
| `write(file, data)` | Writes string data to file handle; returns count of bytes written. |
| `close(file)` | Closes file handle; using a closed handle is an error. |
| `eof(file)` | Whether file handle has reached end of file. |
| `rand()` | Float in `[0.0, 1.0)`. |
| `randint(max)` / `randint(min, max)` | Random integer in `[min, max]` (inclusive). |
| `srand(seed)` | Seeds the PRNG with an integer or float. |
| `to_json(value)` | Serializes a value to a JSON string. |
| `from_json(text)` | Deserializes a JSON string into Spark values. |
| `regex_match(pattern, text)` | Whether `pattern` matches `text`. |
| `regex_find(pattern, text)` | Array of all non-overlapping matches of `pattern` in `text`. |
| `regex_replace(pattern, repl, text)` | Replaces non-overlapping matches with `repl`. |

The script arguments are accessible through the global array `argv`, where `argv[0]`
is the script file path and subsequent elements are arguments passed on the command line
(empty in the REPL).

The higher-order natives (`map`, `filter`, `reduce`, `each`, `sort_by`) accept a
callback that may be a closure or another native (for example `map(a, str)`). `sort`
and `sort_by` use a stable insertion sort; `sort_by`'s comparator `fn(x, y)` returns a
truthy value when `x` must come before `y`.

## 11. Errors

Every diagnostic starts with a category name. Syntax and compilation errors are
reported before anything runs:

```
SyntaxError at line 3: expected ';' after the declaration
   3 | var x = 1
     |         ^
```

Runtime errors show the failing source line with a caret and the call trace:

```
ZeroDivisionError at line 4: division by zero
   4 |     return a / b;
     |            ^
  in divide (line 4)
  in program.spark (line 7)
```

The categories are:

| Category | Raised when |
| --- | --- |
| `SyntaxError` | the source is malformed |
| `CompileError` | the program is valid syntax but semantically wrong |
| `TypeError` | an operation receives a value of the wrong type |
| `NameError` | a global name is not defined |
| `ConstError` | assigning to or redefining a constant at runtime |
| `ZeroDivisionError` | division or modulo by zero |
| `IndexError` | an array or string index is out of range |
| `ValueError` | a value is outside the accepted domain (negative sizes, bad conversion) |
| `ArityError` | a function is called with the wrong number of arguments |
| `AssertionError` | `assert` fails |
| `RecursionError` | the call-depth limit is reached |
| `MemoryError` | an allocation fails or a result would be too large |
| `RuntimeError` | any other runtime failure |

Interpreter exit codes:

| Code | Meaning |
| --- | --- |
| `0` | Successful run |
| `64` | Incorrect command-line usage |
| `65` | Syntax or compilation error |
| `70` | Runtime error |
| `74` | Could not read the file |

## 12. REPL

With no arguments, `spark` opens a REPL. Every expression statement prints its value,
global variables persist and errors do not close the session. Type `salir`, `exit` or
`quit` (or Ctrl+D) to finish. Lines accumulate while there are unclosed braces, so
multi-line functions can be defined.

## 13. Limits

- Integers are 64-bit; a literal or result outside that range becomes a float.
- 255 parameters, arguments and elements per array literal.
- 256 local variables per function.
- 256 call frames (deeper recursion produces a controlled error).
- 16-bit constant index per function.
- No modules, exceptions, threads or file I/O.