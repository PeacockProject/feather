/*
 * install.c — overlay install of a local .feather archive.
 *
 * Flow:
 *
 *   1. mkdtemp /tmp/feather-install-XXXXXX
 *   2. shell out to `tar -xzf <archive> -C <tmpdir>`
 *   3. parse <tmpdir>/manifest.toml
 *   4. resolve the install prefix from layout (peacock|system|app|compat)
 *   5. run hooks/pre-install.sh if present
 *   6. walk <tmpdir>/files/ and copy every regular file + symlink
 *      + dir into <prefix>/, recording each path (relative to the
 *      filesystem root) for the DB
 *   7. run hooks/post-install.sh
 *   8. record into the DB (manifest, files list, hooks dir)
 *   9. rm -rf the tempdir
 *
 * Errors short-circuit; no rollback in phase 4 (use `ftr remove` to
 * clean up partial state). Hook contract:
 *
 *   env FEATHER_PREFIX=<prefix>
 *       FEATHER_PKG_NAME=<name>
 *       FEATHER_PKG_VERSION=<version>
 *       sh <tmpdir>/hooks/<name>.sh
 */

#define _POSIX_C_SOURCE 200809L

#include "install.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "db.h"
#include "manifest.h"
#include "util.h"
#include "verify.h"
#include "keyring.h"

/* ---------------------------------------------------------------- *
 * tar shell-out
 * ---------------------------------------------------------------- */

static int run_tar_extract(const char *archive, const char *dest)
{
	pid_t pid = fork();
	int status;
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		/* GNU/busybox tar already strip a leading '/' and drop ".."
		 * components by default, so a crafted member name can't escape
		 * `dest`. The symlink-target traversal that tar does NOT guard is
		 * handled in walk_overlay via symlink_target_escapes(). */
		execlp("tar", "tar", "-xzf", archive, "-C", dest, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		return 0;
	}
	return -1;
}

/* ---------------------------------------------------------------- *
 * hook runner
 * ---------------------------------------------------------------- */

static int run_hook(const char *script_path, const char *prefix,
                    const char *name, const char *version)
{
	pid_t pid;
	int status;
	if (!script_path || !path_exists(script_path)) {
		return 0; /* hook absent => success */
	}
	pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		setenv("FEATHER_PREFIX", prefix ? prefix : "", 1);
		setenv("FEATHER_PKG_NAME", name ? name : "", 1);
		setenv("FEATHER_PKG_VERSION", version ? version : "", 1);
		execlp("sh", "sh", script_path, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		return 0;
	}
	return -1;
}

/* ---------------------------------------------------------------- *
 * walk + overlay
 * ---------------------------------------------------------------- */

typedef struct {
	char **v;
	size_t n;
	size_t cap;
} pathvec;

static int pathvec_push(pathvec *pv, char *s)
{
	if (pv->n == pv->cap) {
		size_t nc = (pv->cap == 0) ? 64 : pv->cap * 2;
		char **tmp = realloc(pv->v, nc * sizeof(*tmp));
		if (!tmp) {
			return -1;
		}
		pv->v = tmp;
		pv->cap = nc;
	}
	pv->v[pv->n++] = s;
	return 0;
}

static void pathvec_free(pathvec *pv)
{
	size_t i;
	for (i = 0; i < pv->n; i++) {
		free(pv->v[i]);
	}
	free(pv->v);
	pv->v = NULL;
	pv->n = pv->cap = 0;
}

/* symlink_target_escapes — true if a RELATIVE symlink target, resolved from the
 * link's own directory (suffix = the link's path within the install root, e.g.
 * "usr/bin/sh"), would climb above the install root via "..". Absolute targets
 * resolve within the eventual root at runtime, so they're allowed; this only
 * rejects "../.." escapes out of the install tree — a traversal vector once
 * untrusted (remote) feathers become installable. */
static int symlink_target_escapes(const char *suffix, const char *target)
{
	long depth = 0;
	const char *seg;

	if (target[0] == '/') {
		return 0; /* absolute: stays within the install root at runtime */
	}
	/* number of '/' in suffix == directory depth the link sits at */
	for (seg = suffix; *seg; seg++) {
		if (*seg == '/') {
			depth++;
		}
	}
	seg = target;
	while (*seg) {
		const char *slash = strchr(seg, '/');
		size_t seglen = slash ? (size_t)(slash - seg) : strlen(seg);
		if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
			if (--depth < 0) {
				return 1;
			}
		} else if (seglen > 0 && !(seglen == 1 && seg[0] == '.')) {
			depth++;
		}
		if (!slash) {
			break;
		}
		seg = slash + 1;
	}
	return 0;
}

/* Recursively walk `files_root/suffix`, replicating into
 * `prefix/suffix`. Every produced installed path (relative to the
 * filesystem root, leading '/' stripped) gets appended to `pv`. */
static int walk_overlay(const char *files_root, const char *suffix,
                        const char *prefix, pathvec *pv)
{
	char *src = path_join(2, files_root, suffix);
	char *dst = path_join(2, prefix, suffix);
	DIR *d;
	struct dirent *de;
	struct stat st;

	if (!src || !dst) {
		err_log("install: out of memory");
		free(src); free(dst);
		return -1;
	}
	if (lstat(src, &st) != 0) {
		err_log("install: cannot stat '%s': %s", src,
		        strerror(errno));
		free(src); free(dst);
		return -1;
	}

	if (S_ISDIR(st.st_mode)) {
		if (mkdir_p(dst, 0755) != 0) {
			err_log("install: cannot create dir '%s': %s",
			        dst, strerror(errno));
			free(src); free(dst);
			return -1;
		}
		d = opendir(src);
		if (!d) {
			err_log("install: cannot open '%s': %s", src,
			        strerror(errno));
			free(src); free(dst);
			return -1;
		}
		while ((de = readdir(d)) != NULL) {
			char *next_suffix;
			int rc;
			if (strcmp(de->d_name, ".") == 0 ||
			    strcmp(de->d_name, "..") == 0) {
				continue;
			}
			if (suffix && *suffix) {
				next_suffix = path_join(2, suffix, de->d_name);
			} else {
				next_suffix = strdup(de->d_name);
			}
			if (!next_suffix) {
				closedir(d);
				free(src); free(dst);
				return -1;
			}
			rc = walk_overlay(files_root, next_suffix,
			                  prefix, pv);
			free(next_suffix);
			if (rc != 0) {
				closedir(d);
				free(src); free(dst);
				return -1;
			}
		}
		closedir(d);
	} else if (S_ISLNK(st.st_mode)) {
		char target[4096];
		ssize_t len = readlink(src, target, sizeof(target) - 1);
		if (len < 0) {
			err_log("install: readlink '%s': %s", src,
			        strerror(errno));
			free(src); free(dst);
			return -1;
		}
		target[len] = '\0';
		if (symlink_target_escapes(suffix, target)) {
			err_log("install: refusing symlink '%s' -> '%s' "
			        "(escapes install root)", dst, target);
			free(src); free(dst);
			return -1;
		}
		(void)unlink(dst);
		if (symlink(target, dst) != 0) {
			err_log("install: symlink '%s': %s", dst,
			        strerror(errno));
			free(src); free(dst);
			return -1;
		}
	} else if (S_ISREG(st.st_mode)) {
		if (copy_file(src, dst) != 0) {
			err_log("install: cannot copy '%s' -> '%s': %s",
			        src, dst, strerror(errno));
			free(src); free(dst);
			return -1;
		}
	} else {
		err_log("install: unsupported file type at '%s'", src);
		free(src); free(dst);
		return -1;
	}

	/* Record the resulting path. Skip the synthetic root dir
	 * (suffix == "") so the DB doesn't "own" the prefix itself. */
	if (suffix && *suffix) {
		const char *p = dst;
		while (*p == '/') {
			p++;
		}
		{
			char *copy = strdup(p);
			if (!copy || pathvec_push(pv, copy) != 0) {
				free(copy);
				err_log("install: out of memory");
				free(src); free(dst);
				return -1;
			}
		}
	}

	free(src); free(dst);
	return 0;
}

/* ---------------------------------------------------------------- *
 * public entry
 * ---------------------------------------------------------------- */

int ftr_install_local(const char *archive_path,
                      const ftr_install_opts *opts)
{
	char tmpl[] = "/tmp/feather-install-XXXXXX";
	char *tmpdir = NULL;
	char *manifest_path = NULL;
	char *files_root = NULL;
	char *hooks_dir = NULL;
	char *pre_hook = NULL;
	char *post_hook = NULL;
	char *resolved_prefix = NULL; /* heap copy for app/compat layouts */
	char *sig_path = NULL;        /* "<archive>.sig" — may be absent */
	ftr_manifest m;
	char err[512];
	const char *prefix;
	pathvec pv;
	int rc = -1;
	int sig_verified = 0;
	char sig_fingerprint[17] = {0};

	memset(&m, 0, sizeof(m));
	memset(&pv, 0, sizeof(pv));

	if (!path_exists(archive_path)) {
		err_log("install: archive not found: '%s'", archive_path);
		return -1;
	}

	/* Local sidecar signature lookup. <archive>.sig next to the
	 * archive is verified if present; if absent and
	 * --allow-unsigned isn't set, we install but WARN. The repo
	 * path handles its own (mandatory) sig fetch + verify before
	 * calling into here, so by the time we get here the archive
	 * has already been authenticated. We detect that by
	 * --allow-unsigned being unset AND the sidecar missing —
	 * that's the only case where we WARN. */
	{
		size_t n = strlen(archive_path) + strlen(".sig") + 1;
		sig_path = malloc(n);
		if (!sig_path) {
			err_log("install: out of memory");
			return -1;
		}
		(void)snprintf(sig_path, n, "%s.sig", archive_path);
	}

	/* Repo path already authenticated this archive; don't re-verify
	 * (or print misleading "no .sig" lines) on the temp file. */
	if (opts && opts->_signature_already_verified) {
		/* fall through to extraction; success line is the
		 * "(verified by ...)" form printed by ftr_install_by_name. */
	} else if (path_exists(sig_path)) {
		if (opts && opts->allow_unsigned) {
			info_log("install: --allow-unsigned set; skipping "
			         "signature verification for '%s'",
			         archive_path);
		} else {
			ftr_pubkey pk;
			ftr_signature sig;
			memset(&pk, 0, sizeof(pk));
			memset(&sig, 0, sizeof(sig));
			if (ftr_verify_load_signature(sig_path, &sig,
			                              err, sizeof(err)) != 0) {
				err_log("install: %s", err);
				free(sig_path);
				return -1;
			}
			if (ftr_keyring_resolve(NULL, &sig, &pk,
			                        err, sizeof(err)) != 0) {
				err_log("install: cannot load pubkey: %s", err);
				free(sig_path);
				return -1;
			}
			if (ftr_verify_archive(archive_path, &sig, &pk,
			                       err, sizeof(err)) != 0) {
				ftr_pubkey_fingerprint(&pk, sig_fingerprint);
				err_log("install: signature verify failed: %s",
				        err);
				err_log("install: expected signer: %s",
				        sig_fingerprint);
				free(sig_path);
				return -1;
			}
			ftr_pubkey_fingerprint(&pk, sig_fingerprint);
			sig_verified = 1;
		}
	}

	if (!mkdtemp(tmpl)) {
		err_log("install: mkdtemp failed: %s", strerror(errno));
		free(sig_path);
		return -1;
	}
	tmpdir = strdup(tmpl);
	if (!tmpdir) {
		err_log("install: out of memory");
		(void)rm_rf(tmpl);
		free(sig_path);
		return -1;
	}

	if (run_tar_extract(archive_path, tmpdir) != 0) {
		err_log("install: tar -xzf '%s' failed", archive_path);
		goto out;
	}

	manifest_path = path_join(2, tmpdir, "manifest.toml");
	files_root    = path_join(2, tmpdir, "files");
	hooks_dir     = path_join(2, tmpdir, "hooks");
	if (!manifest_path || !files_root || !hooks_dir) {
		err_log("install: out of memory");
		goto out;
	}
	if (!path_exists(manifest_path)) {
		err_log("install: archive missing manifest.toml");
		goto out;
	}
	if (ftr_manifest_load(manifest_path, &m, err, sizeof(err)) != 0) {
		err_log("install: %s", err);
		goto out;
	}

	/* Metapackages (layout=meta) carry only depends + hooks — no files/. */
	if (m.layout != FTR_LAYOUT_META && !is_dir(files_root)) {
		err_log("install: archive missing files/ directory");
		goto out;
	}

	/* Resolve the install prefix from manifest layout + CLI overrides.
	 *
	 *   peacock : <peacock-prefix>             (default /peacock)
	 *   app     : <apps-prefix>/<package-name> (default /apps/<name>)
	 *   compat  : <compat-prefix>/<runtime>    (default /compat/<runtime>)
	 *
	 * For app/compat we materialise a heap path; tracked in
	 * resolved_prefix so out: frees it.
	 */
	prefix = NULL;
	switch (m.layout) {
	case FTR_LAYOUT_PEACOCK:
		if (opts && opts->peacock_prefix) {
			prefix = opts->peacock_prefix;
		} else if (m.prefix && *m.prefix) {
			prefix = m.prefix;
		} else {
			prefix = ftr_layout_default_prefix(FTR_LAYOUT_PEACOCK);
		}
		break;
	case FTR_LAYOUT_APP: {
		const char *base = (opts && opts->apps_prefix)
		                   ? opts->apps_prefix
		                   : ftr_layout_default_prefix(FTR_LAYOUT_APP);
		resolved_prefix = path_join(2, base, m.name);
		if (!resolved_prefix) {
			err_log("install: out of memory");
			goto out;
		}
		prefix = resolved_prefix;
		break;
	}
	case FTR_LAYOUT_COMPAT: {
		const char *base;
		if (!m.runtime || !*m.runtime) {
			err_log("install: layout=compat requires "
			        "[package].runtime (package '%s')", m.name);
			goto out;
		}
		base = (opts && opts->compat_prefix)
		       ? opts->compat_prefix
		       : ftr_layout_default_prefix(FTR_LAYOUT_COMPAT);
		resolved_prefix = path_join(2, base, m.runtime);
		if (!resolved_prefix) {
			err_log("install: out of memory");
			goto out;
		}
		prefix = resolved_prefix;
		break;
	}
	case FTR_LAYOUT_SYSTEM:
		/* Overlay files/ onto the system root (default "/"), or a build
		 * chroot via opts->root: files/usr/... -> <root>/usr/...,
		 * files/boot/zImage -> <root>/boot/zImage. */
		if (opts && opts->root && *opts->root) {
			prefix = opts->root;
		} else if (m.prefix && *m.prefix) {
			prefix = m.prefix;
		} else {
			prefix = ftr_layout_default_prefix(FTR_LAYOUT_SYSTEM);
		}
		break;
	case FTR_LAYOUT_META:
		/* No files to place; prefix only used for the hook env. */
		prefix = (opts && opts->root && *opts->root) ? opts->root : "/";
		break;
	default:
		err_log("install: layout '%s' not yet supported "
		        "(package '%s')",
		        ftr_layout_name(m.layout), m.name);
		goto out;
	}

	if (m.layout != FTR_LAYOUT_META && mkdir_p(prefix, 0755) != 0) {
		err_log("install: cannot create prefix '%s': %s",
		        prefix, strerror(errno));
		goto out;
	}

	/* pre-install hook (in the unpacked tempdir). */
	pre_hook = path_join(2, hooks_dir, "pre-install.sh");
	if (pre_hook && path_exists(pre_hook)) {
		if (run_hook(pre_hook, prefix, m.name, m.version) != 0) {
			err_log("install: pre-install hook failed");
			goto out;
		}
	}

	if (m.layout != FTR_LAYOUT_META) {
		if (walk_overlay(files_root, "", prefix, &pv) != 0) {
			goto out;
		}
		if (pv.n > 1) {
			qsort(pv.v, pv.n, sizeof(*pv.v), strcmp_qsort);
		}
	}

	/* post-install hook. */
	post_hook = path_join(2, hooks_dir, "post-install.sh");
	if (post_hook && path_exists(post_hook)) {
		if (run_hook(post_hook, prefix, m.name, m.version) != 0) {
			err_log("install: post-install hook failed");
			goto out;
		}
	}

	if (ftr_db_record(m.name, m.version, manifest_path,
	                  is_dir(hooks_dir) ? hooks_dir : NULL,
	                  pv.v, pv.n) != 0) {
		goto out;
	}

	if (opts && opts->_signature_already_verified) {
		/* Repo path prints the canonical "installed: ... (verified
		 * by ...)" line; stay quiet here so output isn't doubled. */
	} else if (sig_verified) {
		printf("installed: %s-%s -> %s "
		       "(verified by %s)\n",
		       m.name, m.version, prefix, sig_fingerprint);
	} else if (opts && opts->allow_unsigned) {
		printf("installed: %s-%s -> %s\n",
		       m.name, m.version, prefix);
		fprintf(stderr,
		        "WARN: %s-%s installed without signature "
		        "verification (--allow-unsigned)\n",
		        m.name, m.version);
	} else {
		printf("installed: %s-%s -> %s\n",
		       m.name, m.version, prefix);
		fprintf(stderr,
		        "WARN: %s-%s installed without signature "
		        "(local archive, no .sig file)\n",
		        m.name, m.version);
	}
	rc = 0;

out:
	pathvec_free(&pv);
	ftr_manifest_free(&m);
	free(manifest_path);
	free(files_root);
	free(hooks_dir);
	free(pre_hook);
	free(post_hook);
	free(resolved_prefix);
	free(sig_path);
	if (tmpdir) {
		(void)rm_rf(tmpdir);
		free(tmpdir);
	}
	return rc;
}
