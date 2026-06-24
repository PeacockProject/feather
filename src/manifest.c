/*
 * manifest.c — feather package manifest parser.
 *
 * Uses the vendored tomlc99 reader at src/vendor/. Surface:
 *
 *   ftr_manifest_load(path, &out, errbuf, errbufsz)
 *   ftr_manifest_free(&m)
 *
 * Validation rules (phase 4):
 *   - [package].name           required, non-empty string
 *   - [package].version        required, non-empty string
 *   - [install].layout         required, one of peacock|app|compat|system
 *   - everything else optional
 *
 * Strings retrieved via tomlc99's `_in` accessors are heap-allocated
 * by that library; we move them into our own struct and free them on
 * ftr_manifest_free(). The toml_table_t tree itself is freed at the
 * end of ftr_manifest_load() — none of its internal pointers escape.
 */

/* POSIX.1-2008 for strdup() under -std=c99 -pedantic. */
#define _POSIX_C_SOURCE 200809L

#include "manifest.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toml.h"

/* ---------------------------------------------------------------- *
 * small helpers
 * ---------------------------------------------------------------- */

static void set_err(char *errbuf, size_t errbufsz, const char *fmt, ...)
{
	va_list ap;
	if (!errbuf || errbufsz == 0) {
		return;
	}
	va_start(ap, fmt);
	(void)vsnprintf(errbuf, errbufsz, fmt, ap);
	va_end(ap);
	errbuf[errbufsz - 1] = '\0';
}

/* Pull a required string out of a table. Sets *out to a heap copy
 * owned by the caller. Returns 0 on success, -1 if missing or empty
 * (and writes a message into errbuf). */
static int take_required_string(toml_table_t *tab, const char *key,
                                const char *tablename,
                                char **out,
                                char *errbuf, size_t errbufsz)
{
	toml_datum_t d = toml_string_in(tab, key);
	if (!d.ok) {
		set_err(errbuf, errbufsz,
		        "missing required key [%s].%s", tablename, key);
		return -1;
	}
	if (d.u.s[0] == '\0') {
		free(d.u.s);
		set_err(errbuf, errbufsz,
		        "[%s].%s must be non-empty", tablename, key);
		return -1;
	}
	*out = d.u.s; /* ownership transfers to *out */
	return 0;
}

/* Optional string. *out stays NULL if the key is absent. */
static void take_optional_string(toml_table_t *tab, const char *key,
                                 char **out)
{
	toml_datum_t d = toml_string_in(tab, key);
	if (d.ok) {
		*out = d.u.s;
	}
}

/* Pull an optional array-of-string into *out / *n_out. Returns 0 on
 * success (including "key absent"); -1 on malformed array. */
static int take_optional_string_array(toml_table_t *tab, const char *key,
                                      char ***out, size_t *n_out,
                                      char *errbuf, size_t errbufsz)
{
	toml_array_t *arr;
	int n;
	int i;
	char **buf;

	arr = toml_array_in(tab, key);
	if (!arr) {
		return 0;
	}
	n = toml_array_nelem(arr);
	if (n <= 0) {
		return 0;
	}
	buf = calloc((size_t)n, sizeof(*buf));
	if (!buf) {
		set_err(errbuf, errbufsz, "out of memory");
		return -1;
	}
	for (i = 0; i < n; i++) {
		toml_datum_t d = toml_string_at(arr, i);
		if (!d.ok) {
			int j;
			for (j = 0; j < i; j++) {
				free(buf[j]);
			}
			free(buf);
			set_err(errbuf, errbufsz,
			        "'%s' must be an array of strings", key);
			return -1;
		}
		buf[i] = d.u.s;
	}
	*out = buf;
	*n_out = (size_t)n;
	return 0;
}

/* Walk a [provides] / [conflicts] table: every key=string pair
 * becomes one ftr_capability. Accepts bare versions like "0.1" or
 * the wildcard "*". Returns 0 on success (incl. absent table), -1 on
 * malformed value. */
static int take_capability_table(toml_table_t *root, const char *tablename,
                                 ftr_capability **out, size_t *n_out,
                                 char *errbuf, size_t errbufsz)
{
	toml_table_t *tab;
	int nkv;
	int i;
	ftr_capability *buf;

	tab = toml_table_in(root, tablename);
	if (!tab) {
		return 0;
	}
	nkv = toml_table_nkval(tab);
	if (nkv <= 0) {
		return 0;
	}
	buf = calloc((size_t)nkv, sizeof(*buf));
	if (!buf) {
		set_err(errbuf, errbufsz, "out of memory");
		return -1;
	}
	for (i = 0; i < nkv; i++) {
		const char *key = toml_key_in(tab, i);
		toml_datum_t d;
		if (!key) {
			break;
		}
		d = toml_string_in(tab, key);
		if (!d.ok) {
			int j;
			for (j = 0; j < i; j++) {
				free(buf[j].name);
				free(buf[j].version);
			}
			free(buf);
			set_err(errbuf, errbufsz,
			        "[%s].%s must be a string version (or \"*\")",
			        tablename, key);
			return -1;
		}
		buf[i].name = strdup(key);
		if (!buf[i].name) {
			int j;
			free(d.u.s);
			for (j = 0; j < i; j++) {
				free(buf[j].name);
				free(buf[j].version);
			}
			free(buf);
			set_err(errbuf, errbufsz, "out of memory");
			return -1;
		}
		buf[i].version = d.u.s;
	}
	*out = buf;
	*n_out = (size_t)nkv;
	return 0;
}

static ftr_layout_t parse_layout(const char *s)
{
	if (strcmp(s, "peacock") == 0) {
		return FTR_LAYOUT_PEACOCK;
	}
	if (strcmp(s, "app") == 0) {
		return FTR_LAYOUT_APP;
	}
	if (strcmp(s, "compat") == 0) {
		return FTR_LAYOUT_COMPAT;
	}
	if (strcmp(s, "system") == 0) {
		return FTR_LAYOUT_SYSTEM;
	}
	if (strcmp(s, "meta") == 0) {
		return FTR_LAYOUT_META;
	}
	return FTR_LAYOUT_UNKNOWN;
}

/* ---------------------------------------------------------------- *
 * public API
 * ---------------------------------------------------------------- */

const char *ftr_layout_name(ftr_layout_t l)
{
	switch (l) {
	case FTR_LAYOUT_PEACOCK: return "peacock";
	case FTR_LAYOUT_APP:     return "app";
	case FTR_LAYOUT_COMPAT:  return "compat";
	case FTR_LAYOUT_SYSTEM:  return "system";
	case FTR_LAYOUT_META:    return "meta";
	case FTR_LAYOUT_UNKNOWN:
	default:                 return "unknown";
	}
}

const char *ftr_layout_default_prefix(ftr_layout_t l)
{
	switch (l) {
	case FTR_LAYOUT_PEACOCK: return "/peacock";
	case FTR_LAYOUT_APP:     return "/apps";
	case FTR_LAYOUT_COMPAT:  return "/compat";
	case FTR_LAYOUT_SYSTEM:  return "/";
	case FTR_LAYOUT_META:    return NULL;  /* no files to overlay */
	case FTR_LAYOUT_UNKNOWN:
	default:                 return NULL;
	}
}

int ftr_manifest_load(const char *path, ftr_manifest *out,
                      char *errbuf, size_t errbufsz)
{
	FILE *fp;
	toml_table_t *root = NULL;
	toml_table_t *pkg;
	toml_table_t *inst;
	char toml_err[256];
	char *layout_str = NULL;

	memset(out, 0, sizeof(*out));

	fp = fopen(path, "r");
	if (!fp) {
		set_err(errbuf, errbufsz, "cannot open manifest '%s'", path);
		return -1;
	}
	root = toml_parse_file(fp, toml_err, sizeof(toml_err));
	fclose(fp);
	if (!root) {
		set_err(errbuf, errbufsz, "manifest parse error: %s", toml_err);
		return -1;
	}

	/* [package] */
	pkg = toml_table_in(root, "package");
	if (!pkg) {
		set_err(errbuf, errbufsz, "missing required table [package]");
		goto fail;
	}
	if (take_required_string(pkg, "name",    "package",
	                         &out->name,    errbuf, errbufsz) < 0) goto fail;
	if (take_required_string(pkg, "version", "package",
	                         &out->version, errbuf, errbufsz) < 0) goto fail;
	take_optional_string(pkg, "description", &out->description);
	take_optional_string(pkg, "runtime",     &out->runtime);
	if (take_optional_string_array(pkg, "flavor",
	                               &out->flavors, &out->n_flavors,
	                               errbuf, errbufsz) < 0) goto fail;
	if (take_optional_string_array(pkg, "depends",
	                               &out->depends, &out->n_depends,
	                               errbuf, errbufsz) < 0) goto fail;

	/* [install] */
	inst = toml_table_in(root, "install");
	if (!inst) {
		set_err(errbuf, errbufsz, "missing required table [install]");
		goto fail;
	}
	if (take_required_string(inst, "layout", "install",
	                         &layout_str, errbuf, errbufsz) < 0) goto fail;
	out->layout = parse_layout(layout_str);
	if (out->layout == FTR_LAYOUT_UNKNOWN) {
		set_err(errbuf, errbufsz,
		        "[install].layout = \"%s\" — must be one of "
		        "peacock|app|compat|system|meta", layout_str);
		free(layout_str);
		goto fail;
	}
	free(layout_str);
	take_optional_string(inst, "prefix", &out->prefix);

	/* [provides] / [conflicts] */
	if (take_capability_table(root, "provides",
	                          &out->provides, &out->n_provides,
	                          errbuf, errbufsz) < 0) goto fail;
	if (take_capability_table(root, "conflicts",
	                          &out->conflicts, &out->n_conflicts,
	                          errbuf, errbufsz) < 0) goto fail;

	toml_free(root);
	return 0;

fail:
	toml_free(root);
	ftr_manifest_free(out);
	return -1;
}

void ftr_manifest_free(ftr_manifest *m)
{
	size_t i;

	if (!m) {
		return;
	}

	free(m->name);
	free(m->version);
	free(m->description);
	free(m->runtime);
	free(m->prefix);

	for (i = 0; i < m->n_flavors; i++) {
		free(m->flavors[i]);
	}
	free(m->flavors);

	for (i = 0; i < m->n_depends; i++) {
		free(m->depends[i]);
	}
	free(m->depends);

	for (i = 0; i < m->n_provides; i++) {
		free(m->provides[i].name);
		free(m->provides[i].version);
	}
	free(m->provides);

	for (i = 0; i < m->n_conflicts; i++) {
		free(m->conflicts[i].name);
		free(m->conflicts[i].version);
	}
	free(m->conflicts);

	memset(m, 0, sizeof(*m));
}
