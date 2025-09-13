# SwaziLang

SwaziLang is a scripting language written in C++ that lets you program using natural Swahili keywords and syntax.

## Build Instructions

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Running SwaziLang

After building, run the executable from the `build` directory:

```bash
./swazi
```

Or add it to your system PATH to run from anywhere.

## Dependencies

SwaziLang uses [Conan](https://conan.io/) for modern project dependencies (similar to `npm` in Node.js).

### Install Conan (requires Python):

```bash
pip install conan
```

Or check if Conan is already installed:

```bash
conan --version
```

### Install project dependencies:

```bash
mkdir build
cd build
conan install .. --build=missing
```

---

## SwaziLang Keywords

Below are preserved keywords and their meanings in SwaziLang syntax:

| Keyword      | Meaning (English)   | Usage                                     |
|--------------|---------------------|-------------------------------------------|
| `data`       | data (universal)    | Declare variables                         |
| `kweli`      | true                | Boolean literal (true)                    |
| `sikweli`    | false/not true      | Boolean literal (false)                   |
| `kazi`       | work/task           | Function definition (non-object)          |
| `tabia`      | behavior            | Method definition (object/class function) |
| `chapisha`   | publish/print       | Print to console (output)                 |
| `andika`     | write               | Alternate print to console                |
| `klass`      | class               | Class definition                          |
| `rithi`      | inherit             | Inheritance / extend class                |
| `kama`       | if                  | Conditional branching                     |
| `vinginevyo` | else/otherwise      | Conditional alternative                   |
| `kwa`        | for                 | Loop (for)                                |
| `rudisha`    | return              | Return from function                      |
| `endelea`    | continue            | Continue to next loop iteration           |
| `vunja`      | break               | Break out of loop                         |
| `na`, `&&`   | and                 | Logical AND                               |
| `au`, `||`   | or                  | Logical OR                                |
| `si`, `!`    | not                 | Logical NOT                               |
| `swichi`     | switch              | Switch-case structure                     |
| `ikiwa`      | case                | Case in switch                            |
| `chaguo`     | default             | Default in switch                         |

### Symbols

- Arithmetic: `+`, `-`, `*`, `**`, `/`, `%`
- Bitwise: standard C++ bitwise operators
- Parentheses `( )` are used for calling functions and methods, **not for declaring** them.

---

## Example: Declaring and Using a Function

```swz
kazi jinaLaKaz arg1, arg2, arg3:
    rudisha arg1 + arg2;

# Comment
data x = 20;
jinaLaKaz(3, 9, x)
```

---

## Comments

Use `#` for single-line comments.

---

## Contributing

Feel free to submit issues or pull requests to help improve SwaziLang!

---

## License

This project is open-source. See [LICENSE](LICENSE) for details.