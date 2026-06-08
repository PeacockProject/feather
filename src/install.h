/*
 * install.h — overlay install logic.
 *
 * Phase 4 implements `layout = "peacock"` overlay: unpack a .feather
 * archive into /peacock with conflict detection and record the file
 * list in the local DB. Later phases extend to /apps and /compat.
 */

#ifndef FTR_INSTALL_H
#define FTR_INSTALL_H

#include "manifest.h"

/* Install an already-parsed manifest's payload into the target root.
 * `target_root` lets callers redirect installs (e.g. a staging chroot)
 * away from "/". Phase 4 implements; phase 2 is a stub returning -1. */
int ftr_install_overlay(const ftr_manifest *m, const char *target_root);

#endif /* FTR_INSTALL_H */
