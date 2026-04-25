# Lumi-Compiler

Lumi Compiler is a hand-built C compiler project in C with a real multi-stage
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
- integer literals, including decimal, octal, hexadecimal, and 0b-style
  extensions
- floating literals, including hexadecimal floats like 0x1.fp3
- string and character literals with prefixes like u8, u, U, and L
- comments, digraphs, and multi-character punctuators

### Preprocessor

The preprocessor currently supports:

- quoted includes like #include "file.h"
- local angle-bracket includes like #include <file.h>
- object-like and function-like #define
- #undef
- #if, #ifdef, #ifndef, #elif, #else, and #endif
- defined(NAME) and defined NAME expressions in #if and #elif
- macro expansion and token pasting with ##

### Parser

The parser handles a meaningful C subset, including:

- typedefs, declarations, and function definitions
- pointer and array declarators
- parameter lists
- compound statements
- labels and goto
- if, for, while, do ... while, and switch
- case, default, return, break, and continue
- unary, binary, conditional, cast, call, member, and subscript expressions

### Semantic Analysis

The semantic pass performs:

- scope and symbol-table tracking
- typedef resolution
- duplicate declaration checks
- undeclared identifier checks
- function-call arity checks
- assignment and return-type validation
- break, continue, case, and default validation
- label resolution for goto
- duplicate label, case, and default detection
- simple non-void return-path checks

### Code Generation

The backend lowers parsed C code into a readable intermediate representation (IR) for a supported subset of the language. It handles control flow constructs including loops, conditionals, switch statements, labels, and goto. Basic constant folding is applied to integer expressions during lowering. While the compiler does not yet emit native assembly, this stage establishes a functional code generation pipeline and provides a solid foundation for a future machine-level backend.

## Commands

Build the compiler:

bash
make


The binary is written to:

text
build/bin/C-Compiler


Run the raw lexer:

bash
./build/bin/C-Compiler lex examples/feature_showcase.c


You can also omit lex:

bash
./build/bin/C-Compiler examples/feature_showcase.c


Run preprocessing:

bash
./build/bin/C-Compiler preprocess examples/feature_showcase.c


Print the raw AST:

bash
./build/bin/C-Compiler parse examples/feature_showcase.c


Run semantic analysis on the preprocessed translation unit:

bash
./build/bin/C-Compiler check examples/feature_showcase.c


Emit lowered IR:

bash
./build/bin/C-Compiler code gen examples/feature_showcase.c


## Example Output

Semantic analysis:

text
semantic analysis succeeded
functions: 2
globals: 0
typedefs: 1


Code generation:

text
func main(void) -> int
entry:
  t0 = call mix(7, 0.25)
  t1 = cast int, t0
  ret t1
end function


## Installation

### Requirements

- a C11-compatible compiler such as clang or gcc
- make
- clang if you want to run make analyze

### Build

bash
git clone <https://github.com/IzonIcy/C-Compiler>
cd C-Compiler
make


### Optional PATH Install

bash
install -m 755 build/bin/C-Compiler /usr/local/bin/C-Compiler


If your system requires elevated permissions for /usr/local/bin, use sudo.

## Verification

Run the smoke-test suite:

bash
make test


Run static analysis:

bash
make analyze


Run both:

bash
make verify


The repository also includes a GitHub Actions workflow at
.github/workflows/ci.yml that runs make verify on pushes and pull requests.

## Limits

This project still intentionally stops short of being a full ISO C compiler.
Some notable limits:

- the preprocessor still focuses on local include resolution and common macro flows
- semantic typing is intentionally lightweight
- the backend emits textual IR, not object code or machine assembly
- advanced C constructs like full struct layout and function pointers are not complete yet
