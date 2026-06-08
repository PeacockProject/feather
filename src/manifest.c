/*
 * manifest.c — feather package manifest parser (phase 4 body).
 *
 * Phase 2 stubs out the API so main.c + cmd_*.c can link cleanly.
 */

#include "manifest.h"

#include <stddef.h>
#include <stdlib.h>

struct ftr_manifest {
	int placeholder;
};

ftr_manifest *ftr_manifest_parse(const char *buf, size_t len)
{
	(void)buf;
	(void)len;
	return NULL;
}

void ftr_manifest_free(ftr_manifest *m)
{
	free(m);
}
