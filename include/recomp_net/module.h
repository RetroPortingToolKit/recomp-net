#ifndef RECOMP_NET_MODULE_H
#define RECOMP_NET_MODULE_H

/*
 * The loadable netplay module.
 *
 * recomp-net (with retcomm-rbengine, and libjuice when built with ICE) can be
 * built as ONE shared library, `recomp_net_module`, that an engine or runner
 * loads at run time instead of linking. That lets the netcode be versioned and
 * updated without rebuilding the engine core.
 *
 * Three numbers describe a module; a loader checks them before it calls
 * anything else:
 *
 *   abi_version   The loader contract: which symbols exist and what their
 *                 structs look like. A loader built for ABI N accepts a module
 *                 whose abi_version == N and whose abi_minor >= the minor it
 *                 needs. The version moves only when a layout or signature
 *                 changes, and then every older loader refuses.
 *   abi_minor     Bumped when symbols or trailing struct fields are added.
 *   wire_version  The peer-to-peer protocol. Two peers can play only when
 *                 their wire_versions are equal. It is independent of the
 *                 ABI: a wire change that keeps the ABI still must bump this.
 *                 Engines fold it into the identity they advertise to peers
 *                 (rnet_rb_driver_set_identity), so a module update can never
 *                 silently pair two peers that cannot agree.
 *
 * The module exports every public rnet_* and rbe_* function under its own
 * name, so a loader resolves them with dlsym/GetProcAddress. A loader that
 * cannot find a symbol it needs refuses the module by name; it never falls
 * back to a stub.
 */

#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNET_MODULE_ABI_VERSION 1u
#define RNET_MODULE_ABI_MINOR 0u
#define RNET_MODULE_WIRE_VERSION 1u

#define RNET_MODULE_FEATURE_ICE (1u << 0)      /* built with libjuice */
#define RNET_MODULE_FEATURE_RBENGINE (1u << 1) /* exports rbe_* */

typedef struct RNetModuleInfo {
    rnet_u32 struct_size; /* sizeof(RNetModuleInfo) as the module built it */
    rnet_u32 abi_version;
    rnet_u32 abi_minor;
    rnet_u32 wire_version;
    rnet_u32 features;
    rnet_u32 version_major; /* RNET_VERSION_* of the sources */
    rnet_u32 version_minor;
    rnet_u32 version_patch;
    const char *build_id; /* source commit, or "unknown"; static string */
} RNetModuleInfo;

/* Fills `out` (at most out_size bytes) and returns the bytes written. Callable
 * before any other function. */
size_t rnet_module_info(RNetModuleInfo *out, size_t out_size);

/* Loader-side acceptance test, header-only so a loader needs no module symbol
 * to decide. Returns NULL when `m` is usable by a loader built against this
 * header, otherwise a static string naming why not. */
static inline const char *rnet_module_check(const RNetModuleInfo *m, size_t got,
                                            rnet_u32 need_features) {
    if (got < 5 * sizeof(rnet_u32)) return "module info truncated";
    if (m->abi_version != RNET_MODULE_ABI_VERSION) return "module ABI version differs";
    if (m->abi_minor < RNET_MODULE_ABI_MINOR) return "module ABI minor too old";
    if ((m->features & need_features) != need_features) return "module lacks a required feature";
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_MODULE_H */
