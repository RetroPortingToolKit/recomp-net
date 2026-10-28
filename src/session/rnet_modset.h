#ifndef RNET_MODSET_RX_H
#define RNET_MODSET_RX_H
#include "recomp_net/modset.h"
#include "recomp_net/types.h"

typedef struct RNetModsetRx {
    char text[RNET_MODSET_TEXT_MAX];
    rnet_u32 checksum;
    rnet_u16 total, pieces;
    rnet_u8 source, ready;
} RNetModsetRx;

/* 1: complete verified string; 0: incomplete; -1: malformed/conflicting data.
 * Duplicates/reordering are harmless. A new tuple invalidates pending data. */
int rnet_modset_accept_chunk(RNetModsetRx *rx, rnet_u8 source,
    rnet_u16 total, rnet_u16 offset, const char *data, rnet_u16 length,
    rnet_u32 checksum);
#endif
