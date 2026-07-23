from conan import ConanFile
from conan.tools.files import get, copy
from conan.errors import ConanInvalidConfiguration

import os

required_conan_version = ">=1.53.0"

class WasmtimeConan(ConanFile):
    name = "wasmtime"
    description = ("Wasmtime is a fast and secure runtime for WebAssembly. "
                   "It is a standalone wasm-only optimizing runtime for WebAssembly and WASI.")
    license = "Apache-2.0"
    url = "https://github.com/bytecodealliance/wasmtime"
    homepage = "https://github.com/bytecodealliance/wasmtime"
    topics = ("webassembly", "wasm", "wasi")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"

    @property
    def _compiler_alias(self):
        return {
            "Visual Studio": "Visual Studio",
            "msvc": "Visual Studio",
        }.get(str(self.info.settings.compiler), "gcc")

    def configure(self):
        self.settings.compiler.rm_safe("libcxx")
        self.settings.compiler.rm_safe("cppstd")

    def validate(self):
        try:
            self.conan_data["sources"][self.version][str(self.settings.os)][str(self.settings.arch)][self._compiler_alias]
        except KeyError:
            raise ConanInvalidConfiguration(
                f"Binaries for this combination of version/os/arch/compiler are not available "
                f"({self.version}/{self.settings.os}/{self.settings.arch}/{self._compiler_alias})"
            )
        entry = self.conan_data["sources"][self.version][str(self.settings.os)][str(self.settings.arch)][self._compiler_alias]
        if not entry.get("sha256"):
            raise ConanInvalidConfiguration(
                f"SHA256 not configured for {self.version}/{self.settings.os}/{self.settings.arch}/{self._compiler_alias}"
            )

    def package_id(self):
        # Make binary compatible across compiler versions (since we're downloading prebuilt)
        self.info.settings.rm_safe("compiler.version")
        # Group compilers by their binary compatibility
        compiler_name = str(self.info.settings.compiler)
        if compiler_name in ["Visual Studio", "msvc"]:
            self.info.settings.compiler = "Visual Studio"
        else:
            self.info.settings.compiler = "gcc"

    def build(self):
        # Download the prebuilt tarball
        entry = self.conan_data["sources"][self.version][str(self.settings.os)][str(self.settings.arch)][self._compiler_alias]
        get(self, url=entry["url"], sha256=entry["sha256"],
            destination=self.source_folder, strip_root=True)

    def package(self):
        copy(self, pattern="*.h", dst=os.path.join(self.package_folder, "include"),
             src=os.path.join(self.source_folder, "include"), keep_path=True)

        srclibdir = os.path.join(self.source_folder, "lib")
        dstlibdir = os.path.join(self.package_folder, "lib")

        if self.settings.os == "Windows":
            copy(self, pattern="wasmtime.lib", src=srclibdir, dst=dstlibdir, keep_path=False)
        else:
            copy(self, pattern="libwasmtime.a", src=srclibdir, dst=dstlibdir, keep_path=False)

        copy(self, pattern="LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"), keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["wasmtime"]

        if self.settings.os == "Windows":
            self.cpp_info.defines += ["WASM_API_EXTERN=", "WASI_API_EXTERN="]
            self.cpp_info.system_libs += ["ws2_32", "bcrypt", "advapi32", "userenv", "ntdll", "shell32", "ole32"]

        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs += ["pthread", "dl", "m"]
