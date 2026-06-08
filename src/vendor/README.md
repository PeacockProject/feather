# Vendored third-party code

Files in this directory are imported verbatim from upstream projects.
They are vendored (rather than declared as build dependencies) so that
`ftr` can be built with `make` + a C99 compiler and nothing else —
matching feather's phase 2 "no external libs" policy. Phase 4 carves
out exactly one exception: a TOML parser, needed to read package
manifests embedded in `.feather` archives.

## tomlc99

- **Upstream:** https://github.com/cktan/tomlc99
- **Commit:**   `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac`
- **License:**  MIT (see header of `toml.c` and `toml.h`)
- **Files:**    `toml.c`, `toml.h`

Single-file TOML 1.0 parser in pure C99. Compiles clean under feather's
`-Wall -Wextra -Werror -pedantic -std=c99` without any waivers.

To resync against upstream:

```sh
git clone https://github.com/cktan/tomlc99 /tmp/tomlc99
cp /tmp/tomlc99/toml.c /tmp/tomlc99/toml.h src/vendor/
# bump the commit hash above
```

Do not edit the vendored sources locally — keep them byte-identical to
upstream so resyncing stays a `cp`.

## sha256

- **Upstream:** https://github.com/B-Con/crypto-algorithms
  (file `sha256.c` + `sha256.h`)
- **License:**  public domain (per upstream README)
- **Files:**    `sha256.c`, `sha256.h`

Self-contained SHA-256 implementation, ~150 lines. Used by feather's
repo client to verify `sha256` fields against fetched archives.

Local edits vs. upstream:

- Combined into a single `.c` + `.h` matching feather's vendor layout.
- Upstream's `BYTE`/`WORD`/`LONG` typedefs replaced with `<stdint.h>`
  fixed-width types so the file stays clean under
  `-Wall -Wextra -Werror -pedantic -std=c99` on platforms where
  `unsigned long` is not 32 bits.
- Added `sha256_file_hex(path, hex_out)` convenience that streams a
  file and returns the lowercase-hex digest. Not present upstream.

Verified against FIPS-180-4 Appendix A test vectors. Do not modify
without re-running those vectors.

## TweetNaCl

- **Upstream:** https://tweetnacl.cr.yp.to/
- **Version:** `20140427` snapshot (`tweetnacl.c` + `tweetnacl.h`)
- **License:** public domain
- **Files:** `tweetnacl.c`, `tweetnacl.h`

Self-contained NaCl reimplementation by D. J. Bernstein, Bernard van
Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe and Sjaak Smetsers.
Single ~700-line C file. Used by feather phase 6 for Ed25519 signature
verification (minisign) and SHA-512 (TweetNaCl's `crypto_hash`).

The 2014 snapshot tickles two warnings under feather's strict CFLAGS:

- `-Wsign-compare` — TweetNaCl's `FOR(i,n)` macro compares a signed
  index against an unsigned bound throughout. Waived per-file in the
  Makefile.
- `-Wunterminated-string-initialization` — the `expand 32-byte k`
  Salsa20 constant fills `u8 sigma[16]` exactly, no room for NUL.
  Waived per-file in the Makefile.

Both waivers are scoped to `src/vendor/tweetnacl.o` and never touch
first-party code. TweetNaCl declares `extern void randombytes(u8 *,
u64)` but provides no implementation; feather supplies a
`/dev/urandom`-backed one in `src/randombytes.c`. The signature-verify
path (`crypto_sign_open`) does not call it; only keypair generation
does.

To resync:

```sh
wget https://tweetnacl.cr.yp.to/20140427/tweetnacl.c -O src/vendor/tweetnacl.c
wget https://tweetnacl.cr.yp.to/20140427/tweetnacl.h -O src/vendor/tweetnacl.h
```

Do not edit the vendored sources locally.
