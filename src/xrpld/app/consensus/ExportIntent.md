# Export Design Transition

The former same-ledger Export sidecar authority has been removed. This file is
intentionally minimal until its replacement is implemented and reviewed.

## Current State

- Proposal `exportSignatures` payloads remain bounded and are signed through
  `exportSignaturesHash`.
- Signature harvesting and collection remain available as transport plumbing.
- `ttEXPORT_SIGNATURES` remains readable by direct apply and historical replay.
- No Export signature-set root, establish-phase Export gate, accepted-root
  state, or `onPreBuild` witness materializer remains.
- There is currently no live network producer for a canonical Export witness;
  network Export retries or expires without one.

## TODO

- Persist the validated Export intent/latch and its chosen validator universe.
- Release and relay validator signatures only after the origin ledger validates.
- Require local qC before a witness becomes an ordinary transaction-set
  candidate; peer popularity must never substitute for missing evidence.
- Decide and specify the canonical next-ledger pseudo. The leading option is a
  self-verifying pseudo containing the fully assembled ordinary XRPL multisigned
  transaction, linked to the origin Export transaction and source ledger.
- Keep callback identity bound to the normalized unsigned target intent, not to
  one signature-envelope-dependent target transaction ID.
- Specify retry, expiry, bump, subscription, Import, and historical replay
  behavior before enabling live completion.

The active design record and evidence live under `.ai-docs/`; source comments
should not recreate superseded designs while these TODOs remain open.
