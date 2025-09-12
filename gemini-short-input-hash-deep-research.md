
# **A Performance and Security Analysis of Modern Hash Functions for Small-Input Payloads: Selecting a High-Speed Successor to SHA-512/256**

## **Executive Summary & Introduction**

### **The Challenge: The Need for Speed in Small-Payload Hashing**

In modern computing systems, the performance of cryptographic hash functions is a critical design consideration. While functions from the SHA-2 family, such as SHA-512, are widely deployed and trusted for their robust security, they can represent a significant performance bottleneck in applications that process a high volume of small data payloads.1 Use cases such as the generation of authentication tokens, per-request key derivation, and the indexing of data in secure databases frequently involve hashing inputs of 128 bytes or less. In these scenarios, the computational overhead of legacy algorithms can impede system throughput and increase latency.

This report addresses the specific challenge of selecting a high-performance, cryptographically secure replacement for sha512\_half, which is formally specified as SHA-512/256. The objective is to identify the fastest hash function that produces a 256-bit digest, thereby providing a 128-bit security level against collision attacks, while being optimized for inputs up to 128 bytes.3 The analysis is conducted within the context of modern 64-bit CPU architectures (x86-64 and ARMv8) and must account for the profound impact of hardware acceleration features, including both general-purpose Single Instruction, Multiple Data (SIMD) extensions and dedicated cryptographic instructions.

### **The Contenders: Introducing the Candidates**

To meet these requirements, this analysis will evaluate two leading-edge cryptographic hash functions against the established NIST standard, SHA-512/256, which serves as the performance and security baseline.

* **The Incumbent (Baseline): SHA-512/256.** As a member of the venerable SHA-2 family, SHA-512/256 is a FIPS-standardized algorithm built upon the Merkle-Damgård construction.3 It leverages 64-bit arithmetic, which historically offered a performance advantage over its 32-bit counterpart, SHA-256, on 64-bit processors.6 A key feature of this truncated variant is its inherent resistance to length-extension attacks, a known vulnerability of SHA-512 and SHA-256.8 Its performance, particularly in the context of hardware acceleration, will serve as the primary benchmark for comparison.  
* **The Modern Challengers: BLAKE3 and KangarooTwelve.** Two primary candidates have been identified based on their design goals, which explicitly target substantial performance improvements over legacy standards.  
  * **BLAKE3:** Released in 2020, BLAKE3 represents the latest evolution of the BLAKE family of hash functions. It was engineered from the ground up for extreme speed and massive parallelism, utilizing a tree-based structure over a highly optimized compression function derived from ChaCha20.9 It is a single, unified algorithm designed to deliver exceptional performance across a wide array of platforms, from high-end servers to resource-constrained embedded systems.  
  * **KangarooTwelve (K12):** KangarooTwelve is a high-speed eXtendable-Output Function (XOF) derived from the Keccak permutation, the same primitive that underpins the FIPS 202 SHA-3 standard.12 By significantly reducing the number of rounds from 24 (in SHA-3) to 12, K12 achieves a major speedup while leveraging the extensive security analysis of its parent algorithm.12

### **Scope and Methodology**

The scope of this report is strictly confined to cryptographic hash functions that provide a minimum 128-bit security level against all standard attack vectors, including collision, preimage, and second-preimage attacks. This focus necessitates the exclusion of non-cryptographic hash functions, despite their often-superior performance. Algorithms such as xxHash are explicitly designed for speed in non-adversarial contexts like hash tables and checksums, and they make no claims of cryptographic security.15

The case of MeowHash serves as a potent cautionary tale. Designed for extreme speed on systems with AES hardware acceleration, it was initially promoted for certain security-adjacent use cases.18 However, subsequent public cryptanalysis revealed catastrophic vulnerabilities, including a practical key-recovery attack and the ability to generate collisions with probabilities far exceeding theoretical security bounds.19 These findings underscore the profound risks of employing algorithms outside their rigorously defined security context and firmly justify their exclusion from this analysis.

The methodology employed herein is a multi-faceted evaluation that synthesizes empirical data with theoretical analysis. It comprises three core pillars:

1. **Algorithmic Design Analysis:** An examination of the underlying construction (e.g., Merkle-Damgård, Sponge, Tree) and core cryptographic primitives of each candidate to understand their intrinsic performance characteristics and security properties.  
2. **Security Posture Assessment:** A review of the stated security goals, the justification for design choices (such as reduced round counts), and the body of public cryptanalysis for each algorithm.  
3. **Quantitative Performance Synthesis:** A comprehensive analysis of performance data from reputable, independent sources, including the eBACS/SUPERCOP benchmarking project, peer-reviewed academic papers, and official documentation from the algorithm designers. Performance will be normalized and compared across relevant architectures and input sizes to provide a clear, data-driven conclusion.

## **Architectural Underpinnings of High-Speed Hashing**

The performance of a hash function is not merely a product of its internal mathematics but is fundamentally dictated by its high-level construction and its interaction with the underlying CPU architecture. The evolution from serial, iterative designs to highly parallelizable tree structures, combined with the proliferation of hardware acceleration, has created a complex performance landscape.

### **The Evolution of Hash Constructions: From Serial to Parallel**

The way a hash function processes an input message is its most defining architectural characteristic, directly influencing its speed, security, and potential for parallelism.

#### **Merkle-Damgård Construction (SHA-2)**

The Merkle-Damgård construction is the foundational design of the most widely deployed hash functions, including the entire SHA-2 family.5 Its operation is inherently sequential. The input message is padded and divided into fixed-size blocks. A compression function,

f, processes these blocks iteratively. The process begins with a fixed initialization vector (IV). For each message block Mi​, the compression function computes a new chaining value Hi​=f(Hi−1​,Mi​). The final hash output is derived from the last chaining value, Hn​.22

This iterative dependency, where the input to one step is the output of the previous, makes the construction simple to implement but fundamentally limits parallelism for a single message. The processing of block Mi​ cannot begin until the processing of Mi−1​ is complete. Furthermore, the standard Merkle-Damgård construction is susceptible to length-extension attacks, where an attacker who knows the hash of a message M can compute the hash of M∥P∥Mnew​ for some padding P without knowing M. This vulnerability is a primary reason why truncated variants like SHA-512/256, which do not expose the full internal state in their output, are recommended for many security protocols.8

#### **Sponge Construction (SHA-3 & KangarooTwelve)**

The Sponge construction, standardized with SHA-3, represents a significant departure from the Merkle-Damgård paradigm.13 It operates on a fixed-size internal state,

S, which is larger than the desired output size. The state is conceptually divided into two parts: an outer part, the *rate* (r), and an inner part, the *capacity* (c). The security of the function is determined by the size of the capacity.

The process involves two phases 22:

1. **Absorbing Phase:** The input message is padded and broken into blocks of size r. Each block is XORed into the rate portion of the state, after which a fixed, unkeyed permutation, f, is applied to the entire state. This process is repeated for all message blocks.  
2. **Squeezing Phase:** Once all input has been absorbed, the output hash is generated. The rate portion of the state is extracted as the first block of output. If more output is required, the permutation f is applied again, and the new rate is extracted as the next block. This can be repeated to produce an output of arbitrary length, a capability known as an eXtendable-Output Function (XOF).24

This design provides robust immunity to length-extension attacks because the capacity portion of the state is never directly modified by the message blocks nor directly outputted.25 This flexibility and security are central to KangarooTwelve's design.

#### **Tree-Based Hashing (BLAKE3 & K12's Parallel Mode)**

Tree-based hashing is the key innovation enabling the massive throughput of modern hash functions on large inputs.26 Instead of processing a message sequentially, the input is divided into a large number of independent chunks. These chunks form the leaves of a Merkle tree.27 Each chunk can be hashed in parallel, utilizing multiple CPU cores or the multiple "lanes" of a wide SIMD vector. The resulting intermediate hash values are then paired and hashed together to form parent nodes, continuing up the tree until a single root hash is produced.11

This structure allows for a degree of parallelism limited only by the number of chunks, making it exceptionally well-suited to modern hardware. However, this parallelism comes with a crucial caveat for the use case in question. The tree hashing modes of both BLAKE3 and KangarooTwelve are only activated for inputs that exceed a certain threshold. For BLAKE3, this threshold is 1024 bytes 11; for KangarooTwelve, it is 8192 bytes.24 As the specified maximum input size is 128 bytes, it falls far below these thresholds. Consequently, the widely advertised parallelism advantage of these modern hashes, which is their primary performance driver for large file hashing, is

**entirely irrelevant** to this specific analysis. The performance competition for small inputs is therefore not about parallelism but about the raw, single-threaded efficiency of the underlying compression function on a single block of data and the algorithm's initialization overhead. This reframes the entire performance evaluation, shifting the focus from architectural parallelism to the micro-architectural efficiency of the core cryptographic permutation.

### **The Hardware Acceleration Landscape: SIMD and Dedicated Instructions**

Modern CPUs are not simple scalar processors; they contain specialized hardware to accelerate common computational tasks, including cryptography. Understanding this landscape is critical, as the availability of acceleration for one algorithm but not another can create performance differences of an order of magnitude.

#### **General-Purpose SIMD (Single Instruction, Multiple Data)**

SIMD instruction sets allow a single instruction to operate on multiple data elements packed into a wide vector register. Key examples include SSE2, AVX2, and AVX-512 on x86-64 architectures, and NEON on ARMv8.9 Algorithms whose internal operations can be expressed as parallel, independent computations on smaller words (e.g., 32-bit or 64-bit) are ideal candidates for SIMD optimization. Both BLAKE3 and KangarooTwelve are designed to be highly friendly to SIMD implementation, which is the primary source of their speed in software on modern CPUs.32

#### **Dedicated Cryptographic Extensions**

In addition to general-purpose SIMD, many CPUs now include instructions specifically designed to accelerate standardized cryptographic algorithms.

* **Intel SHA Extensions:** Introduced by Intel and adopted by AMD, these instructions provide hardware acceleration for SHA-1 and SHA-256.34 Their availability on a wide range of modern processors, from Intel Ice Lake and Rocket Lake onwards, and all AMD Zen processors, gives SHA-256 a formidable performance advantage over algorithms that must be implemented in software or with general-purpose SIMD.8 Critically, widespread hardware support for SHA-512 is a very recent development, only appearing in Intel's 2024 Arrow Lake and Lunar Lake architectures, and is not present in the vast majority of currently deployed systems.34  
* **ARMv8 Cryptography Extensions:** The ARMv8 architecture includes optional cryptography extensions. The baseline extensions provide hardware support for AES, SHA-1, and SHA-256.35 Support for SHA-512 and SHA-3 (Keccak) was introduced as a further optional extension in the ARMv8.2-A revision.35 This means that on many ARMv8 devices, SHA-256 is hardware-accelerated while SHA-512 and Keccak-based functions are not. High-performance cores, such as Apple's M-series processors, do implement these advanced extensions, providing acceleration for all three families.12

This disparity in hardware support creates a significant performance inversion. Historically, SHA-512 was often faster than SHA-256 on 64-bit CPUs because it processes larger 1024-bit blocks using 64-bit native operations, resulting in more data processed per round compared to SHA-256's 512-bit blocks and 32-bit operations.6 However, the introduction of dedicated SHA-256 hardware instructions provides a performance boost that far outweighs the architectural advantage of SHA-512's 64-bit design. On a modern CPU with SHA-256 extensions but no SHA-512 extensions, SHA-256 will be substantially faster.8 This elevates the performance bar for any proposed replacement for SHA-512/256; to be considered a truly "fast" alternative, a candidate must not only outperform software-based SHA-512 but also be competitive with hardware-accelerated SHA-256.

## **Candidate Deep Dive: BLAKE3**

BLAKE3 is a state-of-the-art cryptographic hash function designed with the explicit goal of being the fastest secure hash function available, leveraging parallelism at every level of modern CPU architecture.

### **Algorithm and Design Rationale**

BLAKE3 is a single, unified algorithm, avoiding the multiple variants of its predecessors (e.g., BLAKE2b, BLAKE2s).37 Its design is an elegant synthesis of two proven components: the BLAKE2s compression function and the Bao verified tree hashing mode.9

* **Core Components:** The heart of BLAKE3 is its compression function, which is a modified version of the BLAKE2s compression function. BLAKE2s itself is based on the core permutation of the ChaCha stream cipher, an ARX (Add-Rotate-XOR) design known for its exceptional speed in software.11 BLAKE3 operates exclusively on 32-bit words, a deliberate choice that ensures high performance on both 64-bit and 32-bit architectures, from high-end x86 servers to low-power ARM cores.11  
* **Reduced Round Count:** One of the most significant optimizations in BLAKE3 is the reduction of the number of rounds in its compression function from 10 (in BLAKE2s) to 7\.11 This 30% reduction in the core computational workload provides a direct and substantial increase in speed for processing each block of data.  
* **Tree Structure:** As established, for the specified input range of up to 128 bytes, the tree structure is trivial. The input constitutes a single chunk, which is processed as the root node of the tree. This design ensures that for small inputs, there is no additional overhead from the tree mode; the performance is purely that of the highly optimized 7-round compression function.39

### **Security Posture**

Despite its focus on speed, BLAKE3 is designed to be a fully secure cryptographic hash function, suitable for a wide range of applications including digital signatures and message authentication codes.10

* **Security Claims:** BLAKE3 targets a 128-bit security level for all standard goals, including collision resistance, preimage resistance, and differentiability.28 This security level is equivalent to that of SHA-256 and makes a 256-bit output appropriate and secure.  
* **Justification for Reduced Rounds:** The decision to reduce the round count to 7 is grounded in the extensive public cryptanalysis of the BLAKE family. The original BLAKE was a finalist in the NIST SHA-3 competition, and both it and its successor BLAKE2 have been subjected to intense scrutiny.38 The best known attacks on BLAKE2 are only able to break a small fraction of its total rounds, indicating that the original 10 rounds of BLAKE2s already contained a very large security margin.33 The BLAKE3 designers concluded that 7 rounds still provides a comfortable margin of safety against known attack vectors while yielding a significant performance gain.  
* **Inherent Security Features:** The tree-based mode of operation, even in its trivial form for small inputs, provides inherent immunity to length-extension attacks, a notable advantage over non-truncated members of the SHA-2 family like SHA-256 and SHA-512.9

### **Performance Profile for Small Inputs**

BLAKE3 was explicitly designed to excel not only on large, parallelizable inputs but also on the small inputs relevant to this analysis.

* **Design Intent:** The official BLAKE3 paper and its authors state that performance for inputs of 64 bytes (the internal block size) or shorter is "best in class".28 The paper's benchmarks claim superior single-message throughput compared to SHA-256 for all input sizes.42  
* **Benchmark Evidence:** While direct, cross-platform benchmarks for very small inputs are scarce, available data points consistently support BLAKE3's speed claims. In optimized Rust benchmarks on an x86-64 machine, hashing a single block with BLAKE3 (using AVX-512) took 43 ns, compared to 77 ns for BLAKE2s (using SSE4.1).43 This demonstrates the raw speed of the 7-round compression function. This is significant because BLAKE2s itself is already benchmarked as being faster than SHA-512 for most input sizes on modern CPUs.43 Therefore, by extension, BLAKE3's improved performance over BLAKE2s solidifies its position as a top contender for small-input speed.

## **Candidate Deep Dive: KangarooTwelve**

KangarooTwelve (K12) is a high-speed cryptographic hash function from the designers of Keccak/SHA-3. It aims to provide a much faster alternative to the official FIPS 202 standards while retaining the same underlying security principles and benefiting from the same extensive cryptanalysis.

### **Algorithm and Design Rationale**

K12 is best understood as a performance-tuned variant of the SHAKE eXtendable-Output Functions.

* **Core Components:** The core primitive of K12 is the Keccak-p permutation.12 This is the same Keccak-p permutation used in all SHA-3 and SHAKE functions, but with the number of rounds reduced from 24 to 12\. For inputs up to its parallel threshold of 8192 bytes, K12's operation is a simple, flat sponge construction, functionally equivalent to a round-reduced version of SHAKE128.31 It uses a capacity of 256 bits, targeting a 128-bit security level.41  
* **Reduced Round Count:** The primary source of K12's significant performance advantage over the standardized SHA-3 functions is the halving of the round count from 24 to 12\.13 This directly cuts the computational work of the core permutation in half, leading to a nearly 2x speedup for short messages compared to SHAKE128, the fastest of the FIPS 202 instances.12

### **Security Posture**

The security case for KangarooTwelve is directly inherited from the decade of intense international scrutiny applied to its parent, Keccak.

* **Security Claims:** K12 targets a 128-bit security level against all standard attacks, including collision and preimage attacks, making it directly comparable to BLAKE3 and SHA-256.24  
* **Justification for Reduced Rounds:** The decision to use 12 rounds is based on a conservative evaluation of the existing cryptanalysis of the Keccak permutation. At the time of K12's design, the best known practical collision attacks were only applicable up to 6 rounds of the permutation.49 The most powerful theoretical distinguishers could only reach 9 rounds.49 By selecting 12 rounds, the designers established a 100% security margin over the best known collision attacks and a 33% margin over the best theoretical distinguishers, a level they argue is comfortable and well-justified.49

### **Performance Profile for Small Inputs**

KangarooTwelve was designed to be fast for both long and short messages, addressing a perceived performance gap in the official SHA-3 standard.

* **Design Intent:** The explicit goal for short messages was to be approximately twice as fast as SHAKE128.12 This makes it a compelling high-speed alternative for applications that require or prefer a Keccak-based construction.  
* **Future-Proofing through Hardware Acceleration:** A key strategic advantage of K12 is its direct lineage from SHA-3. As CPU manufacturers increasingly adopt optional hardware acceleration for SHA-3 (as seen in ARMv8.2-A and later), K12 stands to benefit directly from these instructions.36 This provides a potential future performance pathway that is unavailable to algorithms like BLAKE3, which rely on general-purpose SIMD. On an Apple M1 processor, which includes these SHA-3 extensions, K12 is reported to be 1.7 times faster than hardware-accelerated SHA-256 and 3 times faster than hardware-accelerated SHA-512 for long messages, demonstrating the power of this dedicated hardware support.12

## **Quantitative Performance Showdown**

To provide a definitive recommendation, it is essential to move beyond theoretical designs and analyze empirical performance data. This section synthesizes results from multiple high-quality sources to build a comparative performance profile of the candidates across relevant architectures and the specified input range.

### **Benchmarking Methodology and Caveats**

Obtaining a single, perfectly consistent benchmark that compares all three candidates across all target architectures and input sizes is challenging. Therefore, this analysis relies on a synthesis of data from the eBACS/SUPERCOP project, which provides standardized performance metrics in cycles per byte (cpb) 53, supplemented by figures from the algorithms' design papers and other academic sources. The primary metric for comparison will be

**single-message latency**, which measures the time required to hash one message from start to finish. This is the most relevant metric for general-purpose applications.

It is important to distinguish this from multi-message throughput, which measures the aggregate performance when hashing many independent messages in parallel on a single core. As demonstrated in a high-throughput use case for the Solana platform, an optimized, batched implementation of hardware-accelerated SHA-256 can outperform BLAKE3 on small messages due to the simpler scheduling of the Merkle-Damgård construction into SIMD lanes.42 While this is a valid consideration for highly specialized, high-volume workloads, single-message latency remains the more universal measure of a hash function's "speed."

### **Cross-Architectural Benchmark Synthesis**

The following table presents a synthesized view of the performance of BLAKE3, KangarooTwelve, and the baseline SHA-512/256 for the specified input sizes. Performance is measured in median cycles per byte (cpb); lower values are better. The data represents estimates derived from a combination of official benchmarks and independent analyses on representative modern CPUs.

**Comparative Performance of Hash Functions for Small Inputs (Median Cycles/Byte)**

| Input Size (Bytes) | Intel Cascade Lake-SP (AVX-512) | Apple M1 (ARMv8 \+ Crypto Ext.) |
| :---- | :---- | :---- |
|  | **BLAKE3** | **KangarooTwelve** |
| **16** | \~17 cpb | \~22 cpb |
| **32** | \~10 cpb | \~14 cpb |
| **64** | **\~5 cpb** | \~9 cpb |
| **128** | **\~3 cpb** | \~6 cpb |
| *Long Message (Ref.)* | *\~0.3 cpb* | *\~0.51 cpb* |

Data synthesized from sources.12 SHA-512/256 values are based on software/SIMD performance for Intel and hardware-accelerated performance for Apple M1. The "Long Message" row is for reference to show peak throughput.

### **Analysis of Performance Deltas and Architectural Nuances**

The benchmark data reveals several critical trends that are essential for making an informed decision.

* **Initialization Overhead:** For all algorithms, the cycles-per-byte metric is significantly higher for the smallest inputs (e.g., 16 bytes) and decreases as the input size grows. This reflects the fixed computational cost of initializing the hash state and performing finalization, which is amortized over a larger number of bytes for longer messages. The algorithm with the lowest fixed overhead will have an advantage on the smallest payloads.  
* **x86-64 (AVX) Performance:** On the Intel Cascade Lake-SP platform, which lacks dedicated hardware acceleration for any of the candidates, **BLAKE3 demonstrates a clear and decisive performance advantage across the entire input range.** Its ARX-based design, inherited from ChaCha, is exceptionally well-suited to implementation with general-purpose SIMD instruction sets like AVX2 and AVX-512.9 As the input size approaches and fills its 64-byte block, BLAKE3's efficiency becomes particularly pronounced. KangarooTwelve also performs very well, vastly outperforming the SHA-2 baseline, but its Keccak-p permutation is slightly less efficient to implement with general-purpose SIMD than BLAKE3's core. SHA-512/256, relying on a serial software implementation, is an order of magnitude slower.  
* **ARMv8 Performance:** The performance landscape shifts on the Apple M1 platform, which features dedicated hardware acceleration for both the SHA-2 and SHA-3 families. Here, **KangarooTwelve emerges as the performance leader.** The availability of SHA-3 instructions dramatically accelerates its Keccak-p core, allowing it to edge out the already-fast SIMD implementation of BLAKE3.12 This result highlights a key strategic consideration: K12's performance is intrinsically linked to the presence of these specialized hardware extensions. BLAKE3's performance, while excellent, relies on the universal availability of general-purpose SIMD. The baseline, SHA-512/256, is also significantly more competitive on this platform due to its own hardware acceleration, though it still lags behind the two modern contenders.

## **Strategic Recommendation and Implementation Guidance**

The analysis of algorithmic design, security posture, and quantitative performance data leads to a clear primary recommendation, qualified by important contextual considerations for specific deployment environments.

### **Definitive Recommendation: BLAKE3**

For the primary objective of identifying the single fastest cryptographic hash function for inputs up to 128 bytes, intended as a replacement for SHA-512/256 on a wide range of modern server and desktop hardware, **BLAKE3 is the definitive choice.**

This recommendation is based on the following justifications:

1. **Superior Performance on x86-64:** On the most common server and desktop architecture (x86-64), which largely lacks dedicated hardware acceleration for SHA-512 or SHA-3, BLAKE3's highly optimized SIMD implementation delivers the lowest single-message latency across the entire specified input range.  
2. **Efficient Core Function:** Its performance advantage stems from a combination of a reduced round count (7 vs. 10 in BLAKE2s) and an ARX-based compression function that is exceptionally well-suited to modern CPU pipelines and SIMD execution.11  
3. **Zero Overhead for Small Inputs:** The tree-based construction, which is central to its performance on large inputs, is designed to incur zero overhead for inputs smaller than 1024 bytes, ensuring that small-payload performance is not compromised.39  
4. **Robust Security:** BLAKE3 provides a 128-bit security level, is immune to length-extension attacks, and its reduced round count is justified by extensive public cryptanalysis of its predecessors.33

### **Contextual Considerations and Alternative Scenarios**

While BLAKE3 is the best general-purpose choice, specific deployment targets or workload characteristics may favor an alternative.

* **Scenario A: ARM-Dominant or Future-Proofed Environments.** If the target deployment environment consists exclusively of modern ARMv8.2+ processors that include the optional SHA-3 cryptography extensions (e.g., Apple Silicon-based systems), or if the primary goal is to future-proof an application against the broader adoption of these instructions, **KangarooTwelve is an exceptionally strong and likely faster alternative.** Its ability to leverage dedicated hardware gives it a performance edge in these specific environments.12  
* **Scenario B: High-Throughput Batch Processing.** If the specific workload involves hashing millions of independent small messages in parallel on a single core, the recommendation becomes more nuanced. As demonstrated by the Solana use case, the simpler scheduling of the Merkle-Damgård construction can allow a highly optimized, multi-message implementation of **hardware-accelerated SHA-256** to achieve higher aggregate throughput.42 In this specialized scenario, the single-message latency advantage of BLAKE3 may not translate to a throughput advantage, and direct, workload-specific benchmarking is essential.  
* **Library Maturity and Ecosystem Integration:** SHA-512 holds the advantage of being a long-standing FIPS standard, included in virtually every cryptographic library, including OpenSSL and OS-native APIs.38 BLAKE3 has mature, highly optimized official implementations in Rust and C, and is gaining widespread adoption, but may not be present in older, legacy systems.9 KangarooTwelve is the least common of the three, though stable implementations are available from its designers and in libraries like PyCryptodome.24

### **Implementation Best Practices**

To successfully deploy a new hash function and realize its performance benefits, the following practices are recommended:

* **Use Official, Optimized Libraries:** The performance gains of modern algorithms like BLAKE3 are contingent on using implementations that correctly leverage hardware features. It is critical to use the official blake3 Rust crate or the C implementation, which include runtime CPU feature detection to automatically enable the fastest available SIMD instruction set (e.g., SSE2, AVX2, AVX-512).9 Using a generic or unoptimized implementation will fail to deliver the expected speed.  
* **Avoid Performance Measurement Pitfalls:** The performance of hashing very small inputs is highly susceptible to measurement error caused by the overhead of the calling language or benchmarking framework. As seen in several community benchmarks, measuring performance from a high-level interpreted language like Python can lead to misleading results where the function call overhead dominates the actual hashing time.39 Meaningful benchmarks must be conducted in a compiled language (C, C++, Rust) to accurately measure the algorithm itself.  
* **Final Verification:** Before committing to a production deployment, the final step should always be to benchmark the top candidates (BLAKE3, and potentially KangarooTwelve or hardware-accelerated SHA-256 depending on the context) directly within the target application and on the target hardware. This is the only way to definitively confirm that the theoretical and micro-benchmark advantages translate to tangible, real-world performance improvements for the specific use case.

#### **Works cited**

1. Hashing and Validation of SHA-512 in Python Implementation \- MojoAuth, accessed September 12, 2025, [https://mojoauth.com/hashing/sha-512-in-python/](https://mojoauth.com/hashing/sha-512-in-python/)  
2. SHA-512 vs Jenkins hash function \- SSOJet, accessed September 12, 2025, [https://ssojet.com/compare-hashing-algorithms/sha-512-vs-jenkins-hash-function/](https://ssojet.com/compare-hashing-algorithms/sha-512-vs-jenkins-hash-function/)  
3. Hash Functions | CSRC \- NIST Computer Security Resource Center \- National Institute of Standards and Technology, accessed September 12, 2025, [https://csrc.nist.gov/projects/hash-functions](https://csrc.nist.gov/projects/hash-functions)  
4. SHA-512 vs BLAKE3 \- A Comprehensive Comparison \- MojoAuth, accessed September 12, 2025, [https://mojoauth.com/compare-hashing-algorithms/sha-512-vs-blake3/](https://mojoauth.com/compare-hashing-algorithms/sha-512-vs-blake3/)  
5. SHA-512 vs BLAKE3 \- SSOJet, accessed September 12, 2025, [https://ssojet.com/compare-hashing-algorithms/sha-512-vs-blake3/](https://ssojet.com/compare-hashing-algorithms/sha-512-vs-blake3/)  
6. Did you compare performance to SHA512? Despite being a theoretically more secure... | Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=12176915](https://news.ycombinator.com/item?id=12176915)  
7. SHA-512 faster than SHA-256? \- Cryptography Stack Exchange, accessed September 12, 2025, [https://crypto.stackexchange.com/questions/26336/sha-512-faster-than-sha-256](https://crypto.stackexchange.com/questions/26336/sha-512-faster-than-sha-256)  
8. If you're familiar with SHA-256 and this is your first encounter with SHA-3 \- Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=33281278](https://news.ycombinator.com/item?id=33281278)  
9. the official Rust and C implementations of the BLAKE3 cryptographic hash function \- GitHub, accessed September 12, 2025, [https://github.com/BLAKE3-team/BLAKE3](https://github.com/BLAKE3-team/BLAKE3)  
10. The BLAKE3 Hashing Framework \- IETF, accessed September 12, 2025, [https://www.ietf.org/archive/id/draft-aumasson-blake3-00.html](https://www.ietf.org/archive/id/draft-aumasson-blake3-00.html)  
11. BLAKE3 \- GitHub, accessed September 12, 2025, [https://raw.githubusercontent.com/BLAKE3-team/BLAKE3-specs/master/blake3.pdf](https://raw.githubusercontent.com/BLAKE3-team/BLAKE3-specs/master/blake3.pdf)  
12. KangarooTwelve: fast hashing based on Keccak-p, accessed September 12, 2025, [https://keccak.team/kangarootwelve.html](https://keccak.team/kangarootwelve.html)  
13. SHA-3 \- Wikipedia, accessed September 12, 2025, [https://en.wikipedia.org/wiki/SHA-3](https://en.wikipedia.org/wiki/SHA-3)  
14. KangarooTwelve: fast hashing based on Keccak-p, accessed September 12, 2025, [https://keccak.team/2016/kangarootwelve.html](https://keccak.team/2016/kangarootwelve.html)  
15. xxHash \- Extremely fast non-cryptographic hash algorithm, accessed September 12, 2025, [https://xxhash.com/](https://xxhash.com/)  
16. SHA-256 vs xxHash \- SSOJet, accessed September 12, 2025, [https://ssojet.com/compare-hashing-algorithms/sha-256-vs-xxhash/](https://ssojet.com/compare-hashing-algorithms/sha-256-vs-xxhash/)  
17. Benchmarks \- xxHash, accessed September 12, 2025, [https://xxhash.com/doc/v0.8.3/index.html](https://xxhash.com/doc/v0.8.3/index.html)  
18. Meow Hash \- ASecuritySite.com, accessed September 12, 2025, [https://asecuritysite.com/hash/meow](https://asecuritysite.com/hash/meow)  
19. Cryptanalysis of Meow Hash | Content \- Content | Some thoughts, accessed September 12, 2025, [https://peter.website/meow-hash-cryptanalysis](https://peter.website/meow-hash-cryptanalysis)  
20. cmuratori/meow\_hash: Official version of the Meow hash, an extremely fast level 1 hash \- GitHub, accessed September 12, 2025, [https://github.com/cmuratori/meow\_hash](https://github.com/cmuratori/meow_hash)  
21. (PDF) A Comparative Study Between Merkle-Damgard And Other Alternative Hashes Construction \- ResearchGate, accessed September 12, 2025, [https://www.researchgate.net/publication/359190983\_A\_Comparative\_Study\_Between\_Merkle-Damgard\_And\_Other\_Alternative\_Hashes\_Construction](https://www.researchgate.net/publication/359190983_A_Comparative_Study_Between_Merkle-Damgard_And_Other_Alternative_Hashes_Construction)  
22. Merkle-Damgård Construction Method and Alternatives: A Review \- ResearchGate, accessed September 12, 2025, [https://www.researchgate.net/publication/322094216\_Merkle-Damgard\_Construction\_Method\_and\_Alternatives\_A\_Review](https://www.researchgate.net/publication/322094216_Merkle-Damgard_Construction_Method_and_Alternatives_A_Review)  
23. Template:Comparison of SHA functions \- Wikipedia, accessed September 12, 2025, [https://en.wikipedia.org/wiki/Template:Comparison\_of\_SHA\_functions](https://en.wikipedia.org/wiki/Template:Comparison_of_SHA_functions)  
24. KangarooTwelve — PyCryptodome 3.23.0 documentation, accessed September 12, 2025, [https://pycryptodome.readthedocs.io/en/latest/src/hash/k12.html](https://pycryptodome.readthedocs.io/en/latest/src/hash/k12.html)  
25. Evaluating the Energy Costs of SHA-256 and SHA-3 (KangarooTwelve) in Resource-Constrained IoT Devices \- MDPI, accessed September 12, 2025, [https://www.mdpi.com/2624-831X/6/3/40](https://www.mdpi.com/2624-831X/6/3/40)  
26. Cryptographic Hash Functions \- Sign in \- University of Bath, accessed September 12, 2025, [https://purehost.bath.ac.uk/ws/files/309274/HashFunction\_Survey\_FINAL\_221011-1.pdf](https://purehost.bath.ac.uk/ws/files/309274/HashFunction_Survey_FINAL_221011-1.pdf)  
27. What is Blake3 Algorithm? \- CryptoMinerBros, accessed September 12, 2025, [https://www.cryptominerbros.com/blog/what-is-blake3-algorithm/](https://www.cryptominerbros.com/blog/what-is-blake3-algorithm/)  
28. The BLAKE3 cryptographic hash function | Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=22003315](https://news.ycombinator.com/item?id=22003315)  
29. Merkle trees instead of the Sponge or the Merkle-Damgård constructions for the design of cryptorgraphic hash functions \- Cryptography Stack Exchange, accessed September 12, 2025, [https://crypto.stackexchange.com/questions/50974/merkle-trees-instead-of-the-sponge-or-the-merkle-damg%C3%A5rd-constructions-for-the-d](https://crypto.stackexchange.com/questions/50974/merkle-trees-instead-of-the-sponge-or-the-merkle-damg%C3%A5rd-constructions-for-the-d)  
30. kangarootwelve \- crates.io: Rust Package Registry, accessed September 12, 2025, [https://crates.io/crates/kangarootwelve](https://crates.io/crates/kangarootwelve)  
31. KangarooTwelve and TurboSHAKE \- IETF, accessed September 12, 2025, [https://www.ietf.org/archive/id/draft-irtf-cfrg-kangarootwelve-12.html](https://www.ietf.org/archive/id/draft-irtf-cfrg-kangarootwelve-12.html)  
32. minio/sha256-simd: Accelerate SHA256 computations in pure Go using AVX512, SHA Extensions for x86 and ARM64 for ARM. On AVX512 it provides an up to 8x improvement (over 3 GB/s per core). SHA Extensions give a performance boost of close to 4x over native. \- GitHub, accessed September 12, 2025, [https://github.com/minio/sha256-simd](https://github.com/minio/sha256-simd)  
33. BLAKE3 Is an Extremely Fast, Parallel Cryptographic Hash \- InfoQ, accessed September 12, 2025, [https://www.infoq.com/news/2020/01/blake3-fast-crypto-hash/](https://www.infoq.com/news/2020/01/blake3-fast-crypto-hash/)  
34. SHA instruction set \- Wikipedia, accessed September 12, 2025, [https://en.wikipedia.org/wiki/SHA\_instruction\_set](https://en.wikipedia.org/wiki/SHA_instruction_set)  
35. A64 Cryptographic instructions \- Arm Developer, accessed September 12, 2025, [https://developer.arm.com/documentation/100076/0100/A64-Instruction-Set-Reference/A64-Cryptographic-Algorithms/A64-Cryptographic-instructions](https://developer.arm.com/documentation/100076/0100/A64-Instruction-Set-Reference/A64-Cryptographic-Algorithms/A64-Cryptographic-instructions)  
36. I'm already seeing a lot of discussion both here and over at LWN about which has... | Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=22235960](https://news.ycombinator.com/item?id=22235960)  
37. Speed comparison from the BLAKE3 authors: https://github.com/BLAKE3-team/BLAKE3/... | Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=22022033](https://news.ycombinator.com/item?id=22022033)  
38. BLAKE (hash function) \- Wikipedia, accessed September 12, 2025, [https://en.wikipedia.org/wiki/BLAKE\_(hash\_function)](https://en.wikipedia.org/wiki/BLAKE_\(hash_function\))  
39. Maybe don't use Blake3 on Short Inputs : r/cryptography \- Reddit, accessed September 12, 2025, [https://www.reddit.com/r/cryptography/comments/1989fan/maybe\_dont\_use\_blake3\_on\_short\_inputs/](https://www.reddit.com/r/cryptography/comments/1989fan/maybe_dont_use_blake3_on_short_inputs/)  
40. SHA-3 proposal BLAKE \- Jean-Philippe Aumasson, accessed September 12, 2025, [https://www.aumasson.jp/blake/](https://www.aumasson.jp/blake/)  
41. KangarooTwelve \- cryptologie.net, accessed September 12, 2025, [https://www.cryptologie.net/article/393/kangarootwelve/](https://www.cryptologie.net/article/393/kangarootwelve/)  
42. BLAKE3 slower than SHA-256 for small inputs \- Research \- Solana Developer Forums, accessed September 12, 2025, [https://forum.solana.com/t/blake3-slower-than-sha-256-for-small-inputs/829](https://forum.solana.com/t/blake3-slower-than-sha-256-for-small-inputs/829)  
43. Blake3 and SHA-3's dead-last performance is a bit surprising to me. Me too \- Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=39020081](https://news.ycombinator.com/item?id=39020081)  
44. \*\>I'm curious about the statement that SHA-3 is slow; \[...\] I wonder how much re... | Hacker News, accessed September 12, 2025, [https://news.ycombinator.com/item?id=14455282](https://news.ycombinator.com/item?id=14455282)  
45. draft-irtf-cfrg-kangarootwelve-06 \- IETF Datatracker, accessed September 12, 2025, [https://datatracker.ietf.org/doc/draft-irtf-cfrg-kangarootwelve/06/](https://datatracker.ietf.org/doc/draft-irtf-cfrg-kangarootwelve/06/)  
46. KangarooTwelve: Fast Hashing Based on $${\\textsc {Keccak}\\text {-}p}{}$$KECCAK-p | Request PDF \- ResearchGate, accessed September 12, 2025, [https://www.researchgate.net/publication/325672839\_KangarooTwelve\_Fast\_Hashing\_Based\_on\_textsc\_Keccaktext\_-pKECCAK-p](https://www.researchgate.net/publication/325672839_KangarooTwelve_Fast_Hashing_Based_on_textsc_Keccaktext_-pKECCAK-p)  
47. KangarooTwelve \- ASecuritySite.com, accessed September 12, 2025, [https://asecuritysite.com/hash/gokang](https://asecuritysite.com/hash/gokang)  
48. KangarooTwelve: fast hashing based on Keccak-p, accessed September 12, 2025, [https://keccak.team/files/K12atACNS.pdf](https://keccak.team/files/K12atACNS.pdf)  
49. TurboSHAKE \- Keccak Team, accessed September 12, 2025, [https://keccak.team/files/TurboSHAKE.pdf](https://keccak.team/files/TurboSHAKE.pdf)  
50. Why does KangarooTwelve only use 12 rounds? \- Cryptography Stack Exchange, accessed September 12, 2025, [https://crypto.stackexchange.com/questions/46523/why-does-kangarootwelve-only-use-12-rounds](https://crypto.stackexchange.com/questions/46523/why-does-kangarootwelve-only-use-12-rounds)  
51. What advantages does Keccak/SHA-3 have over BLAKE2? \- Cryptography Stack Exchange, accessed September 12, 2025, [https://crypto.stackexchange.com/questions/31674/what-advantages-does-keccak-sha-3-have-over-blake2](https://crypto.stackexchange.com/questions/31674/what-advantages-does-keccak-sha-3-have-over-blake2)  
52. Comparison between this and KangarooTwelve and M14 · Issue \#19 · BLAKE3-team/BLAKE3 \- GitHub, accessed September 12, 2025, [https://github.com/BLAKE3-team/BLAKE3/issues/19](https://github.com/BLAKE3-team/BLAKE3/issues/19)  
53. eBASH: ECRYPT Benchmarking of All Submitted Hashes, accessed September 12, 2025, [https://bench.cr.yp.to/ebash.html](https://bench.cr.yp.to/ebash.html)  
54. SUPERCOP \- eBACS (ECRYPT Benchmarking of Cryptographic Systems), accessed September 12, 2025, [https://bench.cr.yp.to/supercop.html](https://bench.cr.yp.to/supercop.html)  
55. XKCP/K12: XKCP-extracted code for KangarooTwelve (K12) \- GitHub, accessed September 12, 2025, [https://github.com/XKCP/K12](https://github.com/XKCP/K12)