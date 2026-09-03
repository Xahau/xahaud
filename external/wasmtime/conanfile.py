import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get
from conan.tools.microsoft import is_msvc


required_conan_version = ">=2.0"


class WasmtimeConan(ConanFile):
    name = "wasmtime"
    description = "Standalone WebAssembly runtime using Cranelift"
    license = "Apache-2.0 WITH LLVM-exception"
    url = "https://github.com/Xahau/xahaud"
    homepage = "https://github.com/bytecodealliance/wasmtime"
    topics = ("webassembly", "wasm")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False]}
    default_options = {"shared": False}
    no_copy_source = True

    @property
    def _sources_os_key(self):
        if is_msvc(self):
            return "Windows"
        if self.settings.os == "Windows" and self.settings.compiler == "gcc":
            return "MinGW"
        return str(self.settings.os)

    def configure(self):
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def package_id(self):
        # Official release archives contain a prebuilt C ABI. Compiler patch
        # versions do not change the packaged bytes.
        if self.info.settings.compiler == "clang":
            self.info.settings.compiler = "gcc"
        del self.info.settings.compiler.version

    def validate(self):
        try:
            self.conan_data["sources"][self.version][self._sources_os_key][
                str(self.settings.arch)
            ]
        except KeyError as error:
            raise ConanInvalidConfiguration(
                "No official Wasmtime C-API archive for "
                f"{self.settings.os}/{self.settings.arch}"
            ) from error

    def build(self):
        get(
            self,
            **self.conan_data["sources"][self.version][self._sources_os_key][
                str(self.settings.arch)
            ],
            destination=self.build_folder,
            strip_root=True,
        )

    def package(self):
        copy(
            self,
            "*.h",
            dst=os.path.join(self.package_folder, "include"),
            src=os.path.join(self.build_folder, "include"),
        )
        copy(
            self,
            "*.hh",
            dst=os.path.join(self.package_folder, "include"),
            src=os.path.join(self.build_folder, "include"),
        )

        source_lib = os.path.join(self.build_folder, "lib")
        package_lib = os.path.join(self.package_folder, "lib")
        package_bin = os.path.join(self.package_folder, "bin")
        if self.options.shared:
            copy(
                self,
                "wasmtime.dll.lib",
                dst=package_lib,
                src=source_lib,
                keep_path=False,
            )
            copy(
                self,
                "wasmtime.dll",
                dst=package_bin,
                src=source_lib,
                keep_path=False,
            )
            copy(
                self,
                "libwasmtime.dll.a",
                dst=package_lib,
                src=source_lib,
                keep_path=False,
            )
            copy(
                self,
                "libwasmtime.so*",
                dst=package_lib,
                src=source_lib,
                keep_path=False,
            )
            copy(
                self,
                "libwasmtime.dylib",
                dst=package_lib,
                src=source_lib,
                keep_path=False,
            )
        else:
            copy(
                self,
                "wasmtime.lib",
                dst=package_lib,
                src=source_lib,
                keep_path=False,
            )
            copy(
                self,
                "libwasmtime.a",
                dst=package_lib,
                src=source_lib,
                keep_path=False,
            )

        copy(
            self,
            "LICENSE",
            dst=os.path.join(self.package_folder, "licenses"),
            src=self.build_folder,
        )

    def package_info(self):
        if self.options.shared:
            self.cpp_info.libs = [
                "wasmtime.dll" if self.settings.os == "Windows" else "wasmtime"
            ]
        else:
            if self.settings.os == "Windows":
                self.cpp_info.defines = ["WASM_API_EXTERN=", "WASI_API_EXTERN="]
            self.cpp_info.libs = ["wasmtime"]

        if self.settings.os == "Windows":
            self.cpp_info.system_libs = [
                "ws2_32",
                "bcrypt",
                "advapi32",
                "userenv",
                "ntdll",
                "shell32",
                "ole32",
            ]
        elif self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread", "dl", "m", "rt"]
