{
  description = "CI toolchains for xahaud: the tools around Conan, keyed by the compiler_id of .github/workflows/xahau-ga-nix.yml";

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

          # Everything a CI job needs besides the compiler. Conan builds the
          # library dependencies, so this is only tooling.
          tools = with pkgs; [
            cacert # TLS trust store for the nix-built git/curl/conan
            ccache
            cmake
            conan
            coreutils # nproc on macOS
            curl # codecov-action
            gcovr
            git
            gnumake
            gnupg # codecov-action verifies its CLI with gpg
            jq
            ninja
            perl # openssl build
            pkg-config
            python3
          ];

          # Nix exposes compilers under unsuffixed names only. The workflow
          # matrix (cc/cxx) and Conan's boost recipe (g++-<major>) use suffixed
          # names, which would otherwise fall through to a system compiler.
          # Exec wrappers, not symlinks: the cc-wrapper scripts dispatch on $0.
          versioned =
            package: version: names:
            pkgs.symlinkJoin {
              name = "${package.name}-versioned-links";
              paths = map (
                tool:
                pkgs.writeShellScriptBin "${tool}-${toString version}" ''
                  exec "${package}/bin/${tool}" "$@"
                ''
              ) names;
            };

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
              ];
            };

          gcc13 = pkgs.gcc13;
          # Outside a Nix stdenv nothing puts libc++'s lib dir on the link line
          # (a stdenv setup hook would), so -lc++ fails without this.
          clang20 = pkgs.llvmPackages_20.libcxxClang.override (old: {
            extraBuildCommands = (old.extraBuildCommands or "") + ''
              echo "-L${pkgs.llvmPackages_20.libcxx}/lib" >> $out/nix-support/cc-ldflags
            '';
          });
          # clang on top of gcc 13's libstdc++, like the apt setup this replaced
          # (clang-16 there; nixpkgs ships clang >= 18).
          clang18 = pkgs.llvmPackages_18.libstdcxxClang.override { gccForLibs = gcc13.cc; };

          linux = rec {
            default = gcc-13-libstdcxx;
            gcc-13-libstdcxx = mkEnv "gcc-13-libstdcxx" [
              gcc13 # the wrapper also exposes its binutils (ar/ld), which the ubuntu image lacks
              (versioned gcc13 13 [ "gcc" "g++" "cpp" ])
            ];
            clang-20-libcxx = mkEnv "clang-20-libcxx" [
              clang20
              pkgs.llvmPackages_20.llvm # llvm-profdata / llvm-cov for coverage
              (versioned clang20 20 [ "clang" "clang++" ])
            ];
            clang-18-libstdcxx-gcc13 = mkEnv "clang-18-libstdcxx-gcc13" [
              clang18
              (versioned clang18 18 [ "clang" "clang++" ])
            ];
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

      # `nix develop .#<env>` for local work; same contents as the CI env.
      # NoCC: the stdenv compiler must not shadow the env's (or Apple Clang).
      devShells = forEachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        builtins.mapAttrs (
          name: env:
          pkgs.mkShellNoCC {
            packages = [ env ];
            # nixpkgs ships CMake 4; several Conan dependencies declare
            # cmake_minimum_required < 3.5 (same as the CI action exports).
            CMAKE_POLICY_VERSION_MINIMUM = "3.5";
          }
        ) envs.${system}
      );
    };
}
