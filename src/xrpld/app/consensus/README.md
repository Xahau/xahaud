# RCL Consensus

This directory holds the types and classes needed
to connect the generic consensus algorithm to the
rippled-specific instance of consensus.

  * `RCLCxTx` adapts a `SHAMapItem` transaction.
  * `RCLCxTxSet` adapts a `SHAMap` to represent a set of transactions.
  * `RCLCxLedger` adapts a `Ledger`.
  * `RCLConsensus` implements the requirements of the generic
    `Consensus` class by connecting to the rest of the `rippled`
    application.

Xahau-specific proposal sidecars, ConsensusEntropy/RNG, and export signature
convergence follow the invariants in
[`ConsensusExtensionsDesign.md`](ConsensusExtensionsDesign.md),
[`ConsensusEntropyIntent.md`](ConsensusEntropyIntent.md), and
[`ExportIntent.md`](ExportIntent.md). Read those notes before changing extension
quorum, sidecar sync, fallback behavior, or replay witnesses.
