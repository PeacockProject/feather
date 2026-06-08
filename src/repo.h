/*
 * repo.h — remote repository client.
 *
 * Phase 6 implements repo metadata refresh + package fetch against
 * the PeacockProject build farm at repo.peacock-project.dev. The
 * channel (stable | testing | unstable) and flavor are resolved from
 * /etc/feather/feather.conf.
 */

#ifndef FTR_REPO_H
#define FTR_REPO_H

/* Refresh repo metadata. Phase 6 body. */
int ftr_repo_sync(void);

#endif /* FTR_REPO_H */
