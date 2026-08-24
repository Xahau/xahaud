#!/usr/bin/env python3
"""Project the sealed QuickJS bundle into one C++ translation unit."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

CHUNK = 64
CMAKE_SET = re.compile(r'^set\(XAHAU_QUICKJS_([A-Z0-9_]+) "([^"]*)"\)\s*$')


def quote(value: str) -> str:
    if not isinstance(value, str) or any(c in value for c in "\n\r"):
        raise ValueError(f"invalid QuickJS policy string: {value!r}")
    return json.dumps(value)


def parse_cmake(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text().splitlines():
        match = CMAKE_SET.match(line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def hex_bytes(value: str) -> str:
    if len(value) != 64 or any(c not in "0123456789abcdefABCDEF" for c in value):
        raise ValueError(f"QuickJS identity must be 32 hexadecimal bytes: {value!r}")
    return ", ".join(f"0x{value[i:i + 2]}" for i in range(0, 64, 2))


def require(cmake: dict[str, str], key: str) -> str:
    if key not in cmake:
        raise ValueError(f"QuickJS cmake projection missing {key}")
    return cmake[key]


def embed_body(data: bytes | None) -> str:
    if data is None:
        return (
            "std::span<std::uint8_t const>\n"
            "embeddedQuickJSProvider()\n"
            "{\n"
            "    return {};\n"
            "}\n"
        )
    lines = []
    for offset in range(0, len(data), CHUNK):
        chunk = data[offset : offset + CHUNK]
        lines.append('    "' + "".join(f"\\x{b:02x}" for b in chunk) + '"')
    return (
        "namespace {\n"
        "\n"
        "constexpr char const sealedProvider[] =\n"
        + "\n".join(lines)
        + ";\n"
        "\n"
        "static_assert(\n"
        f"    sizeof(sealedProvider) - 1 == {len(data)},\n"
        '    "embedded provider size disagrees with its manifest");\n'
        "\n"
        "}  // namespace\n"
        "\n"
        "std::span<std::uint8_t const>\n"
        "embeddedQuickJSProvider()\n"
        "{\n"
        "    return {\n"
        "        reinterpret_cast<std::uint8_t const*>(sealedProvider),\n"
        "        sizeof(sealedProvider) - 1};\n"
        "}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-dir", type=Path, required=True)
    parser.add_argument("--wasmtime-version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    bundle = args.bundle_dir
    profile_path = bundle / "jshookz_provider.manifest.json"
    cmake_path = bundle / "jshookz_provider.manifest.cmake"
    native_path = bundle / "jshookz_provider.native-abi.json"
    for path in (profile_path, cmake_path, native_path):
        if not path.is_file():
            print(f"missing sealed QuickJS bundle file: {path}", file=sys.stderr)
            return 1

    cmake = parse_cmake(cmake_path)
    schema = require(cmake, "MANIFEST_SCHEMA")
    if schema != "xahau.quickjs.runtime-profile-lock.v1":
        print(f"unsupported QuickJS provider manifest schema: {schema}", file=sys.stderr)
        return 1

    actual_manifest = hashlib.sha256(profile_path.read_bytes()).hexdigest()
    if actual_manifest != require(cmake, "MANIFEST_SHA256"):
        print(
            "QuickJS provider JSON manifest does not match its CMake projection",
            file=sys.stderr,
        )
        return 1

    actual_native = hashlib.sha256(native_path.read_bytes()).hexdigest()
    if actual_native != require(cmake, "NATIVE_ABI_SHA256"):
        print(
            "QuickJS native ABI snapshot does not match its sealed digest",
            file=sys.stderr,
        )
        return 1

    pinned_wasmtime = require(cmake, "WASMTIME_VERSION")
    if args.wasmtime_version != pinned_wasmtime:
        print(
            f"QuickJS provider requires Wasmtime {pinned_wasmtime}, "
            f"but CMake resolved {args.wasmtime_version}",
            file=sys.stderr,
        )
        return 1

    profile = json.loads(profile_path.read_text())
    native = json.loads(native_path.read_text())
    expected_count = int(require(cmake, "PROVIDER_IMPORT_COUNT"))
    provider_imports = profile["provider"]["imports"]
    native_imports = native["selected"]
    provider_names = [item["name"] for item in provider_imports]
    native_names = [item["name"] for item in native_imports]
    if (
        len(provider_imports) != expected_count
        or len(native_imports) != expected_count
        or len(set(provider_names)) != expected_count
        or len(set(native_names)) != expected_count
        or set(provider_names) != set(native_names)
    ):
        print("QuickJS provider/native ABI import sets differ", file=sys.stderr)
        return 1

    expected_export_count = int(require(cmake, "PROVIDER_EXPORT_COUNT"))
    provider_exports = profile["provider"]["exports"]
    if len(provider_exports) != expected_export_count:
        print(
            "QuickJS provider export count disagrees with its CMake lock",
            file=sys.stderr,
        )
        return 1
    memory_exports = [
        item
        for item in provider_exports
        if item.get("kind") == "memory" and item.get("name") == "memory"
    ]
    if len(memory_exports) != 1:
        print("QuickJS provider must export exactly one memory", file=sys.stderr)
        return 1
    memory = memory_exports[0]
    if (
        int(memory.get("minimum_pages", -1)) != 6
        or int(memory.get("maximum_pages", -1)) != 512
        or memory.get("memory64") is not False
        or memory.get("shared") is not False
    ):
        print(
            "QuickJS provider memory shape is not min 6 / max 512 / "
            "memory64=false / shared=false",
            file=sys.stderr,
        )
        return 1
    surface = profile["javascript_surface"]
    declaration_sha = str(surface["declaration_sha256"]).lower()
    surface_sha = str(surface["sha256"]).lower()
    if len(declaration_sha) != 64 or len(surface_sha) != 64:
        print("QuickJS javascript surface hashes are malformed", file=sys.stderr)
        return 1

    wasm_name = require(cmake, "PROVIDER_FILE")
    wasm_path = bundle / wasm_name
    expected_sha = require(cmake, "PROVIDER_SHA256").lower()
    expected_size = int(require(cmake, "PROVIDER_SIZE"))
    fingerprint = hashlib.sha256()
    for path in (profile_path, cmake_path, native_path, Path(__file__)):
        fingerprint.update(path.read_bytes())
    fingerprint.update(args.wasmtime_version.encode())
    wasm_bytes: bytes | None = None
    if wasm_path.is_file():
        wasm_bytes = wasm_path.read_bytes()
        digest = hashlib.sha256(wasm_bytes).hexdigest()
        if digest != expected_sha or len(wasm_bytes) != expected_size:
            print(
                "QuickJS provider WASM does not match its generated manifest",
                file=sys.stderr,
            )
            return 1
        fingerprint.update(b"wasm")
        fingerprint.update(bytes.fromhex(digest))
        provenance = f"{wasm_path} sha256={digest} size={len(wasm_bytes)}"
    else:
        fingerprint.update(b"absent")
        provenance = "no provider binary was present in the bundle directory"

    stamp_path = args.output.with_name(args.output.name + ".stamp")
    stamp = fingerprint.hexdigest()
    if (
        args.output.is_file()
        and stamp_path.is_file()
        and stamp_path.read_text() == stamp
    ):
        return 0

    if wasm_bytes is None:
        print(
            f"No QuickJS provider WASM at {wasm_path}; nothing is embedded "
            "and the daemon will refuse to start",
            file=sys.stderr,
        )

    provider_rows = ",\n    ".join(
        "{"
        + ", ".join(
            (
                quote(item["module"]),
                quote(item["name"]),
                quote(",".join(item["params"])),
                quote(",".join(item["results"])),
            )
        )
        + "}"
        for item in provider_imports
    )
    native_rows = ",\n    ".join(
        "{"
        + ", ".join(
            (
                quote(item["name"]),
                quote(item["return_type"]),
                quote(",".join(item["param_types"])),
                quote(item["amendment"] or ""),
            )
        )
        + "}"
        for item in native_imports
    )
    source = native["source"]
    if profile["provider"]["sha256"].lower() != expected_sha:
        print("provider sha256 disagrees between JSON lock and cmake", file=sys.stderr)
        return 1
    if int(profile["provider"]["size"]) != expected_size:
        print("provider size disagrees between JSON lock and cmake", file=sys.stderr)
        return 1

    text = f"""// Generated by cmake/GenerateQuickJSProviderBundle.py; do not edit.
// Source: {provenance}
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace hook::artifact::generated {{

std::array<std::uint8_t, 32> const providerSHA256 = {{
    {hex_bytes(expected_sha)}}};
std::array<std::uint8_t, 32> const bytecodeABI = {{
    {hex_bytes(require(cmake, "BYTECODE_ABI_ID"))}}};
std::array<std::uint8_t, 32> const runtimeProfile = {{
    {hex_bytes(require(cmake, "RUNTIME_PROFILE_ID"))}}};
std::size_t const providerSize = {expected_size};
std::uint16_t const hookApiVersion = {require(cmake, "HOOK_API_VERSION")};
std::uint64_t const initializationFuel = {require(cmake, "INITIALIZATION_FUEL")}ULL;
std::uint64_t const invocationFuel = {require(cmake, "INVOCATION_FUEL")}ULL;
std::string_view const hostWorkMeter = {quote(require(cmake, "HOST_WORK_METER"))};
std::uint64_t const hostWorkBudget = {require(cmake, "HOST_WORK_BUDGET")}ULL;
std::uint64_t const hostWorkBasePerCall = {require(cmake, "HOST_WORK_BASE_PER_CALL")}ULL;
std::uint64_t const hostWorkPerAddressedByte =
    {require(cmake, "HOST_WORK_PER_ADDRESSED_BYTE")}ULL;
std::string_view const hostAdapterPolicy = {quote(require(cmake, "HOST_ADAPTER_POLICY"))};
std::uint32_t const heapBytes = {require(cmake, "HEAP_BYTES")}U;
std::uint32_t const stackBytes = {require(cmake, "STACK_BYTES")}U;
std::uint32_t const serializedObjectMaxBytes =
    {require(cmake, "SERIALIZED_OBJECT_MAX_BYTES")}U;
std::uint32_t const serializedObjectMaxFields =
    {require(cmake, "SERIALIZED_OBJECT_MAX_FIELDS")}U;
std::uint32_t const serializedObjectMaxScopes =
    {require(cmake, "SERIALIZED_OBJECT_MAX_SCOPES")}U;
std::uint32_t const serializedObjectMaxDepth =
    {require(cmake, "SERIALIZED_OBJECT_MAX_DEPTH")}U;
std::uint32_t const providerMemoryMinimumPages = {int(memory["minimum_pages"])}U;
std::uint32_t const providerMemoryMaximumPages = {int(memory["maximum_pages"])}U;
bool const providerMemory64 = false;
bool const providerMemoryShared = false;
std::string_view const javascriptSurfaceDeclarationSHA256 =
    {quote(declaration_sha)};
std::string_view const javascriptSurfaceSHA256 = {quote(surface_sha)};

namespace {{

std::string_view const providerImportNames[] = {{
    {", ".join(quote(name) for name in provider_names)}}};
std::string_view const providerExportNames[] = {{
    {", ".join(quote(item["name"]) for item in provider_exports)}}};
ProviderImportSignature const providerImportSignatureData[] = {{
    {provider_rows}}};
NativeImportSignature const nativeImportSignatureData[] = {{
    {native_rows}}};

}}  // namespace

std::span<std::string_view const> const providerImports{{providerImportNames}};
std::span<std::string_view const> const providerExports{{providerExportNames}};
std::span<ProviderImportSignature const> const providerImportSignatures{{
    providerImportSignatureData}};
std::span<NativeImportSignature const> const nativeImportSignatures{{
    nativeImportSignatureData}};
std::string_view const nativeABISourceRepository = {quote(source["repository"])};
std::string_view const nativeABISourceCommit = {quote(source["commit"])};
std::string_view const nativeABISourcePath = {quote(source["path"])};
std::string_view const nativeABISHA256 = {quote(actual_native)};
std::size_t const nativeABICatalogueCount = {source["macro_function_count"]};

}}  // namespace hook::artifact::generated

namespace hook {{

{embed_body(wasm_bytes)}
}}  // namespace hook
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)
    stamp_path.write_text(stamp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
