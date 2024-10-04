//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#ifndef RIPPLE_TX_SETSIGNERLIST_H_INCLUDED
#define RIPPLE_TX_SETSIGNERLIST_H_INCLUDED

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/tx/impl/SignerEntries.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/Rules.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STTx.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ripple {

/**
See the README.md for an overview of the SetSignerList transaction that
this class implements.
*/
class SetSignerList : public Transactor
{
private:
    // Values determined during preCompute for use later.
    enum Operation { unknown, set, destroy };
    Operation do_{unknown};
    std::uint32_t quorum_{0};
    std::vector<SignerEntries::SignerEntry> signers_;

public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Blocker};

    explicit SetSignerList(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    TER
    doApply() override;
    void
    preCompute() override;

    // Interface used by DeleteAccount
    static TER
    removeFromLedger(
        Application& app,
        ApplyView& view,
        AccountID const& account,
        beast::Journal j);

    static TER
    removeSignersFromLedger(
        Application& app,
        ApplyView& view,
        Keylet const& accountKeylet,
        Keylet const& ownerDirKeylet,
        Keylet const& signerListKeylet,
        beast::Journal j);

    static int
    signerCountBasedOwnerCountDelta(std::size_t entryCount, Rules const& rules);

private:
    static void
    writeSignersToSLE(
        ApplyView& view,
        SLE::pointer const& ledgerEntry,
        std::uint32_t flags,
        std::uint32_t quorum,
        std::vector<SignerEntries::SignerEntry> const& signers);

    static std::tuple<
        NotTEC,
        std::uint32_t,
        std::vector<SignerEntries::SignerEntry>,
        Operation>
    determineOperation(STTx const& tx, ApplyFlags flags, beast::Journal j);

    TER
    replaceSignerList();
    TER
    destroySignerList();

    void
    writeSignersToSLE(SLE::pointer const& ledgerEntry, std::uint32_t flags)
        const;

public:
    static NotTEC
    validateQuorumAndSignerEntries(
        std::uint32_t quorum,
        std::vector<SignerEntries::SignerEntry> const& signers,
        AccountID const& account,
        beast::Journal j,
        Rules const&);

    template <typename V>
    static TER
    replaceSignersFromLedger(
        Application& app,
        V& view,
        beast::Journal j,
        AccountID const& acc,
        std::uint32_t quorum,
        std::vector<SignerEntries::SignerEntry> const& signers,
        XRPAmount const mPriorBalance)
    {
        auto const accountKeylet = keylet::account(acc);
        auto const ownerDirKeylet = keylet::ownerDir(acc);
        auto const signerListKeylet = keylet::signers(acc);

        // This may be either a create or a replace.  Preemptively remove any
        // old signer list.  May reduce the reserve, so this is done before
        // checking the reserve.
        if (TER const ter = removeSignersFromLedger(
                app, view, accountKeylet, ownerDirKeylet, signerListKeylet, j))
            return ter;

        auto const sle = view.peek(accountKeylet);
        if (!sle)
            return tefINTERNAL;

        // Compute new reserve.  Verify the account has funds to meet the
        // reserve.
        std::uint32_t const oldOwnerCount{(*sle)[sfOwnerCount]};

        // The required reserve changes based on featureMultiSignReserve...
        int addedOwnerCount{1};
        std::uint32_t flags{lsfOneOwnerCount};
        if (!view.rules().enabled(featureMultiSignReserve))
        {
            addedOwnerCount =
                signerCountBasedOwnerCountDelta(signers.size(), view.rules());
            flags = 0;
        }

        XRPAmount const newReserve{
            view.fees().accountReserve(oldOwnerCount + addedOwnerCount)};

        // We check the reserve against the starting balance because we want to
        // allow dipping into the reserve to pay fees.  This behavior is
        // consistent with CreateTicket.
        if (mPriorBalance < newReserve)
            return tecINSUFFICIENT_RESERVE;

        // Everything's ducky.  Add the ltSIGNER_LIST to the ledger.
        auto signerList = std::make_shared<SLE>(signerListKeylet);
        view.insert(signerList);
        writeSignersToSLE(view, signerList, flags, quorum, signers);

        // Add the signer list to the account's directory.
        auto const page = view.dirInsert(
            ownerDirKeylet, signerListKeylet, describeOwnerDir(acc));

        JLOG(j.trace()) << "Create signer list for account " << toBase58(acc)
                        << ": " << (page ? "success" : "failure");

        if (!page)
            return tecDIR_FULL;

        signerList->setFieldU64(sfOwnerNode, *page);

        // If we succeeded, the new entry counts against the
        // creator's reserve.
        adjustOwnerCount(view, sle, addedOwnerCount, j);
        return tesSUCCESS;
    }
};

}  // namespace ripple

#endif
