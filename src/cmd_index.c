/*
 * cmd_index.c — `ftr index [--arch <arch>] [--name <repo>] [-o <out>] <dir>`
 *
 * Scan <dir> for *.feather archives, read each one's embedded
 * manifest.toml, and emit a repo index.toml listing every package with
 * its layout / depends / provides / conflicts / sha256 / size (+ the
 * given arch). This is what the build (and genmirror) run to turn a
 * directory of built packages into something `ftr sync` + the resolver
 * can consume. The index still needs signing separately (minisign
 * index.toml.sig or a GPG index.toml.asc).
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "manifest.h"
#include "util.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_help(void)
{
	printf("Usage: ftr index [options] <dir>\n");
	printf("\n");
	printf("Scan <dir>/*.feather and write a repo index.toml.\n");
	printf("\n");
	printf("Options:\n");
	printf("  --arch <arch>   tag every entry with this arch\n");
	printf("  --name <repo>   [repo].name value (default: peacock)\n");
	printf("  -o <path>       output file (default: <dir>/index.toml)\n");
	printf("  -h, --help      show this help\n");
}

static int ends_with(const char *s, const char *suf)
{
	size_t ls = strlen(s);
	size_t lsuf = strlen(suf);
	return ls >= lsuf && strcmp(s + ls - lsuf, suf) == 0;
}

/* Extract just manifest.toml from <archive> into <tmpdir>. 0/-1. */
static int extract_manifest(const char *archive, const char *tmpdir)
{
	pid_t pid = fork();
	int st;
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		int dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			(void)dup2(dn, 2);
			if (dn > 2) {
				(void)close(dn);
			}
		}
		execlp("tar", "tar", "-xzf", archive, "-C", tmpdir,
		       "manifest.toml", (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void emit_str_array(FILE *out, const char *key,
                           char **names, size_t n)
{
	size_t i;
	if (n == 0) {
		return;
	}
	fprintf(out, "%s = [", key);
	for (i = 0; i < n; i++) {
		fprintf(out, "%s\"%s\"", i ? ", " : "", names[i]);
	}
	fprintf(out, "]\n");
}

/* Pull just the capability names out of an ftr_capability array. */
static void emit_capabilities(FILE *out, const char *key,
                              const ftr_capability *caps, size_t n)
{
	size_t i;
	if (n == 0) {
		return;
	}
	fprintf(out, "%s = [", key);
	for (i = 0; i < n; i++) {
		fprintf(out, "%s\"%s\"", i ? ", " : "", caps[i].name);
	}
	fprintf(out, "]\n");
}

int cmd_index(int argc, char **argv)
{
	const char *arch = NULL;
	const char *repo_name = "peacock";
	const char *out_path = NULL;
	const char *dir = NULL;
	char *out_owned = NULL;
	char **files = NULL;
	size_t n_files = 0, cap_files = 0;
	FILE *out = NULL;
	DIR *d = NULL;
	struct dirent *de;
	char tmpl[] = "/tmp/feather-index-XXXXXX";
	int have_tmp = 0;
	size_t i;
	int rc = 1;

	for (i = 1; i < (size_t)argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			print_help();
			return 0;
		} else if (strcmp(a, "--arch") == 0) {
			if (++i >= (size_t)argc) { err_log("index: --arch needs an argument"); return 2; }
			arch = argv[i];
		} else if (strcmp(a, "--name") == 0) {
			if (++i >= (size_t)argc) { err_log("index: --name needs an argument"); return 2; }
			repo_name = argv[i];
		} else if (strcmp(a, "-o") == 0) {
			if (++i >= (size_t)argc) { err_log("index: -o needs an argument"); return 2; }
			out_path = argv[i];
		} else if (a[0] == '-' && a[1] != '\0') {
			err_log("index: unknown option '%s'", a);
			return 2;
		} else {
			dir = a;
		}
	}
	if (!dir) {
		print_help();
		return 1;
	}

	if (!out_path) {
		out_owned = path_join(2, dir, "index.toml");
		if (!out_owned) { err_log("index: out of memory"); return 1; }
		out_path = out_owned;
	}

	/* Collect + sort the .feather filenames for deterministic output. */
	d = opendir(dir);
	if (!d) {
		err_log("index: cannot open '%s': %s", dir, strerror(errno));
		goto out;
	}
	while ((de = readdir(d)) != NULL) {
		if (!ends_with(de->d_name, ".feather")) {
			continue;
		}
		if (n_files == cap_files) {
			size_t nc = cap_files ? cap_files * 2 : 16;
			char **nf = realloc(files, nc * sizeof(*files));
			if (!nf) { err_log("index: out of memory"); goto out; }
			files = nf;
			cap_files = nc;
		}
		files[n_files] = strdup(de->d_name);
		if (!files[n_files]) { err_log("index: out of memory"); goto out; }
		n_files++;
	}
	closedir(d);
	d = NULL;
	if (n_files > 1) {
		qsort(files, n_files, sizeof(*files), strcmp_qsort);
	}

	if (!mkdtemp(tmpl)) {
		err_log("index: mkdtemp failed: %s", strerror(errno));
		goto out;
	}
	have_tmp = 1;

	out = fopen(out_path, "w");
	if (!out) {
		err_log("index: cannot write '%s': %s", out_path, strerror(errno));
		goto out;
	}
	fprintf(out, "[repo]\n");
	fprintf(out, "name = \"%s\"\n\n", repo_name);

	for (i = 0; i < n_files; i++) {
		char *archive = path_join(2, dir, files[i]);
		char *mpath = path_join(2, tmpl, "manifest.toml");
		ftr_manifest m;
		char err[256];
		char hex[65];
		struct stat st;
		memset(&m, 0, sizeof(m));
		if (!archive || !mpath) { free(archive); free(mpath); err_log("index: out of memory"); goto out; }
		(void)unlink(mpath);
		if (extract_manifest(archive, tmpl) != 0) {
			err_log("index: %s: cannot read manifest.toml", files[i]);
			free(archive); free(mpath); goto out;
		}
		if (ftr_manifest_load(mpath, &m, err, sizeof(err)) != 0) {
			err_log("index: %s: %s", files[i], err);
			free(archive); free(mpath); goto out;
		}
		if (sha256_file_hex(archive, hex) != 0) {
			err_log("index: %s: cannot hash", files[i]);
			ftr_manifest_free(&m); free(archive); free(mpath); goto out;
		}
		st.st_size = 0;
		(void)stat(archive, &st);

		fprintf(out, "[[package]]\n");
		fprintf(out, "name = \"%s\"\n", m.name);
		fprintf(out, "version = \"%s\"\n", m.version);
		if (m.description) fprintf(out, "description = \"%s\"\n", m.description);
		if (m.runtime) fprintf(out, "runtime = \"%s\"\n", m.runtime);
		fprintf(out, "layout = \"%s\"\n", ftr_layout_name(m.layout));
		if (arch) fprintf(out, "arch = \"%s\"\n", arch);
		fprintf(out, "archive = \"%s\"\n", files[i]);
		fprintf(out, "sha256 = \"%s\"\n", hex);
		fprintf(out, "size = %lld\n", (long long)st.st_size);
		emit_str_array(out, "depends", m.depends, m.n_depends);
		emit_capabilities(out, "provides", m.provides, m.n_provides);
		emit_capabilities(out, "conflicts", m.conflicts, m.n_conflicts);
		fprintf(out, "\n");

		ftr_manifest_free(&m);
		free(archive);
		free(mpath);
	}

	if (fclose(out) != 0) {
		out = NULL;
		err_log("index: write error on '%s'", out_path);
		goto out;
	}
	out = NULL;
	printf("index: wrote %s (%zu package(s))\n", out_path, n_files);
	rc = 0;

out:
	if (out) fclose(out);
	if (d) closedir(d);
	if (have_tmp) (void)rm_rf(tmpl);
	for (i = 0; i < n_files; i++) free(files[i]);
	free(files);
	free(out_owned);
	return rc;
}
