/*
 * resolve.h — dependency resolver.
 *
 * Given one or more target package names and the set of synced repo
 * indexes, compute the full transitive install set in topological order
 * (dependencies before dependents). Dependencies are matched by package
 * name first, then by any package that [provides] the name. Packages
 * already recorded in the local DB are treated as satisfied and omitted.
 * [conflicts] among the selected + installed set are rejected.
 *
 * The resolver works entirely off index metadata — it does not download
 * or unpack any archive. Result entries are BORROWED pointers into the
 * caller-owned `idxs`; they stay valid only as long as `idxs` does.
 */

#ifndef FTR_RESOLVE_H
#define FTR_RESOLVE_H

#include <stddef.h>

#include "repo.h"

typedef struct {
	const ftr_pkg_index_entry **items; /* topological: deps first */
	size_t n;
} ftr_resolve_result;

/* Resolve `targets` (+ transitive depends). `arch` (if non-NULL) keeps
 * only candidates whose arch matches it or is arch-independent
 * (entry->arch NULL or "any"). Returns 0 on success, -1 on error with a
 * message in errbuf. On success the caller frees with ftr_resolve_free(). */
int ftr_resolve(const char *const *targets, size_t n_targets,
                const ftr_repo_index *idxs, size_t n_idxs,
                const char *arch,
                ftr_resolve_result *out,
                char *errbuf, size_t errbufsz);

void ftr_resolve_free(ftr_resolve_result *out);

#endif /* FTR_RESOLVE_H */
