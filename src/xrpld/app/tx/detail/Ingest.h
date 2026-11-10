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

/** Ingest: what transactions on the other network pay, proven (featureExport).

    An XPOF (proof of fees) is an XPOP without a transaction: the header of
    a ledger of another network and its parent, the UNL and validations that
    prove it, and
    its transaction tree down to the leaves — every inner node, but of the
    leaves only the hashes. Nothing about any one transaction is recorded.

    What it proves. Leaves are the transactions that applied there, tes or
    tec, and every one burned its fee: parent.coins - coins is exactly what
    they burned between them. Nobody can present a child hash the tree does
    not hold, so no entry counts more leaves than there are; one that leaves
    an inner node out and lets its hash pass for a leaf only counts fewer. So
    among entries for one ledger the most leaves wins, and one honest relay
    is enough. A burn divided by a count is a mean, though, and one
    transaction paying 100 XRP owns the mean. So the fee is taken from a
    median instead: `samples` leaves chosen by the ledger's own hash
    (sampleLeaves), which the entry must carry in full, tx and meta. Neither
    the relay nor anyone paying fees there picks which; to move the median
    one must own a majority of every sampled ledger's transactions.

    What it records, in the one ltEXPORT_FEE: the latest ledger's leaves,
    burn and median (reference, see sfExportFeeUnit's neighbours), and
    sfExportFeeUnit, the per-signature fee (Fee / (1 + signers)) that xport()
    fills in for a hook that sets none: a median folded into the previous
    unit as each newer ledger arrives, by eighths, never below
    Export::xrplBaseFee. A hook that sets its own Fee is left alone.

    Who pays. An entry for a ledger newer than the one recorded for its
    network is free; one for the same ledger with more leaves pays the base
    fee.

    Networks. sfRemoteNetworkID names the network the ledger belongs to;
    the list that proves it must be one Import trusts for that network
    (Import::vlNetwork), and each network has an ltEXPORT_FEE of its own. */
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
