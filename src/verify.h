/*
 * verify.h — signature verification.
 *
 * Phase 6 implements signature verification of .feather packages
 * against the build farm's per-channel public key. Signing algorithm
 * (GPG vs minisign) is deferred to phase 6; leaning minisign for
 * size.
 */

#ifndef FTR_VERIFY_H
#define FTR_VERIFY_H

#include <stddef.h>

/* Verify `pkg_buf` (length `len`) against the bundled root cert.
 * Returns 0 on valid signature. Phase 6 body. */
int ftr_verify_package(const unsigned char *pkg_buf, size_t len);

#endif /* FTR_VERIFY_H */
