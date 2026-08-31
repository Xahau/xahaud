# QuickJS provider manifest

These files are generated or sealed together by a jshookz source checkout:

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

That builds and imports the sealed lock files plus the shared six-file API
artifact set. The baseline product remains `provider`; the entropy declaration
and surface are inert evidence required to authenticate that shared manifest.
The importer then prints a self-contained `x-run-tests` command that
regenerates the embedded Hook fixtures and verifies the cut.

`jshookz_provider.lock.json` is the sole consumer acceptance lock. It pins the
product, immutable release artifact, producer manifest, native ABI, API
artifact manifest, runtime profile, bytecode ABI, and Wasmtime version. The
other JSON files are producer evidence authenticated by that lock; there is no
second CMake or source-code pin table to update. Xahau projects C++ constants
from the verified JSON and checks any supplied provider WASM against the locked
size and SHA-256.
`api-artifacts.json` closes the six tracked API artifacts; Xahau verifies its
own digest and every named artifact before projecting their identities. The
runtime-profile source also seals the sole XQJS envelope version, its three
profile codes, and the packed module-validation result layout. Xahau projects
those named values and fails closed on source/lock drift. These files add no
parallel CMake lock-value fields.

Canonical XQJS version 1 keeps the fixed 80-byte header and stores the
big-endian XFL arithmetic-profile code in bytes 10-11: 0 for none, 1 for
`xahauFloatV1`, and 2 for `nearestEvenV1`. No other envelope version or
profile code is accepted.

The provider binary is not vendored in this integration slice.
`bin/fetch-jshookz-provider` derives the immutable GitHub release tag from the
locked provider SHA-256 and verifies both its digest and size. Configure needs
python3 to project these lock files. A daemon whose embed is empty will not
start.

For a local producer-to-consumer build, point Xahau straight at the emitted
bundle instead of copying it:

```sh
cmake -S . -B build \
  -DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR=/path/to/jshookz/build/xahau-provider
```

A build-time generator writes `QuickJSProviderValues.cpp` from that lock and,
if present, the gitignored wasm. Changing the bundle rebuilds that one
translation unit; hash, Wasmtime, and optional-wasm checks run then.
