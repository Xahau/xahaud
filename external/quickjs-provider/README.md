# QuickJS provider manifest

These files are generated together by a jshookz source checkout:

```sh
# Current layout (cpp/provider, python/jshookz). Older checkouts used
# packages/jshookz; bin/update-jshookz-snapshot accepts either CLI path.
uv sync --project python/jshookz --locked --group dev
python/jshookz/.venv/bin/jshookz build provider
```

Update this Xahau branch from a clean, committed jshookz checkout with one
command:

```sh
bin/update-jshookz-snapshot /path/to/jshookz
```

That builds and imports the sealed lock files. CI checks out jshookz
`main` and fails if `build provider` does not reproduce that lock. The
importer then prints a self-contained `x-run-tests` command that
regenerates the embedded Hook fixtures and verifies the cut.

`jshookz_provider.manifest.json` is the verified runtime-profile lock.
`jshookz_provider.manifest.cmake` is its minimal CMake projection. Xahau checks
the JSON hash before generating the C++ profile constants, and the runtime
checks any supplied provider WASM against the projected size and SHA-256.

The provider binary is not vendored in this integration slice. Tests inject
the exact built artifact. Configure needs python3 to project these lock files.
A daemon whose embed is empty will not start.

For a local producer-to-consumer build, point Xahau straight at the emitted
bundle instead of copying it:

```sh
cmake -S . -B build \
  -DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR=/path/to/jshookz/build/xahau-provider
```

A build-time generator writes `QuickJSProviderValues.cpp` from those
locks and, if present, the gitignored wasm. Configure only checks that
the three lock files exist. Changing the bundle rebuilds that one
translation unit; hash, Wasmtime, and optional-wasm checks run then.
