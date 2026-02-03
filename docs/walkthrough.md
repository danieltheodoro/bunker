# Bunker v2.6.0 CLI Walkthrough

This document demonstrates the functionality of the newly implemented **Bunker CLI** and **Package Manager**.

## 1. Overview

The `bunker` command (simulated by `bunker_cli.py`) is the entry point for all language tools. It supports:

- **Creating Projects**: Scaffolding new applications.
- **Building**: Compiling projects (prototype integration).
- **Testing**: Running tests (prototype).

## 2. Creating a New Project

To create a new project, use the `new` command:

```bash
python3 bunker_cli.py new my_app
```

This generates the following structure:

```text
my_app/
├── bunker.toml      # Project manifest (v2.6.0 edition)
├── .gitignore       # Git configuration
├── src/
│   └── main.bnk     # Hello World entry point
└── tests/           # Test directory
```

### Generated `bunker.toml`

```toml
[package]
name = "my_app"
version = "0.1.0"
authors = ["User"]
edition = "2026"

[dependencies]
```

### Generated `src/main.bnk`

```bunker
module my_app

Entity MyAppApp:
    has a public method main:
        print: "Hello, Bunker v2.6.0!".
    .
.
```

## 3. Building the Project

Navigate to the project directory and run `build`:

```bash
cd my_app
python3 ../bunker_cli.py build
```

**Output**:

```text
Building my_app v0.1.0...
Compiling src/main.bnk...
Build successful (prototype).
```

## 4. Verification

The implementation has been verified with **automated end-to-end tests** (`test_bunker_cli.py`), which ensure:

- **Project Structure**: Creates valid v2.6.0 directory layout.
- **Compilation**: `bunker build` successfully invokes the bootstrap compiler (`bunkerc.py`).
- **Native Execution**: The resulting binary runs and prints "Hello, Bunker v2.6.0!".

### Compiler Capabilities (Stage 0)

The bootstrap compiler (`bunkerc.py`) currently supports:

- **Lexing**: Tokenizes Module, Entity, Method, String keywords.
- **Parsing**: Recursive Descent Parser producing an AST for basic constructs.
- **Codegen**: Transpiles AST to C11 code.
- **Linking**: Invokes system `cc` compiler to produce native executables.

## 5. Next Steps

- Expand the parser to support the full grammar (Control Flow, Expressions).
- Implement the Type Checker.
- Add support for the Standard Library.
- Begin work on self-hosting (Stage 1).
