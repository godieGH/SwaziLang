# SwaziLang

[![Release](https://img.shields.io/github/v/release/godieGH/SwaziLang?style=flat-square)](https://github.com/godieGH/SwaziLang/releases)

SwaziLang is a scripting language written in C++ that lets you write programs using natural Swahili keywords and syntax.

Want to learn SwaziLang? [Get started here →](https://swazilang.netlify.app)

---

## Build Instructions

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

To create an installable package for your OS:

```bash
make package
# Packages are output to build/packages/
```

Installing the package (recommended — tracked by your package manager):

```bash
sudo apt install ./packages/<package-name>.deb   # Debian/Ubuntu
```

Or install directly without package tracking:

```bash
sudo make install
# Note: uninstallation requires manual removal
```

---

## Running SwaziLang

After building, run the executable from the `build` directory:

```bash
./swazi
```

Or install and add to your system `PATH` to run from anywhere.

---

## Dependencies

Most dependencies are managed automatically by CMake via `FetchContent` or bundled with the build. You do **not** need Conan to build or use SwaziLang.

> **Contributors only:** If you prefer using Conan for third-party dependency management during development, install it with:
> ```bash
> pip install conan
> conan --version  # verify installation
>
> mkdir build && cd build
> conan install .. --build=missing --output-folder=.
> ```

---

## Keywords

| Keyword | Meaning / Usage |
|---|---|
| `data` | Declare a variable |
| `thabiti` | Declare a constant or a `const` getter method |
| `kazi` | Define a function |
| `tabia` | Define a method inside an object or class |
| `rudisha` | Return a value from a function or method |
| `chapisha` | Print with newline |
| `andika` | Print without newline |
| `kweli` | Boolean literal: `true` |
| `sikweli` | Boolean literal: `false` |
| `kama` | `if` statement / `as` alias in imports |
| `vinginevyo` / `sivyo` | `else` / `else if` branch |
| `kwa` | `for` loop |
| `wakati` | `while` loop |
| `fanya` | `do-while` loop |
| `sawa` | Equality operator (`==`) |
| `sisawa` | Inequality operator (`!=`) |
| `na` | Logical AND (keyword form of `&&`) |
| `au` | Logical OR (keyword form of `\|\|`) |
| `unda` | Create a new object/class instance (`new`) |
| `muundo` | Define a class |
| `rithi` | Inherit from a class |
| `futa` | Manually destroy an object instance |
| `subiri` | Await an async operation |
| `tumia` | Import a module |
| `kutoka` | Specify module source in `tumia` statements |
| `ruhusu` | Export from a module |
| `jaribu` | `try` block |
| `makosa` | `catch` block |
| `kisha` | `finally` block |
| `onesha` | Throw an error or exception |
| `ainaya` | Unary: returns the type of a value |
| `NINAMBA` | Unary: check if value is a number |
| `NINENO` | Unary: check if value is a string |
| `NIBOOL` | Unary: check if value is a boolean |
| `NIKAZI` | Unary: check if value is a function |
| `NIORODHA` | Unary: check if value is a list/array |
| `NIOBJECT` | Unary: check if value is an object (alias: `ob`) |

### Operators & Symbols

- **Arithmetic:** `+`, `-`, `*`, `**`, `/`, `%`
- **Bitwise:** standard C bitwise operators
- **Parentheses `( )`** are used for calling functions and methods — not for declaring them.

---

## Example

```swz
kazi sum(a, b, c):
    rudisha a + b + c;

# This is a comment
data x = 20;
chapisha sum(3, 9, x);  # prints 32
```

---

## Comments

Use `#` for single-line comments.

---

## Documentation

Full language documentation and learning resources are available at [swazilang.netlify.app](https://swazilang.netlify.app).

---

## Contributing

Issues and pull requests are welcome. Feel free to open one if you find a bug or want to improve something.

---

## License

See [LICENSE](https://github.com/godieGH/SwaziLang/blob/main/LICENSE) for details.
