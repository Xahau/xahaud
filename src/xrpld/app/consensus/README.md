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

Xahau-specific proposal sidecars and ConsensusEntropy/RNG follow the
invariants in [`ConsensusExtensionsDesign.md`](ConsensusExtensionsDesign.md)
and [`ConsensusEntropyIntent.md`](ConsensusEntropyIntent.md). Read those notes
before changing extension quorum, local sidecar snapshots, fallback behavior,
or the replay witness (the `ttCONSENSUS_ENTROPY` pseudo-transaction).
