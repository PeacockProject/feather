/*
 * main.c — entrypoint + subcommand dispatch for ftr.
 *
 * Mirrors pacman's flat dispatch style: argv[1] is looked up in a
 * static table of subcommands and the matching function is invoked
 * with (argc-1, argv+1). Global flags (--help, --version) are handled
 * before dispatch.
 */

#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct subcmd {
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *summary;
};

static const struct subcmd SUBCMDS[] = {
	{ "install", cmd_install, "install one or more packages"             },
	{ "remove",  cmd_remove,  "remove an installed package"              },
	{ "sync",    cmd_sync,    "refresh repository metadata"              },
	{ "upgrade", cmd_upgrade, "upgrade installed packages"               },
	{ "search",  cmd_search,  "search repositories for a package"        },
	{ "info",    cmd_info,    "show package metadata"                    },
	{ "list",    cmd_list,    "list installed packages"                  },
	{ "files",   cmd_files,   "list files owned by an installed package" },
	{ "flavor",  cmd_flavor,  "report the active Peacock base flavor"    },
};

static const size_t N_SUBCMDS = sizeof(SUBCMDS) / sizeof(SUBCMDS[0]);

/* ---------------------------------------------------------------- *
 * err_die / info_log
 * ---------------------------------------------------------------- */

void err_die(int code, const char *fmt, ...)
{
	va_list ap;
	fputs("ftr: error: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(code);
}

void info_log(const char *fmt, ...)
{
	va_list ap;
	fputs("ftr: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------------------------------------------------------------- *
 * --help / --version
 * ---------------------------------------------------------------- */

static void print_usage(FILE *out)
{
	size_t i;
	fprintf(out, "Usage: ftr <command> [options]\n");
	fprintf(out, "       ftr --help | --version\n");
	fprintf(out, "\n");
	fprintf(out, "feather %s — Peacock OS package manager\n", FTR_VERSION);
	fprintf(out, "\n");
	fprintf(out, "Commands:\n");
	for (i = 0; i < N_SUBCMDS; i++) {
		fprintf(out, "  %-10s  %s\n", SUBCMDS[i].name, SUBCMDS[i].summary);
	}
	fprintf(out, "\n");
	fprintf(out, "See ftr(1) for full documentation.\n");
}

static void print_version(FILE *out)
{
	fprintf(out, "ftr %s\n", FTR_VERSION);
}

static void print_unknown(const char *bad)
{
	size_t i;
	fprintf(stderr, "ftr: error: unknown command '%s'\n", bad);
	fprintf(stderr, "\n");
	fprintf(stderr, "Valid commands:\n");
	for (i = 0; i < N_SUBCMDS; i++) {
		fprintf(stderr, "  %s\n", SUBCMDS[i].name);
	}
	fprintf(stderr, "\n");
	fprintf(stderr, "Try 'ftr --help' for more information.\n");
}

/* ---------------------------------------------------------------- *
 * main
 * ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *cmd;
	size_t i;

	if (argc < 2) {
		print_usage(stderr);
		return 1;
	}

	cmd = argv[1];

	if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
		print_usage(stdout);
		return 0;
	}

	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
		print_version(stdout);
		return 0;
	}

	for (i = 0; i < N_SUBCMDS; i++) {
		if (strcmp(cmd, SUBCMDS[i].name) == 0) {
			return SUBCMDS[i].fn(argc - 1, argv + 1);
		}
	}

	print_unknown(cmd);
	return 1;
}
