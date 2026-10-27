{
  description = "CI toolchains for xahaud, keyed by the compiler_id of xahau-ga-nix.yml";

  # unstable, pinned by flake.lock: nixos-25.11 has no cached conan on Linux
  # (its hydra build fails), unstable has.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      # No x86_64-darwin: nixpkgs unstable dropped it, and the macOS runners are arm64.
      forEachSystem = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];

      envs = forEachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};

          # The recipes conanfile.py needs under @xahaud/stable, exported into
          # the active CONAN_HOME. Idempotent; run on dev-shell entry and by
          # xahau-ga-dependencies after it has pointed Conan at its cache.
          conanInit = pkgs.writeShellScriptBin "xahaud-conan-init" ''
            set -e
            conan profile detect --exist-ok -verror
            conan export ${./external/snappy} --version 1.1.10 --user xahaud --channel stable -verror
            conan export ${./external/soci} --version 4.0.3 --user xahaud --channel stable -verror
            conan export ${./external/wasmedge} --version 0.11.2 --user xahaud --channel stable -verror
          '';

          # The Conan wasmedge recipe ships a prebuilt libwasmedge.so.0 that
          # needs the system libz.so.1; the Nix glibc loader never looks in
          # /lib. Exposed under its own name so LD_LIBRARY_PATH carries zlib
          # only and shadows nothing else. Linux only: macOS links Apple's.
          runtimeLibs = pkgs.linkFarm "xahaud-runtime-libs" [
            {
              name = "lib/xahaud-runtime";
              path = "${pkgs.zlib}/lib";
            }
          ];

          # Everything a CI job needs besides the compiler. Conan builds the
          # library dependencies, so this is only tooling.
          tools = with pkgs; [
            conanInit
            cacert # TLS trust store for the nix-built git/curl/conan
            ccache
            cmake
            conan
            coreutils # nproc on macOS
            curl # codecov-action
            git
            gnumake # openssl build
            gnupg # codecov-action verifies its CLI with gpg
            ninja
            perl # openssl build
            python3
          ]
          ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isLinux runtimeLibs;

          # Nix exposes compilers under unsuffixed names only. The workflow
          # matrix (cc/cxx) and Conan's boost recipe (g++-<major>) use suffixed
          # names, which would otherwise fall through to a system compiler.
          versioned =
            package: version: names:
            map (
              tool:
              pkgs.writeShellScriptBin "${tool}-${toString version}" ''
                exec "${package}/bin/${tool}" "$@"
              ''
            ) names;

          # A plain directory of binaries, put on PATH by
          # .github/actions/xahau-ga-setup-nix. Not a dev shell: the composite
          # actions run plain bash steps and would escape one.
          mkEnv =
            name: toolchain:
            pkgs.buildEnv {
              name = "xahaud-ci-env-${name}";
              paths = tools ++ toolchain;
              pathsToLink = [
                "/bin"
                "/etc/ssl/certs"
              ]
              ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isLinux "/lib/xahaud-runtime";
            };

          # Outside a Nix stdenv nothing puts libc++'s lib dir on the link line
          # (a stdenv setup hook would), so -lc++ fails without this.
          clang20 = pkgs.llvmPackages_20.libcxxClang.override (old: {
            extraBuildCommands = (old.extraBuildCommands or "") + ''
              echo "-L${pkgs.llvmPackages_20.libcxx}/lib" >> $out/nix-support/cc-ldflags
            '';
          });
          # clang on top of gcc 13's libstdc++, like the apt setup this replaced
          # (clang-16 there; nixpkgs ships clang >= 18).
          clang18 = pkgs.llvmPackages_18.libstdcxxClang.override { gccForLibs = pkgs.gcc13.cc; };

          # The compiler wrappers also expose their binutils (ar/ld), which the
          # ubuntu image lacks.
          linux = rec {
            default = gcc-13-libstdcxx;
            gcc-13-libstdcxx = mkEnv "gcc-13-libstdcxx" (
              [ pkgs.gcc13 ] ++ versioned pkgs.gcc13 13 [ "gcc" "g++" "cpp" ]
            );
            clang-20-libcxx = mkEnv "clang-20-libcxx" (
              [
                clang20
                pkgs.llvmPackages_20.llvm # llvm-profdata / llvm-cov for coverage
              ]
              ++ versioned clang20 20 [ "clang" "clang++" ]
            );
            clang-18-libstdcxx-gcc13 = mkEnv "clang-18-libstdcxx-gcc13" (
              [ clang18 ] ++ versioned clang18 18 [ "clang" "clang++" ]
            );
          };

          # macOS builds with the runner's Apple Clang; Nix only supplies the tools.
          darwin = rec {
            default = apple-clang;
            apple-clang = mkEnv "apple-clang" [ ];
          };
        in
        if pkgs.stdenv.hostPlatform.isLinux then linux else darwin
      );
    in
    {
      packages = envs;

      # `nix develop .#<env>` for local work: the CI env plus the CMake 4 policy
      # the setup-nix action exports. NoCC so the stdenv compiler does not
      # shadow the env's (or Apple Clang).
      devShells = forEachSystem (
        system:
        builtins.mapAttrs (
          _: env:
          nixpkgs.legacyPackages.${system}.mkShellNoCC {
            packages = [ env ];
            CMAKE_POLICY_VERSION_MINIMUM = "3.5";
            # Own Conan home: the Nix conan must not migrate a system conan's cache.
            shellHook = ''
              export CONAN_HOME=~/.conan2-xahaud-nix
              xahaud-conan-init
              # see runtimeLibs
              [ -d ${env}/lib/xahaud-runtime ] && export LD_LIBRARY_PATH=${env}/lib/xahaud-runtime''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
            '';
          }
        ) envs.${system}
      );
    };
}
