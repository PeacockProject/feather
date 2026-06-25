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
	src/cmd_index.c \
	src/cmd_key.c \
	src/keyring.c \
	src/manifest.c \
	src/install.c \
	src/resolve.c \
	src/db.c \
	src/util.c \
	src/repo.c \
	src/verify.c \
	src/randombytes.c

OBJ := $(SRC:.c=.o)

# Vendored single-file libraries. Kept byte-identical (or near-so)
# to upstream so resync stays a `cp`. Compiled through dedicated rules
# rather than the generic src/%.o rule so future upstream syncs that
# trip a new warning can be waived here without polluting the global
# CFLAGS for first-party code.
VENDOR_OBJ := \
	src/vendor/toml.o \
	src/vendor/sha256.o \
	src/vendor/tweetnacl.o

# Per-file warning waivers for TweetNaCl's 2014 snapshot. Two
# diagnostics fire under our strict CFLAGS; both are stylistic and
# scoped to the vendored code, never to first-party sources.
TWEETNACL_WAIVERS := \
	-Wno-sign-compare \
	-Wno-unterminated-string-initialization

BIN := ftr

.PHONY: all build clean install test

all: build

build: $(BIN)

$(BIN): $(OBJ) $(VENDOR_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(VENDOR_OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -c -o $@ $<

src/vendor/toml.o: src/vendor/toml.c src/vendor/toml.h
	$(CC) $(CFLAGS) -Isrc/vendor -c -o $@ $<

src/vendor/sha256.o: src/vendor/sha256.c src/vendor/sha256.h
	$(CC) $(CFLAGS) -Isrc/vendor -c -o $@ $<

src/vendor/tweetnacl.o: src/vendor/tweetnacl.c src/vendor/tweetnacl.h
	$(CC) $(CFLAGS) $(TWEETNACL_WAIVERS) -Isrc/vendor -c -o $@ $<

# --- auxiliary build artifacts ---------------------------------------------
# tools/gen-keypair wraps TweetNaCl's crypto_sign_keypair() with a
# deterministic, seed-driven randombytes() so tests can stamp out
# reproducible minisign keypairs without depending on a system
# `minisign` binary. tools/ftr-sign signs an arbitrary file with such
# a keypair. Neither is part of the default build nor installed by
# `make install`; the deterministic randombytes() defined inside each
# tool intentionally shadows the production /dev/urandom-backed one,
# which is why src/randombytes.o is NOT linked here.
TOOL_LINK_OBJ := src/verify.o src/util.o $(VENDOR_OBJ)

tools/gen-keypair: tools/gen-keypair.c $(TOOL_LINK_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -Isrc/vendor -o $@ \
	    tools/gen-keypair.c $(TOOL_LINK_OBJ)

tools/ftr-sign: tools/ftr-sign.c $(TOOL_LINK_OBJ) src/randombytes.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -Isrc/vendor -o $@ \
	    tools/ftr-sign.c $(TOOL_LINK_OBJ) src/randombytes.o

clean:
	rm -f $(BIN) *.o src/*.o src/vendor/*.o \
	    tools/gen-keypair tools/ftr-sign

install: build
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(BIN) "$(DESTDIR)$(BINDIR)/$(BIN)"

test: build tools/gen-keypair tools/ftr-sign
	./tests/smoke_help.sh
	./tests/phase4_local_install.sh
	./tests/phase4c_symlink_guard.sh
	./tests/phase4b_repo.sh
	./tests/phase6_signed_repo.sh
	./tests/phase7_resolve.sh
	./tests/phase8_gpg.sh
	./tests/phase9_index.sh
