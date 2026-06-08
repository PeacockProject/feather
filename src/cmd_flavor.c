/*
 * cmd_flavor.c — `ftr flavor`
 *
 * Phase 3 / 4 will resolve the active base distro (arch | debian |
 * alpine | ...) from /etc/feather/feather.conf and the runtime
 * environment.
 */

#include "common.h"

#include <stdio.h>

int cmd_flavor(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "ftr flavor: not implemented yet\n");
	return 0;
}
