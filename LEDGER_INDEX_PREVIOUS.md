Yes! You’re onto something quite elegant there. Let me break it down:

---

### 🔑 LedgerEntry “PriorKey” field

If each SLE carried an **optional `LedgerIndexPriorSha512`** (or whatever name), then:

* When you rekey at cutover, you don’t lose continuity.
* Anyone walking the ledger can trivially build the old→new LUT on the fly.
* For “day-to-day” operations, the node only needs the canonical keys.

This basically lets you say:

* **Canonical view** = new Blake3 keys.
* **Continuity view** = traverse new keys, but if you need to resolve old references, follow the `LedgerIndexPriorSha512` link.

---

### 📊 For analysis and data science

You can now reconstruct historical continuity without keeping a giant external LUT file. Just walk the ledger and collect `(old, new)` pairs from the entries.

This means explorers, forensic tools, compliance, etc. can:

* Map old keys to new at any point.
* Handle partially migrated networks (some ledgers pre-cutover, some post).

---

### 🟢 Operational benefits

* Validators don’t need to bloat memory with a global LUT — it’s **distributed inside the ledger itself**.
* New ledgers “just work” for CRUD.
* Old references (hooks, weird integrations) can be handled until people migrate.

---

### ⚠️ Caveats

1. **Storage overhead**: each SLE gets \~32 bytes extra. But given only \~600k objects in the state tree, that’s trivial compared to the win.
2. **Hardcoded keys in hooks/wasm**: still a landmine — if someone has *literally baked in* the old canonical key, they’ll need the LUT or breakage handling at ApplyView.
3. **Consensus rules**: adding this field changes serialization — so it’s an amendment, not just an operator convenience.

---

### 🔄 Proposal workflow

1. Cutover ledger → during bookkeeping, every object is rekeyed into the new canonical tree.
2. At the same time, each new object gets `LedgerIndexPriorSha512 = old_key`.
3. From then on, **every ledger post-cutover contains the LUT implicitly**.
4. After some deprecation period, once nobody references `LedgerIndexPriorSha512`, you can prune support.

---

So in practice:

* **Validators** only care about the canonical map.
* **Explorers, auditors, backward-compat systems** can lazily reconstruct the LUT.
* You don’t poison `succ()` or iteration with mixed domains.

---

This actually solves both your goals:

* Day-to-day simplicity (Blake-only canonical).
* Historical continuity (walk the field if you need).

---

Want me to sketch how `ReadView::read(Keylet)` could transparently try `LUT` lookups only if the canonical miss happens, using that new field? That would make it zero-effort for old call sites.
