#ifndef RECOMP_NET_MODSET_H
#define RECOMP_NET_MODSET_H

/* Canonical settings including the terminating NUL. Large sets are transferred
 * in bounded chunks; this limit never changes the transport's packet MTU. */
#define RNET_MODSET_TEXT_MAX 4096u

#endif
