# &#x20;BLAKE3 Migration

---

## ~~Why touch the cryptographic foundation at all?~~

~~Performance isn't an academic detail — it's dramatic. On modern hardware, BLAKE3 runs an order of magnitude faster than SHA-512 or SHA-256. For example:~~

~~In benchmarks, BLAKE3 achieves \~6.8 GiB/s throughput on a single thread, compared to \~0.7 GiB/s for SHA-512. This headroom matters in a ledger system where *every object key is a hash*. Faster hashing reduces CPU load for consensus, verification, and replay. Here, "performance" primarily means faster **keylet** computation (deriving map/index keys from object components) and less compatibility overhead (LUT hits, try‑both‑hashes), **not** improved data locality between neighboring objects.~~

~~Performance and modern cryptographic hygiene argue strongly for adopting BLAKE3. It's fast, parallelizable, and future-proof. But in this ledger system, the hash is not just a digest: it is the address of every object. Changing the hash function means changing the address of every single entry. This isn't like swapping an internal crypto primitive — it's a rekeying of the entire universe.~~

## Reality Check: BLAKE3 vs SHA-512 on ARM64 (Sept 2025)

**TL;DR: BLAKE3 migration complexity isn't justified by the actual performance gains.**

### Measured Performance (Xahau ledger #16940119)
- **Keylets (22-102 bytes)**: BLAKE3 is 0.68x speed of SHA-512 (47% SLOWER)
- **Inner nodes (516 bytes)**: BLAKE3 is 0.52x speed of SHA-512 (92% SLOWER)
- **Map traversal**: 59-65% of total time (not affected by hash choice)
- **Actual hashing**: Only 35-41% of total time

### Why BLAKE3 Underperforms
1. **Small inputs**: Median keylet is 35 bytes; SIMD overhead exceeds benefit
2. **2020 software vs 2025 hardware**: BLAKE3 NEON intrinsics vs OpenSSL 3.3.2's optimized SHA-512
3. **No parallelism**: Single-threaded SHAMap walks can't use BLAKE3's parallel design
4. **SIMD dependency**: Without NEON, BLAKE3 portable C is 2x slower than SHA-512

### The Verdict
With hashing only 35-41% of total time and BLAKE3 actually SLOWER on typical inputs, the migration would:
- Increase total validation time by ~10-15%
- Add massive complexity (LUTs, heterogeneous trees, compatibility layers)
- Risk consensus stability for negative performance gain

**Recommendation: Abandon BLAKE3 migration. Focus on map traversal optimization instead.**

## Hashes vs Indexes

* **Hashes as keys**: Every blob of data in the NodeStore is keyed by a hash of its contents. This makes the hash the *address* for retrieval.
* **Hashes as indexes**: In a ShaMap (the Merkle tree that represents ledger state), an `index` is derived by hashing stable identity components (like account ID + other static identifiers). This index determines the path through the tree.
* **Takeaway**: Hash = storage key. Index = map position. Both are 256-bit values, but they play different roles.

*Terminology note*: throughout, **keylet/key** = deterministic map/index key composition from object components; this is unrelated to users’ cryptographic signing keys.

## LUT at a glance

A **Lookup Table (LUT)** is an exact‑key alias map used to bridge old and new addressing:

* **Purpose:** allow lookups by a legacy (old) key to resolve to an object stored under its canonical (new) key — or vice‑versa where strictly necessary.
* **Scope:** point lookups only (reads/writes by exact key). Iteration and ordering remain **canonical**; pagination via `next` after a marker requires careful handling (semantics TBD)
* **Population:** built during migration and optionally **rebuildable** from per‑SLE cross‑key fields (e.g., `sfLegacyKey` for move, or `sfBlake3Key` for non‑move).
* **Directionality in practice:** after the flip you typically need **both directions**, but for different eras:

  * **Pre‑cutover objects (stored at old keys):** maintain **`BLAKE3 → SHA512Half`** so new‑style callers (BLAKE3) can reach old objects.
  * **Post‑cutover objects (stored at new keys):** optionally offer a grace **`SHA512Half → BLAKE3`** alias so legacy callers can reach new objects. Time‑box this.
    **Rule of thumb:** annotate the **opposite side of storage** — if storage is **new** (post‑move), annotate **old**; if storage is **old** (non‑move), annotate **new**.

## What actually breaks if you “just change the hash”?!

Every ledger entry’s key changes. That cascades into:

* **State tree**: SHAMap nodes are keyed by hash; every leaf and inner node address moves.
* **Directories**: owner dirs, book dirs, hook state dirs, NFT pages — all are lists of hashes, all must be rebuilt.
* **Order and proofs**: Succession, iteration, and proof-of-inclusion semantics all rely on canonical ordering of keys. Mixing old and new hashes destroys proof integrity.
* **Caches and history**: Node sharing between ledgers ceases to work; replay and verification of past data must know which hash function was active when.

## Lazy vs Big Bang

If you update tree hashes incrementally as state changes, you are effectively doing a **lazy migration**: slowly moving to the new hashing system over time. That implies heterogeneous trees and ongoing complexity. By contrast, a **big bang** migration rekeys everything in a single, well-defined event. Since roughly 50% of hashing compute goes into creating these keys, most of the performance win from BLAKE3 arrives when the generated keys for a given object are used. This can be achieved if the object is **in place at its new key**, **moved within the tree**, or is **reachable via an exact‑key LUT that aliases old→new**.

*Note:* LUT specifics belong in **Move vs Non‑Move** below. At a high level: aliasing can bridge old/new lookups; iteration/pagination semantics are TBD here and treated separately.

### Pros and Cons

**Lazy migration**

* **Pros**: Less disruptive; avoids one massive compute spike; spreads risk over time.
* **Cons**: Creates heterogeneous trees; complicates proofs and historical verification; requires bidirectional LUTs forever; analysts and tools must support mixed keyspaces.

**Big bang migration**

* **Pros**: Clean cutover at a known ledger; easier for analysts and tooling; no need to support mixed proofs long-term; maximizes BLAKE3 performance benefits immediately.
* **Cons**: One heavy compute event; requires strict consensus choreography; higher risk if validators drift or fail mid-migration.

It’s important to distinguish between lazy vs big bang, and also between keys (addresses/indexes) vs hashes (content identifiers).

## Move vs Non‑Move (what does “migrate” change?)

**Non‑Move (annotate‑only):** objects stay at old SHA512‑Half keys; add `sfBlake3Key` (or similar) recording the would‑be BLAKE3 address; alias lookups via **new→old** LUT; iteration/proofs remain in old key order; minimal compute now, **permanent heterogeneity** and LUT dependence; little perf/ordering win.

**Move (rekey):** objects are physically rewritten under BLAKE3 keys either **on‑touch** (per‑tx or at **BuildLedger** end) or **all at once** (Big‑Bang). Requires **old→new** LUT for compatibility; choose a place/time (per‑tx vs BuildLedger vs Big‑Bang) and define iteration contract (prefer canonical‑only).

**Implications to weigh:**

* **LUT shape:** non‑move needs **new→old** (often also old→new for markers); move prefers **old→new** (temporary). Sunsetting is only realistic in the Big‑Bang case; lazy variants may never fully converge.
* **Iteration/pagination:** canonical‑only iteration keeps proofs stable; translating legacy markers implies **bi‑LUT** and more hot‑path complexity.
* **Replay:** both need `hash_options{rules(), ledger_index, phase}`; move policies must be consensus‑deterministic.
* **Compute/ops:** non‑move is cheap now but never converges; move concentrates work (per‑tx, per‑ledger, or one Big‑Bang) and actually delivers BLAKE3’s **iteration/ordering** and **keylet‑compute** benefits (not data‑locality).

### Choice axes (what / when / how)

* **What:** *Move* the object under BLAKE3 **or** *leave in place* and annotate (`sfBlake3Key`).
* **When:** at **end of txn** or in **BuildLedger** (alongside `updateNegativeUNL()` / `updateSkipList()`), or **all at once** (Big‑Bang).
* **How:** *All at once* requires special network conditions (quiet window + consensus hash); *on modification* spreads risk but prolongs heterogeneity.
* **Blob verification note:** a dual‑hash “verify on link” walk works for mixed trees, but you need the same `rules()+phase` plumbing either way, so it doesn’t materially change the engineering lift.

### Client compatibility & new entries

* **Reality:** flipping keylets changes what clients compute. Old clients may still derive SHA512‑Half; new clients may derive BLAKE3.
* **Lazy non‑move (annotate‑only):**

  * **Reads/updates:** accept BLAKE3 via **new→old LUT**; legacy SHA512 keys keep working.
  * **Creates (policy choice):**

    * **Create‑at‑new (heterogeneous by design):** store under **BLAKE3** (the natural post‑flip behavior). For **legacy callers**, provide a grace alias **`SHA512Half → BLAKE3`** for *new* entries; stamp `sfLegacyKey` (old) on creation so the alias can be rebuilt by a leaf scan.
    * *Create‑at‑old (alternative until swap):* store under **old** to keep the map homogeneous; if request included a BLAKE3 key, treat it as a descriptor and translate. *Optional annotation:* add `sfBlake3Key` (new) to make later `new→old` LUT rebuild trivial. *(In the ********move********/post‑swap case, annotate the opposite side: ******`sfLegacyKey`****** = old.)*
    * *Create‑via‑old‑only:* require old keys for creates until swap (simpler server), and document it for SDKs.
    * *(Note:)* a LUT alone can’t route a brand‑new create — there’s no mapping yet — so the server must compute the storage key from identity (old or new, per the policy) and record the opposite‑side annotation for future aliasing.
* **Big‑Bang (move):** creates immediately use **BLAKE3** as canonical; provide **`SHA512Half → BLAKE3`** grace alias for new objects; **old→new** LUT supports stragglers reading old objects by legacy keys.
* **Bottom line:** you still need **`rules()`**\*\* + phase\*\* plumbing and an explicit **create policy**; don’t pick a strategy based purely on “less plumbing”.

### Post‑cutover lookup policy (directional LUT by era)

* **Old objects (pre‑cutover, stored at old keys):** new‑style callers use **BLAKE3** keys → resolve via **`BLAKE3 → SHA512Half`** (keep as long as needed; deprecate when safe).
* **New objects (post‑cutover, stored at new keys):** legacy callers may supply **SHA512‑Half** → resolve via **`SHA512Half → BLAKE3`** *during a grace window*; plan a TTL/deprecation for this path.
* **Iteration/pagination:** always return the **canonical storage key** of the era (old for old objects, new for new objects). Document that markers are era‑canonical; aliases are for **point lookups** only.

### Lazy non‑move: LUT requirements (immediate and ongoing)

* If keylets emit **BLAKE3** keys before a physical swap, you must have a **complete ************`new→old`************ LUT** available at flip time. A cold‑start empty LUT will cause immediate misses because objects still live at old addresses.
* The LUT must be **built during a quiet window** by walking the full state and computing BLAKE3 addresses; you cannot populate it “on demand” without global scans.
* **Persist the LUT**: typically a sidecar DB keyed by `BLAKE3 → SHA512Half`, or rely on per‑SLE **new‑side annotation** (`sfBlake3Key`) so any node can rebuild the LUT deterministically by a leaf scan. `sfBlake3Key` helps you rebuild; it does **not** remove the need for a ready‑to‑query LUT at flip.
* Expect to **carry the LUT indefinitely** in non‑move. Its hit‑rate may drop over time only if you later migrate objects (or switch to Big‑Bang).

## Heterogeneous vs Homogeneous state trees

**Homogeneous** means a single canonical keyspace and ordering (one hash algorithm across the whole state tree). **Heterogeneous** means mixed keys/hashes coexisting (some SHA512‑Half, some BLAKE3), even if reads are made to “work.”

**Why this matters**

* **Proofs & ordering**: Homogeneous trees keep proofs simple and iteration stable. Heterogeneous trees complicate inclusion proofs and `succ()`/pagination semantics.
* **Read path**: With heterogeneity, you either guess (dual‑hash walk), add **hints** (local "unused" nodestore bytes), or introduce **new prefixes** (network‑visible). All add complexity.
* **Replay & determinism**: Homogeneous trees let `rules()`+`ledger_index` fully determine hashing. Heterogeneous trees force policy state (when/where items moved) to be consensus‑deterministic and reproduced in replay.
* **Caches & sharing**: Node sharing across ledgers is cleaner in a homogeneous regime; heterogeneity reduces reuse and increases compute.
* **Operational risk**: Mixed eras inflate your attack and bug surface (LUT correctness, marker translation, proof ambiguity).

**How you end up heterogeneous**

* Lazy hashing or “annotate‑only” lazy keys (non‑move).
* Staged moves (on‑touch) that never reach full coverage.
* Introducing new prefixes and treating both spaces as first‑class for long periods.

**How to avoid it**

* **Big‑Bang** swap in `BuildLedger`, then canonical‑only iteration under BLAKE3.
* Keep a narrow **old→new** LUT as a safety net (rebuildable from `sfLegacyKey`), and plan deprecation.

**If you must tolerate heterogeneity (temporarily)**

* Use **context‑bound hashing** (`hash_options{rules(), ledger_index, phase, classifier}`) everywhere.
* Consider **local hint bytes** or **prefixes** only to remove guesswork; define a strict marker policy (normalize to canonical outputs) and accept perf overhead.

## Options matrix — migration + keylet policies

### 1) Migration strategy (what physically moves when)

| Strategy                                                                                                                                                                                                                                         | What moves & when                                                                | Tree heterogeneity | LUT needs                                                                                          | Iteration / pagination                                                                                 | Replay & hashing context                                                    | Operational risk                                   | Pros                                                               | Cons                                                                               |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------- | ------------------ | -------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------- | -------------------------------------------------- | ------------------------------------------------------------------ | ---------------------------------------------------------------------------------- |
| **Big‑Bang (swap in one ledger, in BuildLedger)**                                                                                                                                                                                                | All SLEs rekeyed in a single, quiet, consensus‑gated ledger; stamp `sfLegacyKey` | None after swap    | **old→new** only (temporary; rebuildable from `sfLegacyKey`)                                       | Immediately canonical under BLAKE3; simple markers                                                     | Straightforward (`rules()`, `ledger_index`, `phase` flip once)              | One heavy compute event; needs strict choreography | Clean proofs & ordering; simplest for tools; fast path to perf win | Requires quiet period + consensus hash; “all‑eggs‑one‑basket” ledger               |
| **Lazy keys — moved, per‑tx**                                                                                                                                                                                                                    | Touched SLEs are **moved** to BLAKE3 keys during tx commit                       | Long‑lived         | **old→new** and often **new→old** (for markers)                                                    | Mixed keys; must normalize or translate; highest complexity                                            | Hardest: movement timing is per‑tx; requires full `hash_options` everywhere | Low per‑ledger spike, but constant complexity      | Spreads compute over time                                          | Permanent heterogeneity; iterator/marker headaches; error‑prone                    |
| **Lazy keys — *********************************************************************************************not********************************************************************************************* moved, per‑tx (annotate only)**      | No SLEs move; touched entries get `sfBlake3Key` / annotation only                | Permanent          | **new→old** (lookups by BLAKE3 must alias to old), often also **old→new** if you normalize outputs | Iteration remains in **old** key order unless you add translation; markers inconsistent without bi‑LUT | Hard: you never converge; replay must honor historic “no‑move” semantics    | Low per‑ledger spike                               | Zero relocation churn; simplest writes                             | You never get canonical BLAKE3 ordering/proofs; LUT forever; limited perf win      |
| **Lazy keys — moved, BuildLedger**                                                                                                                                                                                                               | Touched SLEs are **moved** at end of ledger in BuildLedger                       | Medium‑lived       | **old→new** (likely) and sometimes **new→old** (if you want legacy markers to resume cleanly)      | Still mixed; easier to normalize to canonical at ledger boundary                                       | Moderate: movement is per‑ledger; still need `hash_options`                 | Lower spike than Big‑Bang; simpler than per‑tx     | Centralized move step; cleaner tx metadata                         | Still heterogeneous until coverage is high; LUT on hot paths                       |
| **Lazy keys — *********************************************************************************************not********************************************************************************************* moved, BuildLedger (annotate only)** | No SLEs move; annotate touched entries in BuildLedger only                       | Permanent          | **new→old** (and possibly **old→new** if you normalize)                                            | Iteration stays in **old** order; translation needed for consistency                                   | Moderate: policy is per‑ledger but never converges                          | Lowest spike                                       | Cleanest ops; no relocation diffs                                  | Same drawbacks as per‑tx annotate‑only: permanent heterogeneity and LUT dependence |

**Notes:**

* Prefer **canonical‑only iteration** (return new keys) and accept legacy markers as input → reduces need for bidirectional LUT.
* If you insist on round‑tripping legacy markers, you’ll need **bi‑directional LUT** and iterator translation.
* For **annotate‑only (non‑move)** variants: if you choose **Policy C (flip globally at ledger n)**, you **must** prebuild a complete `new→old` LUT for the entire tree before the flip. To avoid this empty‑LUT hazard, choose **Policy A (flip at swap)** until the physical move occurs.

#### 1a) Big‑Bang — non‑move (alias‑only) at a glance

| What moves & when                                                                                                                                                                              | Tree heterogeneity                                                                                                                                              | LUT needs                                                                                                | Iteration/pagination                               | Pros                                                                                                                 | Cons                                                                                                                                                          |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **No storage move at cutover; global keylet flip; annotate all SLEs with ************`sfBlake3Key`************; full ************`new→old`************ LUT ready or rebuildable by leaf scan** | Ledger map: **old** for legacy, **new** for new objects; NodeStore blobs: **full‑tree rewrite** (choose a single blob‑hash algo post‑cutover to avoid guessing) | Permanent `new→old`; **rebuildable from ************`sfBlake3Key`************ by optimized leaf parser** | Old‑order; document marker policy (no translation) | No **map index** relocation; flip is clean; **LUT always accessible**; rollback = behavior flip only if LUT retained | Proofs/ordering stay old; permanent LUT; **one‑time I/O spike** from full‑tree rewrite (mitigated by preflushing background tree); no homogeneous BLAKE3 tree |

### 2) Keylet flip policy (what keylets *emit*) (what keylets *emit*) (what keylets *emit*) (what keylets *emit*) (what keylets *emit*) (what keylets *emit*)

| Policy                              | What keylets return                     | Empty‑LUT risk        | Need global LUT upfront?        | Client‑visible behavior                 | Pros                                   | Cons                                                       |
| ----------------------------------- | --------------------------------------- | --------------------- | ------------------------------- | --------------------------------------- | -------------------------------------- | ---------------------------------------------------------- |
| **A. Flip at swap only**            | Old keys pre‑swap; new keys post‑swap   | None                  | No                              | Single flip; stable semantics           | Simplest; no prep LUT window           | Requires Big‑Bang or near‑equivalent swap moment           |
| **B. Flip per‑SLE (when migrated)** | New for migrated entries; old otherwise | None                  | No                              | Mixed outputs; must normalize iteration | No global LUT build; smoother ramp     | Clients see mixture unless normalized; still heterogeneous |
| **C. Flip globally at ledger n**    | New everywhere from n                   | **High** if LUT empty | **Yes** (build in quiet period) | Clean switch for clients                | Global behavior is uniform immediately | Must precompute `new→old` LUT; higher prep complexity      |

### 3) Hashing decision representation (perf & memory)

| Option                                             | What changes                                                                                                                                 | Memory/Perf impact                            | ABI impact      | Benefit                                            |
| -------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------- | --------------- | -------------------------------------------------- |
| **0. Context‑bound keylets (recommended default)** | Keep returning 32‑byte keys; keylets choose SHA512‑Half vs BLAKE3 using a small `HashCtx` (`rules()`, `ledger_index`, `phase`, `classifier`) | Tiny branch; no heap; cache optional per‑View | None            | Avoids empty‑LUT trap; simplest to roll out        |
| **1. Thin symbolic descriptors (stack‑only)**      | Keylets can return a small descriptor; callers `resolve(desc, ctx)` immediately                                                              | Minimal; POD structs; optional tiny cache     | None externally | Centralizes decision; testable; still light‑weight |
| **2. Full symbolic (iterators/markers only)**      | Iterators carry `{desc, resolved}` to re‑resolve under different contexts                                                                    | Small per‑iterator cache                      | None externally | Makes pagination/replay robust without broad churn |

### 4) NodeStore hinting for heterogeneous reads (only if you *must* support mixed trees)

| Approach                                    | Scope           | Pros                                               | Cons                                                                   |
| ------------------------------------------- | --------------- | -------------------------------------------------- | ---------------------------------------------------------------------- |
| **No hints (dual‑hash walk)**               | Network‑safe    | Simple to reason about; no store changes           | Costly: try‑both‑hashes while walking; awkward                         |
| **Local hint bytes (use 8–9 unused bytes)** | Local only      | Eliminates guesswork on a node; cheap to implement | Not portable; doesn’t show up in proofs; still need amendment plumbing |
| **New hash prefixes in blobs**              | Network‑visible | Clear namespace separation; easier debugging       | Prefix explosion; code churn; proof/back‑compat complexity             |

### 5) Recommended defaults

* **Migration**: Big‑Bang in `BuildLedger` with quiet period + consensus hash; stamp `sfLegacyKey`.
* **Keylets**: Policy **A** (flip at swap) or **B** if you insist on staging; normalize iteration to canonical.
* **LUT**: **old→new** exact‑key alias as a temporary safety net; rebuildable from `sfLegacyKey`.
* **Hashing decision**: **Option 0 (context‑bound keylets)**; reserve symbolics for iterators only if needed.

## Heterogeneous trees and possible NodeStore tweaks

When loading from the NodeStore with a root hash, in principle you could walk down the tree and try hashing each blob’s contents to check whether it matches. At each link, you verify the blob by recomputing its hash. In theory you could even try both SHA-512 Half and BLAKE3 until the structure links up. This would eventually work, but it is inefficient.

To avoid that inefficiency, one idea is to tweak the NodeStore blobs themselves. There are 8–9 unused bytes (currently stored as zeros) that could be repurposed as a hint. Another option is to change the stored hash prefixes, which would act as explicit namespace markers separating SHA-512 and BLAKE3 content. With the ledger index also available, heuristics could guide which algorithm to use. But none of this removes the need for amendment plumbing — you still have to know if the cutover has occurred.

### Versioned prefixes (use the spare byte)

**Goal:** eliminate guessing in mixed/historical contexts by making the blob self‑describing.

* **Design:** keep the 3‑letter class tag and use the 4th byte as an **algorithm version**.

```cpp
enum class HashPrefix : std::uint32_t {
  innerNode_v0 = detail::make_hash_prefix('M','I','N', 0x00), // SHA512Half
  innerNode_v1 = detail::make_hash_prefix('M','I','N', 0x01), // BLAKE3
  leafNode_v0  = detail::make_hash_prefix('M','L','N', 0x00),
  leafNode_v1  = detail::make_hash_prefix('M','L','N', 0x01),
  // add tx/dir variants only if their blob hashing changes too
};
```

* **Read path:** fetch by hash as usual; after you read the blob, the prefix **discriminates** the hashing algorithm used to produce that key. No dual‑hash trial needed to verify/link.
* **Write path:** when (re)serializing a node, choose the version byte from `hash_options.rules()/phase`; parent/child content stays consistent because each node carries its own version.
* **Pros:** zero‑guess verification; offline tools can parse blobs without external context; makes mixed eras debuggable.
* **Cons:** network‑visible change (new prefixes); code churn where prefixes are assumed fixed; doesn’t solve keylet/index aliasing or iteration semantics — it only removes blob‑hash guessing.

**Note:** you can also avoid guessing entirely by keeping **one blob‑hash algorithm per ledger** (homogeneous per‑ledger eras). Then `rules()+ledger_index` suffices. Versioned prefixes mainly help offline tools and any design that tolerates intra‑ledger mixing.

### Lazy migration headaches

If you attempt a lazy migration, you must decide how keys are rehashed. Is it done during metadata creation at the end of transactions? Do you rely on a LUT to map between new and old indexes? If so, where is this LUT state stored? Another idea is to embed a `LedgerIndexBlake3` in entries, so that keylet helpers can create new indexes while CRUD operations translate through a LUT. But this complicates pagination markers and functions like `ReadView::succ()` that return natural keys. You risk situations where the system must be aware of multiple keys per entry.

Questions like pagination markers and `ReadView::succ()` make this even thornier. One approach might be to encode the hash type in the LUT, and maintain it bidirectionally, so when iteration returns a canonical key it can be translated back to the old form if needed. But this doubles the complexity and still forces every path to be LUT‑aware.

By contrast, in the **Big Bang** version the LUT is just a safety net, handling things that could not be automatically rewritten. This is simpler for analysts and avoids perpetual cross-key complexity.

### Why it feels like a headache

Trying to lazily migrate keys means constantly juggling questions:

* Do you move items immediately when the amendment is enabled, or only on first touch?
* If you move them, when exactly: during metadata creation, during BuildLedger along with the SkipList?
* How do you keep CRUD ops working while also updating LUT state?
* How do you handle pagination markers and `succ()` consistently if multiple keys exist? You would need bidirectional.

Every option adds complexity, requires bidirectional LUTs, and forces awareness of dual keyspaces everywhere. This is why the lazy path feels like a perpetual headache, while the Big Bang keeps the pain contained to one well‑known cutover.

## The Big Bang

From here onward, we focus on the **Big‑Bang** approach (one‑ledger atomic rekey). Lazy/staged variants are summarized above.

### Why Big‑Bang is preferred here

* **Homogeneous immediately:** one canonical keyspace the very next ledger → simple proofs, stable iteration/pagination, no dual‑key semantics.
* **No empty‑LUT window:** keylets flip at the swap; the LUT is **old→new** only, narrow in scope, and realistically deprecable.
* **Deterministic & replay‑friendly:** a single, well‑known cutover ledger anchors tooling and historical verification.
* **Operationally contained risk:** compute is concentrated into the quiet window with explicit consensus checkpoints (single or double), not smeared across months.
* **Cleaner dev/ops surface:** fewer code paths need LUT/translation logic; easier to reason about `succ()`/markers and caches.

### Variant: Big‑Bang “non‑move” (alias‑only swap)

**What it is:** at the cutover ledger, **annotate the entire state tree** by stamping every SLE with its BLAKE3 address (e.g., `sfBlake3Key`). **Do not** rewrite storage keys. During the quiet window, prebuild a complete `new→old` LUT **or** rely on the new field so any node can rebuild the LUT deterministically by scanning leaves with an optimized parser. Flip keylets to emit BLAKE3. Optionally commit a small on‑ledger **annotation/LUT commitment hash** in `MigrationState` so operators can verify their sidecar.

**How it behaves:** point lookups by BLAKE3 resolve via the LUT; writes/erases resolve to the canonical **old** storage key before touching disk; **new objects** are stored under **BLAKE3** keys (post‑flip); legacy callers may be served by a grace **`SHA512Half → BLAKE3`** alias for *new* objects. Iteration/pagination remain in the old order for legacy entries (document marker policy).

**I/O reality & mitigation:**

* Annotating every leaf **changes its bytes**, forcing a **full‑tree NodeStore rewrite** (leaf blob hashes change; inner nodes update). This is a **mass write**, even though map indexes don’t relocate.
* Mitigate the spike by **streaming/staged flush** of the staging tree during BuildLedger (chunked passes), back‑pressure on caches, and rate‑limited node writes; total bytes remain \~“rewrite the tree once.”

**LUT reconstruction paths:**

* **From annotation (fastest):** for each leaf, read `sfBlake3Key` and the current (old) key; record `BLAKE3 → old`.
* **From recompute (belt‑and‑suspenders):** recompute BLAKE3 via keylet helpers from identity components and pair with the observed old key.

**Pros:** no **map index** relocation for legacy entries; minimal end‑user surprise; clean flip semantics; **LUT always reconstructible** from the annotated tree; **rollback is behavioral‑only if the LUT is retained**.

**Cons:** ordering/proofs remain old indefinitely; LUT becomes permanent; you forgo a homogeneous BLAKE3 tree and its simplifications; **full‑tree NodeStore rewrite** (leaf annotation changes bytes → new blob hashes → inner nodes update) causing a one‑time I/O spike.

**Rollback reality:** Once clients rely on BLAKE3 keys on the wire, a “rollback” without a LUT breaks them. Practical rollback means flipping keylet behavior back to SHA512‑Half **and** continuing to serve BLAKE3 lookups via the LUT indefinitely (or performing a reverse‑migration). In other words, rollback is only “easy” if you accept a **permanent LUT**.

**When to pick:** you want Big‑Bang’s clean flip and operational containment, but can’t (or don’t want to) rewrite the entire state tree; you still want a deterministic, cheap way to rebuild the LUT by scanning.

### How to message this (without scaring users)

**Elevator pitch**

> We’re flipping key derivation to BLAKE3 for *new* addresses, but we’re **not relocating existing entries**. We annotate the tree in a maintenance window, so old data stays where it is, new data goes to BLAKE3 addresses, and both key forms work via an alias. Transactions, TxIDs, and signatures don’t change.

**What users/operators should expect**

* **No surprise breakage:** Old clients that synthesize SHA512‑Half keys still read old objects; new clients can use BLAKE3 keys everywhere (old objects resolve via alias).
* **New vs old objects:** Old objects remain at their old locations; **new objects** are stored at **BLAKE3** locations. A **grace alias** can accept SHA512‑Half for *new* objects for a limited time.
* **Ordering/proofs unchanged for old entries:** Iteration order and proofs remain canonical‑old for legacy entries. No bidirectional iteration translation.
* **TxIDs & signing stay the same:** Transaction IDs and signing digests are **unchanged**; do **not** hand‑derive ledger indexes—use keylet APIs.
* **One‑time write spike (planned):** Annotating every leaf causes a **single full‑tree blob rewrite** during the quiet window; we stage/stream this as part of `BuildLedger`.

**Soundbite**

> *“Not a scary rekey-everything rewrite.”* It’s a one‑time annotation and an API flip: old stays reachable, new is faster, and we give legacy callers a grace window.

### Decision & next steps (short list)

1. **Amendment & timing:** finalize `featureBlake3Migration`, `MIGRATION_TIME`, and quiet‑period length.
2. **BuildLedger swap/annotate pass:** implement two‑pass **rekey** (plan → commit), **or** two‑pass **annotate** (stamp `sfBlake3Key` on all SLEs). For rekey, stamp `sfLegacyKey` and materialize **old→new** LUT; for non‑move, stamp `sfBlake3Key` and materialize **new→old** LUT (both rebuildable by leaf scan).
3. **API rules:** reads/writes = canonical‑first, LUT‑on‑miss (point lookups only); **iteration is canonical‑only**; document marker semantics.
4. **Hash context plumbing:** ensure `hash_options{rules(), ledger_index, phase, classifier}` are available down to `SHAMap::getHash()` and relevant callers.
5. **Consensus choreography:** pick **single** vs **double** hash checkpoint; wire pseudo‑tx for the pre‑hash if using two‑step.
6. **Telemetry & deprecation:** ship metrics for LUT hit‑rate and schedule a sunset once hits are negligible.
7. **Test plan:** simulate slow validators, partial LUT rebuilds, replay across the swap, and hook workloads with hardcoded keys.

## Governance first: permission to cut over

Such a migration cannot be unilateral. An amendment (`featureBlake3Migration`) acts as the governance switch, enabling the network to authorize the cutover. This amendment does not itself rekey the world, but it declares consensus intent: from a certain point, ledgers may be rebuilt under the new rules.

A pseudo-transaction (e.g. `ttHASH_MIGRATION`) provides the on-ledger coordination. It marks the trigger point, updates the migration state SLE, and ensures every validator knows exactly *when* and *what* to execute.

## Why not just do it in the pseudo-transaction?

A naive attempt to treat the entire migration as a simple pseudo-transaction — a one-off entry applied like any other — would explode into metadata churn, duplicate entries, and lost referential integrity. The scale of rekeying every SLE makes it unsuitable for a normal transaction context; it has to run in a special execution venue like `BuildLedger` to remain atomic and manageable.

## Choose the battlefield: BuildLedger

The right place to run the migration is inside `BuildLedger` — after applying the (quiet) transaction set, and before finalization. This avoids flooding transaction metadata with millions of deletes and creates, and guarantees atomicity: one ledger before = SHA-512 Half; one ledger after = BLAKE3.

This is also exactly where other ledger-maintenance updates happen: for example `updateNegativeUNL()` runs when processing a flag ledger if the feature is enabled, and `updateSkipList()` is invoked just before flushing SHAMap nodes to the NodeStore. By piggybacking the migration here, it integrates cleanly into the existing lifecycle:

```cpp
if (built->isFlagLedger() && built->rules().enabled(featureNegativeUNL))
{
    built->updateNegativeUNL();
}

OpenView accum(&*built);
applyTxs(accum, built);
accum.apply(*built);

built->updateSkipList();

// Flush modified SHAMap nodes to NodeStore
built->stateMap().flushDirty(hotACCOUNT_NODE);
built->txMap().flushDirty(hotTRANSACTION_NODE);
built->unshare();
```

By inserting the BLAKE3 migration pass into this sequence, it runs atomically alongside the skip list and NegativeUNL updates, ensuring the new canonical tree is finalized consistently.&#x20;

## Hashing and consensus choreography

It may make sense to stretch the choreography into more than one consensus checkpoint, especially given the amount of compute involved. A possible flow:

* **Quiet period** — block transactions so everyone is aligned.
* **Phase 1: Hash the static tree** — compute a BLAKE3 hash of the ledger state, excluding churny structures like skip lists and the migration state.
* **Consensus** — validators agree on this static-hash checkpoint.
* **Phase 2: Hash the full tree** — compute the full state tree hash under BLAKE3.
* **Consensus** — converge again on the complete view.
* **Atomic swap** — only after both steps succeed, rewrite the ledger under new keys.

This extra step could make it easier for validators to stay in sync without network drift, because they checkpoint on a smaller, stable hash before tackling the full-tree rebuild. It reduces wasted compute if things diverge. The downside is protocol complexity: two ballots instead of one. But given the gnarliness of concurrent full-tree rekeying, a double consensus phase could be safer in practice.

Supporting this implies the hash function must be aware of more than just `ledger_index`; it also needs `rules()` (to know if the amendment is enabled) and an explicit state flag indicating whether the swap is pending, in progress, or complete. To safely support background builds of multiple tree variants, `hash_options` must be plumbed everywhere — from `SHAMap::getHash()` down into all call sites, and even up into callers.

## Two-pass rekey with a safety rope

* **Pass 1 (plan)**: Walk the state tree, compute new BLAKE3 keys, build an in-memory LUT (old→new), and stamp each SLE with its legacy key (`sfLegacyKey`).
* **Pass 2 (commit)**: Rebuild the SHAMap with BLAKE3 keys, rewrite all directories and secondary structures from the LUT, and finalize the new canonical tree.

This two-pass structure ensures determinism and lets every validator converge on the same new map without risk of divergence.

## Keep consensus boring during the scary bit

Migration must not race against normal transaction flow. The procedure anchors on **network time**, not ledger index. Once a ledger closes with `closeTime ≥ MIGRATION_TIME`, the network enters a quiet period: all user and pseudo-transactions are blocked, only trivial skip list mechanics advance. During this window, everyone builds the same hash in the background.

When consensus converges on the special BLAKE3 hash (excluding skip lists and migration state), it appears in a validated ledger. In the next ledger, the atomic swap happens — one big bang, then back to normal life.

## Owning the ugly edges (hooks and hardcoded keys)

Hooks may carry hardcoded 32-byte constants. Detecting them with static analysis is brittle; runtime tracing is too heavy. Instead, the LUT strategy provides a compatibility shim: lookups can still resolve old keys, while all new creations require canonical BLAKE3 keys. Over time, policy can deprecate this fallback.

---
