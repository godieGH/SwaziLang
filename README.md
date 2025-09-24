# SwaziLang

SwaziLang is a small scripting language implemented in C++ that lets you program using natural Swahili-like keywords and syntax. It aims to make scripting more approachable by mapping common programming concepts to readable, localized keywords while keeping a familiar, C/JavaScript-like structure under the hood.

This README gives a clear, practical overview so you can quickly understand what SwaziLang is, how to build and run it, and where to start writing scripts.

---

## Key goals

- Provide readable, Swahili-inspired keywords for common programming constructs.
- Keep execution simple and embeddable — language core is implemented in C++.
- Provide a gentle learning curve for non-English speakers and for rapid scripting tasks.
- Make it easy to extend the language and add bindings or runtime features.

---

## Quick start

Prerequisites:
- A C++ toolchain (g++, clang, or MSVC)
- CMake (>= 3.10)
- Python (for Conan)
- Conan package manager (optional if you already have dependencies)

Install Conan:
```bash
pip install conan
```

Build:
```bash
mkdir build
cd build
cmake ..
cmake --build .
```

Install dependencies (if using Conan explicitly):
```bash
mkdir build
cd build
conan install .. --build=missing
cmake ..
cmake --build .
```

Run:
From the `build` directory:
```bash
./swazi
```
(Optional) Add the executable to your PATH to run it from anywhere.

---

## What makes SwaziLang different

- Keywords are mapped to Swahili-style words (e.g., `kazi` for function, `data` for variable).
- Uses natural-language-like constructs while keeping familiar programming operators.
- Comments use `#`, like many scripting languages.
- Supports functions, objects/classes, control flow, exceptions, and asynchronous-style keywords such as `subiri` (await).

---

## Language overview

Concepts and example usage

- Variables and constants:
```swz
data x = 20;
thabiti PI = 3.14159;
```

- Functions:
```swz
kazi jumlisha a, b:
    rudisha a + b;

chapisha jumlisha(3, 4)  # prints 7
```

- Control flow:
```swz
kama x sawa 10:
    chapisha "x ni kumi";
vinginevyo:
    chapisha "x sio kumi";
```

- Loops:
```swz
kwa (data i = 0; i < 5; i = i + 1):
    chapisha i

wakati (i < 10):
    i = i + 1

fanya:
    i = i - 1
wakati (i > 0)
```

- Object / class:
```swz
muundo Mtu:
    tabia jina:
        rudisha this.jina

data p = unda Mtu("Asha")
chapisha p.jina()
```

- Error handling:
```swz
jaribu:
    onesha "hitilafu"
kamata e:
    chapisha "Ilikamatwa:", e
kisha:
    chapisha "hatimaye"
```

- Async-style:
```swz
kazi tumiaAPI:
    data res = subiri fetch("https://example.com")
    rudisha res
```

---

## Keywords (quick reference)

- Declarations & values: `data` (variable), `thabiti` (constant)
- Functions & methods: `kazi` (function), `tabia` (method)
- Control flow: `kama` (if), `vinginevyo` (else / else-if), `kwa` (for), `wakati` (while), `fanya` (do-while)
- Returns & I/O: `rudisha` (return), `chapisha` (print newline), `andika` (print no newline)
- Logical & comparison: `sawa` (==), `sisawa` (!=), `na` (AND), `au` (OR)
- Objects & classes: `muundo` (class), `rithi` (inherit), `unda` (new), `futa` (destroy)
- Modules & imports: `tumia` (import), `kutoka` (from), `kama` (alias)
- Exports: `ruhusu`
- Error handling: `jaribu` (try), `kamata` (catch), `kisha` (finally), `onesha` (throw)
- Async / type checks: `subiri` (await), `AINAYA` (typeof), `NINAMBARI`, `NINENO`, `NIBOOLEAN`, `NIKAZI`, `NILISTI`, `NIOBJECT` / `ob`, `NIMUUNDO`

Note: Some keywords such as `kama` may be used in multiple contexts (e.g., conditional and import alias). See examples for specific usage patterns.

Symbols:
- Arithmetic: +, -, *, **, /, %
- Bitwise: C++ standard bitwise operators
- Parentheses `( )` are used for calling functions and methods.

Comments:
- Single-line comments start with `#`.

---

## Examples

Hello world:
```swz
chapisha "Habari, Dunia!"
```

Function + loop:
```swz
kazi fib n:
    kama n <= 1:
        rudisha n
    rudisha fib(n - 1) + fib(n - 2)

kwa (data i = 0; i < 10; i = i + 1):
    chapisha fib(i)
```

---

## Project structure (typical)
- src/ — language implementation (C++ source)
- include/ — public headers
- examples/ — sample SwaziLang scripts
- CMakeLists.txt — build configuration
- conanfile.txt / conanfile.py — dependency management

(Adjust paths according to the repository layout)

---

## Contributing

Contributions are welcome! Practical ways to help:
- Open issues describing bugs, feature requests, or design suggestions.
- Add examples or improve the standard library.
- Submit pull requests with small, focused changes (tests or examples preferred).
- Improve documentation and README clarity.

When opening issues or PRs, include:
- A clear description of the problem or change.
- Reproduction steps or example code when reporting bugs.
- Platform and toolchain details if build issues are encountered.

---

## License

This project is open-source. See the LICENSE file in the repository for details:
https://github.com/godieGH/SwaziLang/blob/main/LICENSE

