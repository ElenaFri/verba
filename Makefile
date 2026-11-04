# verba — build, test, install.
#
# Targets: all (default), test, clean, install, uninstall.
# Override CC (e.g. `make CC=musl-gcc`) or PREFIX (e.g. `make install PREFIX=~/.local`).

VERSION := 0.0.0

CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Werror
CPPFLAGS += -D_GNU_SOURCE -DVERBA_VERSION=\"$(VERSION)\"
CFLAGS  ?= -O2 -g
CFLAGS  += $(CSTD) $(WARN)
COVFLAGS := $(CSTD) $(WARN) -O0 -g --coverage

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
MANDIR := $(PREFIX)/share/man/man1

BIN := verba
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

# The unit tests link the pure-logic objects (everything bar the ptrace tracer
# and the CLI entry point, which carry ptrace/main() and are covered end-to-end).
UNIT_OBJ := src/heuristic.o src/model.o src/patterns.o src/reconcile.o \
            src/report.o src/util.o src/vec.o src/words.o
TESTBINS := tests/unit tests/syscall_zoo

# The syscall zoo is never instrumented: it is coverage *input* (run under
# verba), not a subject of measurement.
ZOO_CFLAGS := $(CSTD) $(WARN) -O0 -g

.PHONY: all test coverage coverage-html clean install uninstall

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/unit: tests/unit.c $(UNIT_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(LDFLAGS) -o $@ tests/unit.c $(UNIT_OBJ)

tests/syscall_zoo: tests/syscall_zoo.c
	$(CC) $(CPPFLAGS) $(ZOO_CFLAGS) -o $@ $<

test: $(BIN) $(TESTBINS)
	# The static fixture needs a static libc; the leading '-' tolerates
	# toolchains without one (run.sh then skips the check).
	-$(CC) $(CPPFLAGS) $(CSTD) $(WARN) -O0 -g -static -o tests/static-liar \
		tests/static-liar.c
	./tests/unit
	sh tests/run.sh ./$(BIN) tests/syscall_zoo

# Rebuild instrumented, run the full suite (unit tests + CLI + fixtures + the
# syscall zoo), summarise. Leaves coverage.info for upload (Codecov) and the raw
# .gcda/.gcno behind; run `make clean` to reset.
coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(COVFLAGS)" LDFLAGS="--coverage" test
	lcov --capture --directory src --output-file coverage.info
	lcov --list coverage.info

coverage-html: coverage
	genhtml coverage.info --output-directory coverage
	@echo 'coverage report: coverage/index.html'

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MANDIR)
	install -m 0644 man/verba.1 $(DESTDIR)$(MANDIR)/verba.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/verba.1

clean:
	rm -f $(BIN) $(OBJ) $(TESTBINS) tests/static-liar
	rm -f src/*.gcda src/*.gcno tests/*.gcda tests/*.gcno coverage.info
	rm -rf coverage
