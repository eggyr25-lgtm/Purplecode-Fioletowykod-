# Purplecode 2.4 NextGen (NG) compiler Makefile

CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L
LDFLAGS  =
PREFIX  ?= /usr/local
USER_PREFIX ?= $(HOME)/.local

COMPILER_SRCS = compiler/main.c compiler/lexer.c compiler/parser.c compiler/codegen.c compiler/elfemit.c compiler/prelude.c
COMPILER_HEADERS = compiler/ast.h compiler/lexer.h compiler/parser.h compiler/codegen.h compiler/elfemit.h

.PHONY: all clean compiler test ide config install install-user bootcode boot-test purplebc purplesdk purpleimage

all: compiler purplebc purplesdk purpleimage

compiler: purplec

purplebc: purplelinker.ld
	chmod +x purplebc

# Keep the Python command front-ends executable in source checkouts.
purplesdk purpleimage:
	chmod +x $@

test: purplec purplebc
	sh ./tests.sh
	sh ./boot-tests.sh

purplec: $(COMPILER_SRCS) $(COMPILER_HEADERS)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_SRCS) $(LDFLAGS)
	@echo ""
	@echo "Built Purplecode 2.4 compiler: ./purplec"
	@echo ""
	@echo "Quick test:"
	@echo "  ./purplec examples/hello.purple -o hello.elf"
	@echo "  ./hello.elf"
	@echo ""
	@echo "GUI test:"
	@echo "  ./purplec examples/hello_gui.purple -S -o hello_gui.s"
	@echo "  gcc -no-pie hello_gui.s runtime/purple_rt.c -o hello_gui -DPURPLE_HAS_X11 -lX11 -lpng -ljpeg -lssl -lcrypto -lm"
	@echo "  ./hello_gui"

bootcode: purplebc
	mkdir -p build
	./purplebc examples/boot_hello.purplebc -o build/purpleboot.elf
	./purplebc examples/boot_hello.purplebc --format bin -o build/purpleboot.bin

boot-test: purplebc
	sh ./boot-tests.sh

clean:
	rm -f purplec compiler/*.o runtime/*.o examples/*.s out.s hello hello_gui full
	rm -rf build

ide: purplec
	python3 ide/purplecode-ide.py

config:
	python3 tools/purplecode-config.py


# Install compiler, runtime, and library installer system-wide (typically with sudo).
install: purplec
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/share/purplecode/runtime" "$(DESTDIR)$(PREFIX)/share/purplecode/tools" "$(DESTDIR)$(PREFIX)/share/purplecode/libs" "$(DESTDIR)$(PREFIX)/share/purplecode/docs"
	install -m 0755 purplec "$(DESTDIR)$(PREFIX)/bin/purplec"
	install -m 0755 purplebc "$(DESTDIR)$(PREFIX)/bin/purplebc"
	install -m 0755 purplesdk "$(DESTDIR)$(PREFIX)/bin/purplesdk"
	install -m 0644 purplelinker.ld "$(DESTDIR)$(PREFIX)/share/purplecode/purplelinker.ld"
	install -m 0644 purpletutorial "$(DESTDIR)$(PREFIX)/share/purplecode/docs/purpletutorial"
	install -m 0755 tools/purpleimage.py "$(DESTDIR)$(PREFIX)/share/purplecode/tools/purpleimage.py"
	printf '%s\n' '#!/bin/sh' 'exec python3 "$(PREFIX)/share/purplecode/tools/purpleimage.py" "$$@"' > "$(DESTDIR)$(PREFIX)/bin/purpleimage"
	chmod 0755 "$(DESTDIR)$(PREFIX)/bin/purpleimage"
	install -m 0644 runtime/purple_rt.c runtime/purple_rt.h "$(DESTDIR)$(PREFIX)/share/purplecode/runtime/"
	install -m 0755 tools/purplelibinstaller.py "$(DESTDIR)$(PREFIX)/share/purplecode/tools/purplelibinstaller.py"
	printf '%s\n' '#!/bin/sh' 'exec python3 "$(PREFIX)/share/purplecode/tools/purplelibinstaller.py" "$$@"' > "$(DESTDIR)$(PREFIX)/bin/purplelibinstaller"
	chmod 0755 "$(DESTDIR)$(PREFIX)/bin/purplelibinstaller"

# Install for the current user without sudo. Ensure ~/.local/bin is on PATH.
install-user: purplec
	install -d "$(USER_PREFIX)/bin" "$(USER_PREFIX)/share/purplecode/runtime" "$(USER_PREFIX)/share/purplecode/tools" "$(USER_PREFIX)/share/purplecode/libs" "$(USER_PREFIX)/share/purplecode/docs"
	install -m 0755 purplec "$(USER_PREFIX)/bin/purplec"
	install -m 0755 purplebc "$(USER_PREFIX)/bin/purplebc"
	install -m 0755 purplesdk "$(USER_PREFIX)/bin/purplesdk"
	install -m 0644 purplelinker.ld "$(USER_PREFIX)/share/purplecode/purplelinker.ld"
	install -m 0644 purpletutorial "$(USER_PREFIX)/share/purplecode/docs/purpletutorial"
	install -m 0755 tools/purpleimage.py "$(USER_PREFIX)/share/purplecode/tools/purpleimage.py"
	printf '%s\n' '#!/bin/sh' 'exec python3 "$(USER_PREFIX)/share/purplecode/tools/purpleimage.py" "$$@"' > "$(USER_PREFIX)/bin/purpleimage"
	chmod 0755 "$(USER_PREFIX)/bin/purpleimage"
	install -m 0644 runtime/purple_rt.c runtime/purple_rt.h "$(USER_PREFIX)/share/purplecode/runtime/"
	install -m 0755 tools/purplelibinstaller.py "$(USER_PREFIX)/share/purplecode/tools/purplelibinstaller.py"
	printf '%s\n' '#!/bin/sh' 'exec python3 "$(USER_PREFIX)/share/purplecode/tools/purplelibinstaller.py" "$$@"' > "$(USER_PREFIX)/bin/purplelibinstaller"
	chmod 0755 "$(USER_PREFIX)/bin/purplelibinstaller"
	@echo 'Installed. Ensure $(USER_PREFIX)/bin is on PATH.'
