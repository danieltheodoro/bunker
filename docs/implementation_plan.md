# Bunker Language Implementation Plan

**Goal**: Begin the implementation of the Bunker "Production Ready" ecosystem (v2.4.0), starting with the Package Manager (`bunker-pkg`) and CLI tools, to enable project bootstrapping and management.

## User Review Required

> [!NOTE]
> This plan focuses on building the **tooling** around the language first (CLI, Package Manager). The compiler/interpreter integration will follow once the project structure is definable.

## Proposed Changes

We will create a Python-based CLI tool (for prototyping speed, as per previous parser implementation) that acts as the `bunker` command.

### CLI & Package Manager

#### [NEW] [bunker_cli.py](file:///Users/danieltheodoro/.gemini/antigravity/brain/97d11f21-fefc-4570-acfb-716fb5bf93f8/bunker_cli.py)

This file will be the main entry point for the `bunker` command.

- **Features**:
  - `bunker new <name>`: Scaffolds a new project with `bunker.toml` and directory structure.
  - `bunker build`: Placeholder for build command (will call parser later).
  - `bunker test`: Placeholder for test runner.
- **Dependencies**: `argparse`, `toml` (if available, or simple parsing), `os`, `shutil`.

#### [NEW] [bunker_pkg.py](file:///Users/danieltheodoro/.gemini/antigravity/brain/97d11f21-fefc-4570-acfb-716fb5bf93f8/bunker_pkg.py)

Implements the core logic for the Package Manager.

- **Structs/Classes**:
  - `Manifest`: Represents `bunker.toml`.
  - `Project`: Represents the loaded project.
- **Functions**:
  - `create_project(name, template)`: Generates files.
  - `read_manifest(path)`: Parses `bunker.toml`.

### Compiler Backend (Stage 0: Bootstrap)

#### [NEW] [bunkerc.py](file:///Users/danieltheodoro/.gemini/antigravity/brain/97d11f21-fefc-4570-acfb-716fb5bf93f8/bunkerc.py)

This will be the **Bootstrap Compiler**. It will take Bunker source code and compile it into a native binary.

- **Strategy**: Transpilation to C11.
  - **Why**: Allows leveraging existing optimizing compilers (GCC/Clang) for performance immediately.
  - **Process**: Source (`.bnk`) -> Tokenize -> Parse (AST) -> Codegen (`.c`) -> CC -> Binary (`.exe`).

- **Components**:
  - **Lexer/Parser**: Consumes `src/main.bnk`.
    - [x] Hello World (Modules, Methods)
    - [x] Variables (`x <- 10`, `y: int <- 20`)
    - [x] Arithmetic (`+`, `-`, `*`, `/`)
    - [ ] Control Flow (`if condition: block.`, `while condition: block.`)
  - **Transpiler**: Maps Bunker constructs to C.
  - **Interpreter (REPL)**:
    - [x] Basic Printing
    - [x] Variable Store (Environment)
    - [x] Expression Evaluation
    - [x] Conditionals and Loops
  - **Type Checker (New)**:
    - Validates AST before Transpilation/Execution.
    - Enforces:
      - Variable redeclaration type consistency.
      - Binary operand compatibility (e.g., no `int + string`).
      - Assignment type matching (`x: int <- "str"` is Error).
  - **Transpiler**: Maps Bunker constructs to C.
    - [x] Handle `AssignStmt`: generate `int x = ...` or `x = ...`.
    - [x] Handle `BinaryExpr`: map operators `+`, `-`, `*`, `/`.
    - [x] Handle `IfStmt` / `WhileStmt`: map to C `if/while`.
  - **Builder**: orchestrates the call to the system C compiler (`clang` or `gcc`).

### Standard Library (v0.1.0)

Implementation of `bunker.std` modules required for Self-Hosting (File I/O).

#### `bunker.std.io`

- **Entity File**:
  - `open(path: str) -> result File or str`: Maps to `fopen`.
  - `readToString(borrow mut str) -> result`: Maps to `fread`.
  - `writeStr(s: str) -> result`: Maps to `fwrite`.
  - `close()`: Maps to `fclose`.
- **Global**: `print`, `println` (Wrappers around C `printf`).

#### `bunker.std.fs`

- **Global**:
  - `write(path: str, content: arr) -> result`: Helper using `File`.
  - `readToString(path: str) -> result`: Helper using `File`.

#### Implementation Strategy

- **Native Modules**: The compiler will have built-in knowledge of `bunker.std`.
- **C Mapping**:
  - `File` -> `FILE*` (opaque pointer).
  - Methods will form C functions like `bunker_std_io_File_open`.

#### [MODIFY] [bunker_cli.py](file:///Users/danieltheodoro/.gemini/antigravity/brain/97d11f21-fefc-4570-acfb-716fb5bf93f8/bunker_cli.py)

Update the `build` command to invoke `bunkerc.py`.

- **Flow**:
  1. `bunker build` reads `bunker.toml`.
  2. Invokes `bunkerc.py` on `src/main.bnk`.
  3. `bunkerc.py` generates `build/main.c`.
  4. `bunkerc.py` runs `cc build/main.c -o target/app`.

## Verification Plan

### Automated Tests

We will create a test script `test_bunker_cli.py` to verify the CLI commands.

1.  **Test Project Creation**:
    - Run `python3 bunker_cli.py new my_test_app`
    - Verify `my_test_app/bunker.toml` exists.
    - Verify `my_test_app/src/main.bnk` exists.

2.  **Test Manifest Parsing**:
    - Create a dummy `bunker.toml`
    - Run `python3 bunker_cli.py check` (to be implemented as functionality check)

### Manual Verification

- Run the tool manually to create a "Hello World" project.
- Inspect the generated file contents against the `SPEC.md` v2.4.0 examples.
