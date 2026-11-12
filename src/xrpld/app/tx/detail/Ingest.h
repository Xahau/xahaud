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
    fewer. So among entries for one ledger the most leaves wins, and one
    honest relay is enough. A mean would be one whale's to own, so the fee is
    a median over leaves the ledger's own hash picks (sampleLeaves): neither
    the relay nor anyone paying fees there chooses them.

    What it records, in the network's ltEXPORT_FEE: the latest ledger's
    index, leaves, burn and median, and sfExportFeeUnit, the per-signature
    fee (Fee / (1 + signers)) xport_prepare() prices an export by for a hook
    that sets no Fee (Export::feeUnit). Each newer ledger folds the median
    into it by eighths (fold), never below Export::xrplBaseFee. The index is
    also the clock exports expire by. A hook reads all of it with
    xport_fee().

    An entry for a ledger newer than the one recorded is free; one for the
    same ledger must carry more leaves and pays the base fee. The list that
    proves it must be one trusted for sfRemoteNetworkID (Import::vlNetwork). */
class Ingest : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    /** Leaves whose transactions an entry carries in full */
    static constexpr int samples = 8;

    /** The largest XPOF accepted, as for an XPOP (Import) */
    static constexpr std::size_t maxBlob = 512 * 1024;

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
};

}  // namespace ripple

#endif
