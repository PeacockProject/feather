/*
 * manifest.h — feather package manifest parser.
 *
 * A .feather archive embeds a manifest describing name/version,
 * [install].layout (peacock | app | compat | system), runtime,
 * and the file list. Phase 4 lands the parser; for now this header
 * just reserves the API shape so other translation units can include
 * it without circular dependencies.
 *
 * Format decision (toml-c vs json) is deferred to phase 4. The opaque
 * `ftr_manifest` type insulates callers from that choice.
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

typedef struct ftr_manifest ftr_manifest;

/* Phase 4: parse a manifest from a buffer. Returns NULL on error. */
ftr_manifest *ftr_manifest_parse(const char *buf, size_t len);

/* Free a parsed manifest. Safe on NULL. */
void ftr_manifest_free(ftr_manifest *m);

#endif /* FTR_MANIFEST_H */
