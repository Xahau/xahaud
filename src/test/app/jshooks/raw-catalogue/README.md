# Independent Wasmtime host-catalogue fixture

`generate.py` uses Clang's AST for the public `hook/extern.h` declarations,
generates one retained/exported wrapper per function, and compiles an
independent Wasm test guest with one callable wrapper per host import. It does
not read `hook_api.macro` or the runtime descriptor table, so binding it through
the live generic Wasmtime callback is independent evidence rather than a
generator testing itself.

This fixture tests the host adapter only. It is not a selectable C-Hook
Wasmtime engine and is not production execution machinery.

Regenerate with `./generate.py`. By default `wasmcc` from the active toolchain
parses the declarations and builds the guest, avoiding a host-Clang dependency.
The generated-header workflow runs `./generate.py --check`; the recorded
compiler identities, flags, declaration hash, and artifact hashes in
`manifest.json` make toolchain drift fail freshness rather than silently
rewriting the fixture.
