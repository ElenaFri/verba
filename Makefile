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

.PHONY: all test coverage coverage-html clean install uninstall

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	sh tests/run.sh ./$(BIN)

# Rebuild instrumented, run the suite, summarise. Leaves coverage.info for
# upload (Codecov) and the raw .gcda/.gcno behind; run `make clean` to reset.
coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(COVFLAGS)" LDFLAGS="--coverage"
	sh tests/run.sh ./$(BIN)
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
	rm -f $(BIN) $(OBJ)
	rm -f src/*.gcda src/*.gcno coverage.info
	rm -rf coverage
