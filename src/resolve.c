/*
 * resolve.c — dependency resolver (see resolve.h).
 *
 * Plain recursive DFS with post-order emission so dependencies land
 * before dependents. Cycles are tolerated (a node already on the
 * visiting stack is treated as satisfied) rather than erroring, since a
 * dependency cycle still installs fine in any order once each archive is
 * fetched. Version constraints in depends entries (e.g. "foo>=1.2") are
 * accepted syntactically but only the bare name is matched for now.
 */

#define _POSIX_C_SOURCE 200809L

#include "resolve.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "db.h"
#include "util.h"

typedef struct {
	const ftr_repo_index *idxs;
	size_t n_idxs;
	const char *arch;
	const ftr_pkg_index_entry **items;
	size_t n_items, cap_items;
	char **done;       /* names + capabilities already satisfied */
	size_t n_done, cap_done;
	char **visiting;   /* DFS stack, for cycle detection */
	size_t n_visiting, cap_visiting;
	char *err;
	size_t errsz;
} resolve_ctx;

static void set_err(resolve_ctx *c, const char *fmt, ...)
{
	va_list ap;
	if (!c->err || c->errsz == 0) {
		return;
	}
	va_start(ap, fmt);
	(void)vsnprintf(c->err, c->errsz, fmt, ap);
	va_end(ap);
	c->err[c->errsz - 1] = '\0';
}

static int strv_contains(char **a, size_t n, const char *s)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (strcmp(a[i], s) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Append a strdup'd copy to a growable string array. Returns 0/-1. */
static int strv_push(char ***a, size_t *n, size_t *cap, const char *s)
{
	if (*n == *cap) {
		size_t nc = *cap ? *cap * 2 : 8;
		char **na = realloc(*a, nc * sizeof(**a));
		if (!na) {
			return -1;
		}
		*a = na;
		*cap = nc;
	}
	(*a)[*n] = strdup(s);
	if (!(*a)[*n]) {
		return -1;
	}
	(*n)++;
	return 0;
}

static void strv_pop(char **a, size_t *n)
{
	if (*n > 0) {
		free(a[--(*n)]);
	}
}

/* The bare dependency name, ignoring any ">=ver" / "=ver" suffix. The
 * returned copy is heap-owned; caller frees. */
static char *dep_name(const char *spec)
{
	size_t i = 0;
	char *out;
	while (spec[i] && spec[i] != ' ' && spec[i] != '>' && spec[i] != '<' &&
	       spec[i] != '=' && spec[i] != '!') {
		i++;
	}
	out = malloc(i + 1);
	if (!out) {
		return NULL;
	}
	memcpy(out, spec, i);
	out[i] = '\0';
	return out;
}

static int arch_ok(const ftr_pkg_index_entry *e, const char *arch)
{
	if (!arch) {
		return 1;
	}
	if (!e->arch || e->arch[0] == '\0' || strcmp(e->arch, "any") == 0) {
		return 1;
	}
	return strcmp(e->arch, arch) == 0;
}

/* Highest-version entry named `name` (arch-compatible), or NULL. */
static const ftr_pkg_index_entry *best_by_name(resolve_ctx *c, const char *name)
{
	const ftr_pkg_index_entry *best = NULL;
	size_t i, j;
	for (i = 0; i < c->n_idxs; i++) {
		for (j = 0; j < c->idxs[i].n_entries; j++) {
			const ftr_pkg_index_entry *e = &c->idxs[i].entries[j];
			if (strcmp(e->name, name) != 0 || !arch_ok(e, c->arch)) {
				continue;
			}
			if (!best || version_compare(e->version, best->version) > 0) {
				best = e;
			}
		}
	}
	return best;
}

/* Highest-version entry that [provides] `cap` (arch-compatible), or NULL. */
static const ftr_pkg_index_entry *best_by_provides(resolve_ctx *c, const char *cap)
{
	const ftr_pkg_index_entry *best = NULL;
	size_t i, j, k;
	for (i = 0; i < c->n_idxs; i++) {
		for (j = 0; j < c->idxs[i].n_entries; j++) {
			const ftr_pkg_index_entry *e = &c->idxs[i].entries[j];
			if (!arch_ok(e, c->arch)) {
				continue;
			}
			for (k = 0; k < e->n_provides; k++) {
				if (strcmp(e->provides[k], cap) != 0) {
					continue;
				}
				if (!best ||
				    version_compare(e->version, best->version) > 0) {
					best = e;
				}
				break;
			}
		}
	}
	return best;
}

static int append_item(resolve_ctx *c, const ftr_pkg_index_entry *e)
{
	if (c->n_items == c->cap_items) {
		size_t nc = c->cap_items ? c->cap_items * 2 : 8;
		const ftr_pkg_index_entry **ni =
			realloc(c->items, nc * sizeof(*c->items));
		if (!ni) {
			return -1;
		}
		c->items = ni;
		c->cap_items = nc;
	}
	c->items[c->n_items++] = e;
	return 0;
}

static int resolve_one(resolve_ctx *c, const char *spec, int is_target)
{
	char *name = dep_name(spec);
	const ftr_pkg_index_entry *e;
	char *iv;
	size_t k;
	int rc = -1;

	if (!name) {
		set_err(c, "out of memory");
		return -1;
	}
	if (!*name) {
		free(name);
		return 0;
	}
	if (strv_contains(c->done, c->n_done, name)) {
		free(name);
		return 0;
	}
	if (strv_contains(c->visiting, c->n_visiting, name)) {
		/* cycle — treat as satisfied to break the recursion */
		free(name);
		return 0;
	}

	/* An installed *dependency* is satisfied and skipped. An explicitly
	 * requested target is always (re)processed, even if installed. */
	if (!is_target) {
		iv = ftr_db_find_version(name);
		if (iv) {
			free(iv);
			(void)strv_push(&c->done, &c->n_done, &c->cap_done, name);
			free(name);
			return 0;
		}
	}

	e = best_by_name(c, name);
	if (!e) {
		e = best_by_provides(c, name);
	}
	if (!e) {
		set_err(c, "cannot resolve dependency '%s'", name);
		free(name);
		return -1;
	}

	if (strv_push(&c->visiting, &c->n_visiting, &c->cap_visiting, name) != 0) {
		set_err(c, "out of memory");
		free(name);
		return -1;
	}

	for (k = 0; k < e->n_depends; k++) {
		if (resolve_one(c, e->depends[k], 0) != 0) {
			goto done;
		}
	}

	/* mark both the requested name and the real package name satisfied */
	(void)strv_push(&c->done, &c->n_done, &c->cap_done, name);
	if (strcmp(name, e->name) != 0 &&
	    !strv_contains(c->done, c->n_done, e->name)) {
		(void)strv_push(&c->done, &c->n_done, &c->cap_done, e->name);
	}
	if (append_item(c, e) != 0) {
		set_err(c, "out of memory");
		goto done;
	}
	rc = 0;

done:
	strv_pop(c->visiting, &c->n_visiting);
	free(name);
	return rc;
}

/* Reject conflicts among the selected set and against installed packages. */
static int check_conflicts(resolve_ctx *c)
{
	size_t i, k;
	for (i = 0; i < c->n_items; i++) {
		const ftr_pkg_index_entry *e = c->items[i];
		for (k = 0; k < e->n_conflicts; k++) {
			char *cn = dep_name(e->conflicts[k]);
			size_t j;
			char *iv;
			int bad = 0;
			if (!cn) {
				set_err(c, "out of memory");
				return -1;
			}
			for (j = 0; j < c->n_items; j++) {
				if (i != j && strcmp(c->items[j]->name, cn) == 0) {
					bad = 1;
					break;
				}
			}
			if (!bad) {
				iv = ftr_db_find_version(cn);
				if (iv) {
					free(iv);
					bad = 1;
				}
			}
			if (bad) {
				set_err(c, "package '%s' conflicts with '%s'",
				        e->name, cn);
				free(cn);
				return -1;
			}
			free(cn);
		}
	}
	return 0;
}

int ftr_resolve(const char *const *targets, size_t n_targets,
                const ftr_repo_index *idxs, size_t n_idxs,
                const char *arch,
                ftr_resolve_result *out,
                char *errbuf, size_t errbufsz)
{
	resolve_ctx c;
	size_t i;
	int rc = -1;

	memset(&c, 0, sizeof(c));
	memset(out, 0, sizeof(*out));
	c.idxs = idxs;
	c.n_idxs = n_idxs;
	c.arch = (arch && *arch) ? arch : NULL;
	c.err = errbuf;
	c.errsz = errbufsz;

	for (i = 0; i < n_targets; i++) {
		if (resolve_one(&c, targets[i], 1) != 0) {
			goto out;
		}
	}
	if (check_conflicts(&c) != 0) {
		goto out;
	}

	out->items = c.items;
	out->n = c.n_items;
	c.items = NULL; /* ownership transferred */
	rc = 0;

out:
	for (i = 0; i < c.n_done; i++) {
		free(c.done[i]);
	}
	free(c.done);
	for (i = 0; i < c.n_visiting; i++) {
		free(c.visiting[i]);
	}
	free(c.visiting);
	free(c.items);
	return rc;
}

void ftr_resolve_free(ftr_resolve_result *out)
{
	if (!out) {
		return;
	}
	free(out->items); /* entries themselves are borrowed */
	out->items = NULL;
	out->n = 0;
}
