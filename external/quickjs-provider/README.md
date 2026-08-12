# QuickJS provider manifest

These files are generated together by:

```sh
cd quickjs-wasm-compilation/qjs-wasm-py
uv run qjs-wasm build provider
```

`quickjs_contract.manifest.json` is the verified runtime-profile lock.
`quickjs_contract.manifest.cmake` is its minimal CMake projection. Xahau checks
the JSON hash before generating the C++ profile constants, and the runtime
checks any supplied provider WASM against the projected size and SHA-256.

The provider binary is not vendored in this integration slice. Tests inject
the exact built artifact; the production artifact registry will embed or fetch
the same sealed bundle before the JSHooks amendment can be enabled.

For a local producer-to-consumer build, point Xahau straight at the emitted
bundle instead of copying it:

```sh
cmake -S . -B build \
  -DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR=/path/to/quickjs-wasm-compilation/build/xahau-hook-provider
```

CMake reads both generated manifests and, when the provider is present,
verifies its size and SHA-256 before generating the native profile constants.
