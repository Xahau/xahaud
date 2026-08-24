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
    "provider_sha256": "40f9ac0203afa9296196c627bc94e669ad789322ddc1e4ab58ba0cf6d32a5e21",
    "provider_size": "1101461",
    "manifest_sha256": "dce4535b8c9063ee4244c8ac7781d8d17f73fa5443c9724476c4bdb0a5a77f4b",
    "runtime_profile_id": "60950ca8b6fe4dd2a35559367051998bc04f75e8aa41934dd4caad0de5a1c3ba",
    "declaration_sha256": "56b4b2974b8a63a550721abd60350e392990762e76666f050b1a7c810e584957",
    "surface_sha256": "b112346b95da74d04930ba86bd597e56a428e41e11581804ccc49907ae206eb0",
    "bytecode_abi": "75ea54f357d397c4b33899e495bb385dad975a43b1c4a7a2cead30474327d33e",
    "native_abi": "328ec938dcad875f3bdf25b8d779dd77f3571589818ca9271cb98c64c7018f99",
    "wasm_stack_bytes": "131072",
}

TYPED_EXPORT_ROWS = (
    '{"function", "_initialize", "", "", 0U, 0U, false, false}',
    '{"function", "free", "i32", "", 0U, 0U, false, false}',
    '{"function", "malloc", "i32", "i32", 0U, 0U, false, false}',
    '{"memory", "memory", "", "", 6U, 512U, false, false}',
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


def clone_bundle(root: Path) -> Path:
    dest = root / "bundle"
    shutil.copytree(BUNDLE, dest, ignore=shutil.ignore_patterns("README.md"))
    return dest


def generate(bundle: Path, work: Path) -> tuple[int, str, str]:
    output = work / "QuickJSProviderValues.cpp"
    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--bundle-dir",
            str(bundle),
            "--wasmtime-version",
            WASM_VERSION,
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
    return ", ".join(f"0x{value[i:i + 2]}" for i in range(0, 64, 2))


def pin_holds(returncode: int, cpp: str) -> bool:
    if returncode != 0 or not cpp:
        return False
    required = [
        hex_bytes(PIN["provider_sha256"]),
        f"std::size_t const providerSize = {PIN['provider_size']};",
        hex_bytes(PIN["runtime_profile_id"]),
        f'"{PIN["declaration_sha256"]}"',
        f'"{PIN["surface_sha256"]}"',
        hex_bytes(PIN["bytecode_abi"]),
        f'"{PIN["native_abi"]}"',
        f"std::uint32_t const wasmStackBytes = {PIN['wasm_stack_bytes']}U;",
        "std::uint32_t const serializedObjectMaxBytes =\n    1048576U;",
        "std::uint32_t const serializedObjectMaxFields =\n    32768U;",
        "std::uint32_t const serializedObjectMaxScopes =\n    32769U;",
        "std::uint32_t const serializedObjectMaxDepth =\n    10U;",
        "constexpr char const sealedProvider[] =",
    ]
    if any(item not in cpp for item in required):
        return False
    return all(row in cpp for row in TYPED_EXPORT_ROWS)


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
                SCRIPT,
            )
            if not path.is_file()
        ]
        if missing:
            raise AssertionError(
                "sealed QuickJS bundle is incomplete: " + ", ".join(missing)
            )

    def mutate(self, mutator) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            bundle = clone_bundle(work)
            mutator(bundle)
            return generate(bundle, work / "out")

    def assert_generator_red(self, mutator, fragment: str) -> None:
        returncode, stderr, cpp = self.mutate(mutator)
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
            set_cmake(bundle, "PROVIDER_SIZE", "1101460")

        self.assert_generator_red(mutate, "provider size disagrees")

    def test_provider_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(
                bundle,
                "PROVIDER_SHA256",
                "00" + PIN["provider_sha256"][2:],
            )

        self.assert_generator_red(mutate, "provider sha256 disagrees")

    def test_manifest_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(
                bundle,
                "MANIFEST_SHA256",
                "00" + PIN["manifest_sha256"][2:],
            )

        self.assert_generator_red(mutate, "JSON manifest does not match")

    def test_runtime_profile_id_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(
                bundle,
                "RUNTIME_PROFILE_ID",
                "00" + PIN["runtime_profile_id"][2:],
            )

        self.assert_generator_red(mutate, "runtime-profile ID disagrees")

    def test_declaration_sha_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["javascript_surface"]["declaration_sha256"] = (
                "00" + PIN["declaration_sha256"][2:]
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

    def test_native_abi_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            set_cmake(bundle, "NATIVE_ABI_SHA256", "00" + PIN["native_abi"][2:])

        self.assert_generator_red(mutate, "native ABI snapshot does not match")

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

        self.assert_generator_red(mutate, "sealed Receipt-A table")

    def test_coordinated_import_module_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for collection in (
                data["provider"]["imports"],
                data["source"]["provider"]["imports"],
            ):
                for item in collection:
                    if item["name"] == "accept":
                        item["module"] = "env2"
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "sealed Receipt-A table")

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

    def test_memory_shape_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            for item in data["provider"]["exports"]:
                if item["kind"] == "memory":
                    item["minimum_pages"] = 7
            for item in data["source"]["provider"]["allowed_exports"]:
                if item["kind"] == "memory":
                    item["minimum_pages"] = 7
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "memory shape")

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

        self.assert_generator_red(mutate, "sealed Receipt-A table")

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

    def test_wasm_stack_bytes_mutation(self) -> None:
        def mutate(bundle: Path) -> None:
            data = load_json(bundle)
            data["provider"]["build"]["wasm_stack_bytes"] = 65536
            data["source"]["provider"]["build"]["wasm_stack_bytes"] = 65536
            write_json(bundle, data)
            rehash_manifest(bundle)

        self.assert_generator_red(mutate, "wasm_stack_bytes")


if __name__ == "__main__":
    unittest.main()
