/*
 * cmd_remove.c — `ftr remove <pkg>...`
 *
 * Phase 4 will implement reverse-overlay removal and prompt for
 * /data/<pkg> cleanup.
 */

#include "common.h"

#include <stdio.h>

int cmd_remove(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "ftr remove: not implemented yet\n");
	return 0;
}
