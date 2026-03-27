# C-Compiler

`C-Compiler` is a hand-built C compiler project in C with a real multi-stage
pipeline:

- lexical analysis
- preprocessing
- recursive-descent parsing
- semantic analysis
- lowered IR-style code generation

It is still a subset compiler, not a full production C implementation, but it
now handles enough of the language to lex, preprocess, parse, validate, and
lower non-trivial C programs with useful diagnostics.

## Current Pipeline

### Lexer

The lexer recognizes:

- identifiers and C keywords
- integer literals, including decimal, octal, hexadecimal, and `0b`-style
  extensions
- floating literals, including hexadecimal floats like `0x1.fp3`
- string and character literals with prefixes like `u8`, `u`, `U`, and `L`
- comments, digraphs, and multi-character punctuators

### Preprocessor

The preprocessor currently supports:

- quoted `#include "file.h"`
- object-like and function-like `#define`
- `#undef`
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, and `#endif`
- macro expansion and token pasting with `##`

### Parser

The parser handles a meaningful C subset, including:

- typedefs, declarations, and function definitions
- pointer and array declarators
- parameter lists
- compound statements
- `if`, `for`, `while`, `return`, `break`, and `continue`
- unary, binary, conditional, cast, call, member, and subscript expressions

### Semantic Analysis

The semantic pass performs:

- scope and symbol-table tracking
- typedef resolution
- duplicate declaration checks
- undeclared identifier checks
- function-call arity checks
- assignment and return-type validation
- `break` and `continue` validation
- simple non-void return-path checks

### Code Generation

The backend emits a readable lowered IR for the supported subset. It is not
native assembly yet, but it gives the project a real codegen stage and a solid
bridge to a future machine backend.

## Commands

Build the compiler:

```bash
make
```

The binary is written to:

```text
build/bin/ccompiler
```

Run the raw lexer:

```bash
./build/bin/ccompiler lex examples/feature_showcase.c
```

You can also omit `lex`:

```bash
./build/bin/ccompiler examples/feature_showcase.c
```

Run preprocessing:

```bash
./build/bin/ccompiler preprocess examples/feature_showcase.c
```

Print the raw AST:

```bash
./build/bin/ccompiler parse examples/feature_showcase.c
```

Run semantic analysis on the preprocessed translation unit:

```bash
./build/bin/ccompiler check examples/feature_showcase.c
```

Emit lowered IR:

```bash
./build/bin/ccompiler codegen examples/feature_showcase.c
```

## Example Output

Semantic analysis:

```text
semantic analysis succeeded
functions: 2
globals: 0
typedefs: 1
```

Code generation:

```text
func main(void) -> int
entry:
  t0 = call mix(7u, 0.25)
  t1 = cast int, t0
  ret t1
endfunc
```

## Installation

### Requirements

- a C11-compatible compiler such as `clang` or `gcc`
- `make`
- `clang` if you want to run `make analyze`

### Build

```bash
git clone <your-repo-url>
cd C-Compiler
make
```

### Optional PATH Install

```bash
install -m 755 build/bin/ccompiler /usr/local/bin/ccompiler
```

If your system requires elevated permissions for `/usr/local/bin`, use `sudo`.

## Verification

Run the smoke-test suite:

```bash
make test
```

Run static analysis:

```bash
make analyze
```

Run both:

```bash
make verify
```

The repository also includes a GitHub Actions workflow at
`.github/workflows/ci.yml` that runs `make verify` on pushes and pull requests.

## Project Layout

```text
C-Compiler/
├── Makefile
├── README.md
├── examples/
├── include/ccompiler/
│   ├── codegen.h
│   ├── lexer.h
│   ├── parser.h
│   ├── preprocessor.h
│   ├── sema.h
│   └── token.h
├── src/ccompiler/
│   ├── codegen.c
│   ├── diagnostic.c
│   ├── lexer.c
│   ├── main.c
│   ├── parser.c
│   ├── preprocessor.c
│   ├── sema.c
│   └── token.c
└── tests/
    ├── codegen_smoke.sh
    ├── lexer_smoke.sh
    ├── parser_smoke.sh
    ├── preprocessor_smoke.sh
    └── sema_smoke.sh
```

## Limits

This project still intentionally stops short of being a full ISO C compiler.
Some notable limits:

- the preprocessor currently focuses on quoted includes and common macro flows
- semantic typing is intentionally lightweight
- the backend emits textual IR, not object code or machine assembly
- advanced C constructs like full struct layout, switch lowering, and function
  pointers are not complete yet

## Next Steps

Strong next upgrades would be:

- richer type checking and struct/union semantics
- source mapping through preprocessing and includes
- native assembly or object-file emission
- optimization passes over the generated IR

## License

Add a license file if you want to distribute or publish the project.
