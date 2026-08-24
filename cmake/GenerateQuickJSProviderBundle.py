#!/usr/bin/env python3
"""Project the sealed QuickJS bundle into one C++ translation unit."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

CHUNK = 64
CMAKE_SET = re.compile(r'^set\(XAHAU_QUICKJS_([A-Z0-9_]+) "([^"]*)"\)\s*$')
WASM_VALTYPE = {"i32", "i64"}
WASM_BYTE = {"i32": "0x7f", "i64": "0x7e"}
WASM_STACK_BYTES = 131072
WASM_PAGE_BYTES = 65536
PROVIDER_MEMORY_MINIMUM_PAGES = 6
PROVIDER_MEMORY_MAXIMUM_PAGES = 512
PROVIDER_MEMORY_MAX_BYTES = PROVIDER_MEMORY_MAXIMUM_PAGES * WASM_PAGE_BYTES


class LockError(ValueError):
    """A sealed QuickJS lock identity failed a fail-closed check."""


def quote(value: str) -> str:
    if not isinstance(value, str) or any(c in value for c in "\n\r"):
        raise LockError(f"invalid QuickJS policy string: {value!r}")
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
        raise LockError(f"QuickJS identity must be 32 hexadecimal bytes: {value!r}")
    return ", ".join(f"0x{value[i:i + 2]}" for i in range(0, 64, 2))


def require(cmake: dict[str, str], key: str) -> str:
    if key not in cmake:
        raise LockError(f"QuickJS cmake projection missing {key}")
    return cmake[key]


def require_int(cmake: dict[str, str], key: str) -> int:
    return int(require(cmake, key))


def require_hex(value: object, label: str) -> str:
    text = str(value).lower()
    if len(text) != 64 or any(c not in "0123456789abcdef" for c in text):
        raise LockError(f"{label} is not a SHA-256 hex digest: {value!r}")
    return text


def cpp_bool(value: object) -> str:
    if value is True:
        return "true"
    if value is False:
        return "false"
    raise LockError(f"expected JSON boolean, got {value!r}")


def join_types(values: object, label: str) -> str:
    if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
        raise LockError(f"{label} is not a list of wasm value types: {values!r}")
    for item in values:
        if item not in WASM_VALTYPE:
            raise LockError(f"{label} has unsupported wasm value type {item!r}")
    return ",".join(values)


def wasm_signature(params: object, results: object, label: str) -> list[str]:
    join_types(params, f"{label} params")
    join_types(results, f"{label} results")
    assert isinstance(params, list) and isinstance(results, list)
    return [WASM_BYTE[item] for item in results] + [WASM_BYTE[item] for item in params]


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


def format_export_row(item: dict[str, Any]) -> str:
    kind = item["kind"]
    name = item["name"]
    if kind == "memory":
        fields = (
            quote(kind),
            quote(name),
            quote(""),
            quote(""),
            f"{int(item['minimum_pages'])}U",
            f"{int(item['maximum_pages'])}U",
            cpp_bool(item["memory64"]),
            cpp_bool(item["shared"]),
        )
    elif kind == "function":
        fields = (
            quote(kind),
            quote(name),
            quote(join_types(item["params"], f"export {name} params")),
            quote(join_types(item["results"], f"export {name} results")),
            "0U",
            "0U",
            "false",
            "false",
        )
    else:
        raise LockError(f"unsupported provider export kind: {kind!r}")
    return "{" + ", ".join(fields) + "}"


def require_typed_exports(exports: object, label: str) -> list[dict[str, Any]]:
    if not isinstance(exports, list) or not all(isinstance(item, dict) for item in exports):
        raise LockError(f"{label} must be a list of typed export rows")
    names: list[str] = []
    memory_rows: list[dict[str, Any]] = []
    for item in exports:
        kind = item.get("kind")
        name = item.get("name")
        if not isinstance(kind, str) or not isinstance(name, str) or not name:
            raise LockError(f"{label} row is missing kind/name: {item!r}")
        if kind == "function":
            join_types(item.get("params"), f"{label} {name} params")
            join_types(item.get("results"), f"{label} {name} results")
            if any(
                key in item
                for key in ("minimum_pages", "maximum_pages", "memory64", "shared")
            ):
                raise LockError(f"{label} function {name} carries memory fields")
        elif kind == "memory":
            for key in ("minimum_pages", "maximum_pages"):
                if not isinstance(item.get(key), int):
                    raise LockError(f"{label} memory row missing integer {key}")
            if item.get("memory64") is not False or item.get("shared") is not False:
                raise LockError(
                    f"{label} memory row is not memory64=false / shared=false"
                )
            if "params" in item or "results" in item:
                raise LockError(f"{label} memory row must not carry params/results")
            memory_rows.append(item)
        else:
            raise LockError(f"{label} has unsupported export kind {kind!r}")
        names.append(name)
    if len(names) != len(set(names)):
        raise LockError(f"{label} contains duplicate export names")
    if names != sorted(names):
        raise LockError(f"{label} must be normalized by export name")
    if len(memory_rows) != 1 or memory_rows[0].get("name") != "memory":
        raise LockError(f"{label} must export exactly one memory named memory")
    return exports


def require_typed_imports(imports: object, label: str) -> list[dict[str, Any]]:
    if not isinstance(imports, list) or not all(isinstance(item, dict) for item in imports):
        raise LockError(f"{label} must be a list of typed import rows")
    names: list[str] = []
    for item in imports:
        module = item.get("module")
        name = item.get("name")
        if not isinstance(module, str) or not isinstance(name, str) or not name:
            raise LockError(f"{label} row is missing module/name: {item!r}")
        wasm_signature(item.get("params"), item.get("results"), f"{label} {name}")
        names.append(name)
    if len(names) != len(set(names)):
        raise LockError(f"{label} contains duplicate import names")
    return imports


def require_build(build: object, label: str) -> dict[str, Any]:
    if not isinstance(build, dict):
        raise LockError(f"{label} is missing build metadata")
    stack = build.get("wasm_stack_bytes")
    memory = build.get("wasm_memory_max_bytes")
    if stack != WASM_STACK_BYTES:
        raise LockError(
            f"{label} wasm_stack_bytes is {stack!r}, expected {WASM_STACK_BYTES}"
        )
    if memory != PROVIDER_MEMORY_MAX_BYTES:
        raise LockError(
            f"{label} wasm_memory_max_bytes is {memory!r}, "
            f"expected {PROVIDER_MEMORY_MAX_BYTES}"
        )
    return build


def cross_compare_int(cmake: dict[str, str], key: str, actual: object, label: str) -> int:
    expected = require_int(cmake, key)
    if actual != expected:
        raise LockError(
            f"{label} disagrees between JSON ({actual!r}) and CMake {key} ({expected!r})"
        )
    return expected


def cross_compare_str(cmake: dict[str, str], key: str, actual: object, label: str) -> str:
    expected = require(cmake, key)
    if str(actual) != expected:
        raise LockError(
            f"{label} disagrees between JSON ({actual!r}) and CMake {key} ({expected!r})"
        )
    return expected


def validate_lock(
    profile: dict[str, Any],
    native: dict[str, Any],
    cmake: dict[str, str],
    wasmtime_version: str,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any], str, str, int]:
    schema = require(cmake, "MANIFEST_SCHEMA")
    if schema != "xahau.quickjs.runtime-profile-lock.v1" or profile.get("schema") != schema:
        raise LockError(f"unsupported QuickJS provider manifest schema: {schema}")

    expected_sha = require_hex(require(cmake, "PROVIDER_SHA256"), "CMake provider SHA-256")
    json_sha = require_hex(profile.get("provider", {}).get("sha256"), "JSON provider SHA-256")
    if json_sha != expected_sha:
        raise LockError("provider sha256 disagrees between JSON lock and cmake")
    expected_size = require_int(cmake, "PROVIDER_SIZE")
    json_size = profile.get("provider", {}).get("size")
    if json_size != expected_size:
        raise LockError("provider size disagrees between JSON lock and cmake")

    bytecode = require_hex(require(cmake, "BYTECODE_ABI_ID"), "CMake bytecode ABI")
    if require_hex(profile.get("bytecode_abi_id"), "JSON bytecode ABI") != bytecode:
        raise LockError("bytecode ABI disagrees between JSON lock and cmake")
    runtime_profile = require_hex(
        require(cmake, "RUNTIME_PROFILE_ID"), "CMake runtime-profile ID"
    )
    if require_hex(profile.get("runtime_profile_id"), "JSON runtime-profile ID") != runtime_profile:
        raise LockError("runtime-profile ID disagrees between JSON lock and cmake")

    pinned_wasmtime = require(cmake, "WASMTIME_VERSION")
    engine_version = profile.get("source", {}).get("engine", {}).get("version")
    if wasmtime_version != pinned_wasmtime or engine_version != pinned_wasmtime:
        raise LockError(
            f"QuickJS provider requires Wasmtime {pinned_wasmtime}, "
            f"but CMake resolved {wasmtime_version} and JSON has {engine_version!r}"
        )

    limits = profile.get("source", {}).get("limits")
    if not isinstance(limits, dict):
        raise LockError("JSON runtime-profile source is missing limits")
    cross_compare_int(
        cmake, "SERIALIZED_OBJECT_MAX_BYTES", limits.get("serialized_object_max_bytes"),
        "serialized_object_max_bytes",
    )
    cross_compare_int(
        cmake, "SERIALIZED_OBJECT_MAX_FIELDS", limits.get("serialized_object_max_fields"),
        "serialized_object_max_fields",
    )
    cross_compare_int(
        cmake, "SERIALIZED_OBJECT_MAX_SCOPES", limits.get("serialized_object_max_scopes"),
        "serialized_object_max_scopes",
    )
    cross_compare_int(
        cmake, "SERIALIZED_OBJECT_MAX_DEPTH", limits.get("serialized_object_max_depth"),
        "serialized_object_max_depth",
    )
    cross_compare_int(cmake, "HEAP_BYTES", limits.get("quickjs_heap_bytes"), "quickjs_heap_bytes")
    cross_compare_int(cmake, "STACK_BYTES", limits.get("quickjs_stack_bytes"), "quickjs_stack_bytes")
    cross_compare_int(
        cmake, "INITIALIZATION_FUEL", limits.get("wasmtime_fuel_per_initialization"),
        "initialization fuel",
    )
    cross_compare_int(
        cmake, "INVOCATION_FUEL", limits.get("wasmtime_fuel_per_invocation"),
        "invocation fuel",
    )
    cross_compare_int(cmake, "HOST_WORK_BUDGET", limits.get("host_work_budget"), "host_work_budget")
    cross_compare_int(
        cmake, "HOST_WORK_BASE_PER_CALL", limits.get("host_work_base_per_call"),
        "host_work_base_per_call",
    )
    cross_compare_int(
        cmake, "HOST_WORK_PER_ADDRESSED_BYTE", limits.get("host_work_per_addressed_byte"),
        "host_work_per_addressed_byte",
    )
    cross_compare_str(cmake, "HOST_WORK_METER", limits.get("host_work_meter"), "host_work_meter")
    cross_compare_str(
        cmake, "HOST_ADAPTER_POLICY",
        profile.get("source", {}).get("execution", {}).get("host_adapter_policy"),
        "host_adapter_policy",
    )
    cross_compare_int(
        cmake, "HOOK_API_VERSION",
        profile.get("source", {}).get("artifact", {}).get("hook_api_version"),
        "hook_api_version",
    )

    provider = profile.get("provider")
    source_provider = profile.get("source", {}).get("provider")
    if not isinstance(provider, dict) or not isinstance(source_provider, dict):
        raise LockError("JSON lock is missing provider or source.provider")
    require_build(provider.get("build"), "provider.build")
    require_build(source_provider.get("build"), "source.provider.build")

    expected_import_count = require_int(cmake, "PROVIDER_IMPORT_COUNT")
    provider_imports = require_typed_imports(provider.get("imports"), "provider.imports")
    source_imports = require_typed_imports(
        source_provider.get("imports"), "source.provider.imports"
    )
    native_imports = native.get("selected")
    if not isinstance(native_imports, list):
        raise LockError("native ABI snapshot is missing selected imports")
    if provider_imports != source_imports:
        raise LockError("provider import signatures disagree between JSON copies")
    if (
        len(provider_imports) != expected_import_count
        or len(native_imports) != expected_import_count
    ):
        raise LockError("QuickJS provider/native ABI import counts differ")
    provider_names = [item["name"] for item in provider_imports]
    native_names = [item["name"] for item in native_imports]
    if len(set(provider_names)) != expected_import_count or set(provider_names) != set(
        native_names
    ):
        raise LockError("QuickJS provider/native ABI import sets differ")
    native_by_name = {item["name"]: item for item in native_imports}
    for item in provider_imports:
        native_item = native_by_name[item["name"]]
        expected_sig = wasm_signature(
            item["params"], item["results"], f"import {item['name']}"
        )
        if native_item.get("wasm_signature") != expected_sig:
            raise LockError(
                f"import {item['name']} wasm signature disagrees between JSON and native ABI"
            )

    expected_export_count = require_int(cmake, "PROVIDER_EXPORT_COUNT")
    provider_exports = require_typed_exports(provider.get("exports"), "provider.exports")
    allowed_exports = require_typed_exports(
        source_provider.get("allowed_exports"), "source.provider.allowed_exports"
    )
    if len(provider_exports) != expected_export_count:
        raise LockError("QuickJS provider export count disagrees with its CMake lock")
    if provider_exports != allowed_exports:
        raise LockError("provider export signatures disagree between JSON copies")
    memory = next(item for item in provider_exports if item["kind"] == "memory")
    if (
        int(memory["minimum_pages"]) != PROVIDER_MEMORY_MINIMUM_PAGES
        or int(memory["maximum_pages"]) != PROVIDER_MEMORY_MAXIMUM_PAGES
        or memory["memory64"] is not False
        or memory["shared"] is not False
    ):
        raise LockError(
            "QuickJS provider memory shape is not min 6 / max 512 / "
            "memory64=false / shared=false"
        )

    surface = profile.get("javascript_surface")
    nested_surface = profile.get("source", {}).get("javascript_surface")
    if not isinstance(surface, dict) or not isinstance(nested_surface, dict):
        raise LockError("JSON lock is missing javascript_surface")
    declaration_sha = require_hex(
        surface.get("declaration_sha256"), "javascript surface declaration SHA-256"
    )
    surface_sha = require_hex(surface.get("sha256"), "javascript surface SHA-256")
    for key in ("declaration", "manifest", "schema"):
        if surface.get(key) != nested_surface.get(key):
            raise LockError(f"javascript_surface.{key} disagrees between JSON copies")

    return (
        provider_imports,
        provider_exports,
        native_imports,
        declaration_sha,
        surface_sha,
        expected_size,
    )


def render_source(
    *,
    provenance: str,
    cmake: dict[str, str],
    native: dict[str, Any],
    provider_imports: list[dict[str, Any]],
    provider_exports: list[dict[str, Any]],
    native_imports: list[dict[str, Any]],
    declaration_sha: str,
    surface_sha: str,
    expected_sha: str,
    expected_size: int,
    wasm_bytes: bytes | None,
    actual_native: str,
) -> str:
    provider_names = [item["name"] for item in provider_imports]
    provider_rows = ",\n    ".join(
        "{"
        + ", ".join(
            (
                quote(item["module"]),
                quote(item["name"]),
                quote(join_types(item["params"], f"import {item['name']} params")),
                quote(join_types(item["results"], f"import {item['name']} results")),
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
    export_rows = ",\n    ".join(format_export_row(item) for item in provider_exports)
    source = native["source"]
    memory = next(item for item in provider_exports if item["kind"] == "memory")
    return f"""// Generated by cmake/GenerateQuickJSProviderBundle.py; do not edit.
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
std::uint32_t const wasmStackBytes = {WASM_STACK_BYTES}U;
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
ProviderExportSignature const providerExportSignatureData[] = {{
    {export_rows}}};
NativeImportSignature const nativeImportSignatureData[] = {{
    {native_rows}}};

}}  // namespace

std::span<std::string_view const> const providerImports{{providerImportNames}};
std::span<std::string_view const> const providerExports{{providerExportNames}};
std::span<ProviderImportSignature const> const providerImportSignatures{{
    providerImportSignatureData}};
std::span<ProviderExportSignature const> const providerExportSignatures{{
    providerExportSignatureData}};
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


def project_bundle(bundle: Path, wasmtime_version: str, output: Path) -> int:
    profile_path = bundle / "jshookz_provider.manifest.json"
    cmake_path = bundle / "jshookz_provider.manifest.cmake"
    native_path = bundle / "jshookz_provider.native-abi.json"
    for path in (profile_path, cmake_path, native_path):
        if not path.is_file():
            print(f"missing sealed QuickJS bundle file: {path}", file=sys.stderr)
            return 1

    cmake = parse_cmake(cmake_path)
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

    try:
        profile = json.loads(profile_path.read_text())
        native = json.loads(native_path.read_text())
        (
            provider_imports,
            provider_exports,
            native_imports,
            declaration_sha,
            surface_sha,
            expected_size,
        ) = validate_lock(profile, native, cmake, wasmtime_version)
    except LockError as error:
        print(str(error), file=sys.stderr)
        return 1

    wasm_name = require(cmake, "PROVIDER_FILE")
    wasm_path = bundle / wasm_name
    expected_sha = require(cmake, "PROVIDER_SHA256").lower()
    fingerprint = hashlib.sha256()
    for path in (profile_path, cmake_path, native_path, Path(__file__)):
        fingerprint.update(path.read_bytes())
    fingerprint.update(wasmtime_version.encode())
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

    stamp_path = output.with_name(output.name + ".stamp")
    stamp = fingerprint.hexdigest()
    if output.is_file() and stamp_path.is_file() and stamp_path.read_text() == stamp:
        return 0

    if wasm_bytes is None:
        print(
            f"No QuickJS provider WASM at {wasm_path}; nothing is embedded "
            "and the daemon will refuse to start",
            file=sys.stderr,
        )

    text = render_source(
        provenance=provenance,
        cmake=cmake,
        native=native,
        provider_imports=provider_imports,
        provider_exports=provider_exports,
        native_imports=native_imports,
        declaration_sha=declaration_sha,
        surface_sha=surface_sha,
        expected_sha=expected_sha,
        expected_size=expected_size,
        wasm_bytes=wasm_bytes,
        actual_native=actual_native,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    stamp_path.write_text(stamp)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-dir", type=Path, required=True)
    parser.add_argument("--wasmtime-version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        return project_bundle(args.bundle_dir, args.wasmtime_version, args.output)
    except LockError as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
