/*
 * install.h — overlay install of a local .feather archive.
 *
 * A .feather archive is a gzip-compressed tarball with the layout:
 *
 *   <root>/
 *   ├── manifest.toml         mandatory
 *   ├── files/                mandatory — tree to overlay onto the prefix
 *   └── hooks/                optional — pre/post-install + pre/post-remove
 *
 * Supported layouts: `peacock` (-> /peacock) and `system` (-> /, or a
 * build chroot via opts->root, used by the Peacock build to install a
 * package's files into a chroot). `app` / `compat` parse but
 * ftr_install_local() still rejects them as "not yet supported".
 *
 * Rollback on partial-install failure is *not* in scope for phase 4
 * (documented in `ftr install --help`); leftover state must be
 * cleaned up by `ftr remove` later.
 */

#ifndef FTR_INSTALL_H
#define FTR_INSTALL_H

typedef struct {
	/* Override the install prefix for each layout. NULL = use the
	 * layout's default (e.g. "/peacock" for peacock). Tests set
	 * these to mktemp'd sandbox dirs. */
	const char *peacock_prefix;
	const char *apps_prefix;
	const char *compat_prefix;
	const char *data_prefix;

	/* Root for `layout = "system"` installs — the files/ tree is
	 * overlaid onto <root>/ (so files/boot/zImage -> <root>/boot/
	 * zImage, files/usr/... -> <root>/usr/...). NULL = "/". The build
	 * sets this to a build chroot so a package (kernel, toolchain) is
	 * installed into the chroot rather than the host; pair it with
	 * FTR_DB_ROOT=<root>/var/lib/feather so the DB follows. */
	const char *root;

	/* Local installs only: when 1, skip signature verification
	 * even if a sidecar .sig is present. Ignored by the repo path
	 * (repo installs always require a verified signature). */
	int allow_unsigned;

	/* Internal: set by ftr_install_by_name() once the repo path
	 * has already verified the sig+pubkey, so ftr_install_local()
	 * doesn't print a misleading "no .sig" WARN about the temp
	 * file it just extracted. Tests and the public CLI never set
	 * this. */
	int _signature_already_verified;
} ftr_install_opts;

/* Install the .feather archive at `archive_path` according to its
 * embedded manifest. Returns 0 on success, non-zero on failure (a
 * diagnostic has already been printed to stderr).
 */
int ftr_install_local(const char *archive_path,
                      const ftr_install_opts *opts);

/* Install a package by name, resolving it against the synced repo
 * indexes (highest version wins; first-listed repo wins on tie).
 * Fetches + sha256-verifies + installs through ftr_install_local.
 * Returns 0 on success, -1 on failure. */
int ftr_install_by_name(const char *name, const ftr_install_opts *opts);

#endif /* FTR_INSTALL_H */
