#include "rnet_modset.h"
#include "../protocol/rnet_protocol.h"
#include <string.h>

int rnet_modset_accept_chunk(RNetModsetRx *rx, rnet_u8 source,
    rnet_u16 total, rnet_u16 offset, const char *data, rnet_u16 length,
    rnet_u32 checksum)
{
    unsigned bit, count;
    if (!rx || !data || !total || total >= RNET_MODSET_TEXT_MAX ||
        offset >= total || offset % RNET_MODSET_CHUNK_BYTES || !length ||
        length != ((unsigned)(total - offset) > RNET_MODSET_CHUNK_BYTES ?
                   RNET_MODSET_CHUNK_BYTES : (unsigned)(total - offset)) ||
        memchr(data, 0, length))
        return -1;
    if (rx->total != total || rx->checksum != checksum || rx->source != source) {
        rx->total = total; rx->checksum = checksum; rx->source = source;
        rx->pieces = 0; rx->ready = 0; rx->text[0] = 0;
    }
    bit = 1u << (offset / RNET_MODSET_CHUNK_BYTES);
    if ((rx->pieces & bit) && memcmp(rx->text + offset, data, length)) {
        rx->pieces = 0; rx->ready = 0; rx->text[0] = 0;
        return -1;
    }
    memcpy(rx->text + offset, data, length);
    rx->pieces = (rnet_u16)(rx->pieces | bit);
    count = (total + RNET_MODSET_CHUNK_BYTES - 1u) / RNET_MODSET_CHUNK_BYTES;
    if (rx->pieces != (1u << count) - 1u) return 0;
    if (rnet_proto_checksum((const rnet_u8 *)rx->text, total) != checksum) {
        rx->pieces = 0; rx->ready = 0; rx->text[0] = 0;
        return -1;
    }
    rx->text[total] = 0; rx->ready = 1;
    return 1;
}
