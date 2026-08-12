# Raw C Hook catalogue fixture

`generate.py` uses Clang's AST for the public `hook/extern.h` declarations,
generates one retained/exported wrapper per function, and compiles a Wasm guest
whose `hook(selector)` dispatches to exactly one wrapper. It does not read
`hook_api.macro` or the runtime descriptor table, so comparison with the live
macro projection is independent evidence rather than a generator testing
itself.

Regenerate with `./generate.py`. By default the pinned `wasmcc` compiler parses
the declarations and builds the guest, avoiding a host-Clang dependency. The
generated-header workflow runs `./generate.py --check`; its exact compiler
identities, flags, declaration hash, and artifact hashes are recorded in
`manifest.json`, so toolchain drift fails freshness rather than silently
rewriting the fixture.
