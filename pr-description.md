# Add Clang Compiler Support to Linux CI Build Matrix

## Summary

This PR adds Clang compiler support (versions 14, 16, 17) to the GitHub Actions Linux build matrix, alongside existing GCC builds. The implementation includes workarounds for C++20 compatibility issues between different Clang versions and GCC headers, plus dynamic matrix generation to control CI resource usage.

## Key Features

### 1. **Multi-Compiler Build Matrix**
- **gcc-11-libstdcxx**: GCC 11 with libstdc++ (baseline compatibility)
- **gcc-13-libstdcxx**: GCC 13 with libstdc++ (modern GCC)
- **clang-14-libstdcxx-gcc11**: Clang 14 using GCC 11's libstdc++
- **clang-16-libstdcxx-gcc13**: Clang 16 using GCC 13's libstdc++
- **clang-17-libcxx**: Clang 17 with LLVM's libc++ (slower builds but tests LLVM stack)
- **clang-18-libcxx**: Clang 18 with LLVM's libc++ (requires Conan settings patch)
- Total of 6 compiler configurations for Linux coverage

### 2. **Dynamic Matrix Generation**

#### Build Matrix Configurations

| Compiler ID | Compiler | Version | Stdlib | GCC Toolchain | Runs When |
|------------|----------|---------|--------|---------------|-----------|
| gcc-11-libstdcxx | GCC | 11 | libstdc++ | - | Full matrix only |
| gcc-13-libstdcxx | GCC | 13 | libstdc++ | - | Always (minimal + full) |
| clang-14-libstdcxx-gcc11 | Clang | 14 | libstdc++ | GCC 11 | Always (minimal + full) |
| clang-16-libstdcxx-gcc13 | Clang | 16 | libstdc++ | GCC 13 | Full matrix only |
| clang-17-libcxx | Clang | 17 | libc++ | - | Full matrix only |
| clang-18-libcxx | Clang | 18 | libc++ | - | Full matrix only |

#### Matrix Selection Logic

| Condition | Matrix Used | Configs Run |
|-----------|-------------|-------------|
| PR to dev or feature branch | Minimal | gcc-13, clang-14 |
| PR to release/candidate | Full | All 6 configs |
| Push to main branch (dev/release/candidate) | Full | All 6 configs |
| `[ci-nix-full-matrix]` in commit/PR | Full (override) | All 6 configs |

**Note**: PRs to `dev` use minimal matrix since most are feature branches being merged. PRs to `release`/`candidate` use full matrix as these are critical merges (see [PR #541](https://github.com/Xahau/xahaud/pull/541) as an example of dev→release merge that warrants full testing).

- Matrix generation optimized to ~7s using `python:3-slim` container
- Fully customizable matrix configurations via inline Python script

### 3. **C++20 Compatibility Solutions**

#### The GCC Header Problem
Clang 14 cannot compile against GCC 13/14 headers due to `consteval` usage incompatibilities. The workaround:

**GCC Directory Renaming** (for Clang < 16):
- Clang's `--gcc-toolchain` flag uses discovery that always picks the highest version number
- Solution: Rename newer GCC directories to low integers (1, 2, 3...)
- Example: GCC 12→1, GCC 13→2, GCC 14→3
- Result: Clang picks GCC 11 (highest actual number) over renamed versions

#### Standard Library Configuration
- Clang 14/16: Use libstdc++ (GCC's stdlib) due to missing `lexicographical_compare_three_way` in older libc++
- Clang 17: Can use libc++ (has full C++20 support)
- GCC: Always uses libstdc++ (doesn't support -stdlib flag)

### 4. **Implementation Details**

#### Ubuntu 24.04 Compiler Availability
- **Pre-installed**: gcc-12, gcc-13, gcc-14 (ubuntu-latest provides these)
- **Requires installation**: gcc-11, clang-14, clang-16, clang-17
- All Clang versions and gcc-11 are installed via apt-get in the workflow

#### Compiler Selection Flags
- **Clang 16+**: Uses `--gcc-install-dir` (precise path)
- **Clang 14-15**: Uses `--gcc-toolchain` (deprecated but required)

#### Cache Key Strategy
- Format: `compiler-version-stdlib[-gccversion]`
- Examples: `clang-14-libstdcxx-gcc11`, `gcc-13-libstdcxx`
- Separate caches for different compiler/stdlib combinations

#### Conan Package Manager
- Limited to v1.x (v2 not yet supported)
- Conan v1 settings.yml supports Clang versions up to 17:
  ```yaml
  clang:
    version: ["3.3", "3.4", "3.5", "3.6", "3.7", "3.8", "3.9", "4.0",
              "5.0", "6.0", "7.0", "7.1",
              "8", "9", "10", "11", "12", "13", "14", "15", "16", "17"]
  ```
- Ubuntu 24.04 ships with Clang 18 by default
- Clang 18 support added via automatic settings.yml patching in the workflow
- Profile configured based on matrix parameters

## Files Changed

### Workflow Files
- `.github/workflows/xahau-ga-nix.yml` - Main workflow with matrix generation and build jobs
- `.github/actions/xahau-ga-build/action.yml` - Build action with compiler flag configuration
- `.github/actions/xahau-ga-dependencies/action.yml` - Dependencies action with cache management

### Key Changes

#### Matrix Setup Job (.github/workflows/xahau-ga-nix.yml:14-126)
- Inline Python script for matrix generation
- Branch detection for minimal vs full builds
- Override tag detection in commit messages/PR titles

#### GCC Hiding Implementation (.github/workflows/xahau-ga-nix.yml:171-186)
```bash
# Rename GCC directories to low integers for Clang < 16
if [ "$compiler_version" -lt "16" ]; then
  for dir in /usr/lib/gcc/x86_64-linux-gnu/*/; do
    if version > target_version; then
      mv "$dir" "/usr/lib/gcc/x86_64-linux-gnu/$counter"
    fi
  done
fi
```

#### Compiler Flag Configuration (.github/actions/xahau-ga-build/action.yml:99-123)
```bash
# Only apply -stdlib flag to Clang (not GCC)
if [[ "$cxx" == clang* ]]; then
  CMAKE_CXX_FLAGS="-stdlib=libstdc++"
fi

# Use appropriate GCC toolchain flag based on Clang version
if [ "$clang_version" -ge "16" ]; then
  CMAKE_CXX_FLAGS="$CMAKE_CXX_FLAGS --gcc-install-dir=/path/to/gcc"
else
  CMAKE_CXX_FLAGS="$CMAKE_CXX_FLAGS --gcc-toolchain=/usr"
fi
```

## Testing & Validation

- All 5 compiler configurations build and pass tests
- Cache keys differentiate between configurations
- Matrix generation correctly detects branch context
- GCC directory renaming verified to work with Clang 14
- Matrix generation reduced from ~26s to ~7s

## Future Improvements

- Can be updated to newer Clang versions once Conan v2 support is added
- GCC directory renaming can be removed once all Clang versions support `--gcc-install-dir`
- Matrix configurations can be adjusted based on team feedback

## Notes

- The GCC directory renaming is the only reliable way to control Clang's GCC detection for versions < 16
- Inline comments explain the workarounds
- All workarounds are necessary due to limitations in Clang/GCC/Conan interactions
- Configuration field supports both Debug and Release builds (currently set to Debug)
- clang-17-libcxx builds are notably slower than libstdc++ builds