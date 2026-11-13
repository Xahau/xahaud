//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_TX_INGEST_H_INCLUDED
#define RIPPLE_TX_INGEST_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/shamap/SHAMapTreeNode.h>
#include <xrpl/protocol/LedgerHeader.h>
#include <xrpl/protocol/STTx.h>
#include <map>

namespace ripple {

/** Ingest: what transactions on another network pay, proven (featureExport).

    An XPOF (proof of fees) is an XPOP without a transaction: a ledger's
    header and its parent's, the UNL and validations that prove it, every
    inner node of its transaction tree (of the leaves only the hashes), and
    `samples` of its transactions in full.

    What it proves. Every leaf is a transaction that applied and burned its
    fee, so parent.coins - coins is what they burned between them. Nobody can
    present a child hash the tree does not hold, so no entry counts more
    leaves than there are; one that leaves an inner node out only counts
    fewer, which makes the leaf count a lower bound and nothing more. A mean
    would be one whale's to own (and would lean on that count), so the fee
    is a median over leaves the ledger's own hash picks (sampleLeaves):
    neither the relay nor anyone paying fees there chooses them, and every
    inner node on the way to them must be given, so it is the same whatever
    else a proof leaves out.

    What it records, in the network's ltEXPORT_FEE: the latest ledger's
    index, burn and median, and sfExportFeeUnit, the per-signature fee
    (Fee / (1 + signers)) xport_prepare() prices an export by for a hook
    that sets no Fee (Export::feeUnit). Each newer ledger folds the median
    into it by eighths (fold), never below Export::xrplBaseFee. The index is
    also the clock exports expire by. A hook reads these with xport_fee().
    Beside them, for admitting the next proof only (below): this network's
    clock when the record was taken, and the leaves its proof counted.

    Nobody's transaction. A proof is its own authority, so a ttINGEST has no
    account: it is the one envelope `make` builds around the proof, from
    account zero, unsigned, unsequenced and free, and whoever holds a proof
    may submit it. It is not a pseudo-transaction (it is relayed, and comes
    from outside the network), but it applies like one: nobody is charged,
    no Sequence is used and no hook runs, there being no account to have
    one. In place of a signature the proof itself is checked wherever a
    signature would be (checkValidity).

    One sample a minute. A proof is free and up to maxBlob long, so a
    network's record is taken at most once per `spacing` of this network's
    own clock, the parent close time in the header of the ledger a proof is
    applied to (tefALREADY sooner), and only from a ledger strictly newer
    than the one recorded (tefPAST_SEQ otherwise): the record only moves
    forward.

    The fullest proof is the sample. Of the proofs offered for one minute,
    the one that counts the most leaves is the one to keep: a count is a
    lower bound that cannot be inflated, so that is the proof that leaves
    the least out, and of several ledgers the one that most transactions
    paid in, which is the hardest for whoever is submitting to have chosen
    for what it holds. The envelope repeats the count the proof arrives at
    (sfTransactionCount, checked against it like sfLedgerSequence), so
    that it can be compared without parsing anything, twice over:

      - a transaction set applies its ttINGESTs fullest first
        (CanonicalTXSet), so in a closed ledger the fullest valid proof is
        the one that lands and every other is refused for counting no more;
      - until that ledger closes, a proof that counts strictly more leaves
        than the one recorded in it, of that ledger or a newer one, takes
        its place (tefALREADY otherwise), so an open ledger passes a
        fuller proof on rather than stopping at the first to arrive.

    That second comparison is why the record keeps the count at all (an
    open ledger does not thread an entry to the transaction that changed
    it, so there is nowhere else to read it from). It is the bar a rival
    must clear and nothing more: burn and median are the same however much
    of the tree a proof gives, so nothing else rests on it, and neither
    xport_fee() nor export_info reports it.

    The list that proves a ledger must be one trusted for sfRemoteNetworkID
    (Import::vlNetwork), and in date: a proof that is not, or not yet, is
    refused with a tef, not as malformed, so that the one id it has is not
    remembered as bad by a node that only saw it too early. */
class Ingest : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    /** Leaves whose transactions an entry carries in full */
    static constexpr int samples = 8;

    /** The largest XPOF accepted, as for an XPOP (Import) */
    static constexpr std::size_t maxBlob = 512 * 1024;

    /** The least time between two records of one network, in seconds of
        this network's parent close time */
    static constexpr std::uint32_t spacing = 60;

    using Nodes = std::map<uint256, std::shared_ptr<SHAMapTreeNode>>;

    /** The verified content of an XPOF */
    struct Xpof
    {
        LedgerInfo ledger;  // hash computed
        std::uint64_t burn;
        std::uint32_t leaves;
        std::uint32_t median;  // drops per signature over the samples
        PublicKey vlKey;       // the UNL's publisher, checked by preclaim
        std::uint32_t vlSeq, vlEffective, vlExpiration;
    };

    /** The whole XPOF checked, apart from what needs a ledger: that vlKey
        is trusted, the list in date and the ledger newer than recorded. */
    static std::optional<Xpof>
    parse(Slice blob, beast::Journal j);

    /** The one transaction that may carry `blob`, an XPOF of ledger `seq`
        of network `remote` counting `leaves`, on network `networkID` (this
        one): Account zero, Sequence 0, Fee 0, an empty SigningPubKey,
        NetworkID as every transaction here has it or has it not
        (preflight0), and the four fields of its own. No signature, and no
        other field: it is free and unsigned, so any would be a way to put
        data into the ledger for nothing and to give one proof as many ids
        as wanted. */
    static STTx
    make(
        std::uint32_t networkID,
        std::uint32_t remote,
        std::uint32_t seq,
        std::uint32_t leaves,
        Slice blob);

    /** Whether `tx` is, byte for byte, what `make` builds from its own
        sfRemoteNetworkID, sfLedgerSequence, sfTransactionCount and sfBlob */
    static bool
    isCanonical(STTx const& tx, std::uint32_t networkID) noexcept;

    /** What `tx` proves, if it is canonical (isCanonical) and its blob an
        XPOF (parse) of the ledger it names, counting the leaves it says,
        no larger than maxBlob. All a ttINGEST can be held to without a
        ledger: what checkValidity requires in place of a signature, and
        preflight of it. */
    static std::optional<Xpof>
    proof(STTx const& tx, std::uint32_t networkID, beast::Journal j);

    /** The leaf hashes a complete tree for `ledger` with root `root` must
        carry in full: for sample i, descend from the root along the nibbles
        of sha512Half(ledger, i), taking at each inner node that branch or
        the next non-empty one above it, wrapping. Every inner node has at
        least two children, so each path ends at a leaf; `nodes` must hold
        every inner node on it. Empty if the tree is incomplete there. */
    static std::vector<uint256>
    sampleLeaves(
        Nodes const& nodes,
        uint256 const& root,
        uint256 const& ledger);

    /** The unit folded `sample` into */
    static std::uint32_t
    fold(std::uint32_t unit, std::uint32_t sample);

    explicit Ingest(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;

    void
    preCompute() override
    {
        // there is no account
    }
};

}  // namespace ripple

#endif
