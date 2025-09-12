# BLAKE3 vs SHA-512 Performance Analysis for Ripple Data Structures

## Executive Summary

This document presents empirical performance comparisons between BLAKE3 and SHA-512 (specifically SHA512Half) when hashing Ripple/Xahau blockchain data structures. Tests were conducted on Apple Silicon (M-series) hardware using real-world data distributions.

## Test Environment

- **Platform**: Apple Silicon (ARM64)
- **OpenSSL Version**: 1.1.1u (likely without ARMv8.2 SHA-512 hardware acceleration)
- **BLAKE3**: C reference implementation with NEON optimizations
- **Test Data**: Production ledger #16940119 from Xahau network

## Results by Data Type

### 1. Keylet Lookups (22-102 bytes, 35-byte weighted average)

Keylets are namespace discriminators used for ledger lookups. The SHAMap requires high-entropy keys for balanced tree structure, necessitating cryptographic hashing even for small inputs.

**Distribution:**
- 76,478 ACCOUNT lookups (22 bytes)
- 41,740 HOOK lookups (22 bytes)  
- 19,939 HOOK_STATE_DIR lookups (54 bytes)
- 17,587 HOOK_DEFINITION lookups (34 bytes)
- 17,100 HOOK_STATE lookups (86 bytes)
- Other types: ~15,000 operations (22-102 bytes)

**Performance (627,131 operations):**
- **BLAKE3**: 128 ns/hash, 7.81M hashes/sec
- **SHA-512**: 228 ns/hash, 4.38M hashes/sec
- **Speedup**: 1.78x

### 2. Leaf Node Data (167-byte average)

Leaf nodes contain serialized ledger entries (accounts, trustlines, offers, etc.). These represent the actual state data in the ledger.

**Distribution:**
- 626,326 total leaf nodes
- 104.6 MB total data
- Types: AccountRoot (145k), DirectoryNode (118k), RippleState (115k), HookState (124k), URIToken (114k)

**Performance (from production benchmark):**
- **SHA-512**: 446 ns/hash, 357 MB/s (measured)
- **BLAKE3**: ~330 ns/hash, 480 MB/s (projected)
- **Expected Speedup**: ~1.35x

### 3. Inner Nodes (516 bytes exactly)

Inner nodes contain 16 child hashes (32 bytes each) plus a 4-byte prefix. These form the Merkle tree structure enabling cryptographic proofs.

**Distribution:**
- 211,364 inner nodes
- 104.1 MB total data (nearly equal to leaf data volume)

**Performance (211,364 operations):**
- **BLAKE3**: 898 ns/hash, 548 MB/s
- **SHA-512**: 1081 ns/hash, 455 MB/s
- **Speedup**: 1.20x

## Overall Impact Analysis

### Current System Profile

From production measurements, the ledger validation process shows:
- **Map traversal**: 47% of time
- **SHA-512 hashing**: 53% of time

Within the hashing time specifically:
- **Keylet lookups**: ~50% of hashing time
- **Leaf/inner nodes**: ~50% of hashing time

### Projected Improvement with BLAKE3

Given the measured speedups:
- Keylet operations: 1.78x faster → 28% time reduction
- Leaf operations: 1.35x faster → 26% time reduction  
- Inner operations: 1.20x faster → 17% time reduction

**Net improvement**: ~20-25% reduction in total hashing time, or **10-13% reduction in overall validation time**.

## Key Observations

1. **Small Input Performance**: BLAKE3 shows its greatest advantage (1.78x) on small keylet inputs where function call overhead dominates.

2. **Diminishing Returns**: As input size increases to SHA-512's block size (128 bytes) and multiples thereof, the performance gap narrows significantly.

3. **Architectural Constraint**: The SHAMap design requires cryptographic hashing for all operations to maintain high-entropy keys, preventing optimization through non-cryptographic alternatives.

4. **Implementation Effort**: Transitioning from SHA-512 to BLAKE3 would require:
   - Updating all hash generation code
   - Maintaining backward compatibility
   - Extensive testing of consensus-critical code
   - Potential network upgrade coordination

## Conclusion

BLAKE3 offers measurable performance improvements over SHA-512 for Ripple data structures, particularly for small inputs. However, the gains are modest (1.2-1.78x depending on input size) rather than revolutionary. With map traversal consuming nearly half the total time, even perfect hashing would only double overall performance.

For keylet operations specifically, the 1.78x speedup is significant given that keylet hashing accounts for approximately 50% of all hashing time. However, the measured improvements must be weighed against the engineering effort and risk of modifying consensus-critical cryptographic primitives. A 10-13% overall performance gain may not justify the migration complexity unless combined with other architectural improvements.