# QuickJS provider receipt

The jshookz producer exports the complete consumer bundle with:

```sh
python/jshookz/.venv/bin/jshookz build provider
python/jshookz/.venv/bin/jshookz export-bundle provider \
  -o /path/to/xahaud/external/quickjs-provider
```

From a clean, committed producer checkout, the Xahau wrapper performs both
steps and prints the focused runtime test command:

```sh
bin/update-jshookz-snapshot /path/to/jshookz
```

`jshookz_provider.receipt` is the sole consumer acceptance record. It is a
strict, sorted `key value` file that pins the provider WASM, the preprojected
C++ values, the producer provenance, the runtime profile, the bytecode ABI,
and the exact Wasmtime version. It is data, not an executable CMake include.

The Xahau build consumes exactly three bundle files:

- `jshookz_provider.receipt`;
- `jshookz_provider.values.cpp`, verified by the receipt and compiled as-is;
- `jshookz_provider.wasm`, verified by the receipt and embedded by CMake.

The JSON manifests, declarations, and JavaScript surfaces shipped in the
producer bundle remain reviewable provenance. Xahau does not parse or require
them during configure or build; their identities are already projected into
the receipt and values source by the producer's fail-closed export.

Provider releases use `provider-<receipt sha256>` as the tag. The receipt pins
every other build input, so a metadata or API change behind unchanged WASM
bytes still creates a distinct release. `bin/fetch-jshookz-provider` derives
that tag from the committed receipt, downloads the gitignored WASM, and checks
its pinned digest and size without Python.

For a local producer-to-consumer build, point Xahau directly at the exported
bundle:

```sh
cmake -S . -B build \
  -DXAHAU_QUICKJS_PROVIDER_BUNDLE_DIR=/path/to/jshookz/build/xahau-provider-bundle
```

If the WASM is absent, configuration remains possible but the generated embed
is empty and the daemon refuses to start.

## Fuel snapshot

Exact native execution measurements live together in
`src/test/app/JSHooks_fuel.snapshot`. They are consumer-side values because
they cover the provider, Wasmtime, Xahau's native host, and the compiled test
Hooks together; they are not producer release metadata.

`bin/update-jshookz-snapshot` prints one authoritative `x-run-tests` command
with `XAHAU_UPDATE_QJS_FUEL_SNAPSHOT=1`. That run compiles the current Hook
fixtures, executes every JSHooks measurement, echoes the complete sorted
`key value` snapshot, and rewrites the file only after the suite reaches its
end. A normal run reads the snapshot and fails if a named value changes or a
row is missing or added.
