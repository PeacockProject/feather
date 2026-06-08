/*
 * db.h — local install database at /var/lib/feather/local/.
 *
 * Mirrors pacman's split (local | sync | gpg) so admins recognize
 * the layout, but the on-disk format is feather's own. Format choice
 * (directory-of-files vs LMDB vs JSON) is deferred to phase 4.
 */

#ifndef FTR_DB_H
#define FTR_DB_H

/* Open / create the local DB at FTR_DB_ROOT. Returns 0 on success. */
int ftr_db_open(void);

/* Close the local DB. */
void ftr_db_close(void);

#endif /* FTR_DB_H */
