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
PROVIDER_MEMORY_MINIMUM_PAGES = 8
PROVIDER_MEMORY_MAXIMUM_PAGES = 512
PROVIDER_MEMORY_MAX_BYTES = PROVIDER_MEMORY_MAXIMUM_PAGES * WASM_PAGE_BYTES
SEALED_MANIFEST_SHA256 = (
    "d987997ed97f11d9d1db8ee156eb41781fa0c6966d6760523e1218dd40833498"
)
SEALED_PROVIDER_SHA256 = (
    "eef126d573dcec94b3c5255ac9d66333fdac15438f4109e62d378a44c9de9390"
)
SEALED_PROVIDER_SIZE = 1247172
SEALED_NATIVE_ABI_SHA256 = (
    "1c7cccdc3085b6a5c5fab47cccbee154609547a1d67f2407ff0449bf3325cda7"
)
SEALED_BYTECODE_ABI_ID = (
    "75ea54f357d397c4b33899e495bb385dad975a43b1c4a7a2cead30474327d33e"
)
SEALED_RUNTIME_PROFILE_ID = (
    "5dae59fa453879321783a22476a9c7334acaa636b00be2c54fae1991e20b93b2"
)
SEALED_WASMTIME_VERSION = "47.0.3"
SEALED_HOOK_API_VERSION = 1
SEALED_HOST_ADAPTER_POLICY = "xahau-raw-hook-host-v1"
SEALED_BROAD_DECLARATION_SHA256 = (
    "a83c16b97fb4faf98962f4c746b07d43ec5de0de7e689e7c225bc4ceb93b8c3e"
)
SEALED_EXACT_V1_DECLARATION_SHA256 = (
    "e03101173bd8775c118feb313fb89e974c4887d61727451c9993043ee41ea4aa"
)
SEALED_SURFACE_SHA256 = (
    "8074d80057977d3a4998dba443448df0430b98227c6a27c6b0e010917d279c21"
)
SEALED_API_ARTIFACT_MANIFEST_SHA256 = (
    "cdf88b15524f7f0d8c15b46cfe67fd95ec7917b1e20a09a2db90bfd5a26aa64d"
)
SEALED_XFL_PROFILE_LEDGER_SHA256 = (
    "cfcb68fe9a195f6e70c88a1b8f2d2936838b8c98b3d70cbe2cab9a53e056fd80"
)
API_ARTIFACT_SCHEMA = "jshookz.api-artifacts.v1"
SEALED_API_ARTIFACTS = {
    "python/jshookz/src/jshookz/types/hooks-api.d.ts": (
        "broad_declaration",
        "hooks-api.d.ts",
        SEALED_BROAD_DECLARATION_SHA256,
    ),
    "python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts": (
        "exact_v1_declaration",
        "xahau-quickjs-v1.d.ts",
        SEALED_EXACT_V1_DECLARATION_SHA256,
    ),
    "python/jshookz/src/jshookz/types/xahau-quickjs-v1.surface.json": (
        "selected_surface",
        "xahau-quickjs-v1.surface.json",
        SEALED_SURFACE_SHA256,
    ),
    "python/jshookz/src/jshookz/xfl_profile_ledger.ts": (
        "xfl_profile_ledger",
        "xfl-profile-ledger.ts",
        SEALED_XFL_PROFILE_LEDGER_SHA256,
    ),
}
SEALED_ARTIFACT = {
    "envelope_version": 1,
    "hook_api_version": 1,
    "kind": "quickjs-bytecode",
    "xfl_arithmetic_profile_codes": {
        "none": 0,
        "xahauFloatV1": 1,
        "nearestEvenV1": 2,
    },
    "xfl_arithmetic_profile_implementations": {
        "none": [],
        "xahauFloatV1": [
            "XFLDecimal.add",
            "XFLDecimal.divide",
            "XFLDecimal.multiply",
            "XFLDecimal.subtract",
        ],
        "nearestEvenV1": [],
    },
}
SEALED_MODULE_VALIDATION_RESULT = {
    "layout_version": 1,
    "failure_sentinel": -1,
    "main_bit": 1,
    "callback_bit": 2,
    "entry_mask": 3,
    "reserved_mask": 0x800000FC,
    "profile_mask": 0x00FFFF00,
    "profile_shift": 8,
    "version_mask": 0x7F000000,
    "version_shift": 24,
}
SEALED_LIMITS = {
    "host_work_base_per_call": 1,
    "host_work_budget": 2097152,
    "host_work_meter": "base-plus-addressed-byte-v1",
    "host_work_per_addressed_byte": 1,
    "quickjs_heap_bytes": 16777216,
    "quickjs_stack_bytes": 65536,
    "serialized_object_max_bytes": 1048576,
    "serialized_object_max_fields": 32768,
    "serialized_object_max_scopes": 32769,
    "serialized_object_max_depth": 10,
    "wasmtime_fuel_per_initialization": 5000000,
    "wasmtime_fuel_per_invocation": 50000000,
}
SEALED_HOST_WORK_ADDRESSED_LENGTH_INDICES = {
    "accept": [1],
    "emit": [1, 3],
    "etxn_details": [1],
    "etxn_fee_base": [1],
    "hook_account": [1],
    "hook_param": [1, 3],
    "ledger_last_hash": [1],
    "ledger_nonce": [1],
    "otxn_param": [1, 3],
    "prepare": [1, 3],
    "rollback": [1],
    "slot": [1],
    "slot_set": [1],
    "state": [1, 3],
    "state_foreign": [1, 3, 5, 7],
    "state_foreign_set": [1, 3, 5, 7],
    "state_set": [1, 3],
    "trace": [1, 3],
}
SEALED_ENGINE_CONFIGURATION = {
    "consume_fuel": True,
    "cranelift_nan_canonicalization": True,
    "wasi": False,
    "wasm_memory64": False,
    "wasm_multi_memory": False,
    "wasm_relaxed_simd": False,
    "wasm_tail_call": False,
    "wasm_threads": False,
}
IMPORT_KEYS = frozenset({"module", "name", "params", "results"})
FUNCTION_EXPORT_KEYS = frozenset({"kind", "name", "params", "results"})
MEMORY_EXPORT_KEYS = frozenset(
    {"kind", "name", "minimum_pages", "maximum_pages", "memory64", "shared"}
)
SURFACE_KEYS = frozenset(
    {"declaration", "declaration_sha256", "manifest", "schema", "sha256"}
)
NESTED_SURFACE_KEYS = frozenset({"declaration", "manifest", "schema"})


def _import_row(name: str, params: list[str], results: list[str]) -> dict[str, Any]:
    return {"module": "env", "name": name, "params": params, "results": results}


def _function_export(
    name: str, params: list[str], results: list[str]
) -> dict[str, Any]:
    return {"kind": "function", "name": name, "params": params, "results": results}


SEALED_IMPORTS = [
    _import_row("accept", ["i32", "i32", "i64"], ["i64"]),
    _import_row("emit", ["i32", "i32", "i32", "i32"], ["i64"]),
    _import_row("etxn_details", ["i32", "i32"], ["i64"]),
    _import_row("etxn_fee_base", ["i32", "i32"], ["i64"]),
    _import_row("etxn_reserve", ["i32"], ["i64"]),
    _import_row("fee_base", [], ["i64"]),
    _import_row("hook_account", ["i32", "i32"], ["i64"]),
    _import_row("hook_again", [], ["i64"]),
    _import_row("hook_param", ["i32", "i32", "i32", "i32"], ["i64"]),
    _import_row("ledger_last_hash", ["i32", "i32"], ["i64"]),
    _import_row("ledger_last_time", [], ["i64"]),
    _import_row("ledger_nonce", ["i32", "i32"], ["i64"]),
    _import_row("ledger_seq", [], ["i64"]),
    _import_row("otxn_param", ["i32", "i32", "i32", "i32"], ["i64"]),
    _import_row("otxn_slot", ["i32"], ["i64"]),
    _import_row("otxn_type", [], ["i64"]),
    _import_row("prepare", ["i32", "i32", "i32", "i32"], ["i64"]),
    _import_row("rollback", ["i32", "i32", "i64"], ["i64"]),
    _import_row("slot", ["i32", "i32", "i32"], ["i64"]),
    _import_row("slot_clear", ["i32"], ["i64"]),
    _import_row("slot_set", ["i32", "i32", "i32"], ["i64"]),
    _import_row("slot_size", ["i32"], ["i64"]),
    _import_row("state", ["i32", "i32", "i32", "i32"], ["i64"]),
    _import_row(
        "state_foreign",
        ["i32", "i32", "i32", "i32", "i32", "i32", "i32", "i32"],
        ["i64"],
    ),
    _import_row(
        "state_foreign_set",
        ["i32", "i32", "i32", "i32", "i32", "i32", "i32", "i32"],
        ["i64"],
    ),
    _import_row("state_set", ["i32", "i32", "i32", "i32"], ["i64"]),
    _import_row("trace", ["i32", "i32", "i32", "i32", "i32"], ["i64"]),
]
SEALED_EXPORTS = [
    _function_export("_initialize", [], []),
    _function_export("free", ["i32"], []),
    _function_export("malloc", ["i32"], ["i32"]),
    {
        "kind": "memory",
        "maximum_pages": 512,
        "memory64": False,
        "minimum_pages": 8,
        "name": "memory",
        "shared": False,
    },
    _function_export("qjs_cbak", ["i32", "i32", "i32"], ["i32"]),
    _function_export("qjs_compile", ["i32", "i32"], ["i32"]),
    _function_export("qjs_compile_module", ["i32", "i32"], ["i32"]),
    _function_export("qjs_destroy", [], []),
    _function_export("qjs_enable_coverage", ["i32"], []),
    _function_export("qjs_eval", ["i32", "i32"], ["i32"]),
    _function_export("qjs_eval_bytecode", ["i32", "i32"], ["i32"]),
    _function_export("qjs_eval_module", ["i32", "i32"], ["i32"]),
    _function_export("qjs_get_bytecode_len", [], ["i32"]),
    _function_export("qjs_get_bytecode_ptr", [], ["i32"]),
    _function_export("qjs_get_result_len", [], ["i32"]),
    _function_export("qjs_get_result_ptr", [], ["i32"]),
    _function_export("qjs_hook", ["i32", "i32", "i32"], ["i32"]),
    _function_export("qjs_init", [], []),
    _function_export("qjs_set_max_stack_size", ["i32"], []),
    _function_export("qjs_set_memory_limit", ["i32"], []),
    _function_export("qjs_set_seed", ["i32"], []),
    _function_export("qjs_validate_hook_module", ["i32", "i32"], ["i32"]),
]


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
    return ", ".join(f"0x{value[i : i + 2]}" for i in range(0, 64, 2))


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


def require_sealed(actual: object, expected: object, label: str) -> None:
    if actual != expected:
        raise LockError(
            f"{label} disagrees with the sealed F0 table: {actual!r} != {expected!r}"
        )


def validate_api_artifacts(bundle: Path) -> dict[str, str]:
    manifest_path = bundle / "api-artifacts.json"
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict) or set(manifest) != {"artifacts", "schema"}:
        raise LockError("API artifact manifest has extra or missing keys")
    if manifest.get("schema") != API_ARTIFACT_SCHEMA:
        raise LockError(
            "API artifact manifest schema disagrees with the sealed F0 table"
        )
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict) or set(artifacts) != set(SEALED_API_ARTIFACTS):
        raise LockError(
            "API artifact manifest file set disagrees with the sealed F0 table"
        )

    identities: dict[str, str] = {}
    for source_path, artifact in SEALED_API_ARTIFACTS.items():
        identity_name, local_name, sealed_digest = artifact
        actual_digest = hashlib.sha256((bundle / local_name).read_bytes()).hexdigest()
        declared_digest = require_hex(
            artifacts.get(source_path), f"API artifact {source_path} SHA-256"
        )
        if actual_digest != declared_digest:
            raise LockError(
                f"API artifact {source_path} disagrees with its manifest digest"
            )
        require_sealed(actual_digest, sealed_digest, f"API artifact {source_path}")
        identities[identity_name] = actual_digest

    actual_manifest = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    require_sealed(
        actual_manifest,
        SEALED_API_ARTIFACT_MANIFEST_SHA256,
        "API artifact manifest SHA-256",
    )
    identities["api_artifact_manifest"] = actual_manifest
    return identities


def cpp_bool(value: object) -> str:
    if value is True:
        return "true"
    if value is False:
        return "false"
    raise LockError(f"expected JSON boolean, got {value!r}")


def join_types(values: object, label: str) -> str:
    if not isinstance(values, list) or not all(
        isinstance(item, str) for item in values
    ):
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
        "constexpr char const sealedProvider[] =\n" + "\n".join(lines) + ";\n"
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
    if not isinstance(exports, list) or not all(
        isinstance(item, dict) for item in exports
    ):
        raise LockError(f"{label} must be a list of typed export rows")
    names: list[str] = []
    memory_rows: list[dict[str, Any]] = []
    for item in exports:
        kind = item.get("kind")
        name = item.get("name")
        if not isinstance(kind, str) or not isinstance(name, str) or not name:
            raise LockError(f"{label} row is missing kind/name: {item!r}")
        if kind == "function":
            if set(item) != FUNCTION_EXPORT_KEYS:
                raise LockError(f"{label} function {name} has extra or missing keys")
            join_types(item.get("params"), f"{label} {name} params")
            join_types(item.get("results"), f"{label} {name} results")
        elif kind == "memory":
            if set(item) != MEMORY_EXPORT_KEYS:
                raise LockError(f"{label} memory row has extra or missing keys")
            for key in ("minimum_pages", "maximum_pages"):
                if not isinstance(item.get(key), int):
                    raise LockError(f"{label} memory row missing integer {key}")
            if item.get("memory64") is not False or item.get("shared") is not False:
                raise LockError(
                    f"{label} memory row is not memory64=false / shared=false"
                )
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
    if not isinstance(imports, list) or not all(
        isinstance(item, dict) for item in imports
    ):
        raise LockError(f"{label} must be a list of typed import rows")
    names: list[str] = []
    for item in imports:
        module = item.get("module")
        name = item.get("name")
        if not isinstance(module, str) or not isinstance(name, str) or not name:
            raise LockError(f"{label} row is missing module/name: {item!r}")
        if set(item) != IMPORT_KEYS:
            raise LockError(f"{label} {name} has extra or missing keys")
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


def cross_compare_int(
    cmake: dict[str, str], key: str, actual: object, label: str
) -> int:
    expected = require_int(cmake, key)
    if actual != expected:
        raise LockError(
            f"{label} disagrees between JSON ({actual!r}) and CMake {key} ({expected!r})"
        )
    return expected


def cross_compare_str(
    cmake: dict[str, str], key: str, actual: object, label: str
) -> str:
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
    api_identities: dict[str, str],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any], str, str, int]:
    schema = require(cmake, "MANIFEST_SCHEMA")
    if (
        schema != "xahau.quickjs.runtime-profile-lock.v1"
        or profile.get("schema") != schema
    ):
        raise LockError(f"unsupported QuickJS provider manifest schema: {schema}")

    expected_sha = require_hex(
        require(cmake, "PROVIDER_SHA256"), "CMake provider SHA-256"
    )
    json_sha = require_hex(
        profile.get("provider", {}).get("sha256"), "JSON provider SHA-256"
    )
    if json_sha != expected_sha:
        raise LockError("provider sha256 disagrees between JSON lock and cmake")
    require_sealed(expected_sha, SEALED_PROVIDER_SHA256, "provider SHA-256")
    expected_size = require_int(cmake, "PROVIDER_SIZE")
    json_size = profile.get("provider", {}).get("size")
    if json_size != expected_size:
        raise LockError("provider size disagrees between JSON lock and cmake")
    require_sealed(expected_size, SEALED_PROVIDER_SIZE, "provider size")

    bytecode = require_hex(require(cmake, "BYTECODE_ABI_ID"), "CMake bytecode ABI")
    if require_hex(profile.get("bytecode_abi_id"), "JSON bytecode ABI") != bytecode:
        raise LockError("bytecode ABI disagrees between JSON lock and cmake")
    require_sealed(bytecode, SEALED_BYTECODE_ABI_ID, "bytecode ABI")
    runtime_profile = require_hex(
        require(cmake, "RUNTIME_PROFILE_ID"), "CMake runtime-profile ID"
    )
    if (
        require_hex(profile.get("runtime_profile_id"), "JSON runtime-profile ID")
        != runtime_profile
    ):
        raise LockError("runtime-profile ID disagrees between JSON lock and cmake")
    require_sealed(runtime_profile, SEALED_RUNTIME_PROFILE_ID, "runtime-profile ID")

    pinned_wasmtime = require(cmake, "WASMTIME_VERSION")
    engine = profile.get("source", {}).get("engine", {})
    engine_version = engine.get("version")
    if wasmtime_version != pinned_wasmtime or engine_version != pinned_wasmtime:
        raise LockError(
            f"QuickJS provider requires Wasmtime {pinned_wasmtime}, "
            f"but CMake resolved {wasmtime_version} and JSON has {engine_version!r}"
        )
    require_sealed(pinned_wasmtime, SEALED_WASMTIME_VERSION, "Wasmtime version")
    require_sealed(
        engine.get("configuration"),
        SEALED_ENGINE_CONFIGURATION,
        "Wasmtime engine configuration",
    )

    limits = profile.get("source", {}).get("limits")
    if not isinstance(limits, dict):
        raise LockError("JSON runtime-profile source is missing limits")
    cross_compare_int(
        cmake,
        "SERIALIZED_OBJECT_MAX_BYTES",
        limits.get("serialized_object_max_bytes"),
        "serialized_object_max_bytes",
    )
    cross_compare_int(
        cmake,
        "SERIALIZED_OBJECT_MAX_FIELDS",
        limits.get("serialized_object_max_fields"),
        "serialized_object_max_fields",
    )
    cross_compare_int(
        cmake,
        "SERIALIZED_OBJECT_MAX_SCOPES",
        limits.get("serialized_object_max_scopes"),
        "serialized_object_max_scopes",
    )
    cross_compare_int(
        cmake,
        "SERIALIZED_OBJECT_MAX_DEPTH",
        limits.get("serialized_object_max_depth"),
        "serialized_object_max_depth",
    )
    for key, expected in SEALED_LIMITS.items():
        require_sealed(limits.get(key), expected, key)
    require_sealed(
        limits.get("host_work_addressed_length_indices"),
        SEALED_HOST_WORK_ADDRESSED_LENGTH_INDICES,
        "host_work_addressed_length_indices",
    )
    cross_compare_int(
        cmake, "HEAP_BYTES", limits.get("quickjs_heap_bytes"), "quickjs_heap_bytes"
    )
    cross_compare_int(
        cmake, "STACK_BYTES", limits.get("quickjs_stack_bytes"), "quickjs_stack_bytes"
    )
    cross_compare_int(
        cmake,
        "INITIALIZATION_FUEL",
        limits.get("wasmtime_fuel_per_initialization"),
        "initialization fuel",
    )
    cross_compare_int(
        cmake,
        "INVOCATION_FUEL",
        limits.get("wasmtime_fuel_per_invocation"),
        "invocation fuel",
    )
    cross_compare_int(
        cmake, "HOST_WORK_BUDGET", limits.get("host_work_budget"), "host_work_budget"
    )
    cross_compare_int(
        cmake,
        "HOST_WORK_BASE_PER_CALL",
        limits.get("host_work_base_per_call"),
        "host_work_base_per_call",
    )
    cross_compare_int(
        cmake,
        "HOST_WORK_PER_ADDRESSED_BYTE",
        limits.get("host_work_per_addressed_byte"),
        "host_work_per_addressed_byte",
    )
    cross_compare_str(
        cmake, "HOST_WORK_METER", limits.get("host_work_meter"), "host_work_meter"
    )
    host_adapter_policy = cross_compare_str(
        cmake,
        "HOST_ADAPTER_POLICY",
        profile.get("source", {}).get("execution", {}).get("host_adapter_policy"),
        "host_adapter_policy",
    )
    require_sealed(
        host_adapter_policy, SEALED_HOST_ADAPTER_POLICY, "host_adapter_policy"
    )
    artifact = profile.get("source", {}).get("artifact")
    if not isinstance(artifact, dict):
        raise LockError("JSON runtime-profile source is missing artifact metadata")
    require_sealed(artifact, SEALED_ARTIFACT, "artifact activation contract")
    hook_api_version = cross_compare_int(
        cmake,
        "HOOK_API_VERSION",
        artifact.get("hook_api_version"),
        "hook_api_version",
    )
    require_sealed(hook_api_version, SEALED_HOOK_API_VERSION, "hook_api_version")

    provider = profile.get("provider")
    source_provider = profile.get("source", {}).get("provider")
    if not isinstance(provider, dict) or not isinstance(source_provider, dict):
        raise LockError("JSON lock is missing provider or source.provider")
    require_sealed(
        source_provider.get("module_validation_result"),
        SEALED_MODULE_VALIDATION_RESULT,
        "module-validation result layout",
    )
    require_build(provider.get("build"), "provider.build")
    require_build(source_provider.get("build"), "source.provider.build")
    require_sealed(
        source_provider.get("forbidden_import_modules"),
        ["wasi_snapshot_preview1"],
        "forbidden provider import modules",
    )

    expected_import_count = require_int(cmake, "PROVIDER_IMPORT_COUNT")
    provider_imports = require_typed_imports(
        provider.get("imports"), "provider.imports"
    )
    source_imports = require_typed_imports(
        source_provider.get("imports"), "source.provider.imports"
    )
    native_imports = native.get("selected")
    if not isinstance(native_imports, list):
        raise LockError("native ABI snapshot is missing selected imports")
    if provider_imports != source_imports:
        raise LockError("provider import signatures disagree between JSON copies")
    if provider_imports != SEALED_IMPORTS:
        raise LockError("provider import signatures disagree with the sealed F0 table")
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
    provider_exports = require_typed_exports(
        provider.get("exports"), "provider.exports"
    )
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
            "QuickJS provider memory shape is not min 8 / max 512 / "
            "memory64=false / shared=false"
        )
    if provider_exports != SEALED_EXPORTS:
        raise LockError("provider export signatures disagree with the sealed F0 table")

    surface = profile.get("javascript_surface")
    nested_surface = profile.get("source", {}).get("javascript_surface")
    if not isinstance(surface, dict) or not isinstance(nested_surface, dict):
        raise LockError("JSON lock is missing javascript_surface")
    if set(surface) != SURFACE_KEYS or set(nested_surface) != NESTED_SURFACE_KEYS:
        raise LockError("javascript_surface has extra or missing keys")
    declaration_sha = require_hex(
        surface.get("declaration_sha256"), "javascript surface declaration SHA-256"
    )
    surface_sha = require_hex(surface.get("sha256"), "javascript surface SHA-256")
    require_sealed(
        declaration_sha,
        api_identities["exact_v1_declaration"],
        "exact-v1 declaration SHA-256",
    )
    require_sealed(
        surface_sha, api_identities["selected_surface"], "selected surface SHA-256"
    )
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
    broad_declaration_sha: str,
    xfl_profile_ledger_sha: str,
    api_artifact_manifest_sha: str,
    manifest_sha: str,
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
std::string_view const providerManifestSHA256 = {quote(manifest_sha)};
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
std::string_view const javascriptBroadDeclarationSHA256 =
    {quote(broad_declaration_sha)};
std::string_view const javascriptExactV1DeclarationSHA256 =
    {quote(declaration_sha)};
std::string_view const javascriptSurfaceSHA256 = {quote(surface_sha)};
std::string_view const javascriptXFLProfileLedgerSHA256 =
    {quote(xfl_profile_ledger_sha)};
std::string_view const javascriptAPIArtifactManifestSHA256 =
    {quote(api_artifact_manifest_sha)};
std::uint8_t const xqjsEnvelopeVersion = {SEALED_ARTIFACT["envelope_version"]}U;
std::uint16_t const xflArithmeticProfileNone =
    {SEALED_ARTIFACT["xfl_arithmetic_profile_codes"]["none"]}U;
std::uint16_t const xflArithmeticProfileXahauFloatV1 =
    {SEALED_ARTIFACT["xfl_arithmetic_profile_codes"]["xahauFloatV1"]}U;
std::uint16_t const xflArithmeticProfileNearestEvenV1 =
    {SEALED_ARTIFACT["xfl_arithmetic_profile_codes"]["nearestEvenV1"]}U;
std::uint32_t const moduleValidationLayoutVersion =
    {SEALED_MODULE_VALIDATION_RESULT["layout_version"]}U;
std::int32_t const moduleValidationFailureSentinel =
    {SEALED_MODULE_VALIDATION_RESULT["failure_sentinel"]};
std::uint32_t const moduleValidationMainBit =
    {SEALED_MODULE_VALIDATION_RESULT["main_bit"]}U;
std::uint32_t const moduleValidationCallbackBit =
    {SEALED_MODULE_VALIDATION_RESULT["callback_bit"]}U;
std::uint32_t const moduleValidationEntryMask =
    {SEALED_MODULE_VALIDATION_RESULT["entry_mask"]}U;
std::uint32_t const moduleValidationReservedMask =
    {SEALED_MODULE_VALIDATION_RESULT["reserved_mask"]}U;
std::uint32_t const moduleValidationProfileMask =
    {SEALED_MODULE_VALIDATION_RESULT["profile_mask"]}U;
std::uint32_t const moduleValidationProfileShift =
    {SEALED_MODULE_VALIDATION_RESULT["profile_shift"]}U;
std::uint32_t const moduleValidationVersionMask =
    {SEALED_MODULE_VALIDATION_RESULT["version_mask"]}U;
std::uint32_t const moduleValidationVersionShift =
    {SEALED_MODULE_VALIDATION_RESULT["version_shift"]}U;

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
    api_manifest_path = bundle / "api-artifacts.json"
    api_paths = tuple(
        bundle / local_name for _, local_name, _ in SEALED_API_ARTIFACTS.values()
    )
    for path in (profile_path, cmake_path, native_path, api_manifest_path, *api_paths):
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
        api_identities = validate_api_artifacts(bundle)
        (
            provider_imports,
            provider_exports,
            native_imports,
            declaration_sha,
            surface_sha,
            expected_size,
        ) = validate_lock(profile, native, cmake, wasmtime_version, api_identities)
        require_sealed(
            actual_manifest, SEALED_MANIFEST_SHA256, "provider manifest SHA-256"
        )
        require_sealed(actual_native, SEALED_NATIVE_ABI_SHA256, "native ABI SHA-256")
    except LockError as error:
        print(str(error), file=sys.stderr)
        return 1

    wasm_name = require(cmake, "PROVIDER_FILE")
    wasm_path = bundle / wasm_name
    expected_sha = require(cmake, "PROVIDER_SHA256").lower()
    fingerprint = hashlib.sha256()
    for path in (
        profile_path,
        cmake_path,
        native_path,
        api_manifest_path,
        *api_paths,
        Path(__file__),
    ):
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
        broad_declaration_sha=api_identities["broad_declaration"],
        xfl_profile_ledger_sha=api_identities["xfl_profile_ledger"],
        api_artifact_manifest_sha=api_identities["api_artifact_manifest"],
        manifest_sha=actual_manifest,
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
