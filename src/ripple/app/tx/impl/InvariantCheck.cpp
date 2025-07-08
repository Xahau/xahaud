//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2012-2016 Ripple Labs Inc.

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

#include <ripple/app/tx/impl/InvariantCheck.h>

#include <ripple/app/tx/impl/Import.h>
#include <ripple/app/tx/impl/details/NFTokenUtils.h>
#include <ripple/basics/FeeUnits.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/SystemParameters.h>
#include <ripple/protocol/nftPageMask.h>

namespace ripple {

void
TransactionFeeCheck::visitEntry(
    bool,
    std::shared_ptr<SLE const> const&,
    std::shared_ptr<SLE const> const&)
{
    // nothing to do
}

bool
TransactionFeeCheck::finalize(
    STTx const& tx,
    TER const,
    XRPAmount const fee,
    ReadView const&,
    beast::Journal const& j)
{
    // We should never charge a negative fee
    if (fee.drops() < 0)
    {
        JLOG(j.fatal()) << "Invariant failed: fee paid was negative: "
                        << fee.drops();
        return false;
    }

    // We should never charge a fee that's greater than or equal to the
    // entire XRP supply.
    if (fee >= INITIAL_XRP)
    {
        JLOG(j.fatal()) << "Invariant failed: fee paid exceeds system limit: "
                        << fee.drops();
        return false;
    }

    // We should never charge more for a transaction than the transaction
    // authorizes. It's possible to charge less in some circumstances.
    if (fee > tx.getFieldAmount(sfFee).xrp())
    {
        JLOG(j.fatal()) << "Invariant failed: fee paid is " << fee.drops()
                        << " exceeds fee specified in transaction.";
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------

void
XRPNotCreated::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    /* We go through all modified ledger entries, looking only at account roots,
     * escrow payments, and payment channels. We remove from the total any
     * previous XRP values and add to the total any new XRP values. The net
     * balance of a payment channel is computed from two fields (amount and
     * balance) and deletions are ignored for paychan and escrow because the
     * amount fields have not been adjusted for those in the case of deletion.
     */
    if (before)
    {
        switch (before->getType())
        {
            case ltACCOUNT_ROOT:
                drops_ -= (*before)[sfBalance].xrp().drops();
                break;
            case ltPAYCHAN:
                if (isXRP((*before)[sfAmount]))
                    drops_ -= ((*before)[sfAmount] - (*before)[sfBalance])
                                  .xrp()
                                  .drops();
                break;
            case ltESCROW:
                if (isXRP((*before)[sfAmount]))
                    drops_ -= (*before)[sfAmount].xrp().drops();
                break;
            default:
                break;
        }
    }

    if (after)
    {
        switch (after->getType())
        {
            case ltACCOUNT_ROOT:
                drops_ += (*after)[sfBalance].xrp().drops();
                break;
            case ltPAYCHAN:
                if (!isDelete && isXRP((*after)[sfAmount]))
                    drops_ += ((*after)[sfAmount] - (*after)[sfBalance])
                                  .xrp()
                                  .drops();
                break;
            case ltESCROW:
                if (!isDelete && isXRP((*after)[sfAmount]))
                    drops_ += (*after)[sfAmount].xrp().drops();
                break;
            default:
                break;
        }
    }

    if (!before && after->getType() == ltACCOUNT_ROOT)
        accountsCreated_++;
}

bool
XRPNotCreated::finalize(
    STTx const& tx,
    TER const res,
    XRPAmount const fee,
    ReadView const& view,
    beast::Journal const& j)
{
    auto const tt = tx.getTxnType();

    if (tt == ttAMENDMENT &&
        tx.getFieldH256(sfAmendment) == featureXahauGenesis)
        return true;

    if (view.rules().enabled(featureImport) && tt == ttIMPORT &&
        isTesSuccess(res))
    {
        // different rules for ttIMPORT
        auto const [inner, meta] = Import::getInnerTxn(tx, j);
        if (!inner || !meta)
            return false;

        auto const result = meta->getFieldU8(sfTransactionResult);

        XRPAmount maxDropsAdded = isTesSuccess(result) ||
                (result >= tecCLAIM && result <= tecLAST_POSSIBLE_ENTRY)
            ? inner->getFieldAmount(sfFee).xrp()  // burned in PoB
            : beast::zero;  // if the txn didnt burn a fee we add nothing

        if (accountsCreated_ == 1)
            maxDropsAdded += Import::computeStartingBonus(view);

        JLOG(j.trace()) << "Invariant XRPNotCreated Import: "
                        << "maxDropsAdded: " << maxDropsAdded
                        << " fee.drops(): " << fee.drops()
                        << " drops_: " << drops_
                        << " <= maxDropsAdded - fee.drops(): "
                        << maxDropsAdded - fee.drops();

        // We should never allow more than the max supply in totalCoins.
        XRPAmount const newTotal = view.info().drops + maxDropsAdded;
        if (newTotal > INITIAL_XRP)
        {
            JLOG(j.fatal())
                << "Invariant failed Import: total coins paid exceeds "
                << "system limit: " << INITIAL_XRP
                << "maxDropsAdded: " << maxDropsAdded
                << " fee.drops(): " << fee.drops()
                << " info().drops: " << view.info().drops
                << " newTotal: " << newTotal;
            return false;
        }

        bool const passed = (drops_ <= maxDropsAdded.drops() - fee.drops());
        if (!passed)
        {
            JLOG(j.trace()) << "XRPNotCreated failed.";
        }

        return passed;
    }

    if (view.rules().enabled(featureXahauGenesis) && tt == ttGENESIS_MINT &&
        isTesSuccess(res))
    {
        // different rules for ttGENESIS_MINT
        auto const& dests = tx.getFieldArray(sfGenesisMints);
        XRPAmount dropsAdded{beast::zero};
        for (auto const& dest : dests)
            dropsAdded += dest.getFieldAmount(sfAmount).xrp();

        JLOG(j.trace()) << "Invariant XRPNotCreated GenesisMint: "
                        << "dropsAdded: " << dropsAdded
                        << " fee.drops(): " << fee.drops()
                        << " drops_: " << drops_
                        << " dropsAdded - fee.drops(): "
                        << dropsAdded - fee.drops();

        int64_t drops = dropsAdded.drops() - fee.drops();

        // catch any overflow or funny business
        if (drops > dropsAdded.drops())
            return false;

        // We should never allow more than the max supply in totalCoins.
        XRPAmount const newTotal = view.info().drops + dropsAdded;
        if (newTotal > INITIAL_XRP)
        {
            JLOG(j.fatal())
                << "Invariant failed GenesisMint: total coins exceeds "
                << "system limit: " << INITIAL_XRP
                << "dropsAdded: " << dropsAdded
                << " fee.drops(): " << fee.drops()
                << " info().drops: " << view.info().drops
                << " newTotal: " << newTotal;
            return false;
        }

        return drops_ == drops;
    }

    // The net change should never be positive, as this would mean that the
    // transaction created XRP out of thin air. That's not possible.
    if (drops_ > 0)
    {
        JLOG(j.fatal()) << "Invariant failed: XRP net change was positive: "
                        << drops_;
        return false;
    }

    // The negative of the net change should be equal to actual fee charged.
    if (-drops_ != fee.drops())
    {
        JLOG(j.fatal()) << "Invariant failed: XRP net change of " << drops_
                        << " doesn't match fee " << fee.drops();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------

void
XRPBalanceChecks::visitEntry(
    bool,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    auto isBad = [](STAmount const& balance) {
        if (!balance.native())
            return true;

        auto const drops = balance.xrp();

        // Can't have more than the number of drops instantiated
        // in the genesis ledger.
        if (drops > INITIAL_XRP)
            return true;

        // Can't have a negative balance (0 is OK)
        if (drops < XRPAmount{0})
            return true;

        return false;
    };

    if (before && before->getType() == ltACCOUNT_ROOT)
        bad_ |= isBad((*before)[sfBalance]);

    if (after && after->getType() == ltACCOUNT_ROOT)
        bad_ |= isBad((*after)[sfBalance]);
}

bool
XRPBalanceChecks::finalize(
    STTx const&,
    TER const,
    XRPAmount const,
    ReadView const&,
    beast::Journal const& j)
{
    if (bad_)
    {
        JLOG(j.fatal()) << "Invariant failed: incorrect account XRP balance";
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------

void
NoBadOffers::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    auto isBad = [](STAmount const& pays, STAmount const& gets) {
        // An offer should never be negative
        if (pays < beast::zero)
            return true;

        if (gets < beast::zero)
            return true;

        // Can't have an XRP to XRP offer:
        return pays.native() && gets.native();
    };

    if (before && before->getType() == ltOFFER)
        bad_ |= isBad((*before)[sfTakerPays], (*before)[sfTakerGets]);

    if (after && after->getType() == ltOFFER)
        bad_ |= isBad((*after)[sfTakerPays], (*after)[sfTakerGets]);
}

bool
NoBadOffers::finalize(
    STTx const&,
    TER const,
    XRPAmount const,
    ReadView const&,
    beast::Journal const& j)
{
    if (bad_)
    {
        JLOG(j.fatal()) << "Invariant failed: offer with a bad amount";
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------

void
NoZeroEscrow::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    auto isBad = [](STAmount const& amount) {
        if (!amount.native())
            return true;

        if (amount.xrp() <= XRPAmount{0})
            return true;

        if (amount.xrp() >= INITIAL_XRP)
            return true;

        return false;
    };

    if (before && before->getType() == ltESCROW)
        bad_ |= isBad((*before)[sfAmount]);

    if (after && after->getType() == ltESCROW)
        bad_ |= isBad((*after)[sfAmount]);
}

bool
NoZeroEscrow::finalize(
    STTx const& txn,
    TER const,
    XRPAmount const,
    ReadView const& rv,
    beast::Journal const& j)
{
    // bypass this invariant check for IOU escrows
    if (bad_ && rv.rules().enabled(featurePaychanAndEscrowForTokens) &&
        txn.isFieldPresent(sfTransactionType))
    {
        uint16_t const tt = txn.getFieldU16(sfTransactionType);
        if (tt == ttESCROW_CANCEL || tt == ttESCROW_FINISH)
            return true;

        if (txn.isFieldPresent(sfAmount) &&
            !isXRP(txn.getFieldAmount(sfAmount)))
            return true;
    }

    if (bad_)
    {
        JLOG(j.fatal()) << "Invariant failed: escrow specifies invalid amount";
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------

void
AccountRootsNotDeleted::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const&)
{
    if (isDelete && before && before->getType() == ltACCOUNT_ROOT)
        accountsDeleted_++;
}

bool
AccountRootsNotDeleted::finalize(
    STTx const& tx,
    TER const result,
    XRPAmount const,
    ReadView const&,
    beast::Journal const& j)
{
    if (tx.getTxnType() == ttACCOUNT_DELETE && isTesSuccess(result))
    {
        if (accountsDeleted_ == 1)
            return true;

        if (accountsDeleted_ == 0)
            JLOG(j.fatal()) << "Invariant failed: account deletion "
                               "succeeded without deleting an account";
        else
            JLOG(j.fatal()) << "Invariant failed: account deletion "
                               "succeeded but deleted multiple accounts!";
        return false;
    }

    if (accountsDeleted_ == 0)
        return true;

    JLOG(j.fatal()) << "Invariant failed: an account root was deleted";
    return false;
}

//------------------------------------------------------------------------------

void
LedgerEntryTypesMatch::visitEntry(
    bool,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    if (before && after && before->getType() != after->getType())
        typeMismatch_ = true;

    if (after)
    {
        switch (after->getType())
        {
            case ltACCOUNT_ROOT:
            case ltDIR_NODE:
            case ltRIPPLE_STATE:
            case ltTICKET:
            case ltSIGNER_LIST:
            case ltOFFER:
            case ltLEDGER_HASHES:
            case ltAMENDMENTS:
            case ltFEE_SETTINGS:
            case ltESCROW:
            case ltPAYCHAN:
            case ltCHECK:
            case ltDEPOSIT_PREAUTH:
            case ltNEGATIVE_UNL:
            case ltHOOK:
            case ltHOOK_DEFINITION:
            case ltHOOK_STATE:
            case ltEMITTED_TXN:
            case ltNFTOKEN_PAGE:
            case ltNFTOKEN_OFFER:
            case ltURI_TOKEN:
            case ltIMPORT_VLSEQ:
            case ltUNL_REPORT:
                break;
            default:
                invalidTypeAdded_ = true;
                break;
        }
    }
}

bool
LedgerEntryTypesMatch::finalize(
    STTx const&,
    TER const,
    XRPAmount const,
    ReadView const&,
    beast::Journal const& j)
{
    if ((!typeMismatch_) && (!invalidTypeAdded_))
        return true;

    if (typeMismatch_)
    {
        JLOG(j.fatal()) << "Invariant failed: ledger entry type mismatch";
    }

    if (invalidTypeAdded_)
    {
        JLOG(j.fatal()) << "Invariant failed: invalid ledger entry type added";
    }

    return false;
}

//------------------------------------------------------------------------------

void
NoXRPTrustLines::visitEntry(
    bool,
    std::shared_ptr<SLE const> const&,
    std::shared_ptr<SLE const> const& after)
{
    if (after && after->getType() == ltRIPPLE_STATE)
    {
        // checking the issue directly here instead of
        // relying on .native() just in case native somehow
        // were systematically incorrect
        xrpTrustLine_ =
            after->getFieldAmount(sfLowLimit).issue() == xrpIssue() ||
            after->getFieldAmount(sfHighLimit).issue() == xrpIssue();
    }
}

bool
NoXRPTrustLines::finalize(
    STTx const&,
    TER const,
    XRPAmount const,
    ReadView const&,
    beast::Journal const& j)
{
    if (!xrpTrustLine_)
        return true;

    JLOG(j.fatal()) << "Invariant failed: an XRP trust line was created";
    return false;
}

//------------------------------------------------------------------------------

void
NoDeepFreezeTrustLinesWithoutFreeze::visitEntry(
    bool,
    std::shared_ptr<SLE const> const&,
    std::shared_ptr<SLE const> const& after)
{
    if (after && after->getType() == ltRIPPLE_STATE)
    {
        std::uint32_t const uFlags = after->getFieldU32(sfFlags);
        bool const lowFreeze = uFlags & lsfLowFreeze;
        bool const lowDeepFreeze = uFlags & lsfLowDeepFreeze;

        bool const highFreeze = uFlags & lsfHighFreeze;
        bool const highDeepFreeze = uFlags & lsfHighDeepFreeze;

        deepFreezeWithoutFreeze_ =
            (lowDeepFreeze && !lowFreeze) || (highDeepFreeze && !highFreeze);
    }
}

bool
NoDeepFreezeTrustLinesWithoutFreeze::finalize(
    STTx const&,
    TER const,
    XRPAmount const,
    ReadView const&,
    beast::Journal const& j)
{
    if (!deepFreezeWithoutFreeze_)
        return true;

    JLOG(j.fatal()) << "Invariant failed: a trust line with deep freeze flag "
                       "without normal freeze was created";
    return false;
}

//------------------------------------------------------------------------------

void
TransfersNotFrozen::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    /*
     * A trust line freeze state alone doesn't determine if a transfer is
     * frozen. The transfer must be examined "end-to-end" because both sides of
     * the transfer may have different freeze states and freeze impact depends
     * on the transfer direction. This is why first we need to track the
     * transfers using IssuerChanges senders/receivers.
     *
     * Only in validateIssuerChanges, after we collected all changes can we
     * determine if the transfer is valid.
     */
    if (!isValidEntry(before, after))
    {
        return;
    }

    auto const balanceChange = calculateBalanceChange(before, after, isDelete);
    if (balanceChange.signum() == 0)
    {
        return;
    }

    recordBalanceChanges(after, balanceChange);
}

bool
TransfersNotFrozen::finalize(
    STTx const& tx,
    TER const ter,
    XRPAmount const fee,
    ReadView const& view,
    beast::Journal const& j)
{
    /*
     * We check this invariant regardless of deep freeze amendment status,
     * allowing for detection and logging of potential issues even when the
     * amendment is disabled.
     *
     * If an exploit that allows moving frozen assets is discovered,
     * we can alert operators who monitor fatal messages and trigger assert in
     * debug builds for an early warning.
     *
     * In an unlikely event that an exploit is found, this early detection
     * enables encouraging the UNL to expedite deep freeze amendment activation
     * or deploy hotfixes via new amendments. In case of a new amendment, we'd
     * only have to change this line setting 'enforce' variable.
     * enforce = view.rules().enabled(featureDeepFreeze) ||
     *           view.rules().enabled(fixFreezeExploit);
     */
    [[maybe_unused]] bool const enforce =
        view.rules().enabled(featureDeepFreeze);

    for (auto const& [issue, changes] : balanceChanges_)
    {
        auto const issuerSle = findIssuer(issue.account, view);
        // It should be impossible for the issuer to not be found, but check
        // just in case so rippled doesn't crash in release.
        if (!issuerSle)
        {
            assert(enforce);
            if (enforce)
            {
                return false;
            }
            continue;
        }

        if (!validateIssuerChanges(issuerSle, changes, tx, j, enforce))
        {
            return false;
        }
    }

    return true;
}

bool
TransfersNotFrozen::isValidEntry(
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    // `after` can never be null, even if the trust line is deleted.
    assert(after);
    if (!after)
    {
        return false;
    }

    if (after->getType() == ltACCOUNT_ROOT)
    {
        possibleIssuers_.emplace(after->getAccountID(sfAccount), after);
        return false;
    }

    /* While LedgerEntryTypesMatch invariant also checks types, all invariants
     * are processed regardless of previous failures.
     *
     * This type check is still necessary here because it prevents potential
     * issues in subsequent processing.
     */
    return after->getType() == ltRIPPLE_STATE &&
        (!before || before->getType() == ltRIPPLE_STATE);
}

STAmount
TransfersNotFrozen::calculateBalanceChange(
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after,
    bool isDelete)
{
    auto const getBalance = [](auto const& line, auto const& other, bool zero) {
        STAmount amt = line ? line->getFieldAmount(sfBalance)
                            : other->getFieldAmount(sfBalance).zeroed();
        return zero ? amt.zeroed() : amt;
    };

    /* Trust lines can be created dynamically by other transactions such as
     * Payment and OfferCreate that cross offers. Such trust line won't be
     * created frozen, but the sender might be, so the starting balance must be
     * treated as zero.
     */
    auto const balanceBefore = getBalance(before, after, false);

    /* Same as above, trust lines can be dynamically deleted, and for frozen
     * trust lines, payments not involving the issuer must be blocked. This is
     * achieved by treating the final balance as zero when isDelete=true to
     * ensure frozen line restrictions are enforced even during deletion.
     */
    auto const balanceAfter = getBalance(after, before, isDelete);

    return balanceAfter - balanceBefore;
}

void
TransfersNotFrozen::recordBalance(Issue const& issue, BalanceChange change)
{
    assert(change.balanceChangeSign);
    auto& changes = balanceChanges_[issue];
    if (change.balanceChangeSign < 0)
        changes.senders.emplace_back(std::move(change));
    else
        changes.receivers.emplace_back(std::move(change));
}

void
TransfersNotFrozen::recordBalanceChanges(
    std::shared_ptr<SLE const> const& after,
    STAmount const& balanceChange)
{
    auto const balanceChangeSign = balanceChange.signum();
    auto const currency = after->getFieldAmount(sfBalance).getCurrency();

    // Change from low account's perspective, which is trust line default
    recordBalance(
        {currency, after->getFieldAmount(sfHighLimit).getIssuer()},
        {after, balanceChangeSign});

    // Change from high account's perspective, which reverses the sign.
    recordBalance(
        {currency, after->getFieldAmount(sfLowLimit).getIssuer()},
        {after, -balanceChangeSign});
}

std::shared_ptr<SLE const>
TransfersNotFrozen::findIssuer(AccountID const& issuerID, ReadView const& view)
{
    if (auto it = possibleIssuers_.find(issuerID); it != possibleIssuers_.end())
    {
        return it->second;
    }

    return view.read(keylet::account(issuerID));
}

bool
TransfersNotFrozen::validateIssuerChanges(
    std::shared_ptr<SLE const> const& issuer,
    IssuerChanges const& changes,
    STTx const& tx,
    beast::Journal const& j,
    bool enforce)
{
    if (!issuer)
    {
        return false;
    }

    bool const globalFreeze = issuer->isFlag(lsfGlobalFreeze);
    if (changes.receivers.empty() || changes.senders.empty())
    {
        /* If there are no receivers, then the holder(s) are returning
         * their tokens to the issuer. Likewise, if there are no
         * senders, then the issuer is issuing tokens to the holder(s).
         * This is allowed regardless of the issuer's freeze flags. (The
         * holder may have contradicting freeze flags, but that will be
         * checked when the holder is treated as issuer.)
         */
        return true;
    }

    for (auto const& actors : {changes.senders, changes.receivers})
    {
        for (auto const& change : actors)
        {
            bool const high =
                change.line->getFieldAmount(sfLowLimit).getIssuer() ==
                issuer->getAccountID(sfAccount);

            if (!validateFrozenState(
                    change, high, tx, j, enforce, globalFreeze))
            {
                return false;
            }
        }
    }
    return true;
}

bool
TransfersNotFrozen::validateFrozenState(
    BalanceChange const& change,
    bool high,
    STTx const& tx,
    beast::Journal const& j,
    bool enforce,
    bool globalFreeze)
{
    bool const freeze = change.balanceChangeSign < 0 &&
        change.line->isFlag(high ? lsfLowFreeze : lsfHighFreeze);
    bool const deepFreeze =
        change.line->isFlag(high ? lsfLowDeepFreeze : lsfHighDeepFreeze);
    bool const frozen = globalFreeze || deepFreeze || freeze;

    // bool const isAMMLine = change.line->isFlag(lsfAMMNode);

    if (!frozen)
    {
        return true;
    }

    JLOG(j.fatal()) << "Invariant failed: Attempting to move frozen funds for "
                    << tx.getTransactionID();
    assert(enforce);

    if (enforce)
    {
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------

void
ValidNewAccountRoot::visitEntry(
    bool,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    if (!before && after->getType() == ltACCOUNT_ROOT)
    {
        accountsCreated_++;
        accountSeq_ = (*after)[sfSequence];
    }
}

bool
ValidNewAccountRoot::finalize(
    STTx const& tx,
    TER const result,
    XRPAmount const,
    ReadView const& view,
    beast::Journal const& j)
{
    if (accountsCreated_ == 0)
        return true;

    auto tt = tx.getTxnType();

    if (tt == ttAMENDMENT &&
        tx.getFieldH256(sfAmendment) == featureXahauGenesis)
        return true;

    if (accountsCreated_ > 1 && tt != ttGENESIS_MINT)
    {
        JLOG(j.fatal()) << "Invariant failed: multiple accounts "
                           "created in a single transaction";
        return false;
    }

    if ((tt == ttPAYMENT || tt == ttIMPORT || tt == ttGENESIS_MINT ||
         tt == ttREMIT) &&
        isTesSuccess(result))
    {
        std::uint32_t const startingSeq{
            view.rules().enabled(featureXahauGenesis)
                ? view.info().parentCloseTime.time_since_epoch().count()
                : view.rules().enabled(featureDeletableAccounts) ? view.seq()
                                                                 : 1};

        if (accountSeq_ != startingSeq)
        {
            JLOG(j.fatal()) << "Invariant failed: account created with "
                               "wrong starting sequence number";
            return false;
        }
        return true;
    }

    JLOG(j.fatal()) << "Invariant failed: account root created "
                       "by a non-Payment or by an unsuccessful transaction";
    return false;
}

//------------------------------------------------------------------------------

void
ValidNFTokenPage::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    static constexpr uint256 const& pageBits = nft::pageMask;
    static constexpr uint256 const accountBits = ~pageBits;

    auto check = [this, isDelete](std::shared_ptr<SLE const> const& sle) {
        uint256 const account = sle->key() & accountBits;
        uint256 const hiLimit = sle->key() & pageBits;
        std::optional<uint256> const prev = (*sle)[~sfPreviousPageMin];

        // Make sure that any page links...
        //  1. Are properly associated with the owning account and
        //  2. The page is correctly ordered between links.
        if (prev)
        {
            if (account != (*prev & accountBits))
                badLink_ = true;

            if (hiLimit <= (*prev & pageBits))
                badLink_ = true;
        }

        if (auto const next = (*sle)[~sfNextPageMin])
        {
            if (account != (*next & accountBits))
                badLink_ = true;

            if (hiLimit >= (*next & pageBits))
                badLink_ = true;
        }

        {
            auto const& nftokens = sle->getFieldArray(sfNFTokens);

            // An NFTokenPage should never contain too many tokens or be empty.
            if (std::size_t const nftokenCount = nftokens.size();
                (!isDelete && nftokenCount == 0) ||
                nftokenCount > dirMaxTokensPerPage)
                invalidSize_ = true;

            // If prev is valid, use it to establish a lower bound for
            // page entries.  If prev is not valid the lower bound is zero.
            uint256 const loLimit =
                prev ? *prev & pageBits : uint256(beast::zero);

            // Also verify that all NFTokenIDs in the page are sorted.
            uint256 loCmp = loLimit;
            for (auto const& obj : nftokens)
            {
                uint256 const tokenID = obj[sfNFTokenID];
                if (!nft::compareTokens(loCmp, tokenID))
                    badSort_ = true;
                loCmp = tokenID;

                // None of the NFTs on this page should belong on lower or
                // higher pages.
                if (uint256 const tokenPageBits = tokenID & pageBits;
                    tokenPageBits < loLimit || tokenPageBits >= hiLimit)
                    badEntry_ = true;

                if (auto uri = obj[~sfURI]; uri && uri->empty())
                    badURI_ = true;
            }
        }
    };

    if (before && before->getType() == ltNFTOKEN_PAGE)
        check(before);

    if (after && after->getType() == ltNFTOKEN_PAGE)
        check(after);
}

bool
ValidNFTokenPage::finalize(
    STTx const& tx,
    TER const result,
    XRPAmount const,
    ReadView const& view,
    beast::Journal const& j)
{
    if (badLink_)
    {
        JLOG(j.fatal()) << "Invariant failed: NFT page is improperly linked.";
        return false;
    }

    if (badEntry_)
    {
        JLOG(j.fatal()) << "Invariant failed: NFT found in incorrect page.";
        return false;
    }

    if (badSort_)
    {
        JLOG(j.fatal()) << "Invariant failed: NFTs on page are not sorted.";
        return false;
    }

    if (badURI_)
    {
        JLOG(j.fatal()) << "Invariant failed: NFT contains empty URI.";
        return false;
    }

    if (invalidSize_)
    {
        JLOG(j.fatal()) << "Invariant failed: NFT page has invalid size.";
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
void
NFTokenCountTracking::visitEntry(
    bool,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    if (before && before->getType() == ltACCOUNT_ROOT)
    {
        beforeMintedTotal += (*before)[~sfMintedNFTokens].value_or(0);
        beforeBurnedTotal += (*before)[~sfBurnedNFTokens].value_or(0);
    }

    if (after && after->getType() == ltACCOUNT_ROOT)
    {
        afterMintedTotal += (*after)[~sfMintedNFTokens].value_or(0);
        afterBurnedTotal += (*after)[~sfBurnedNFTokens].value_or(0);
    }
}

bool
NFTokenCountTracking::finalize(
    STTx const& tx,
    TER const result,
    XRPAmount const,
    ReadView const& view,
    beast::Journal const& j)
{
    if (TxType const txType = tx.getTxnType();
        txType != ttNFTOKEN_MINT && txType != ttNFTOKEN_BURN)
    {
        if (beforeMintedTotal != afterMintedTotal)
        {
            JLOG(j.fatal()) << "Invariant failed: the number of minted tokens "
                               "changed without a mint transaction!";
            return false;
        }

        if (beforeBurnedTotal != afterBurnedTotal)
        {
            JLOG(j.fatal()) << "Invariant failed: the number of burned tokens "
                               "changed without a burn transaction!";
            return false;
        }

        return true;
    }

    if (tx.getTxnType() == ttNFTOKEN_MINT)
    {
        if (isTesSuccess(result) && beforeMintedTotal >= afterMintedTotal)
        {
            JLOG(j.fatal())
                << "Invariant failed: successful minting didn't increase "
                   "the number of minted tokens.";
            return false;
        }

        if (!isTesSuccess(result) && beforeMintedTotal != afterMintedTotal)
        {
            JLOG(j.fatal()) << "Invariant failed: failed minting changed the "
                               "number of minted tokens.";
            return false;
        }

        if (beforeBurnedTotal != afterBurnedTotal)
        {
            JLOG(j.fatal())
                << "Invariant failed: minting changed the number of "
                   "burned tokens.";
            return false;
        }
    }

    if (tx.getTxnType() == ttNFTOKEN_BURN)
    {
        if (isTesSuccess(result))
        {
            if (beforeBurnedTotal >= afterBurnedTotal)
            {
                JLOG(j.fatal())
                    << "Invariant failed: successful burning didn't increase "
                       "the number of burned tokens.";
                return false;
            }
        }

        if (!isTesSuccess(result) && beforeBurnedTotal != afterBurnedTotal)
        {
            JLOG(j.fatal()) << "Invariant failed: failed burning changed the "
                               "number of burned tokens.";
            return false;
        }

        if (beforeMintedTotal != afterMintedTotal)
        {
            JLOG(j.fatal())
                << "Invariant failed: burning changed the number of "
                   "minted tokens.";
            return false;
        }
    }

    return true;
}

}  // namespace ripple
