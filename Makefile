CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g
CPPFLAGS ?= -Iinclude
ANALYZER ?= clang

TARGET := build/bin/C-Compiler
SOURCES := $(wildcard src/ccompiler/*.c)
OBJECTS := $(patsubst src/%.c,build/obj/%.o,$(SOURCES))

.PHONY: all clean test analyze verify

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	sh tests/lexer_smoke.sh
	sh tests/parser_smoke.sh
	sh tests/preprocessor_smoke.sh
	sh tests/sema_smoke.sh
	sh tests/codegen_smoke.sh

analyze:
	$(ANALYZER) --analyze $(CPPFLAGS) $(CFLAGS) src/ccompiler/*.c

verify: test analyze

clean:
	rm -rf build
