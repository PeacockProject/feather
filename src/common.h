/*
 * common.h — shared constants, error/log helpers, and subcommand prototypes
 * for feather (`ftr`), the Peacock OS package manager.
 *
 * Phase 2 (skeleton): defines version string + reserved namespace paths used
 * by phases 4/6 once install/sync/verify bodies land.
 */

#ifndef FTR_COMMON_H
#define FTR_COMMON_H

#include <stdarg.h>
#include <stddef.h>

/* ---------------------------------------------------------------- *
 * Version
 * ---------------------------------------------------------------- */

#define FTR_VERSION "0.1.0-skeleton"

/* ---------------------------------------------------------------- *
 * Filesystem namespaces owned by feather.
 *
 * The base distro (Arch/Debian/Alpine) owns /usr, /etc, /var, /home.
 * Feather writes only within the paths below (plus /etc/feather/,
 * /var/lib/feather/).
 * ---------------------------------------------------------------- */

#define FTR_DB_ROOT         "/var/lib/feather"
#define FTR_PEACOCK_PREFIX  "/peacock"
#define FTR_APPS_PREFIX     "/apps"
#define FTR_COMPAT_PREFIX   "/compat"
#define FTR_DATA_PREFIX     "/data"
#define FTR_CONFIG          "/etc/feather/feather.conf"

/* ---------------------------------------------------------------- *
 * Error / log helpers
 * ---------------------------------------------------------------- */

/* Print "ftr: error: <fmt>" to stderr and exit with `code`. */
void err_die(int code, const char *fmt, ...);

/* Print "ftr: error: <fmt>" to stderr. Does not exit. */
void err_log(const char *fmt, ...);

/* Print "ftr: <fmt>" to stderr. Does not exit. */
void info_log(const char *fmt, ...);

/* ---------------------------------------------------------------- *
 * Subcommand prototypes — one per cmd_<name>.c.
 *
 * Each returns 0 on success, non-zero on failure. argv[0] is the
 * subcommand name; argv[1..argc-1] are its remaining arguments.
 * ---------------------------------------------------------------- */

int cmd_install(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_sync(int argc, char **argv);
int cmd_upgrade(int argc, char **argv);
int cmd_search(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_files(int argc, char **argv);
int cmd_flavor(int argc, char **argv);
int cmd_index(int argc, char **argv);
int cmd_key(int argc, char **argv);

#endif /* FTR_COMMON_H */
