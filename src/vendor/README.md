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
