#!/usr/bin/env python3
"""Project sealed QuickJS provider/native ABI JSON into C++ constants."""

import argparse
import hashlib
import json
from pathlib import Path


def quote(value: str) -> str:
    if not isinstance(value, str) or any(c in value for c in "\n\r"):
        raise ValueError(f"invalid QuickJS policy string: {value!r}")
    return json.dumps(value)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--native-abi", type=Path, required=True)
    parser.add_argument("--expected-count", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    profile = json.loads(args.profile.read_text())
    native = json.loads(args.native_abi.read_text())
    provider_imports = profile["provider"]["imports"]
    native_imports = native["selected"]
    provider_names = [item["name"] for item in provider_imports]
    native_names = [item["name"] for item in native_imports]
    if (
        len(provider_imports) != args.expected_count
        or len(native_imports) != args.expected_count
        or len(set(provider_names)) != args.expected_count
        or len(set(native_names)) != args.expected_count
        or set(provider_names) != set(native_names)
    ):
        raise ValueError("QuickJS provider/native ABI import sets differ")

    provider_rows = ",\n    ".join(
        "{" + ", ".join(
            (
                quote(item["module"]),
                quote(item["name"]),
                quote(",".join(item["params"])),
                quote(",".join(item["results"])),
            )
        ) + "}"
        for item in provider_imports
    )
    native_rows = ",\n    ".join(
        "{" + ", ".join(
            (
                quote(item["name"]),
                quote(item["return_type"]),
                quote(",".join(item["param_types"])),
                quote(item["amendment"] or ""),
            )
        ) + "}"
        for item in native_imports
    )
    source = native["source"]
    native_sha = hashlib.sha256(args.native_abi.read_bytes()).hexdigest()
    args.output.write_text(
        "// Generated from digest-checked provider/native ABI JSON.\n"
        f"inline constexpr std::array<std::string_view, {args.expected_count}>\n"
        "    providerImports = {\n    "
        + ", ".join(quote(name) for name in provider_names)
        + "};\n"
        f"inline constexpr std::array<ProviderImportSignature, {args.expected_count}>\n"
        "    providerImportSignatures = {{\n    "
        + provider_rows
        + "}};\n"
        f"inline constexpr std::array<NativeImportSignature, {args.expected_count}>\n"
        "    nativeImportSignatures = {{\n    "
        + native_rows
        + "}};\n"
        "inline constexpr std::string_view nativeABISourceRepository = "
        + quote(source["repository"])
        + ";\n"
        "inline constexpr std::string_view nativeABISourceCommit = "
        + quote(source["commit"])
        + ";\n"
        "inline constexpr std::string_view nativeABISourcePath = "
        + quote(source["path"])
        + ";\n"
        "inline constexpr std::string_view nativeABISHA256 = "
        + quote(native_sha)
        + ";\n"
        "inline constexpr std::size_t nativeABICatalogueCount = "
        + str(source["macro_function_count"])
        + ";\n"
    )


if __name__ == "__main__":
    main()
