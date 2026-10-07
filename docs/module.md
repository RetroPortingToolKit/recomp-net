# Loadable module

`-DRNET_BUILD_MODULE=ON -DRNET_RBENGINE_DIR=<retcomm-rbengine>` also builds
`recomp_net_module` (`.so` / `.dll` / `.dylib`): recomp-net, rbengine and, with
`RNET_ENABLE_ICE`, libjuice, in one shared library. An engine or runner loads it
at run time so the netcode updates without rebuilding the engine.

Contract: `include/recomp_net/module.h`.

- `rnet_module_info()` reports `abi_version`, `abi_minor`, `wire_version`,
  `features`, the library version and a build id. `rnet_module_check()` is the
  header-only acceptance test a loader runs first.
- Every public `rnet_*` / `rbe_*` function is exported under its own name
  (Linux: `module/exports.map`; Windows: export-all). Nothing else is.
- **Bump `RNET_MODULE_ABI_VERSION`** when a struct layout or signature changes;
  **`ABI_MINOR`** when symbols or trailing fields are added;
  **`RNET_MODULE_WIRE_VERSION`** on any change two peers must agree on. Engines
  fold the wire version into the identity passed to
  `rnet_rb_driver_set_identity`.
- Gate: `module_test` dlopens the built library and resolves the driver.
