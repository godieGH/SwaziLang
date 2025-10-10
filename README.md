# SwaziLang

SwaziLang is a scripting language written in C++ that lets you program using natural Swahili keywords and syntax.

Do you want to learn SwaziLang?
[get started here](https://swazilang.netlify.app)

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

# SwaziLang Keywords

| Keyword      | Meaning / Usage                                             |
|--------------|-------------------------------------------------------------|
| `data`       | Declare a variable                                          |
| `thabiti`    | Declare a constant variable or a const method (getter)      |
| `kazi`       | Define a function                                           |
| `tabia`      | Define a method inside an object/class                      |
| `rudisha`    | Return a value from function/method                         |
| `chapisha`   | Print with newline                                          |
| `andika`     | Print without newline                                       |
| `kweli`      | Boolean literal: true                                       |
| `sikweli`    | Boolean literal: false                                      |
| `kama`       | If statement                                                |
| `vinginevyo` | Else / else-if branch                                       |
| `kwa`        | For loop (C-style or Python-style)                          |
| `wakati`     | While loop (C-style or Python-style)                        |
| `fanya`      | Do-while loop (C-style or Python-style)                     |
| `sawa`       | Equality operator (`==`)                                    |
| `sisawa`     | Inequality operator (`!=`)                                  |
| `na`         | Logical AND (keyword form of `&&`)                          |
| `au`         | Logical OR (keyword form of `||`)                           |
| `unda`       | Create new object/class instance (like `new` in JS)         |
| `muundo`     | Define a class                                              |
| `rithi`      | Inherit from a class                                        |
| `futa`       | Destroy an object instance manually                         |
| `subiri`     | Await (async/await support in functions/methods)            |
| `tumia`      | Import modules                                              |
| `kutoka`     | Used with `tumia` to specify module source                  |
| `kama`       | Alias keyword in imports (`as`)                             |
| `ruhusu`     | Export from a module                                        |
| `jaribu`     | Try block for error handling                                |
| `makosa`     | Catch block                                                 |
| `kisha`      | Finally block                                               |
| `onesha`     | Throw an error/exception                                    |
| `AINAYA`     | Unary keyword to return type of a value                     |
| `NINAMBARI`  | Unary keyword: check if value is number                     |
| `NINENO`     | Unary keyword: check if value is string                     |
| `NIBOOLEAN`  | Unary keyword: check if value is boolean                    |
| `NIKAZI`     | Unary keyword: check if value is function                   |
| `NILISTI`    | Unary keyword: check if value is list/array                 |
| `NIOBJECT`   | Unary keyword: check if value is object (aliased by `ob`)   |
| `NIMUUNDO`   | Unary keyword: check if value is class structure            |

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

**See more about the language documentations and learning resources [here](https://swazilang.netlify.app)**


## Contributing

Feel free to submit issues or pull requests to help improve SwaziLang!

---


## License

See [LICENSE](https://github.com/godieGH/SwaziLang/blob/main/LICENSE) for details.
