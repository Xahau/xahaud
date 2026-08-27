# Amendment-gated peer protocol features

Some amendments change the bytes carried by the peer protocol. Advertising
support during the HTTP upgrade is not enough: once such an amendment is
active, a capable server must stop maintaining sessions with peers that did
not negotiate the matching capability.

Consensus Entropy is the first feature using this mechanism.

## The three different questions

These checks are deliberately separate:

```cpp
// What did this particular connection negotiate?
peer->supportsFeature(ProtocolFeature::ConsensusEntropy);

// Is the amendment active in this ledger's rule set?
ledger.rules().enabled(featureConsensusEntropy);

// Has this process made the capability mandatory for overlay sessions?
app.overlay().isProtocolFeatureRequired(
    ProtocolFeature::ConsensusEntropy);
```

The first value is cached when `PeerImp` is constructed from the handshake. It
does not change during that connection. The second is ledger state. The third
is a process-lifetime, monotonic admission rule derived from accepted ledger
state.

## Activation boundary

For the observed rollout, the relevant ledgers are:

| Ledger | Meaning | Overlay rule |
|---:|---|---|
| 255 | validators advertise `VOTE` | capability optional |
| 256 | flag ledger (`seq % 256 == 0`) | capability optional |
| 257 | contains `EnableAmendment`; its resulting rules enable CE | install the required-capability gate |
| 258 | first consensus round built with CE active | only capable sessions may participate |

`NetworkOPsImp::beginConsensus` is the boundary. Its `prevLedger` is the newly
closed ledger whose rules govern the round about to start. If those rules
enable Consensus Entropy, it calls:

```cpp
app.overlay().requireProtocolFeature(
    ProtocolFeature::ConsensusEntropy);
```

This happens before `RCLConsensus::startRound`. Thus accepting ledger 257
disconnects incompatible sessions before any ledger-258 proposal is created or
relayed. Local `VOTE`/`VETO` state is not used: a validator that locally vetoed
the amendment must still obey the rules of the ledger the network accepted.

The same path runs for the first consensus round after a restart or catch-up,
so a node starting from an already-enabled ledger installs the requirement
before joining consensus.

## Session behavior

`requireProtocolFeature` first publishes the requirement atomically, then
visits active peers and closes every session whose cached capability is false.
Publishing first is important: concurrent handshakes see the new admission
rule.

Both handshake directions reject missing capabilities after identity and
security-cookie verification but before PeerFinder activation:

- inbound requests must advertise `xahau-consensus-entropy=1`;
- outbound responses must echo the token, proving that both ends negotiated it.

There is also a second check when a `PeerImp` enters the active set. It closes
the race in which the requirement changes after the HTTP headers were checked
but before the peer was inserted. Cluster membership and reserved slots do not
bypass the requirement.

Calling `requireProtocolFeature` again is harmless. Requirements do not clear
during the process lifetime because amendments do not deactivate on a ledger's
descendant chain.

## Why disconnect instead of filtering only CE messages

After activation, the peer and ledger protocols are one compatibility unit.
An old peer can remain TCP-connected and answer some RPCs while silently
dropping extended proposals or returning incomplete ledger JSON. That is a
misleading zombie state, not interoperability. Disconnecting makes the failure
explicit and prevents incompatible proposal bytes from crossing the boundary.

## Rollout window

Operators may use an earlier `activation - window` point to drain or alert on
legacy sessions. That is an operational policy, not the correctness boundary.
The hard gate is the accepted enable-amendment ledger, because it is the first
deterministic point at which every honest node knows the rules for the next
round. This implementation adds the hard gate; it does not add a configurable
pre-activation drain window.

## Non-goals

- The token does not create a new `XRPL/2.x` version.
- It does not downgrade proposal encoding per destination.
- It does not include request-ID negotiation.
- The capability assertion is part of the authenticated TLS peer handshake,
  but it is still a compatibility claim; normal message parsing remains
  defensive.
