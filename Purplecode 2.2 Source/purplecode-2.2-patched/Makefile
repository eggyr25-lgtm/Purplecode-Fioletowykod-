# Purplecode 2.1 – Systems language (no toy stubs)

CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L
LDFLAGS  =
PREFIX  ?= /usr/local
USER_PREFIX ?= $(HOME)/.local

COMPILER_SRCS = compiler/main.c compiler/lexer.c compiler/parser.c compiler/codegen.c compiler/elfemit.c compiler/prelude.c
COMPILER_HEADERS = compiler/ast.h compiler/lexer.h compiler/parser.h compiler/codegen.h compiler/elfemit.h

BOOT_SRCS = compiler/bootmain.c compiler/native_mb.c
BOOT_HEADERS = compiler/native_mb.h

.PHONY: all clean compiler test ide config install install-user bootcode boot-test purplebc purplesdk purpleimage

all: compiler purplebc purplesdk purpleimage mbcheck

compiler: purplec

# Pure C binary – native Multiboot by default, --gcchost for GCC/Clang path
purplebc: $(BOOT_SRCS) $(BOOT_HEADERS) purplelinker.ld
	$(CC) $(CFLAGS) -o $@ $(BOOT_SRCS) $(LDFLAGS)
	@echo ""
	@echo "Built Purplebootcode (C binary): ./purplebc"
	@echo "  Default : native machine code (no GCC wrapper)"
	@echo "  Optional: --gcchost   use GCC/Clang host toolchain"
	@echo "  ./purplebc examples/boot_hello.purplebc -o kernel.elf"
	@echo "  ./purplebc examples/boot_hello.purplebc --format bin -o kernel.bin"
	@echo "  ./purplebc examples/uefi_hello.purplebc --format uefi --gcchost -o BOOTX64.EFI"
	@echo ""

# purpleimage is now a pure C binary (no Python)
mbcheck: tools/mbcheck.c
	$(CC) $(CFLAGS) -o $@ tools/mbcheck.c $(LDFLAGS)
	@echo "Built mbcheck (pure C Multiboot validator)"

purpleimage: tools/purpleimage.c
	$(CC) $(CFLAGS) -o $@ tools/purpleimage.c $(LDFLAGS)
	@echo "Built purpleimage (C binary, no Python)"

purplesdk:
	chmod +x $@ 2>/dev/null || true

test: purplec purplebc purpleimage mbcheck
	sh ./tests.sh
	sh ./boot-tests.sh

purplec: $(COMPILER_SRCS) $(COMPILER_HEADERS)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_SRCS) $(LDFLAGS)
	@echo ""
	@echo "Built Purplecode 2.1 compiler: ./purplec"
	@echo "  Hosted path (Linux). Use --direct for pure ELF64, no C runtime."
	@echo ""

bootcode: purplebc
	mkdir -p build
	./purplebc examples/boot_hello.purplebc -o build/purpleboot.elf
	./purplebc examples/boot_hello.purplebc --format bin -o build/purpleboot.bin

boot-test: purplebc
	sh ./boot-tests.sh

clean:
	rm -f purplec purplebc compiler/*.o runtime/*.o examples/*.s out.s hello hello_gui full
	rm -rf build




# Install compiler, runtime, and library installer system-wide (typically with sudo).
install: purplec
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/share/purplecode/runtime" "$(DESTDIR)$(PREFIX)/share/purplecode/tools" "$(DESTDIR)$(PREFIX)/share/purplecode/libs" "$(DESTDIR)$(PREFIX)/share/purplecode/docs"
	install -m 0755 purplec "$(DESTDIR)$(PREFIX)/bin/purplec"
	install -m 0755 purplebc "$(DESTDIR)$(PREFIX)/bin/purplebc"
	install -m 0755 purplesdk "$(DESTDIR)$(PREFIX)/bin/purplesdk"
	install -m 0644 purplelinker.ld "$(DESTDIR)$(PREFIX)/share/purplecode/purplelinker.ld"
	install -m 0644 purpletutorial "$(DESTDIR)$(PREFIX)/share/purplecode/docs/purpletutorial"
	install -m 0755 purpleimage "$(DESTDIR)$(PREFIX)/bin/purpleimage"
	install -m 0644 runtime/purple_rt.c runtime/purple_rt.h runtime/purple_blow.inc runtime/purple_net.inc runtime/purple_files.inc "$(DESTDIR)$(PREFIX)/share/purplecode/runtime/"

# Install for the current user without sudo. Ensure ~/.local/bin is on PATH.
install-user: purplec
	install -d "$(USER_PREFIX)/bin" "$(USER_PREFIX)/share/purplecode/runtime" "$(USER_PREFIX)/share/purplecode/tools" "$(USER_PREFIX)/share/purplecode/libs" "$(USER_PREFIX)/share/purplecode/docs"
	install -m 0755 purplec "$(USER_PREFIX)/bin/purplec"
	install -m 0755 purplebc "$(USER_PREFIX)/bin/purplebc"
	install -m 0755 purplesdk "$(USER_PREFIX)/bin/purplesdk"
	install -m 0644 purplelinker.ld "$(USER_PREFIX)/share/purplecode/purplelinker.ld"
	install -m 0644 purpletutorial "$(USER_PREFIX)/share/purplecode/docs/purpletutorial"
	install -m 0755 purpleimage "$(DESTDIR)$(PREFIX)/bin/purpleimage"
	install -m 0644 runtime/purple_rt.c runtime/purple_rt.h runtime/purple_blow.inc runtime/purple_net.inc runtime/purple_files.inc "$(USER_PREFIX)/share/purplecode/runtime/"
	@echo 'Installed. Ensure $(USER_PREFIX)/bin is on PATH.'
