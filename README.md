# feather

`ftr` — the Peacock OS package manager.

Peacock is a meta-distro: a base distro (Arch, Debian, Alpine, ...) owns
`/usr`, `/etc`, `/var`, `/home`; feather owns the Peacock platform layer at
`/peacock`, user apps at `/apps`, runtime shims at `/compat`, and per-app
state at `/data`. The base package manager (pacman / apt / apk) manages the
base system; `ftr` manages everything Peacock-specific on top.

This repo is the C source for the `ftr` binary. It ships as
`/peacock/bin/ftr` and is statically linked so it can run inside any
flavor's chroot without dragging in a libc dependency.

## How it fits in

```
   peacock-ports/           manifest source
       │
       ▼
   Peacock CLI              builds .feather archives (in chroot)
       │
       ▼
   build farm (Phase 6)     signs archives with farm key
       │
       ▼
   repo.peacock-project.dev published archives + signed index.toml
       │
       ▼
   ftr (this binary)        on-device install / upgrade / remove
       │
       ├─ /peacock/...      Peacock platform layer
       ├─ /apps/<name>/     per-app prefixes
       ├─ /compat/<rt>/     alternate-runtime rootfs slices
       └─ /data/<pkg>/      per-app persistent state
```

Sibling repos under `PeacockProject/`:

- **[Peacock](../Peacock/)** — Go build front-end. Invokes `ftr` against the
  staging chroot during `peacock build` to overlay the Peacock platform
  layer on top of the base distro.
- **[peacock-ports](../peacock-ports/)** — manifest source. The same
  `package.toml` files Peacock builds become `.feather` archives that `ftr`
  installs.
- **build-farm** (Phase 6, not yet created) — CI infrastructure that builds
  + signs `.feather` archives per `(port, flavor, arch)` triple and serves
  them via channeled repos (`stable` / `testing` / `unstable`).

## Status

The repo is well past Phase 2's "skeleton" milestone. Subcommand wiring is
real, the manifest parser works, the local install + uninstall path lands
files into the configured namespace, and the Phase 6 signed-repo client
(HTTPS sync, minisign signature verify on the index and per-archive) is in
place. The end-to-end "build farm publishes signed archives, device runs
`ftr upgrade`" loop still depends on the build-farm repo + production key
ceremony, neither of which exist yet.

Track open work in `Peacock/BACKLOG.md` (root-level cross-repo backlog).

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

Global flags: `--help` / `-h`, `--version` / `-V`. Install-time flags
include `--allow-unsigned` (only valid for `ftr install ./local.feather`;
rejected when the source is a configured repo).

## Quick start

Host prerequisites:

- A C99 compiler — `gcc` or `clang`.
- `make`. No autotools, no CMake.
- `curl` available at runtime (called via `exec` for HTTPS fetches).
- For development: `clangd` or similar for `compile_flags.txt`-driven LSP.

```sh
git clone https://github.com/PeacockProject/feather.git
cd feather
make build       # produces ./ftr (statically linked, no libc dependency
                 # beyond the host's, which is fine for build-time but the
                 # release artifact is built static against musl)
make test        # runs tests/smoke_help.sh + Phase 6 repo round-trip tests
./ftr --help

# Build a tiny local archive and install it into a fake root:
./tools/ftr-sign --help              # signing helper
./ftr install ./my-package.feather   # local install path
```

Compiled with `-std=c99 -Wall -Wextra -Werror -pedantic`. C99 is the floor;
nothing newer.

A `peacock doctor`-style sweep for missing C-side prereqs would be nice but
isn't written yet.

## Project layout

```
feather/
├── doc/ftr.1.in        man page source
├── src/
│   ├── main.c            entrypoint + subcommand dispatch
│   ├── common.h          version, namespace paths, error helpers
│   ├── cmd_install.c     ftr install
│   ├── cmd_remove.c      ftr remove
│   ├── cmd_sync.c        ftr sync (Phase 6 repo client)
│   ├── cmd_upgrade.c     ftr upgrade
│   ├── cmd_search.c      ftr search
│   ├── cmd_info.c        ftr info
│   ├── cmd_list.c        ftr list
│   ├── cmd_files.c       ftr files
│   ├── cmd_flavor.c      ftr flavor
│   ├── manifest.[ch]     manifest parser
│   ├── install.[ch]      overlay logic (extract → place → record)
│   ├── db.[ch]           local install DB at /var/lib/feather/local/
│   ├── repo.[ch]         repo client (curl + parse index.toml)
│   ├── verify.[ch]       minisign signature verification
│   ├── util.[ch]         I/O + path helpers
│   ├── randombytes.c     RNG used by signing tools
│   └── vendor/           TweetNaCl Ed25519 + SHA-512 (public domain)
├── tools/
│   ├── gen-keypair.c     minisign-style Ed25519 keypair generator
│   └── ftr-sign.c        sign an archive or index file
├── tests/                shell-driven smoke + round-trip tests
├── compile_flags.txt     clangd config
├── Makefile
├── LICENSE               GPL-3.0
└── README.md
```

## Who calls feather

- **Humans** on a Peacock device: `ftr install peacock-shell`,
  `ftr upgrade`, etc.
- **The Peacock CLI** (`internal/feather/feather.go`) during `peacock build`:
  invokes `ftr install` against the staging chroot to overlay the Peacock
  platform onto the base distro's rootfs.
- **The build farm** (Phase 6) wraps the reverse direction: builds and signs
  `.feather` packages that ftr later installs.

## Where to go next

- **Cross-repo backlog** — `Peacock/BACKLOG.md`.
- **Meta-distro architecture overview** — `peacock-ports/SCHEMA.md` for the
  install-layout model; `peacock-ports/README.md` for the manifest source
  tree.
- **Peacock CLI side** — [`Peacock/README.md`](../Peacock/README.md).

## License

GPL-3.0 — see `LICENSE`. Matches pacman's licensing posture.

## Contributing

TODO. No `CONTRIBUTING.md` yet. Until one lands:

- Commits use `<area>: <subject>` (`install:`, `repo:`, `verify:`, `cleanup:`,
  `tooling:`, `tests:`, `docs:`).
- `make test` must pass.
- `clang-format -i` over touched files. Strict C99; no GCC extensions.
- Don't push to `main` directly while pre-release.
