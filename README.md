# Bunker Programming Language

**Architecture of Digital Sovereignty**

[![Status](https://img.shields.io/badge/Status-Production%20Ready-brightgreen)](https://github.com/bunker-lang/bunker)
[![Version](https://img.shields.io/badge/Version-2.6.0-blue)](docs/SPEC.md)

Bunker is a systems programming language specialized in Security, Digital Sovereignty, and Resilience.

## Project Structure

This repository follows a standard bootstrap compiler layout:

```text
bunker/
├── docs/                   # Specifications and design docs
│   ├── SPEC.md             # The Language Specification (v2.6.0)
│   └── grammars/           # EBNF and PEG grammars
├── src/
│   └── bootstrap/          # Stage 0 Compiler (Python Implementation)
│       ├── bunkerc.py      # Bootstrap Compiler (Transpiler -> C11)
│       ├── bunker_cli.py   # CLI entry point (`bunker`)
│       └── bunker_pkg.py   # Package Manager logic
├── tests/                  # Test suite
└── examples/               # Example Bunker programs
```

## Getting Started (Bootstrap)

The current implementation is in **Stage 0 (Bootstrap)**, written in Python to allow rapid semantics validation before self-hosting.

### Prerequisites

- Python 3.8+
- GCC or Clang (for the C backend)

### CLI Usage

The `bunker` command is simulated by `src/bootstrap/bunker_cli.py`.

1.  **Create a New Project**:

    ```bash
    python3 src/bootstrap/bunker_cli.py new my_app
    ```

    This creates a new directory `my_app` with:
    - `bunker.toml`: Project manifest
    - `src/main.bnk`: Source code

2.  **Build a Project**:

    ```bash
    cd my_app
    python3 ../src/bootstrap/bunker_cli.py build
    ```

    This will:
    - Parse your `.bnk` files using the Stage 0 compiler.
    - Transpile them to C11 (`build/`).
    - Compile the C code to a native executable in `target/`.

3.  **Run**:
    ```bash
    ./target/my_app
    ```

## Development

- **Run Tests**:
  ```bash
  python3 tests/test_bunker_cli.py
  ```

---

_Created by the Bunker Team_
