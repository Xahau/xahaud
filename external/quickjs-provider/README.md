# QuickJS provider manifest

These files are generated together by a jshookz source checkout:

```sh
uv sync --project packages/jshookz --locked --group dev
packages/jshookz/.venv/bin/jshookz build provider
```

Update this Xahau branch from a clean, committed jshookz checkout with one
command:

```sh
bin/update-jshookz-snapshot /path/to/jshookz
```

That builds and imports the sealed bundle and pins the end-to-end workflow to
the exact producer commit. It then prints a self-contained `x-run-tests`
command that regenerates the embedded Hook fixtures and verifies the cut.

`jshookz_provider.manifest.json` is the verified runtime-profile lock.
`jshookz_provider.manifest.cmake` is its minimal CMake projection. Xahau checks
the JSON hash before generating the C++ profile constants, and the runtime
checks any supplied provider WASM against the projected size and SHA-256.

The provider binary is not vendored in this integration slice. Tests inject
the exact built artifact; the production artifact registry will embed or fetch
the same sealed bundle before the JSHooks amendment can be enabled.

For a local producer-to-consumer build, point Xahau straight at the emitted
bundle instead of copying it:

```sh
cmake -S . -B build \
  -DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR=/path/to/jshookz/build/xahau-provider
```

CMake reads both generated manifests and, when the provider is present,
verifies its size and SHA-256 before generating the native profile constants.
