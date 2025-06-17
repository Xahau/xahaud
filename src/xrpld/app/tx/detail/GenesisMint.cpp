//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/tx/impl/GenesisMint.h>
#include <ripple/app/tx/impl/Import.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/st.h>

namespace ripple {

TxConsequences
GenesisMint::makeTxConsequences(PreflightContext const& ctx)
{
    // RH TODO: review this
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
GenesisMint::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureHooks))
        return temDISABLED;

    if (!ctx.rules.enabled(featureXahauGenesis))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;

    auto const id = ctx.tx[sfAccount];

    static auto const genesisAccountId = calcAccountID(
        generateKeyPair(KeyType::secp256k1, generateSeed("masterpassphrase"))
            .first);

    if (id != genesisAccountId || !tx.isFieldPresent(sfEmitDetails))
    {
        JLOG(ctx.j.warn()) << "GenesisMint: can only be used by the genesis "
                              "account in an emitted transaction.";
        return temMALFORMED;
    }

    auto const& dests = tx.getFieldArray(sfGenesisMints);
    if (dests.empty())
    {
        JLOG(ctx.j.warn()) << "GenesisMint: destinations array empty.";
        return temMALFORMED;
    }

    if (dests.size() > 512)
    {
        JLOG(ctx.j.warn())
            << "GenesisMint: destinations array exceeds 512 entries.";
        return temMALFORMED;
    }

    bool const allowDuplicates = ctx.rules.enabled(fixXahauV1);

    std::unordered_set<AccountID> alreadySeen;
    for (auto const& dest : dests)
    {
        if (dest.getFName() != sfGenesisMint)
        {
            JLOG(ctx.j.warn()) << "GenesisMint: destinations array contained "
                                  "an invalid entry.";
            return temMALFORMED;
        }

        if (!dest.isFieldPresent(sfDestination))
        {
            JLOG(ctx.j.warn())
                << "GenesisMint: DestinationField missing in array entry.";
            return temMALFORMED;
        }

        bool const hasAmt = dest.isFieldPresent(sfAmount);
        bool const hasMarks = dest.isFieldPresent(sfGovernanceMarks);
        bool const hasFlags = dest.isFieldPresent(sfGovernanceFlags);

        if (!hasAmt && !hasMarks && !hasFlags)
        {
            JLOG(ctx.j.warn())
                << "GenesisMint: each destination must have at least one of: "
                << "sfAmount, sfGovernanceFlags, sfGovernance marks.";
            return temMALFORMED;
        }

        if (hasAmt)
        {
            auto const amt = dest.getFieldAmount(sfAmount);
            if (!isXRP(amt))
            {
                JLOG(ctx.j.warn())
                    << "GenesisMint: only native amounts can be minted.";
                return temMALFORMED;
            }

            if (amt < beast::zero)
            {
                JLOG(ctx.j.warn())
                    << "GenesisMint: only non-negative amounts can be minted.";
                return temMALFORMED;
            }

            if (amt.xrp().drops() > 10'000'000'000'000ULL)
            {
                JLOG(ctx.j.warn()) << "GenesisMint: cannot mint more than 10MM "
                                      "in a single txn.";
                return temMALFORMED;
            }
        }

        auto const accid = dest.getAccountID(sfDestination);

        if (accid == noAccount() || accid == xrpAccount())
        {
            JLOG(ctx.j.warn()) << "GenesisMint: destinations includes "
                                  "disallowed account zero or one.";
            return temMALFORMED;
        }

        if (allowDuplicates)
            continue;

        if (alreadySeen.find(accid) != alreadySeen.end())
        {
            JLOG(ctx.j.warn()) << "GenesisMint: duplicate in destinations.";
            return temMALFORMED;
        }

        alreadySeen.emplace(accid);
    }

    return preflight2(ctx);
}

TER
GenesisMint::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureHooks))
        return temDISABLED;

    if (!ctx.view.rules().enabled(featureXahauGenesis))
        return temDISABLED;

    // RH UPTO:
    // check that printing won't exceed 200% of the total coins on the ledger
    // this will act as a hard cap against malfunction
    // modify the invariant checkers

    return tesSUCCESS;
}

TER
GenesisMint::doApply()
{
    auto const& dests = ctx_.tx.getFieldArray(sfGenesisMints);

    if (!view().rules().enabled(fixXahauV1))
    {
        STAmount dropsAdded{0};
        for (auto const& dest : dests)
        {
            auto const amt = dest[~sfAmount];

            if (amt && !isXRP(*amt))
            {
                JLOG(ctx_.journal.warn()) << "GenesisMint: Non-xrp amount.";
                return tecINTERNAL;
            }

            auto const flags = dest[~sfGovernanceFlags];
            auto const marks = dest[~sfGovernanceMarks];

            auto const id = dest.getAccountID(sfDestination);
            auto const k = keylet::account(id);
            auto sle = view().peek(k);

            bool const created = !sle;

            if (created)
            {
                // Create the account.
                std::uint32_t const seqno{
                    view().info().parentCloseTime.time_since_epoch().count()};

                sle = std::make_shared<SLE>(k);
                sle->setAccountID(sfAccount, id);

                sle->setFieldU32(sfSequence, seqno);

                if (amt)
                {
                    sle->setFieldAmount(sfBalance, *amt);
                    dropsAdded += *amt;
                }
                else  // give them 2 XRP if the account didn't exist, same as
                      // ttIMPORT
                {
                    XRPAmount const initialXrp =
                        Import::computeStartingBonus(ctx_.view());
                    sle->setFieldAmount(sfBalance, initialXrp);
                    dropsAdded += initialXrp;
                }
            }
            else if (amt)
            {
                // Credit the account
                STAmount startBal = sle->getFieldAmount(sfBalance);
                STAmount finalBal = startBal + *amt;
                if (finalBal <= startBal)
                {
                    JLOG(ctx_.journal.warn())
                        << "GenesisMint: cannot credit " << dest
                        << " due to balance overflow";
                    return tecINTERNAL;
                }

                sle->setFieldAmount(sfBalance, finalBal);
                dropsAdded += *amt;
            }

            // set flags and marks as applicable
            if (flags)
                sle->setFieldH256(sfGovernanceFlags, *flags);

            if (marks)
                sle->setFieldH256(sfGovernanceMarks, *marks);

            if (created)
                view().insert(sle);
            else
                view().update(sle);
        }

        // update ledger header
        if (dropsAdded < beast::zero ||
            dropsAdded.xrp() + view().info().drops < view().info().drops)
        {
            JLOG(ctx_.journal.warn()) << "GenesisMint: dropsAdded overflowed\n";
            return tecINTERNAL;
        }

        if (dropsAdded > beast::zero)
            ctx_.rawView().rawDestroyXRP(-dropsAdded.xrp());

        return tesSUCCESS;
    }

    // RH NOTE:
    // As of fixXahauV1, duplicate accounts are allowed
    // so we first do a summation loop then an actioning loop
    // if amendment is not active then there's exactly one entry per

    std::map<
        AccountID,  // account to credit
        std::tuple<
            XRPAmount,                // amount to credit
            std::optional<uint256>,   // gov flags
            std::optional<uint256>>>  // gov marks

        mints;

    STAmount dropsAdded{0};

    // sum loop
    for (auto const& dest : dests)
    {
        auto const amt = dest[~sfAmount];

        if (amt && !isXRP(*amt))
        {
            JLOG(ctx_.journal.warn()) << "GenesisMint: Non-xrp amount.";
            return tecINTERNAL;
        }

        XRPAmount toCredit = amt ? amt->xrp() : XRPAmount{0};

        auto const flags = dest[~sfGovernanceFlags];
        auto const marks = dest[~sfGovernanceMarks];
        auto const id = dest.getAccountID(sfDestination);

        bool const firstOccurance = mints.find(id) == mints.end();

        dropsAdded += toCredit;

        // if flags / marks appear more than once we just take the first
        // appearance
        if (firstOccurance)
            mints[id] = {toCredit, flags, marks};
        else
        {
            // duplicated entries accumulate in the map

            auto& [accTotal, f, m] = mints[id];

            // detect overflow
            if (accTotal + toCredit < accTotal)
            {
                JLOG(ctx_.journal.warn()) << "GenesisMint: cannot credit " << id
                                          << " due to balance overflow";
                return tecINTERNAL;
            }

            accTotal += toCredit;
        }
    }

    // check for ledger header overflow
    if (dropsAdded < beast::zero ||
        dropsAdded.xrp() + view().info().drops < view().info().drops)
    {
        JLOG(ctx_.journal.warn()) << "GenesisMint: dropsAdded overflowed\n";
        return tecINTERNAL;
    }

    // action loop
    for (auto const& [id, values] : mints)
    {
        auto const& [amt, flags, marks] = values;

        auto const k = keylet::account(id);
        auto sle = view().peek(k);
        bool const created = !sle;

        if (created)
        {
            // Create the account.
            std::uint32_t const seqno{
                view().info().parentCloseTime.time_since_epoch().count()};

            sle = std::make_shared<SLE>(k);
            sle->setAccountID(sfAccount, id);

            sle->setFieldU32(sfSequence, seqno);

            sle->setFieldAmount(sfBalance, amt);
        }
        else if (amt > beast::zero)
        {
            // Credit the account
            STAmount startBal = sle->getFieldAmount(sfBalance);
            STAmount finalBal = startBal + amt;
            if (finalBal <= startBal)
            {
                JLOG(ctx_.journal.warn()) << "GenesisMint: cannot credit " << id
                                          << " due to balance overflow";
                return tecINTERNAL;
            }

            sle->setFieldAmount(sfBalance, finalBal);
        }

        // set flags and marks as applicable
        if (flags)
            sle->setFieldH256(sfGovernanceFlags, *flags);

        if (marks)
            sle->setFieldH256(sfGovernanceMarks, *marks);

        if (created)
            view().insert(sle);
        else
            view().update(sle);
    }

    if (dropsAdded > beast::zero)
        ctx_.rawView().rawDestroyXRP(-dropsAdded.xrp());

    return tesSUCCESS;
}

XRPAmount
GenesisMint::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return XRPAmount{0};
}

}  // namespace ripple
