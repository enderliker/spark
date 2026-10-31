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
true false nil and or not import try catch finally throw class new extends
static super instanceof yield
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
| `coroutine` | `spawn(worker)` | always truthy |
| `channel` | `channel()` | always truthy |

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
- The global constant `nan` represents IEEE 754 NaN (Not a Number). Following IEEE 754,
  `nan == nan` is `false`. Use the native `is_nan(x)` to test if a numeric value is NaN.

Arrays and maps can hold any value, including other arrays and functions. Functions,
arrays and maps are compared by identity; strings by content. Integers use the same
equality rules as numbers (`1 == 1.0`, but `2^53 + 1 != 2^53` as a float).

## 3. Operators

From highest to lowest precedence:

| Operators | Associativity | Description |
| --- | --- | --- |
| `()` `[]` `.` `?.` | left | call, indexing/slicing and member access |
| `++` `--` (postfix) | left | increment and decrement |
| `!` `not` `-` `~` `++` `--` (prefix) | right | logical, arithmetic and bitwise negation; increment and decrement |
| `**` | right | exponentiation |
| `*` `/` `%` `div` | left | multiplication, division, modulo and integer division |
| `+` `-` | left | addition/concatenation and subtraction |
| `<<` `>>` | left | bit shifts |
| `<` `<=` `>` `>=` | left | comparison of numbers or strings |
| `in` `instanceof` | left | membership and type test |
| `==` `!=` | left | equality |
| `&` | left | bitwise AND |
| `^` | left | bitwise XOR |
| `\|` | left | bitwise OR |
| `and` | left | short-circuited conjunction |
| `or` | left | short-circuited disjunction |
| `??` | right | null-coalescing |
| `?:` | right | conditional expression |
| `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` `??=` | n/a | assignment (section 4; statements, not expressions) |

`**` is exponentiation and is right-associative, so `2 ** 3 ** 2` is `2 ** (3 ** 2)`,
`512`. Unary minus binds **tighter** than `**`, so `-2 ** 2` is `(-2) ** 2`, `4`; Python
reads the same expression as `-4`. A negative exponent is a float (`2 ** -1` is `0.5`).

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

`in` tests membership: an array by value, a string by substring, a map by key. Any other
container is a runtime error. `instanceof` tests whether the left operand is an instance
of the class on the right, subclasses included; `nil` and values that are not instances
give `false` rather than an error. Both bind looser than the comparisons and tighter
than `==`:

```spark
print("a" in ["a", "b"], "ar" in "spark", "k" in {"k": 1});   // true true true
print(2 in [1, 2] == true);   // true   ((2 in [1, 2]) == true)
print(1 < 2 in [true]);       // true   ((1 < 2) in [true])
print(row instanceof Record); // true for a subclass of Record as well
```

`++` and `--` increment and decrement a variable or an element of an array or a map.
They are **expressions**, so unlike the assignments in section 4 they can appear inside
another expression; the postfix form yields the old value and the prefix form the new
one:

```spark
var i = 0;
print(i++, i, ++i);      // 0 1 2
var counts = {"a": 1};
counts["a"]++;
print(counts);           // {"a": 2}
```

A field of an instance is not a valid target: `self.n++` is a runtime error, so write
`self.n = self.n + 1`.

`a.b` reads a field or calls a method; `a?.b` and `a?.b(...)` are the same reads and
calls, except that they yield `nil` when `a` is `nil` instead of raising. The safe form
guards only the access it is written on, so every link of a longer chain needs its own
`?.`:

```spark
print(user?.name);           // nil when user is nil
print(user?.getName());      // the call form short-circuits the same way
print(user?.address?.city);  // nil when either link is nil
print(counts?.len());        // method form on a map
```

An array literal can inline another array with the spread operator (`[1, ...others, 4]`)
and a call can unpack arguments with it (`f(...args)`); section 6 describes both. Short
lambdas have a concise form for a single expression: `fn(x) => x * 2` (section 6).

### Indexing and slicing

`value[index]` reads one element: an element of an array, or one UTF-8 character of a
string. The index must be an integer, and a negative index counts from the end (`a[-1]`
is the last element, `"spark"[-1]` is `"k"`). An index outside the range is a runtime
`IndexError`.

`value[start:end:step]` builds a **new** array or string out of the elements in
`[start, end)` taken every `step` characters or elements (default `1`). Every part is
optional: `a[:]` copies, `a[::2]` takes every other element, `a[::-1]` reverses. Negative
positions count from the end, and a position outside the range is **clamped** instead of
raising. The step must not be `0` (a runtime error). Only arrays and strings can be
sliced, and a slice is a value rather than a place: `a[1:3] = x` is a syntax error.

```spark
var a = [1, 2, 3, 4, 5];
print(a[1], a[-1]);            // 2 5
print(a[1:3], a[:2], a[2:]);   // [2, 3] [1, 2] [3, 4, 5]
print(a[::2], a[::-1]);        // [1, 3, 5] [5, 4, 3, 2, 1]
print(a[10:20]);               // []      (clamped, not an error)
print("spark"[1:3], "spark"[::-1]);   // pa kraps
```

The rule to keep in mind: **indexing is exact and raises when the position is out of
range, while slicing clamps**. Sections 7 and 9 refer back here.

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
cache ??= {};   // assigns only when the target is nil
array[0] = 5;
array[1] += 10;
map["key"] = 42;
map["key"] += 1;
instance.field = 3;
instance.field += 1;
```

The left-hand side must be a variable, an indexing or an instance field. `print(x = 1)`
and `print(x += 1)` are syntax errors, and sequencing `a = b = c` is not available.
`++` and `--` are the exception to the rule: they are expressions (section 3), so they
can appear inside another expression, but their target may only be a variable or an
array or map element - an instance field is a runtime error, so use
`instance.field += 1`.

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
- **Short lambdas**: `fn(x) => expression` is a one-expression function, equivalent to
  `fn(x) { return expression; }`. It accepts the same parameters (defaults and varargs
  included) and may be called immediately, as in `(fn(x) => x * 2)(5)`. The body must be
  an expression: `fn(x) => { ... }` is a syntax error because `{` starts a map literal,
  so anything statement-shaped needs the full form.
- A function without `return` returns `nil`.
- **Default parameters**: `fn f(a, b = 10)` allows omitting trailing arguments; default
  expressions are evaluated per call and may reference earlier parameters. Parameters
  without defaults cannot follow a parameter with a default value.
- **Varargs**: `fn f(first, ...rest)` collects any extra arguments into an array. The
  varargs parameter `...name` must be the last parameter and cannot specify a default
  value.
- **Spread operator (`...`)**: `f(...args)` or `f(1, ...args, 2)` unpacks an array into
  function arguments. Spreading a non-array value raises a `TypeError`.
- **Tail-call optimization (TCO)**: Calls in tail position (`return f(...);` or ternary
  branches `return cond ? f() : g();`) reuse the current call frame. Tail recursion
  operates in $O(1)$ stack frames, allowing arbitrary recursion depth without stack
  overflow.
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

- Indexes must be integers; a negative index counts from the end and an out-of-range
  index is a runtime error. Slicing clamps instead of raising (section 3).
- Arrays are passed by reference (they are objects).
- Array literals support the spread operator: `[1, ...items, 4]` unpacks array elements.
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
print("spark"[0], "spark"[-1]);    // s k
print(substr("abcdef", 1, 3));     // bcd
print(substr("program", 5));       // am
print(len("ñandú"));               // 5
print(split("a,b,c", ","));        // ["a", "b", "c"]
print(upper("hello"), trim("  x  "));
```

- The index must be an integer; a negative index counts from the end (`"spark"[-1]` is
  `"k"`) and an out-of-range index is a runtime error. Slicing clamps instead of raising
  (section 3).
- `substr(text, start, count?)` starts at `start` and takes `count` characters (or the
  rest if omitted). A negative `start` counts from the end (`substr("abcdef", -2, 1)` is
  `"e"`, `substr("abcdef", -3)` is `"def"`), `count` must not be negative, and a `start`
  beyond the end returns `""`.
- `split`, `join`, `trim`, `upper`, `lower`, `repeat`, `replace`, `starts_with`,
  `ends_with`, `contains` and `index_of` are documented in section 10. `upper` and
  `lower` only change ASCII letters; `trim` only removes ASCII whitespace. Other
  characters are left untouched.

### Building strings

Strings are immutable, so `s += x` creates a new string every time: repeating it in a
loop copies the whole accumulator on each step and the total cost grows **quadratically**
with the result. For a handful of fragments that is fine; for thousands, assemble the
text with `string_builder()` (section 10), which appends into one growable buffer in
amortized **O(1)** per append:

```spark
var builder = string_builder();              // also: string_builder("seed"), string_builder(4096)
for (var i in range(3)) {
    builder.append("row ").append(i).append("; ");
}
print(builder.to_string());                  // row 0; row 1; row 2;
print(builder.len());                        // 21
```

`append` stringifies whatever it is given, so numbers, booleans and maps can be
appended directly. Collecting the fragments in an array and using `join(parts, "")` is
the other linear option.

Appending 200,000 single characters takes about 6 ms with `string_builder` and about
2.2 s with `s += x` on the development machine (`benchmarks/string_build.spark`).

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
| `is_nan(x)` | `true` when the value is IEEE 754 NaN. |
| `type(x)` | `"nil"`, `"bool"`, `"number"`, `"string"`, `"array"`, `"map"`, `"function"`, `"class"`, `"file"`, `"string_builder"`, `"coroutine"`, `"channel"`, `"socket"`, `"server"`, `"websocket"`, or the class name of an instance. |
| `isinstance(x, Class)` | `true` when `x` is an instance of `Class` or a subclass; a type name string (`"array"`, `"number"`, a class name) also works. |
| `is_a(x, Class)` | Like `isinstance`, but the second argument must be a class; a type name string is a `TypeError`. |
| `copy(array_or_map)` | Shallow copy; any other value is returned unchanged. |
| `clock()` | Monotonic clock seconds, for measuring. |
| `time()` | Wall-clock seconds since the Unix epoch. |
| `sleep(ms)` | Suspends the program for the given number of milliseconds. |
| `assert(cond, message?)` | Runtime error if `cond` is falsey. |
| `has(map, key)` | Whether the map contains the key. |
| `del(map, key)` | Deletes the key and returns whether it existed. |
| `keys(map)` | Array with the map's keys. |
| `values(map)` | Array with the map's values. |
| `range(n)` / `range(start, end)` | Array of integers in `[start, end)`. |
| `substr(text, start, count?)` | Substring by UTF-8 characters; a negative `start` counts from the end, `count` must not be negative, and a `start` past the end gives `""`. |
| `contains(container, value)` | `true` if an array has the value, a string has the substring, or a map has the key. |
| `index_of(container, value)` | First index (in array elements or UTF-8 characters) or `-1`. |
| `insert(array, index, value)` | Inserts at `index`; returns the array. |
| `remove(array, index)` | Removes the element at `index` and returns it. |
| `reverse(array)` | Reverses the array in place; returns it. |
| `fill(array, value)` | Sets every element to `value`; returns the array. |
| `slice(array_or_string, start, end?)` | New array or string with `[start, end)`; negative positions count from the end and out-of-range positions are clamped. There is no step; use the `a[start:end:step]` syntax for that (section 3). |
| `starts_with(text, prefix)` / `ends_with(text, suffix)` | Prefix/suffix test. |
| `split(text, sep)` | Array of parts; an empty separator splits into characters. |
| `join(array, sep)` | Text joining the stringified elements. |
| `trim(text)` | Removes leading and trailing ASCII whitespace. |
| `upper(text)` / `lower(text)` | ASCII upper/lower case. |
| `repeat(text, n)` | `text` repeated `n` times. |
| `replace(text, old, new)` | Replaces every occurrence of `old`. |
| `chr(code)` | One-character string for a Unicode codepoint. |
| `ord(text)` | Unicode codepoint of the first character of a non-empty string. |
| `pad_start(text, width, fill?)` / `pad_end(...)` | Pads to `width` characters (the fill defaults to a space). |
| `clear(map)` | Empties the map; returns it. |
| `merge(dest, src)` | Copies `src` entries into `dest` (src wins); returns `dest`. |
| `get(map, key, default?)` | Value for `key`, or the default (or `nil`) when absent. |
| `map(array, fn)` / `filter(array, fn)` | New array with `fn(x)` applied / kept when truthy. |
| `reduce(array, fn, initial?)` | Left fold with `fn(acc, x)`; without `initial`, uses the first element. |
| `each(array, fn)` | Calls `fn(x)` for every element; returns `nil`. |
| `any(array, fn?)` / `all(array, fn?)` | Whether any/all elements are truthy, or satisfy `fn(x)`. |
| `sum(array)` | Sum of the numeric elements; integer when every element is an integer. |
| `enumerate(array)` | Array of `[index, value]` pairs. |
| `zip(a, b)` | Array of `[a[i], b[i]]` pairs, up to the shorter array. |
| `sort(array)` | Sorts numbers or strings in place; mixing types is an error. |
| `sort_by(array, fn)` | Sorts in place with `fn(x, y)` returning whether `x` goes before `y`. |
| `abs(x)` | Absolute value; an integer stays an integer. |
| `floor(x)` / `ceil(x)` / `round(x)` | Rounding; an integer input is returned unchanged. |
| `sqrt(x)` | Square root (error on negative input). |
| `pow(x, y)` | `x` raised to `y`. |
| `sin(x)` / `cos(x)` | Sine and cosine (radians). |
| `log(x)` / `exp(x)` | Natural logarithm and exponential. |
| `min(a, b, ...)` / `max(a, b, ...)` | Smallest/largest of the arguments, or of an array when given a single array. |
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
| `tcp_connect(host, port, timeout_ms?)` | Connects a TCP client; returns a `socket`. |
| `tls_connect(host, port, timeout_ms?, ca_file?)` | Connects a TLS client; returns a `socket` with `read`/`write`. |
| `tcp_listen(host, port, backlog?)` | Starts a TCP listener; returns a `server` (port `0` binds an ephemeral port). |
| `udp_bind(host, port, timeout_ms?)` | Binds a UDP socket for datagrams; returns a `socket`. |
| `net_hostname()` | The local host name. |
| `url_parse(url)` | Splits an absolute URL into a `{scheme, host, port, path, query, fragment}` map. |
| `url_encode(text)` / `url_decode(text)` | Percent-encodes / decodes form data (a space becomes `+`). |
| `parse_query(query)` | Parses `key=value&...` into a map from key to an array of values. |
| `http_get(url, ...)` / `http_post` / `http_put` / `http_delete` | Sends an HTTP/1.1 request; returns `{status, headers, body}`. |
| `http_request(options)` | The map-driven form of the HTTP client. |
| `list_dir(path)` | Array with the names of the entries in a directory. |
| `mkdir(path)` | Creates a directory (succeeds when it already exists). |
| `exec(command)` | Runs a shell command; returns a map with `exit_code`, `stdout` and `stderr`. |
| `rand()` | Float in `[0.0, 1.0)`. |
| `randint(max)` / `randint(min, max)` | Random integer in `[min, max]` (inclusive). |
| `srand(seed)` | Seeds the PRNG with an integer or float. |
| `to_json(value)` | Serializes a value to a JSON string. |
| `from_json(text)` | Deserializes a JSON string into Spark values. |
| `regex_match(pattern, text)` | Whether `pattern` matches `text`. |
| `regex_find(pattern, text)` | Array of all non-overlapping matches of `pattern` in `text`. |
| `regex_replace(pattern, repl, text)` | Replaces non-overlapping matches with `repl`. |
| `gc()` | Runs a garbage collection cycle immediately and updates adaptive thresholds. |
| `spawn(fn, a, b, ...)` | Starts a coroutine that calls `fn` with the given arguments; returns the coroutine object. |
| `channel(n?)` | Creates a channel; without `n` it is unbuffered, with `n` it buffers `n` values. |
| `string_builder(initial?)` | New string builder; a string argument seeds its content, an integer sets the initial capacity. |
| `sb_append(sb, value)` | Stringifies `value` and appends it; returns the builder (so calls can be chained). |
| `sb_to_string(sb)` | The accumulated text as an immutable string. |
| `sb_len(sb)` | Length of the accumulated text in UTF-8 characters. |
| `sb_clear(sb)` | Empties the builder; returns it. |

Builders also accept the method form, which reads better when chaining:
`sb.append(value)`, `sb.to_string()`, `sb.len()` and `sb.clear()`.

### URLs and query strings

`url_parse` splits an absolute URL into a map:

```spark
var u = url_parse("http://user@[::1]:8080/a/b?x=1#top");
print(u.scheme);    // "http"
print(u.host);      // "[::1]" (IPv6 literals keep their brackets)
print(u.port);      // 8080, or the scheme default (http 80, https 443)
print(u.path);      // "/a/b" (defaults to "/")
print(u.query);     // "x=1" (an empty string when absent)
print(u.fragment);  // "top" (an empty string when absent)
```

- `port` is `nil` when the URL has no port and the scheme has no default.
- `host` is everything after an optional `userinfo@`; the userinfo is ignored.
- `url_encode` escapes everything except `A-Z a-z 0-9 - _ . ~`; a space becomes
  `+`. `url_decode` is its inverse and raises `ValueError` on a truncated or
  invalid escape.
- `parse_query("a=1&a=2&b=")` returns `{"a": ["1", "2"], "b": [""]}`: each key
  maps to an array of values, so repeated keys keep every occurrence. A leading
  `?` is accepted and empty pairs are skipped.
- A malformed URL (`url_parse`) or escape (`url_decode`, `parse_query`) raises
  `ValueError` instead of yielding a partial result.

### Networking

Sockets and servers are GC-managed resources: each owns a file descriptor and is
released by `close()`, by the collector's finalizer when it becomes unreachable, and
at shutdown. Using a closed handle is a runtime error and `type()` reports `"socket"`
or `"server"`.

```spark
// TCP client
var socket = tcp_connect("localhost", 8080, 5000); // 5 s deadline
socket.send("ping\n");
print(socket.recv_line());      // up to the next newline
print(socket.recv(1024));       // up to 1024 bytes, or nil at EOF
socket.close();

// TCP server
var server = tcp_listen("127.0.0.1", 0); // 0 binds an ephemeral port
var client = server.accept(5000);        // nil when nothing connects in time
client.send("pong\n");
client.close();
server.close();

// UDP
var datagram = udp_bind("127.0.0.1", 0);
datagram.send_to("ping", "127.0.0.1", 9999);
var message = datagram.recv_from(1024);  // {data, host, port}
```

Socket and server methods:

| Call | Result |
| --- | --- |
| `sock.send(data)` | Writes all of `data`; returns the byte count. TCP only. |
| `sock.recv(max)` | Up to `max` bytes, or `nil` at end of stream. |
| `sock.recv_line(max?)` | Up to the next newline (the newline, and a preceding `\r`, are dropped); `nil` at end of stream. Default bound 64 KiB. |
| `sock.wait(read?, write?, timeout_ms?)` | Suspends until the socket is readable/writable; `true` when ready, `TimeoutError` on the deadline. |
| `sock.send_to(data, host, port)` | Sends one datagram; returns the byte count. UDP only. |
| `sock.recv_from(max)` | A `{data, host, port}` map for one datagram. UDP only. |
| `sock.peer()` / `sock.local()` | A `{host, port}` map with the numeric peer / local address. |
| `sock.close()` | Closes the socket. |
| `srv.accept(timeout_ms?)` | An accepted `socket`, or `nil` on timeout. |
| `srv.port()` | The bound port. |
| `srv.close()` | Closes the listener. |
| `net_select(readables, timeout_ms?)` | Waits until any socket in the array is readable; returns the array of ready indexes (empty on timeout). |

- `host` is a name, an IPv4 literal, or an IPv6 literal in brackets (`"[::1]"`).
  `getaddrinfo` resolves names and every resolved address is tried in turn, so
  `localhost` works over either family.
- `port` must be in `0..65535`.
- `timeout_ms` on `tcp_connect`/`udp_bind` is stored as the socket's default deadline
  and bounds its later reads and writes; an accepted socket inherits the `accept`
  timeout. Omitted or `nil` means "wait forever"; a negative value is an error.
- Failure categories: `NetworkError` for an unknown host, a refused connection, an
  invalid port, a reset connection or descriptor exhaustion; `TimeoutError` when an
  operation exceeds its deadline. `accept` returns `nil` on timeout instead of raising.
- Payload bounds: `recv`/`recv_from` accept at most 16 MiB and `recv_line` at most
  16 MiB (default 64 KiB); larger values raise `ValueError`.

Blocking socket operations **yield the coroutine** instead of stalling the process: a
call that would block registers a wait on its descriptor, the scheduler polls the
registered sockets (sleeping until the nearest deadline), and wakes the coroutine when
the descriptor is ready or the deadline passes. `tcp_connect`, `accept`, `recv`,
`recv_line` and `send` all participate, so one process serves many connections with
`spawn`. A would-block call inside a native callback (`map`, `each`, `sort_by`, ...) or
an imported module raises a `RuntimeError` instead of suspending, because those run at
a deeper `run_depth`.

```spark
fn echo(server) {
    while (true) {
        var client = server.accept();
        if (client == nil) { break; }
        client.send(client.recv_line() + "\n");
        client.close();
    }
}
var server = tcp_listen("127.0.0.1", 8080);
var job = spawn(echo, server);
```

### TLS

`tls_connect(host, port, timeout_ms?, ca_file?)` opens a TCP connection and performs
a TLS handshake. It returns a `socket` whose `read(max)`, `write(data)` and `close()`
methods mirror `recv`/`send`:

```spark
var socket = tls_connect("example.com", 443, 5000);
socket.write("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
var reply = socket.read(65536);
while (reply != nil) {
    print(reply);
    reply = socket.read(65536);
}
socket.close();
```

- Verification is always on. The chain is checked against the system trust store, or
  against `ca_file` when supplied, and the host name (or IP address) must match the
  certificate. `ca_file` is a PEM file with one or more trusted certificates.
- SNI is sent for DNS names. IPv4 and IPv6 literals are verified against the
  certificate's IP subject alternative names instead.
- TLS 1.2 is the minimum accepted version. There is no option to skip verification.
- A certificate failure (self-signed, unknown issuer, wrong host name, expired)
  raises `NetworkError` with `TLS certificate verification failed: ...`. A timeout
  during connect or the handshake raises `TimeoutError`.
- `timeout_ms` is stored as the socket's default deadline and bounds the handshake,
  every `read` and every `write`; omitted or `nil` waits forever. The handshake and
  the reads/writes yield the coroutine, like the TCP methods.
- `read(max)` returns up to `max` bytes (at most 16 MiB), or `nil` at end of stream.
  `write(data)` writes all of `data` and returns the byte count. TLS sockets reject
  `send`, `recv`, `recv_line`, `wait` and `net_select`; use `read`/`write` instead.

### HTTP client

`http_get`/`http_post`/`http_put`/`http_delete` send an HTTP/1.1 request and return
`{status, headers, body}`:

```spark
var response = http_get("http://example.com/");
print(response.status);            // 200
print(response.headers["server"]); // header names are lowercased
print(response.body);              // the whole body as a string

// url, body?, headers?, timeout_ms?, follow_redirects?, max_body?
var created = http_post("http://api.local/items", "{\"name\":\"x\"}",
                        {"Content-Type": "application/json"}, 5000);
```

`http_request(options)` is the map-driven form; it accepts `url` (required),
`method`, `body`, `headers`, `timeout_ms`, `follow_redirects`, `max_body` and
`ca_file`, and rejects unknown keys. `ca_file` injects a PEM trust store for
`https://` (useful for private CAs and tests); the positional helpers always use the
system trust store.

| Call | Result |
| --- | --- |
| `http_get(url, body?, headers?, timeout_ms?, follow_redirects?, max_body?)` | `{status, headers, body}`. |
| `http_post` / `http_put` / `http_delete` | The same, with `POST` / `PUT` / `DELETE`. |
| `http_request(options)` | The same, driven by a map. |

- `headers` in the result is a map from lowercased name to value; a repeated header
  keeps the last value.
- A `Content-Length` body, a `Transfer-Encoding: chunked` body and a `Connection:
  close` body that ends at EOF are all handled. `301`, `302`, `303`, `307` and `308`
  are followed by default up to five hops (`303`, and `301`/`302` after a `POST`,
  switch to `GET` and drop the body). A relative `Location` is resolved against the
  request URL.
- The request is bounded: `timeout_ms` defaults to 30000 (a value of `0` also uses
  the default) and covers the whole request, including redirects. `max_body`
  defaults to 16 MiB and may not exceed that. The status line plus the headers may
  not exceed 64 KiB and at most 1000 headers and five redirects are accepted. Each
  limit fails with an explicit `NetworkError`.
- `http://` and `https://` are both supported. HTTPS verifies the certificate chain
  and the host name with the same rules as `tls_connect`; verification failures raise
  `NetworkError` and there is no way to disable them. A malformed status line, a
  premature EOF, an unknown host and a refused connection all raise `NetworkError`,
  and exceeding the deadline raises `TimeoutError`.
- A header value or URL containing a newline raises `ValueError`, so a request
  cannot be split into extra headers.
- Unlike the socket methods, the HTTP client is synchronous: it blocks the process
  (bounded by `timeout_ms`) instead of yielding the coroutine.

### WebSocket client

`ws_connect(url, timeout_ms?, ca_file?)` opens an RFC 6455 WebSocket client over
`ws://` or `wss://`. The URL supplies the path and query; the upgrade handshake is
verified (status `101`, `Upgrade`/`Connection` headers and the
`Sec-WebSocket-Accept` digest) and the result is a GC-managed `websocket`
(`type()` reports `"websocket"`):

```spark
var socket = ws_connect("wss://gateway.example/?v=10", 5000);
socket.send_text("hello");
var frame = socket.receive();          // {opcode: "text", data: "hello"}
print(frame.opcode, frame.data);
socket.ping("keepalive");
print(socket.receive().opcode);        // "pong"
socket.close(1000, "done");
print(socket.receive());               // nil once the peer closes
```

| Call | Result |
| --- | --- |
| `ws.send_text(data)` / `ws.send_binary(data)` | Sends one masked text or binary message; returns the byte count. |
| `ws.receive(timeout_ms?)` | The next `{opcode, data}` message, or `nil` when closed. |
| `ws.ping(data?)` | Sends a ping; returns the payload byte count. |
| `ws.close(code?, reason?)` | Starts the close handshake (default code `1000`). |

- `receive` returns `{opcode, data}` with `opcode` equal to `"text"`, `"binary"` or
  `"pong"`. Fragmented messages are reassembled before they are returned. `nil`
  means the peer closed the connection, the close handshake completed, or a
  previous `close` already closed it.
- Pings from the peer are answered automatically with a pong carrying the same
  payload and are not returned to the program; unsolicited pongs are returned as
  `"pong"` messages. Control frames may arrive between the fragments of a message.
- `send_text` requires valid UTF-8. Received text and close reasons are validated
  too. A protocol violation (masked server frame, reserved bit or opcode, orphan
  continuation, invalid UTF-8, invalid close code) raises `NetworkError`, sends a
  close frame with code `1002`, `1007` or `1009`, and closes the transport.
- Limits: one frame and one assembled message may not exceed 16 MiB, a control
  payload is at most 125 bytes and a close reason at most 123 bytes. Exceeding a
  limit raises `ValueError` on send and `NetworkError` on receive.
- `wss://` uses the same chain and host-name verification as `tls_connect`;
  `ca_file` injects a PEM trust store and is ignored by `ws://`. The TCP connect
  and the TLS handshake are synchronous and bounded by `timeout_ms`, which also
  becomes the default deadline for `receive`; pass an explicit `timeout_ms` to
  `receive` when the protocol needs heartbeats.
- `receive` yields the coroutine while it waits for bytes; `send_text`,
  `send_binary`, `ping` and `close` write synchronously, bounded by the connection
  timeout.

### Discord gateway example

`examples/discord_bot.spark` is a small Discord-shaped gateway client. It reads
`DISCORD_TOKEN` from the environment (and never prints it), connects to
`DISCORD_GATEWAY_URL` or the real `wss://gateway.discord.gg/?v=10&encoding=json`,
waits for `HELLO`, sends `IDENTIFY`, heartbeats at the interval from `HELLO` and
answers a `MESSAGE_CREATE` whose content is `ping` with `pong`.

1. Build Spark with `make`.
2. Start the offline mock gateway in one terminal:
   `python3 tests/run_tests.py --mock-gateway`. It prints a
   `ws://127.0.0.1:PORT/?v=10` URL and keeps running.
3. In another terminal, run the example against it:

   ```bash
   DISCORD_TOKEN=test-token \
   DISCORD_GATEWAY_URL="ws://127.0.0.1:PORT/?v=10" \
   spark examples/discord_bot.spark
   ```

   Expected output:

   ```
   ready
   message: ping
   reply sent
   closed
   ```

4. To use the real gateway, export a bot token and omit `DISCORD_GATEWAY_URL`:
   `DISCORD_TOKEN=... spark examples/discord_bot.spark`.

The test suite starts the same mock gateway on an ephemeral port, so the example is
exercised on every run without contacting Discord.

### Network security

Spark is not a sandbox. `exec` already lets a program run arbitrary commands and
reach the network, and the socket, HTTP and TLS natives deliberately impose no
destination allow-list. A program that accepts URLs from untrusted input must apply
its own policy (for example, restrict schemes and reject loopback or private address
ranges) before connecting. TLS does not change that: certificate verification
authenticates the peer, it does not make an arbitrary destination safe to contact.

- URL userinfo (`https://user:secret@host/`) is accepted for compatibility but is
  not sent. Network error messages redact it (`https://***@host/`), and header
  values, request bodies and response bodies are never echoed by an error.
- Timeouts: socket, TLS and WebSocket operations accept `timeout_ms` and wait
  forever when it is omitted, so a server can idle; the HTTP client defaults to a
  30 s deadline. Pass an explicit timeout for any operation driven by untrusted
  input.
- **Embedders.** Treat Spark as a language runtime, not a security boundary. Run
  untrusted programs inside an OS sandbox (container, seccomp, namespaces), cap
  descriptors with `RLIMIT_NOFILE` (exhaustion raises `NetworkError`), and apply an
  egress firewall or proxy when destinations must be restricted. Any in-language
  policy can be bypassed with `exec`.

### HTTP server library

`lib/http.spark` builds a server on the socket layer, with no native server code:

```spark
import "./lib/http";

fn handle(request) {
    // request: {method, target, path, query, version, headers, body}
    return {"status": 200, "body": "hello " + request.path + "\n"};
}

var server = tcp_listen("127.0.0.1", 8080);
http_serve(server, handle);   // serves forever; add a count to stop after N
```

`http_serve(server, handler, max_requests?)` accepts one connection at a time,
parses the request, calls `handler` and writes the response (`{status, headers?,
body?}`); a missing status is `200` and a missing body is empty. A parsing or handler
failure answers `500`. The request line and each header are capped at 8192
characters, at most 100 headers are read, and the body is read up to
`Content-Length`. See `examples/http_server.spark`.

The global constants `pi` and `nan` are predefined, and the REPL keeps the result of
the last expression in `_`.

The script arguments are accessible through the global array `argv`, where `argv[0]`
is the script file path and subsequent elements are arguments passed on the command line
(empty in the REPL).

The higher-order natives (`map`, `filter`, `reduce`, `each`, `sort_by`) accept a
callback that may be a closure or another native (for example `map(a, str)`). `sort`
and `sort_by` use a stable merge sort ($O(n \log n)$); `sort_by`'s comparator `fn(x, y)` returns a
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
| `NetworkError` | a socket or name-resolution failure (refused, unknown host, invalid port, reset, descriptor exhaustion) |
| `TimeoutError` | a network operation exceeded its deadline |
| `RuntimeError` | any other runtime failure |

Interpreter exit codes:

| Code | Meaning |
| --- | --- |
| `0` | Successful run |
| `64` | Incorrect command-line usage |
| `65` | Syntax or compilation error |
| `70` | Runtime error |
| `74` | Could not read the file |

## 12. Modules

Spark supports multi-file programs via the `import` statement:

```spark
import "./math_utils";
import "./helpers/string.spark";
```

- Paths are relative to the importing file (or current directory in the REPL).
- The `.spark` file extension is optional; if omitted, it is appended automatically.
- Modules share the global execution environment: any top-level `var`, `const`, `fn`, or `class` declared in an imported module becomes available to the importer.
- The runtime canonicalizes module paths using `realpath()` and caches loaded modules. Each module is parsed, compiled, and executed at most once. Cyclic imports are detected and handled without re-execution or infinite loops.

## 13. Exception handling

Exceptions are raised with `throw` and handled with `try` / `catch` / `finally`:

```spark
try {
    throw "something failed";
} catch (e) {
    print("Caught:", e);
} finally {
    print("Cleanup");
}
```

- Any Spark value can be thrown (`string`, `number`, `map`, etc.).
- `catch` can optionally bind an identifier to the thrown value: `catch (e)` or `catch err`.
- `finally` blocks always execute, whether the `try` block completes normally, handles an exception, returns early via `return`, or leaves an uncaught exception propagating upward.
- If an exception reaches the top of the call stack without finding a handler, the VM terminates execution with exit code 70 and reports `uncaught exception: <value>`.

## 14. Object-oriented programming

Spark has two related object models: functions stored in maps act as methods, and
`class` declarations create real instances with their own fields.

### Map method call sugar and `self`

Calling a function stored in a map using the dot operator automatically binds `self`
to the map receiver:

```spark
var counter = {
    "count": 0,
    "increment": fn(step = 1) {
        self.count += step;
        return self.count;
    }
};

print(counter.increment(2)); // 2
```

- Dot notation can also read and write map properties: `obj.field = value;`.
- Inside any method invoked via `recv.method(...)`, the identifier `self` refers to `recv`.
- If a function accessing `self` is called directly rather than via a method call, a runtime `NameError` is raised indicating `self` is not bound.
- Method calls can be chained: `obj.step().step().get()`.

### Classes and instances

The `class` statement declares the fields, constructor and methods of a new type:

```spark
class Person {
    var greeting = "Hello"; // declared field with a default value

    init(name, age = 0) {
        self.name = name;
        self.age = age;
    }

    greet() {
        return self.greeting + ", I am " + self.name;
    }
}

var p = new Person("Ada", 36);
print(p.greet());
```

- Construction always uses `new`: `new Person("Ada")` creates the instance and runs `init`, while `Person("Ada")` is a `TypeError` telling you to use `new`.
- `init` is optional. It may declare parameters with default values, and `new` checks the argument count against it.
- A class may declare fields with `var name = expression;` (the `= expression` part is optional). Fields are initialized base-first, before `init` runs, and every instance receives its own copy.
- Fields can still be created dynamically by assigning to `self.name`; they do not need to be declared.
- `init` implicitly returns the new instance. A bare `return` ends the constructor early, but `return value` inside `init` is a compile error.
- Methods are looked up through the superclass chain. A nested function or lambda can capture `self` like any other local.

### Inheritance and `super`

```spark
class Animal {
    init(name) { self.name = name; }
    speak() { return self.name + " makes a sound"; }
    static kingdom() { return "Animalia"; }
}

class Dog extends Animal {
    init(name) { super(name); }
    speak() { return self.name + " barks"; }
    parent_speak() { return super.speak(); }
    static kingdom() { return super.kingdom() + "/Canidae"; }
}

var d = new Dog("Rex");
print(d.speak());        // Rex barks
print(d.parent_speak()); // Rex makes a sound
```

- `extends` names a single superclass; a class without `extends` has none.
- `super(args)` calls the superclass `init` and is only valid inside an instance method.
- `super.method(args)` starts the lookup at the superclass instead of the receiver's class. It works in instance methods and in static methods (where it resolves the superclass's static).
- Using `super` outside a class or in a class without a superclass is an error.
- A subclass that does not define `init` inherits the closest one, and `new Sub(...)` runs it normally.

### Static fields and methods

```spark
class Config {
    static var level = 1;

    static describe() {
        return "level " + str(Config.level);
    }
}

print(Config.level);   // 1
Config.level = 5;
print(Config.describe());
```

- `static var` fields and `static` methods belong to the class and are read or called as `Class.member`.
- Statics are inherited: a subclass can read a static defined by an ancestor. Assigning through a subclass (`Sub.value = 1`) creates a member on that subclass instead of modifying the ancestor.
- `self` is not bound in a static method. Calling a static on an instance, or an instance method on the class, raises an error that points at the right receiver.

### Type checks

- `type(instance)` returns the class name as a string, and `type(SomeClass)` returns `"class"`.
- `value instanceof Class` is true for instances of that class or any subclass. `nil instanceof Class` is false.
- `is_a(value, Class)` is the function form and also respects inheritance; its second argument must be a class value.
- `isinstance(value, Class)` accepts a class value or a type-name string. The string form is kept for compatibility with built-in types and with classes.
- Class names cannot collide with built-in type names such as `map` or `number`.

### Printing and `to_string`

- If a class defines `to_string()`, `print(value)`, `str(value)` and string interpolation use its result. It must return a string.
- Without an override, instances print as `<ClassName instance>`.
- Collections format their elements with the default representation: an instance nested inside an array or a map prints as `<ClassName instance>` even when the class defines `to_string()`. This keeps formatting non-recursive and avoids calling user code while building another value.

### Operator overloading

Special methods let a class define the meaning of the operators for its instances:

| Expression | Method |
| --- | --- |
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b`, `a ** b` | `__add__`, `__sub__`, `__mul__`, `__div__`, `__mod__`, `__pow__` |
| `-a` | `__neg__()` |
| `a == b`, `a != b` | `__eq__(b)` |
| `a < b`, `a <= b`, `a > b`, `a >= b` | `__lt__`, `__le__`, `__gt__`, `__ge__` |
| `a[i]` | `__index__(i)` |
| `a[i] = value` | `__index_set__(i, value)` |
| `len(a)` | `__len__()` |
| `str(a)`, `print(a)`, `"${a}"` | `to_string()` |

```spark
class Vec {
    init(x, y) { self.x = x; self.y = y; }

    __add__(o) { return new Vec(self.x + o.x, self.y + o.y); }
    __eq__(o) { return (o instanceof Vec) and self.x == o.x and self.y == o.y; }
    __lt__(o) { return self.x < o.x; }
    __len__() { return 2; }
    __index__(i) { return i == 0 ? self.x : self.y; }

    to_string() { return "Vec(" + str(self.x) + ", " + str(self.y) + ")"; }
}
```

Rules that apply to all special methods:

- Only the left operand's method is tried. Reflected variants (`__radd__` and friends) are not implemented; the right operand is consulted only for `__eq__` when the left one does not define it.
- The result of a comparison method uses Spark truthiness, so returning `0` or `""` means false.
- `a == nil` and `nil == b` always use identity comparison, even when the class defines `__eq__`, so nil checks remain reliable.
- A string key always addresses a field before `__index__` runs, and `a["name"] = value` always writes a field instead of calling `__index_set__`. This keeps `self.x` usable inside the index methods. Other key types go to the index methods.
- `__len__` must return a non-negative integer.
- Membership (`in`), `contains`, `index_of` and the default `sort` still use primitive equality and ordering rather than `__eq__` and `__lt__`.

## 15. Coroutines and channels

`spawn` starts a **coroutine** that runs a function concurrently with the rest of
the program, and channels move values between coroutines. Coroutines are
cooperative and single-threaded: the running coroutine keeps the CPU until it
suspends itself, so there are no data races and no locks.

```spark
fn worker(name) {
    print("start", name);
    yield;                    // suspend until the scheduler resumes us
    print("end", name);
    return name + "!";
}

var a = spawn(worker, "a");
var b = spawn(worker, "b");
print("main");
print(a.join(), b.join());
// main
// start a
// start b
// end a
// end b
// a! b!
```

The main script is itself a coroutine, so it can `yield` and use channels too.
`spawn` queues the new coroutine without switching to it: it starts as soon as
the current coroutine suspends. The scheduler is FIFO, so coroutines take turns
in the order they became runnable. When the main coroutine finishes, the program
ends and pending coroutines are abandoned.

### Coroutine methods

| Call | Result |
| --- | --- |
| `c.join()` | Blocks until `c` finishes and returns its result. Joining the running coroutine is an error. |
| `c.done()` | `true` once `c` has finished. |

`spawn` accepts a function followed by its arguments and validates the arity
before queueing, so a mismatch is reported at the `spawn` call instead of when
the coroutine starts. Any other value raises a `TypeError`.

### Channels

A channel hands values between coroutines in FIFO order. `channel()` is an
unbuffered **rendezvous** channel, where a sender blocks until a receiver takes
the value; `channel(n)` is a buffered channel that holds up to `n` values before
senders block. The capacity must be a non-negative integer.

| Call | Result |
| --- | --- |
| `ch.send(value)` | Sends a value, blocking while the channel is full. Error when the channel is closed. |
| `ch.receive()` | Receives the next value, blocking while the channel is empty. Returns `nil` once the channel is closed and drained. |
| `ch.close()` | Closes the channel: waiting receivers get `nil` and waiting senders are woken with an error. |

```spark
var ch = channel();
fn producer() {
    for (var i in range(3)) {
        ch.send(i);
    }
    ch.close();
}
fn consumer() {
    var total = 0;
    while (true) {
        var value = ch.receive();
        if (value == nil) { break; }
        total += value;
    }
    return total;
}
spawn(producer);
print(spawn(consumer).join());   // 3
```

Values still buffered when a channel closes are delivered before `receive()`
starts returning `nil`, and closing an already closed channel is allowed. Both
coroutine and channel methods can also be called with spread arguments
(`ch.send(...[1])`).

### Suspension rules

- `yield` and the blocking forms of `send`, `receive` and `join` are only allowed
  in the outermost loop of a coroutine. Inside a callback passed to `map`,
  `filter`, `each`, `sort_by` or another higher-order native, or inside an
  imported module, they raise a runtime error instead of suspending the caller.
- Blocking when no other coroutine can make progress aborts the program with a
  runtime error (`deadlock: every coroutine is blocked`).
- An uncaught error in any coroutine aborts the whole program, exactly as it
  would in the main script.
- Blocked coroutines, their stacks and the locals they captured are kept alive by
  the collector, so calling `gc()` never breaks a pending `send`, `receive` or
  `join`.

## 16. REPL

With no arguments, `spark` opens a REPL. Every expression statement prints its value,
global variables persist and errors do not close the session. The special variable `_`
automatically holds the result of the last evaluated expression statement. Type
`exit` or `quit` (or Ctrl+D) to finish. Lines accumulate while there are
unclosed braces, so multi-line functions can be defined.

## 17. Precompiled bytecode (`.sparkc`)

Source programs can be precompiled into binary bytecode files using `--compile`:

```bash
spark --compile script.spark              # writes script.sparkc
spark --compile script.spark -o out.sparkc # writes to custom output
spark script.sparkc                       # runs precompiled file directly
```

- Bytecode files begin with the 4-byte magic signature `SPRK` followed by the binary format version.
- Precompiled files execute with zero parse/compile overhead and can be imported directly or resolved automatically via `import`.

## 18. Developer Tooling

Spark provides built-in tools for debugging, formatting, and static analysis:

### Interactive Debugger (`--debug`)

Run any script with `--debug` to start an interactive debugging session:

```bash
spark --debug script.spark
```

Available debugger commands:
- `break <[file:]line>` (or `b`): Set a breakpoint at a source line or file:line.
- `delete <id>` (or `d`): Delete a breakpoint by its number.
- `list`: List all active breakpoints.
- `step` (or `s`): Step into the next source line.
- `next` (or `n`): Step over the next line (skipping into called functions).
- `continue` (or `c`): Resume execution until the next breakpoint or program end.
- `locals` (or `l`): Inspect local variables in the current call frame.
- `stack` (or `bt`): Print call stack backtrace.
- `help` (or `h`): Display available debugger commands.
- `quit` (or `q`): Terminate execution immediately.

### Code Formatter (`--format`)

Format source code canonically with `--format`:

```bash
spark --format file.spark       # outputs formatted source to stdout
spark --format -w file.spark    # formats file in-place
```

Formatting rules:
- Indentation: 4 spaces per nesting level.
- Semicolons followed by newline (except within `for` loop headers).
- Binary and assignment operators surrounded by spaces.
- Commas followed by a single space.
- Comments (`//` and `/* ... */`) and string interpolations preserved verbatim.
- Fully idempotent: running the formatter multiple times produces identical output.

### Linter (`--lint`)

Analyze source code statically for potential issues:

```bash
spark --lint file.spark
```

Linter rules:
- `unused-variable`: Warns when local variables or parameters are declared but never referenced. Names beginning with an underscore (`_`) are intentionally ignored.
- `unreachable-code`: Warns when statements appear after a terminator (`return`, `throw`, `break`, `continue`) in the same block.
- `shadowed-variable`: Warns when a variable declaration in an inner scope shadows a variable declared in an enclosing scope.
- Returns exit code `0` when clean, `1` when warnings are found, `65` on syntax errors, and `74` if the file cannot be read.

## 19. Limits

- Integers are 64-bit; a literal or result outside that range becomes a float.
- 255 parameters, arguments and elements per array literal.
- Up to 1024 local variables per function (slots beyond 255 use 16-bit local instructions).
- 4096 call frames (non-tail calls deeper than this produce a `RecursionError`; tail calls reuse frames via TCO and are unconstrained).
- Dynamic operand value stack that automatically expands from 2,048 up to 4,194,304 items (64 MB).
- 16-bit constant index per function (up to 65,536 constants per chunk).
- 16,777,216 values of channel capacity.
- Coroutines are cooperative and single-threaded; each keeps its own operand
  stack (up to 4,194,304 items) and up to 4096 call frames.
- A `recv`/`recv_from` returns at most 16 MiB; `recv_line` is bounded to 16 MiB
  (default 64 KiB) and raises `ValueError` past that.
- A TLS `read` returns at most 16 MiB and a `write` accepts at most `INT32_MAX`
  bytes. TLS 1.2 is the minimum accepted version; the handshake, reads and writes
  are bounded by the socket's `timeout_ms` (0 waits forever).
- Ports are 16-bit (`0..65535`); port `0` asks the kernel for an ephemeral port.
- Blocking network calls yield only their coroutine; the scheduler sleeps in `poll(2)`
  until a descriptor is ready or the nearest deadline fires, so an idle wait uses no
  CPU. Inside a native callback or an imported module a would-block call is an error.
- The HTTP client is bounded: the status line plus the headers may not exceed 64 KiB,
  at most 1000 headers and five redirects are accepted, the body defaults to a 16 MiB
  cap (`max_body`) and the whole request defaults to a 30 s deadline (`timeout_ms`).
  It blocks the process rather than yielding the coroutine.
- A WebSocket frame and an assembled message are capped at 16 MiB, a control payload
  at 125 bytes and a close reason at 123 bytes. Text and close reasons are validated
  as UTF-8; protocol violations close with `1002`, `1007` or `1009`.
- Descriptors are capped by the process limit (`RLIMIT_NOFILE`); exhausting it raises
  `NetworkError: too many open descriptors` instead of leaking or crashing.
- The `lib/http.spark` server reads at most 8192 characters per line, 100 headers and
  the body up to `Content-Length`; it is a text server, so bodies with multibyte
  characters count differently from bytes.