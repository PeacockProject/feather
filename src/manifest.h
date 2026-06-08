/*
 * manifest.h — feather package manifest parser.
 *
 * A .feather archive embeds `manifest.toml` describing the package's
 * identity (name, version), runtime, install layout (peacock | app |
 * compat | system), capabilities it provides, and packages it
 * conflicts with. Phase 4 implements the parser against the vendored
 * tomlc99 reader at src/vendor/.
 *
 * Owned strings on the manifest struct live until ftr_manifest_free()
 * is called. Callers should not mutate them; treat them as `const`.
 */

#ifndef FTR_MANIFEST_H
#define FTR_MANIFEST_H

#include <stddef.h>

typedef enum {
	FTR_LAYOUT_PEACOCK = 0,
	FTR_LAYOUT_APP,
	FTR_LAYOUT_COMPAT,
	FTR_LAYOUT_SYSTEM,
	FTR_LAYOUT_UNKNOWN
} ftr_layout_t;

/* One key=value pair under [provides] or [conflicts]. */
typedef struct {
	char *name;
	char *version; /* "*" allowed; never NULL once parsed */
} ftr_capability;

typedef struct {
	/* [package] */
	char *name;           /* mandatory */
	char *version;        /* mandatory */
	char *description;    /* optional, NULL if absent */
	char *runtime;        /* optional, NULL if absent */
	char **flavors;       /* optional, NULL if absent */
	size_t n_flavors;

	/* [install] */
	ftr_layout_t layout;  /* mandatory */
	char *prefix;         /* optional override; NULL = use layout default */

	/* [provides] */
	ftr_capability *provides;
	size_t n_provides;

	/* [conflicts] */
	ftr_capability *conflicts;
	size_t n_conflicts;
} ftr_manifest;

/* Parse a TOML manifest from a file on disk. Returns 0 on success,
 * -1 on error; on error, writes a human-readable message into
 * `errbuf` (up to errbufsz bytes, NUL-terminated). `out` is
 * zero-initialised on entry and only filled on success.
 */
int ftr_manifest_load(const char *path, ftr_manifest *out,
                      char *errbuf, size_t errbufsz);

/* Free heap-allocated fields on `m`. Safe on a zero-initialised
 * struct (no-op). Does not free `m` itself — caller owns the outer
 * storage (typically a stack variable).
 */
void ftr_manifest_free(ftr_manifest *m);

/* Map an ftr_layout_t back to its canonical string ("peacock", etc.). */
const char *ftr_layout_name(ftr_layout_t l);

/* Default install prefix for a given layout. For FTR_LAYOUT_APP the
 * caller appends /<pkgname>; this helper returns the base path
 * ("/apps") in that case.
 */
const char *ftr_layout_default_prefix(ftr_layout_t l);

#endif /* FTR_MANIFEST_H */
