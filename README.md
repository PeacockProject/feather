# feather

`ftr` — the Peacock OS package manager.

Peacock is a meta-distro: a base distro (Arch, Debian, Alpine, ...) owns
`/usr`, `/etc`, `/var`, `/home`; feather owns the Peacock platform
layer at `/peacock`, user apps at `/apps`, runtime shims at `/compat`,
and per-app state at `/data`. The base package manager (pacman / apt /
apk) manages the base system; `ftr` manages everything Peacock-specific
on top.

This repo is the C source for the `ftr` binary. It ships as
`/peacock/bin/ftr` and is statically linked so it can run inside any
flavor's chroot without dragging in a libc dependency.

## Status

**Phase 2 (skeleton).** Every subcommand prints `not implemented yet`
and exits 0. Build + dispatch + smoke test work; real bodies land in
phase 4 (local install + DB) and phase 6 (repo client + signature
verify).

See the migration plan at
`PeacockProject/peacock-ports` for the full roadmap (phases 0–8).

## Surface

```
ftr install <pkg>...    install one or more packages
ftr remove <pkg>...     remove an installed package
ftr sync                refresh repository metadata
ftr upgrade [<pkg>...]  upgrade installed packages
ftr search <query>      search repositories for a package
ftr info <pkg>          show package metadata
ftr list                list installed packages
ftr files <pkg>         list files owned by an installed package
ftr flavor              report the active Peacock base flavor
```

Global flags: `--help` / `-h`, `--version` / `-V`.

## Build

Plain `make`. No autotools, no CMake, no external libs (in phase 2).

```sh
make build       # produces ./ftr (statically linked)
make test        # runs tests/smoke_help.sh
make install     # installs to $(DESTDIR)$(PREFIX)/bin/ftr
                 # PREFIX defaults to /peacock
```

Compiled with `-std=c99 -Wall -Wextra -Werror -pedantic`. C99 is the
floor; nothing newer.

## Who calls feather

- **Humans** on a Peacock device: `ftr install peacock-shell`,
  `ftr upgrade`, etc.
- **The Peacock CLI** (`internal/feather/feather.go`) during
  `peacock build`: invokes `ftr install` against the staging chroot
  to overlay the Peacock platform onto the base distro's rootfs.
- **The build farm** (`PeacockProject/build-farm`, phase 6) wraps the
  reverse direction: builds and signs `.feather` packages that ftr
  later installs.

## Layout

```
feather/
├── doc/ftr.1.in       — man page source (filled phase 6)
├── src/main.c         — entrypoint + subcommand dispatch
├── src/cmd_*.c        — one stub per subcommand
├── src/common.h       — version, namespace paths, error helpers
├── src/manifest.[ch]  — manifest parser (phase 4)
├── src/install.[ch]   — overlay logic (phase 4)
├── src/db.[ch]        — local install DB (phase 4)
├── src/repo.[ch]      — repo client (phase 6)
├── src/verify.[ch]    — signature verification (phase 6)
└── tests/smoke_help.sh
```

## License

GPL-3.0 — see `LICENSE`. Matches pacman's licensing posture.
