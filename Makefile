CC = gcc
CFLAGS = -O2 -I src/include
PYTHON = python3

SRC_DIR = src
BOOTSTRAP_DIR = $(SRC_DIR)/bootstrap
SELFHOST_DIR = $(SRC_DIR)/selfhost
BIN_DIR = bin
BUILD_DIR = build
TESTS_DIR = tests

# Targets
.PHONY: all clean bootstrap selfhost test

all: bootstrap selfhost

clean:
	rm -rf $(BIN_DIR)/* $(BUILD_DIR)/*
	rm -f $(SELFHOST_DIR)/*.c $(TESTS_DIR)/*.c

# Phase 0: Bootstrap Compiler (Python -> C -> Executable)
# bunkerc.py inlines the runtime, so no -I needed for the internal gcc call,
# but we need to ensure the python script can find its imports if any.
bootstrap: $(BIN_DIR)/bunker-stage0

$(BIN_DIR)/bunker-stage0: $(BOOTSTRAP_DIR)/bunkerc.py $(SELFHOST_DIR)/bunker.bnk
	@mkdir -p $(BIN_DIR)
	@echo "--- Building Stage 0 (Bootstrap) ---"
	$(PYTHON) $(BOOTSTRAP_DIR)/bunkerc.py $(SELFHOST_DIR)/bunker.bnk -o $(BIN_DIR)/bunker-stage0

# Phase 1: Self-Hosted Compiler (Stage 0 -> C -> Executable)
# The stage0 compiler outputs <source>.c next to the source file.
selfhost: $(BIN_DIR)/bunker-stage1

$(BIN_DIR)/bunker-stage1: $(BIN_DIR)/bunker-stage0 $(SELFHOST_DIR)/bunker.bnk
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)
	@echo "--- Building Stage 1 (Self-Host) ---"
	$(BIN_DIR)/bunker-stage0 $(SELFHOST_DIR)/bunker.bnk
	@# Move the generated file from src/selfhost/bunker.bnk.c to build/
	mv $(SELFHOST_DIR)/bunker.bnk.c $(BUILD_DIR)/stage1.c
	@echo "--- Compiling Stage 1 C Code ---"
	$(CC) $(CFLAGS) $(BUILD_DIR)/stage1.c -o $(BIN_DIR)/bunker-stage1

# Test: Compile hello code using Stage 1
test: $(BIN_DIR)/bunker-stage1
	@echo "--- Testing Stage 1 ---"
	$(BIN_DIR)/bunker-stage1 $(TESTS_DIR)/hello.bnk
	mv $(TESTS_DIR)/hello.bnk.c $(BUILD_DIR)/hello.c
	$(CC) $(CFLAGS) $(BUILD_DIR)/hello.c -o $(BIN_DIR)/hello
	@echo "--- Running Hello ---"
	$(BIN_DIR)/hello
