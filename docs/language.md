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

- Indexes must be non-negative integers within range.
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
| `is_nan(x)` | `true` when the value is IEEE 754 NaN. |
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
| `gc()` | Runs a garbage collection cycle immediately and updates adaptive thresholds. |
| `spawn(fn, a, b, ...)` | Starts a coroutine that calls `fn` with the given arguments; returns the coroutine object. |
| `channel(n?)` | Creates a channel; without `n` it is unbuffered, with `n` it buffers `n` values. |

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
`salir`, `exit` or `quit` (or Ctrl+D) to finish. Lines accumulate while there are
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