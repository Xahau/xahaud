#!/usr/bin/env python3
"""Fail-closed generator/checker mutation reds for the sealed QuickJS lock."""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).with_name("GenerateQuickJSProviderBundle.py")
BUNDLE = ROOT / "external" / "quickjs-provider"
WASM_VERSION = "47.0.3"
CMAKE_SET = re.compile(r'^set\(XAHAU_QUICKJS_([A-Z0-9_]+) "([^"]*)"\)\s*$')

PIN = {
    "provider_sha256": "a801c6759b92979ed4d846ceebc51b173606976882aa32de9cdd349494e251ea",
    "provider_size": "1219461",
    "manifest_sha256": "ab04142f01444ce3dd3afd1390e9f0480bc09461c49fef93dc0f65e3b866cb36",
    "runtime_profile_id": "846f98eada58e09d40f5a2ed1311abe85e55b8e461c93627e8d0b5c42ca51231",
    "broad_declaration_sha256": "b2eac24c19f13fb321678b8e090669e36ad443ec8b73e8f22e63ca62800998c5",
    "exact_v1_declaration_sha256": "1e89c29eadd671ad8884c9da155ba73e05ac3358b6993869e6e046338fa49cf1",
    "surface_sha256": "012b483ffcd575bdeaa38b8652823c84b5013d7ef25729c49051cc7a204297d6",
    "xfl_profile_ledger_sha256": "cfcb68fe9a195f6e70c88a1b8f2d2936838b8c98b3d70cbe2cab9a53e056fd80",
    "api_artifact_manifest_sha256": "bea5f03efea0ab8eb9e7886710fbf8aaf173ab620f233ec50e83ae5f3d2ed42c",
    "bytecode_abi": "75ea54f357d397c4b33899e495bb385dad975a43b1c4a7a2cead30474327d33e",
    "native_abi": "db5c633dda8c29c809649fdbc60e06c18814e769e659d850275b1f8cff7e87b9",
    "wasm_stack_bytes": "131072",
}

RECEIPT_B = {
    "provider_sha256": "40f9ac0203afa9296196c627bc94e669ad789322ddc1e4ab58ba0cf6d32a5e21",
    "provider_size": "1101461",
    "runtime_profile_id": "60950ca8b6fe4dd2a35559367051998bc04f75e8aa41934dd4caad0de5a1c3ba",
    "declaration_sha256": "56b4b2974b8a63a550721abd60350e392990762e76666f050b1a7c810e584957",
    "surface_sha256": "b112346b95da74d04930ba86bd597e56a428e41e11581804ccc49907ae206eb0",
}

TYPED_IMPORT_ROWS = (
    '{"env", "accept", "i32,i32,i64", "i64"}',
    '{"env", "emit", "i32,i32,i32,i32", "i64"}',
    '{"env", "etxn_details", "i32,i32", "i64"}',
    '{"env", "etxn_fee_base", "i32,i32", "i64"}',
    '{"env", "etxn_reserve", "i32", "i64"}',
    '{"env", "hook_account", "i32,i32", "i64"}',
    '{"env", "hook_again", "", "i64"}',
    '{"env", "hook_param", "i32,i32,i32,i32", "i64"}',
    '{"env", "ledger_last_hash", "i32,i32", "i64"}',
    '{"env", "ledger_last_time", "", "i64"}',
    '{"env", "ledger_nonce", "i32,i32", "i64"}',
    '{"env", "ledger_seq", "", "i64"}',
    '{"env", "otxn_param", "i32,i32,i32,i32", "i64"}',
    '{"env", "otxn_slot", "i32", "i64"}',
    '{"env", "otxn_type", "", "i64"}',
    '{"env", "prepare", "i32,i32,i32,i32", "i64"}',
    '{"env", "rollback", "i32,i32,i64", "i64"}',
    '{"env", "slot", "i32,i32,i32", "i64"}',
    '{"env", "slot_clear", "i32", "i64"}',
    '{"env", "slot_set", "i32,i32,i32", "i64"}',
    '{"env", "slot_size", "i32", "i64"}',
    '{"env", "state", "i32,i32,i32,i32", "i64"}',
    '{"env", "state_foreign", "i32,i32,i32,i32,i32,i32,i32,i32", "i64"}',
    '{"env", "state_set", "i32,i32,i32,i32", "i64"}',
    '{"env", "trace", "i32,i32,i32,i32,i32", "i64"}',
)

TYPED_EXPORT_ROWS = (
    '{"function", "_initialize", "", "", 0U, 0U, false, false}',
    '{"function", "free", "i32", "", 0U, 0U, false, false}',
    '{"function", "malloc", "i32", "i32", 0U, 0U, false, false}',
    '{"memory", "memory", "", "", 8U, 512U, false, false}',
    '{"function", "qjs_cbak", "i32,i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_compile", "i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_compile_module", "i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_destroy", "", "", 0U, 0U, false, false}',
    '{"function", "qjs_enable_coverage", "i32", "", 0U, 0U, false, false}',
    '{"function", "qjs_eval", "i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_eval_bytecode", "i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_eval_module", "i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_get_bytecode_len", "", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_get_bytecode_ptr", "", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_get_result_len", "", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_get_result_ptr", "", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_hook", "i32,i32,i32", "i32", 0U, 0U, false, false}',
    '{"function", "qjs_init", "", "", 0U, 0U, false, false}',
    '{"function", "qjs_set_max_stack_size", "i32", "", 0U, 0U, false, false}',
    '{"function", "qjs_set_memory_limit", "i32", "", 0U, 0U, false, false}',
    '{"function", "qjs_set_seed", "i32", "", 0U, 0U, false, false}',
    '{"function", "qjs_validate_hook_module", "i32,i32", "i32", 0U, 0U, false, false}',
)


def json_path(bundle: Path) -> Path:
    return bundle / "jshookz_provider.manifest.json"


def cmake_path(bundle: Path) -> Path:
    return bundle / "jshookz_provider.manifest.cmake"


def native_path(bundle: Path) -> Path:
    return bundle / "jshookz_provider.native-abi.json"


def wasm_path(bundle: Path) -> Path:
    return bundle / "jshookz_provider.wasm"


def api_manifest_path(bundle: Path) -> Path:
    return bundle / "api-artifacts.json"


def broad_declaration_path(bundle: Path) -> Path:
    return bundle / "hooks-api.d.ts"


def exact_v1_declaration_path(bundle: Path) -> Path:
    return bundle / "xahau-quickjs-v1.d.ts"


def surface_path(bundle: Path) -> Path:
    return bundle / "xahau-quickjs-v1.surface.json"


def xfl_profile_ledger_path(bundle: Path) -> Path:
    return bundle / "xfl-profile-ledger.ts"


def load_json(bundle: Path) -> dict:
    return json.loads(json_path(bundle).read_text())


def write_json(bundle: Path, data: dict) -> None:
    json_path(bundle).write_text(json.dumps(data, indent=2) + "\n")


def set_cmake(bundle: Path, key: str, value: str) -> None:
    path = cmake_path(bundle)
    lines = []
    replaced = False
    for line in path.read_text().splitlines():
        match = CMAKE_SET.match(line)
        if match and match.group(1) == key:
            lines.append(f'set(XAHAU_QUICKJS_{key} "{value}")')
            replaced = True
        else:
            lines.append(line)
    if not replaced:
        raise AssertionError(f"CMake lock is missing {key}")
    path.write_text("\n".join(lines) + "\n")


def rehash_manifest(bundle: Path) -> None:
    digest = hashlib.sha256(json_path(bundle).read_bytes()).hexdigest()
    set_cmake(bundle, "MANIFEST_SHA256", digest)


def rehash_native(bundle: Path) -> None:
    digest = hashlib.sha256(native_path(bundle).read_bytes()).hexdigest()
    set_cmake(bundle, "NATIVE_ABI_SHA256", digest)


def load_api_manifest(bundle: Path) -> dict:
    return json.loads(api_manifest_path(bundle).read_text())


def write_api_manifest(bundle: Path, data: dict) -> None:
    api_manifest_path(bundle).write_text(json.dumps(data, indent=2) + "\n")


def clone_bundle(root: Path) -> Path:
    dest = root / "bundle"
    shutil.copytree(BUNDLE, dest, ignore=shutil.ignore_patterns("README.md"))
    return dest


def generate(
    bundle: Path, work: Path, wasmtime_version: str = WASM_VERSION
) -> tuple[int, str, str]:
    output = work / "QuickJSProviderValues.cpp"
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--bundle-dir",
            str(bundle),
            "--wasmtime-version",
            wasmtime_version,
            "--output",
            str(output),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    cpp = output.read_text() if output.is_file() else ""
    return completed.returncode, completed.stderr, cpp


def hex_bytes(value: str) -> str:
    return ", ".join(f"0x{value[i : i + 2]}" for i in range(0, 64, 2))


def pin_holds(returncode: int, cpp: str) -> bool:
    if returncode != 0 or not cpp:
        return False
    required = [
        hex_bytes(PIN["provider_sha256"]),
        f"std::size_t const providerSize = {PIN['provider_size']};",
        f'"{PIN["manifest_sha256"]}"',
        hex_bytes(PIN["runtime_profile_id"]),
        f'"{PIN["broad_declaration_sha256"]}"',
        f'"{PIN["exact_v1_declaration_sha256"]}"',
        f'"{PIN["surface_sha256"]}"',
        f'"{PIN["xfl_profile_ledger_sha256"]}"',
        f'"{PIN["api_artifact_manifest_sha256"]}"',
        hex_bytes(PIN["bytecode_abi"]),
        f'"{PIN["native_abi"]}"',
        f"std::uint32_t const wasmStackBytes = {PIN['wasm_stack_bytes']}U;",
        "std::uint32_t const serializedObjectMaxBytes =\n    1048576U;",
        "std::uint32_t const serializedObjectMaxFields =\n    32768U;",
        "std::uint32_t const serializedObjectMaxScopes =\n    32769U;",
        "std::uint32_t const serializedObjectMaxDepth =\n    10U;",
        "std::uint32_t const providerMemoryMinimumPages = 8U;",
        "std::uint32_t const providerMemoryMaximumPages = 512U;",
        "std::uint64_t const hostWorkBudget = 2097152ULL;",
        "constexpr char const sealedProvider[] =",
        f"sizeof(sealedProvider) - 1 == {PIN['provider_size']}",
        "std::uint8_t const xqjsEnvelopeVersion = 1U;",
        "std::uint16_t const xflArithmeticProfileNone =\n    0U;",
        "std::uint16_t const xflArithmeticProfileXahauFloatV1 =\n    1U;",
        "std::uint16_t const xflArithmeticProfileNearestEvenV1 =\n    2U;",
        "std::uint32_t const moduleValidationLayoutVersion =\n    1U;",
        "std::int32_t const moduleValidationFailureSentinel =\n    -1;",
        "std::uint32_t const moduleValidationMainBit =\n    1U;",
        "std::uint32_t const moduleValidationCallbackBit =\n    2U;",
        "std::uint32_t const moduleValidationEntryMask =\n    3U;",
        "std::uint32_t const moduleValidationReservedMask =\n    2147483900U;",
        "std::uint32_t const moduleValidationProfileMask =\n    16776960U;",
        "std::uint32_t const moduleValidationProfileShift =\n    8U;",
        "std::uint32_t const moduleValidationVersionMask =\n    2130706432U;",
        "std::uint32_t const moduleValidationVersionShift =\n    24U;",
    ]
    if any(item not in cpp for item in required):
        return False
    return all(row in cpp for row in (*TYPED_IMPORT_ROWS, *TYPED_EXPORT_ROWS))


class GenerateQuickJSProviderBundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        missing = [
            path.name
            for path in (
                json_path(BUNDLE),
                cmake_path(BUNDLE),
                native_path(BUNDLE),
                wasm_path(BUNDLE),
                api_manifest_path(BUNDLE),
                broad_declaration_path(BUNDLE),
                exact_v1_declaration_path(BUNDLE),
                surface_path(BUNDLE),
                xfl_profile_ledger_path(BUNDLE),
                SCRIPT,
            )
            if not path.is_file()
        ]
        if missing:
            raise AssertionError(
                "sealed QuickJS bundle is incomplete: " + ", ".join(missing)
            )

    def mutate(
        self, mutator, wasmtime_version: str = WASM_VERSION
    ) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            bundle = clone_bundle(work)
            mutator(bundle)
            return generate(bundle, work / "out", wasmtime_version)

    def assert_generator_red(
        self, mutator, fragment: str, wasmtime_version: str = WASM_VERSION
    ) -> None:
        returncode, stderr, cpp = self.mutate(mutator, wasmtime_version)
        self.assertNotEqual(returncode, 0, stderr)
        self.assertIn(fragment, stderr)
        self.assertFalse(pin_holds(returncode, cpp), stderr)

    def test_unmutated_lock_projects_pin_and_typed_exports(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            returncode, stderr, cpp = generate(clone_bundle(work), work / "out")
        self.assertEqual(returncode, 0, stderr)
        self.assertTrue(pin_holds(returncode, cpp), stderr)
        self.assertEqual(cpp.count("ProviderExportSignature const"), 2)

    def test_provider_bytes_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            path = wasm_path(bundle)
            data = bytearray(path.read_bytes())
            data[-1] ^= 0xFF
            path.write_bytes(data)

        self.assert_generator_red(mutate, "WASM does not match")

    def test_provider_size_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "PROVIDER_SIZE", str(int(PIN["provider_size"]) - 1))

        self.assert_generator_red(mutate, "provider size disagrees")

    def test_coordinated_provider_size_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            value = int(PIN["provider_size"]) - 1
            data = load_json(bundle)
            data["provider"]["size"] = value
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "PROVIDER_SIZE", str(value))

        self.assert_generator_red(mutate, "provider size disagrees with the sealed F0")

    def test_provider_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(
                bundle,
                "PROVIDER_SHA256",
                "00" + PIN["provider_sha256"][2:],
            )

        self.assert_generator_red(mutate, "provider sha256 disagrees")

    def test_coordinated_provider_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            value = "00" + PIN["provider_sha256"][2:]
            data = load_json(bundle)
            data["provider"]["sha256"] = value
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "PROVIDER_SHA256", value)

        self.assert_generator_red(
            mutate, "provider SHA-256 disagrees with the sealed F0"
        )

    def test_manifest_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(
                bundle,
                "MANIFEST_SHA256",
                "00" + PIN["manifest_sha256"][2:],
            )

        self.assert_generator_red(mutate, "JSON manifest does not match")

    def test_coordinated_manifest_identity_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["diagnostic_note"] = "not F0"
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "provider manifest SHA-256")

    def test_runtime_profile_id_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(
                bundle,
                "RUNTIME_PROFILE_ID",
                "00" + PIN["runtime_profile_id"][2:],
            )

        self.assert_generator_red(mutate, "runtime-profile ID disagrees")

    def test_coordinated_runtime_profile_id_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            value = "00" + PIN["runtime_profile_id"][2:]
            data = load_json(bundle)
            data["runtime_profile_id"] = value
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "RUNTIME_PROFILE_ID", value)

        self.assert_generator_red(
            mutate, "runtime-profile ID disagrees with the sealed F0"
        )

    def _coordinated_api_artifact_mutation(
        self, source_path: str, local_path, fragment: str
    ) -> None:
        def mutate(bundle: Path) -> None:
            path = local_path(bundle)
            path.write_bytes(path.read_bytes() + b"\n")
            manifest = load_api_manifest(bundle)
            manifest["artifacts"][source_path] = hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
            write_api_manifest(bundle, manifest)

        self.assert_generator_red(mutate, fragment)

    def test_broad_declaration_identity_mutation(self) -> None:
        self._coordinated_api_artifact_mutation(
            "python/jshookz/src/jshookz/types/hooks-api.d.ts",
            broad_declaration_path,
            "hooks-api.d.ts disagrees with the sealed F0 table",
        )

    def test_exact_v1_declaration_identity_mutation(self) -> None:
        self._coordinated_api_artifact_mutation(
            "python/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts",
            exact_v1_declaration_path,
            "xahau-quickjs-v1.d.ts disagrees with the sealed F0 table",
        )

    def test_selected_surface_identity_mutation(self) -> None:
        self._coordinated_api_artifact_mutation(
            "python/jshookz/src/jshookz/types/xahau-quickjs-v1.surface.json",
            surface_path,
            "xahau-quickjs-v1.surface.json disagrees with the sealed F0 table",
        )

    def test_xfl_profile_ledger_identity_mutation(self) -> None:
        self._coordinated_api_artifact_mutation(
            "python/jshookz/src/jshookz/xfl_profile_ledger.ts",
            xfl_profile_ledger_path,
            "xfl_profile_ledger.ts disagrees with the sealed F0 table",
        )

    def test_api_artifact_manifest_identity_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_api_manifest(bundle)
            api_manifest_path(bundle).write_text(json.dumps(data, indent=4) + "\n")

        self.assert_generator_red(mutate, "API artifact manifest SHA-256")

    def test_declaration_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["javascript_surface"]["declaration_sha256"] = (
                "00" + PIN["exact_v1_declaration_sha256"][2:]
            )
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "declaration SHA-256 disagrees")

    def test_surface_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["javascript_surface"]["sha256"] = "00" + PIN["surface_sha256"][2:]
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "surface SHA-256 disagrees")

    def test_bytecode_abi_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "BYTECODE_ABI_ID", "00" + PIN["bytecode_abi"][2:])

        self.assert_generator_red(mutate, "bytecode ABI disagrees")

    def test_coordinated_bytecode_abi_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            value = "00" + PIN["bytecode_abi"][2:]
            data = load_json(bundle)
            data["bytecode_abi_id"] = value
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "BYTECODE_ABI_ID", value)

        self.assert_generator_red(mutate, "bytecode ABI disagrees with the sealed F0")

    def test_native_abi_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "NATIVE_ABI_SHA256", "00" + PIN["native_abi"][2:])

        self.assert_generator_red(mutate, "native ABI snapshot does not match")

    def test_coordinated_native_abi_identity_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            path = native_path(bundle)
            path.write_bytes(path.read_bytes() + b"\n")
            rehash_native(bundle)

        self.assert_generator_red(mutate, "native ABI SHA-256")

    def test_import_signature_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["provider"]["imports"][0]["params"].append("i32")
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "import signatures disagree")

    def test_export_signature_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for item in data["provider"]["exports"]:
                if item["name"] == "qjs_hook":
                    item["params"].append("i32")
                    break
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "export signatures disagree")

    def test_coordinated_export_signature_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for collection in (
                data["provider"]["exports"],
                data["source"]["provider"]["allowed_exports"],
            ):
                for item in collection:
                    if item["name"] == "qjs_hook":
                        item["params"].append("i32")
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "sealed F0 table")

    def test_coordinated_wasi_import_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for collection in (
                data["provider"]["imports"],
                data["source"]["provider"]["imports"],
            ):
                for item in collection:
                    if item["name"] == "accept":
                        item["module"] = "wasi_snapshot_preview1"
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "sealed F0 table")

    def test_coordinated_import_signature_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for collection in (
                data["provider"]["imports"],
                data["source"]["provider"]["imports"],
            ):
                for item in collection:
                    if item["name"] == "accept":
                        item["params"].append("i32")
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "sealed F0 table")

    def test_export_extra_key_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for collection in (
                data["provider"]["exports"],
                data["source"]["provider"]["allowed_exports"],
            ):
                for item in collection:
                    if item["name"] == "qjs_init":
                        item["note"] = "extra"
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "extra or missing keys")

    def _memory_shape_mutation(self, key: str, value) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for collection in (
                data["provider"]["exports"],
                data["source"]["provider"]["allowed_exports"],
            ):
                for item in collection:
                    if item["kind"] == "memory":
                        item[key] = value
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "memory")

    def test_memory_minimum_shape_mutation(self) -> None:
        self._memory_shape_mutation("minimum_pages", 6)

    def test_memory_maximum_shape_mutation(self) -> None:
        self._memory_shape_mutation("maximum_pages", 511)

    def test_memory64_shape_mutation(self) -> None:
        self._memory_shape_mutation("memory64", True)

    def test_shared_memory_shape_mutation(self) -> None:
        self._memory_shape_mutation("shared", True)

    def test_serialized_object_max_bytes_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "SERIALIZED_OBJECT_MAX_BYTES", "1048577")

        self.assert_generator_red(mutate, "serialized_object_max_bytes")

    def test_serialized_object_max_fields_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "SERIALIZED_OBJECT_MAX_FIELDS", "32769")

        self.assert_generator_red(mutate, "serialized_object_max_fields")

    def test_serialized_object_max_scopes_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "SERIALIZED_OBJECT_MAX_SCOPES", "32770")

        self.assert_generator_red(mutate, "serialized_object_max_scopes")

    def test_serialized_object_max_depth_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "SERIALIZED_OBJECT_MAX_DEPTH", "11")

        self.assert_generator_red(mutate, "serialized_object_max_depth")

    def _coordinated_limit(self, json_key: str, cmake_key: str, value: int) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["limits"][json_key] = value
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, cmake_key, str(value))

        self.assert_generator_red(mutate, "sealed F0 table")

    def test_coordinated_serialized_object_max_bytes_mutation(self) -> None:
        self._coordinated_limit(
            "serialized_object_max_bytes", "SERIALIZED_OBJECT_MAX_BYTES", 1048578
        )

    def test_coordinated_serialized_object_max_fields_mutation(self) -> None:
        self._coordinated_limit(
            "serialized_object_max_fields", "SERIALIZED_OBJECT_MAX_FIELDS", 1
        )

    def test_coordinated_serialized_object_max_scopes_mutation(self) -> None:
        self._coordinated_limit(
            "serialized_object_max_scopes", "SERIALIZED_OBJECT_MAX_SCOPES", 1
        )

    def test_coordinated_serialized_object_max_depth_mutation(self) -> None:
        self._coordinated_limit(
            "serialized_object_max_depth", "SERIALIZED_OBJECT_MAX_DEPTH", 1
        )

    def test_coordinated_heap_limit_mutation(self) -> None:
        self._coordinated_limit("quickjs_heap_bytes", "HEAP_BYTES", 16777215)

    def test_coordinated_stack_limit_mutation(self) -> None:
        self._coordinated_limit("quickjs_stack_bytes", "STACK_BYTES", 65535)

    def test_coordinated_initialization_fuel_mutation(self) -> None:
        self._coordinated_limit(
            "wasmtime_fuel_per_initialization", "INITIALIZATION_FUEL", 4999999
        )

    def test_coordinated_invocation_fuel_mutation(self) -> None:
        self._coordinated_limit(
            "wasmtime_fuel_per_invocation", "INVOCATION_FUEL", 49999999
        )

    def test_coordinated_host_work_budget_mutation(self) -> None:
        self._coordinated_limit("host_work_budget", "HOST_WORK_BUDGET", 2097151)

    def test_coordinated_host_work_base_mutation(self) -> None:
        self._coordinated_limit("host_work_base_per_call", "HOST_WORK_BASE_PER_CALL", 2)

    def test_coordinated_host_work_per_byte_mutation(self) -> None:
        self._coordinated_limit(
            "host_work_per_addressed_byte", "HOST_WORK_PER_ADDRESSED_BYTE", 2
        )

    def test_coordinated_host_work_meter_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["limits"]["host_work_meter"] = "other-meter"
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "HOST_WORK_METER", "other-meter")

        self.assert_generator_red(mutate, "host_work_meter")

    def test_host_work_address_mapping_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["limits"]["host_work_addressed_length_indices"]["accept"] = [
                0
            ]
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "host_work_addressed_length_indices")

    def test_slot_host_work_address_mapping_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["limits"]["host_work_addressed_length_indices"]["slot"] = [0]
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "host_work_addressed_length_indices")

    def test_coordinated_host_adapter_policy_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["execution"]["host_adapter_policy"] = "other-policy"
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "HOST_ADAPTER_POLICY", "other-policy")

        self.assert_generator_red(mutate, "host_adapter_policy")

    def test_coordinated_hook_api_version_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["artifact"]["hook_api_version"] = 2
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "HOOK_API_VERSION", "2")

        self.assert_generator_red(mutate, "hook_api_version")

    def test_artifact_envelope_version_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["artifact"]["envelope_version"] = 2
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "artifact activation contract")

    def test_artifact_profile_code_mutations(self) -> None:
        mutations = {
            "none": 7,
            "xahauFloatV1": 7,
            "nearestEvenV1": 7,
        }
        for name, value in mutations.items():
            with self.subTest(name=name):

                def mutate(bundle: Path, name=name, value=value) -> None:
                    data = load_json(bundle)
                    data["source"]["artifact"]["xfl_arithmetic_profile_codes"][name] = (
                        value
                    )
                    write_json(bundle, data)
                    rehash_manifest(bundle)

                self.assert_generator_red(mutate, "artifact activation contract")

    def test_artifact_profile_table_schema_mutations(self) -> None:
        for operation in ("missing", "extra"):
            with self.subTest(operation=operation):

                def mutate(bundle: Path, operation=operation) -> None:
                    data = load_json(bundle)
                    table = data["source"]["artifact"]["xfl_arithmetic_profile_codes"]
                    if operation == "missing":
                        del table["nearestEvenV1"]
                    else:
                        table["future"] = 3
                    write_json(bundle, data)
                    rehash_manifest(bundle)

                self.assert_generator_red(mutate, "artifact activation contract")

    def test_artifact_profile_implementation_mutations(self) -> None:
        mutations = {
            "none": ["XFLDecimal.add"],
            "nearestEvenV1": ["XFLDecimal.add"],
            "xahauFloatV1 missing add": [
                "XFLDecimal.divide",
                "XFLDecimal.multiply",
                "XFLDecimal.subtract",
            ],
            "xahauFloatV1 missing divide": [
                "XFLDecimal.add",
                "XFLDecimal.multiply",
                "XFLDecimal.subtract",
            ],
            "xahauFloatV1 missing multiply": [
                "XFLDecimal.add",
                "XFLDecimal.divide",
                "XFLDecimal.subtract",
            ],
            "xahauFloatV1 missing subtract": [
                "XFLDecimal.add",
                "XFLDecimal.divide",
                "XFLDecimal.multiply",
            ],
            "xahauFloatV1 reordered": [
                "XFLDecimal.add",
                "XFLDecimal.multiply",
                "XFLDecimal.divide",
                "XFLDecimal.subtract",
            ],
            "xahauFloatV1 extra": [
                "XFLDecimal.add",
                "XFLDecimal.divide",
                "XFLDecimal.invert",
                "XFLDecimal.multiply",
                "XFLDecimal.subtract",
            ],
        }
        for name, value in mutations.items():
            with self.subTest(name=name):

                def mutate(bundle: Path, name=name, value=value) -> None:
                    data = load_json(bundle)
                    profile = name.split(" ", 1)[0]
                    data["source"]["artifact"][
                        "xfl_arithmetic_profile_implementations"
                    ][profile] = value
                    write_json(bundle, data)
                    rehash_manifest(bundle)

                self.assert_generator_red(mutate, "artifact activation contract")

    def test_artifact_profile_implementation_schema_mutations(self) -> None:
        for operation in ("missing", "extra"):
            with self.subTest(operation=operation):

                def mutate(bundle: Path, operation=operation) -> None:
                    data = load_json(bundle)
                    table = data["source"]["artifact"][
                        "xfl_arithmetic_profile_implementations"
                    ]
                    if operation == "missing":
                        del table["nearestEvenV1"]
                    else:
                        table["future"] = []
                    write_json(bundle, data)
                    rehash_manifest(bundle)

                self.assert_generator_red(mutate, "artifact activation contract")

    def test_module_validation_layout_mutations(self) -> None:
        mutations = {
            "layout_version": 2,
            "failure_sentinel": -2,
            "main_bit": 4,
            "callback_bit": 4,
            "entry_mask": 7,
            "reserved_mask": 0,
            "profile_mask": 0x0000FF00,
            "profile_shift": 9,
            "version_mask": 0xFF000000,
            "version_shift": 23,
        }
        for name, value in mutations.items():
            with self.subTest(name=name):

                def mutate(bundle: Path, name=name, value=value) -> None:
                    data = load_json(bundle)
                    data["source"]["provider"]["module_validation_result"][name] = value
                    write_json(bundle, data)
                    rehash_manifest(bundle)

                self.assert_generator_red(mutate, "module-validation result layout")

    def test_module_validation_layout_schema_mutations(self) -> None:
        for operation in ("missing", "extra"):
            with self.subTest(operation=operation):

                def mutate(bundle: Path, operation=operation) -> None:
                    data = load_json(bundle)
                    layout = data["source"]["provider"]["module_validation_result"]
                    if operation == "missing":
                        del layout["version_shift"]
                    else:
                        layout["future"] = 1
                    write_json(bundle, data)
                    rehash_manifest(bundle)

                self.assert_generator_red(mutate, "module-validation result layout")

    def test_wasmtime_engine_configuration_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["engine"]["configuration"]["wasi"] = True
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "Wasmtime engine configuration")

    def test_coordinated_wasmtime_version_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["source"]["engine"]["version"] = "48.0.0"
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "WASMTIME_VERSION", "48.0.0")

        self.assert_generator_red(
            mutate, "Wasmtime version disagrees with the sealed F0", "48.0.0"
        )

    def test_provider_memory_max_bytes_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["provider"]["build"]["wasm_memory_max_bytes"] = 33554431
            data["source"]["provider"]["build"]["wasm_memory_max_bytes"] = 33554431
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "wasm_memory_max_bytes")

    def test_wasm_stack_bytes_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["provider"]["build"]["wasm_stack_bytes"] = 65536
            data["source"]["provider"]["build"]["wasm_stack_bytes"] = 65536
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "wasm_stack_bytes")

    def test_stale_receipt_b_lock_control(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["provider"]["sha256"] = RECEIPT_B["provider_sha256"]
            data["provider"]["size"] = int(RECEIPT_B["provider_size"])
            data["runtime_profile_id"] = RECEIPT_B["runtime_profile_id"]
            data["javascript_surface"]["declaration_sha256"] = RECEIPT_B[
                "declaration_sha256"
            ]
            data["javascript_surface"]["sha256"] = RECEIPT_B["surface_sha256"]
            for collection in (
                data["provider"]["exports"],
                data["source"]["provider"]["allowed_exports"],
            ):
                for item in collection:
                    if item["kind"] == "memory":
                        item["minimum_pages"] = 6
            write_json(bundle, data)
            rehash_manifest(bundle)
            set_cmake(bundle, "PROVIDER_SHA256", RECEIPT_B["provider_sha256"])
            set_cmake(bundle, "PROVIDER_SIZE", RECEIPT_B["provider_size"])
            set_cmake(bundle, "RUNTIME_PROFILE_ID", RECEIPT_B["runtime_profile_id"])

        self.assert_generator_red(
            mutate, "provider SHA-256 disagrees with the sealed F0"
        )


if __name__ == "__main__":
    unittest.main()
