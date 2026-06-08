# Makefile — feather (`ftr`), the Peacock OS package manager.
#
# Plain make, no autotools, no CMake. Mirrors pacman's
# src/pacman/Makefile.am style at a high level: flat, readable, one
# target per concern. Phases 4 / 6 add dependencies (a TOML parser,
# a signature lib); for the phase 2 skeleton no external libs are
# linked.
#
# Common targets:
#
#   make build     — compile src/*.c into ./ftr (statically linked)
#   make clean     — remove build artifacts
#   make install   — install ftr into $(DESTDIR)$(BINDIR)
#   make test      — run the smoke test in tests/
#
# Override PREFIX / DESTDIR per the usual conventions:
#
#   make install PREFIX=/usr DESTDIR=/tmp/stage

PREFIX  ?= /peacock
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=

CC      ?= cc
CSTD    ?= -std=c99
WARNS   := -Wall -Wextra -Werror -pedantic
CFLAGS  ?= -O2
CFLAGS  += $(CSTD) $(WARNS)
LDFLAGS ?= -static

SRC := \
	src/main.c \
	src/cmd_install.c \
	src/cmd_remove.c \
	src/cmd_sync.c \
	src/cmd_upgrade.c \
	src/cmd_search.c \
	src/cmd_info.c \
	src/cmd_list.c \
	src/cmd_files.c \
	src/cmd_flavor.c \
	src/manifest.c \
	src/install.c \
	src/db.c \
	src/repo.c \
	src/verify.c

OBJ := $(SRC:.c=.o)

BIN := ftr

.PHONY: all build clean install test

all: build

build: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

clean:
	rm -f $(BIN) *.o src/*.o

install: build
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(BIN) "$(DESTDIR)$(BINDIR)/$(BIN)"

test: build
	./tests/smoke_help.sh
