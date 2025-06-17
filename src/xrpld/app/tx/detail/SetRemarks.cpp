//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <ripple/app/tx/impl/SetRemarks.h>
#include <ripple/basics/Log.h>
#include <ripple/core/Config.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/Quality.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>

namespace ripple {

TxConsequences
SetRemarks::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
SetRemarks::validateRemarks(STArray const& remarks, beast::Journal const& j)
{
    std::set<Blob> already_seen;

    if (remarks.empty() || remarks.size() > 32)
    {
        JLOG(j.warn()) << "SetRemarks: Cannot set more than 32 remarks (or "
                          "fewer than 1) in a txn.";
        return temMALFORMED;
    }

    for (auto const& remark : remarks)
    {
        if (remark.getFName() != sfRemark)
        {
            JLOG(j.warn()) << "SetRemarks: contained non-sfRemark field.";
            return temMALFORMED;
        }

        // will be checked by template system, extra check for security
        if (!remark.isFieldPresent(sfRemarkName))
            return temMALFORMED;

        Blob const& name = remark.getFieldVL(sfRemarkName);
        if (name.size() == 0 || name.size() > 256)
        {
            JLOG(j.warn()) << "SetRemarks: RemarkName cannot be empty or "
                              "larger than 256 chars.";
            return temMALFORMED;
        }

        if (already_seen.find(name) != already_seen.end())
        {
            JLOG(j.warn()) << "SetRemarks: duplicate RemarkName entry.";
            return temMALFORMED;
        }

        already_seen.emplace(name);

        uint32_t flags =
            remark.isFieldPresent(sfFlags) ? remark.getFieldU32(sfFlags) : 0;
        if (flags != 0 && flags != tfImmutable)
        {
            JLOG(j.warn())
                << "SetRemarks: Flags must be either tfImmutable or 0";
            return temMALFORMED;
        }

        if (!remark.isFieldPresent(sfRemarkValue))
        {
            if (flags & tfImmutable)
            {
                JLOG(j.warn()) << "SetRemarks: A remark deletion cannot be "
                                  "marked immutable.";
                return temMALFORMED;
            }
            continue;
        }

        Blob const& val = remark.getFieldVL(sfRemarkValue);
        if (val.size() == 0 || val.size() > 256)
        {
            JLOG(j.warn()) << "SetRemarks: RemarkValue cannot be empty or "
                              "larger than 256 chars.";
            return temMALFORMED;
        }
    }

    return tesSUCCESS;
}

NotTEC
SetRemarks::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureRemarks))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;
    auto& j = ctx.j;

    if (tx.getFlags() & tfUniversalMask)
    {
        JLOG(j.warn()) << "SetRemarks: Invalid flags set.";
        return temINVALID_FLAG;
    }

    auto const& remarks = tx.getFieldArray(sfRemarks);
    if (NotTEC result = validateRemarks(remarks, j); !isTesSuccess(result))
        return result;

    return preflight2(ctx);
}

template <typename T>
inline std::optional<AccountID>
getRemarksIssuer(T const& sleO)
{
    std::optional<AccountID> issuer;

    // check if it's an allowable object type
    uint16_t lt = sleO->getFieldU16(sfLedgerEntryType);

    switch (lt)
    {
        case ltACCOUNT_ROOT:
        case ltOFFER:
        case ltESCROW:
        case ltTICKET:
        case ltPAYCHAN:
        case ltCHECK:
        case ltDEPOSIT_PREAUTH: {
            issuer = sleO->getAccountID(sfAccount);
            break;
        }

        case ltNFTOKEN_OFFER: {
            issuer = sleO->getAccountID(sfOwner);
            break;
        }

        case ltURI_TOKEN: {
            issuer = sleO->getAccountID(sfIssuer);
            break;
        }

        case ltRIPPLE_STATE: {
            // remarks can only be attached to a trustline by the issuer
            AccountID lowAcc = sleO->getFieldAmount(sfLowLimit).getIssuer();
            AccountID highAcc = sleO->getFieldAmount(sfHighLimit).getIssuer();

            STAmount bal = sleO->getFieldAmount(sfBalance);

            if (bal < beast::zero)
            {
                // low account is issuer
                issuer = lowAcc;
                break;
            }

            if (bal > beast::zero)
            {
                // high acccount is issuer
                issuer = highAcc;
                break;
            }

            // if the balance is zero we'll look for the side in default state
            // and assume this is the issuer
            uint32_t flags = sleO->getFieldU32(sfFlags);
            bool const highReserve = (flags & lsfHighReserve);
            bool const lowReserve = (flags & lsfLowReserve);

            if (!highReserve && !lowReserve)
            {
                // error state
                // do nothing, fallthru.
            }
            else if (highReserve && lowReserve)
            {
                // in this edge case we don't know who is the issuer, because
                // there isn't a clear issuer. do nothing, fallthru.
            }
            else
            {
                issuer = (highReserve ? lowAcc : highAcc);
                break;
            }
        }
    }

    return issuer;
}

TER
SetRemarks::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureRemarks))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    auto const objID = ctx.tx[sfObjectID];
    auto const sleO = ctx.view.read(keylet::unchecked(objID));
    if (!sleO)
        return tecNO_TARGET;

    std::optional<AccountID> issuer = getRemarksIssuer(sleO);

    if (!issuer || *issuer != id)
        return tecNO_PERMISSION;

    // sanity check the remarks merge between txn and obj
    auto const& remarksTxn = ctx.tx.getFieldArray(sfRemarks);

    std::map<Blob, std::pair<Blob, bool>> keys;
    if (sleO->isFieldPresent(sfRemarks))
    {
        auto const& remarksObj = sleO->getFieldArray(sfRemarks);

        // map the remark name to its value and whether it's immutable
        for (auto const& remark : remarksObj)
            keys.emplace(std::make_pair(
                remark.getFieldVL(sfRemarkName),
                std::make_pair(
                    remark.getFieldVL(sfRemarkValue),
                    remark.isFieldPresent(sfFlags) &&
                        remark.getFieldU32(sfFlags) & tfImmutable)));
    }

    int64_t count = keys.size();

    for (auto const& remark : remarksTxn)
    {
        std::optional<Blob> valTxn;
        if (remark.isFieldPresent(sfRemarkValue))
            valTxn = remark.getFieldVL(sfRemarkValue);
        bool const isDeletion = !valTxn.has_value();

        Blob name = remark.getFieldVL(sfRemarkName);
        if (keys.find(name) == keys.end())
        {
            // new remark
            if (isDeletion)
            {
                // this could have been an error but deleting something
                // that doesn't exist is traditionally not an error in xrpl
                continue;
            }

            ++count;
            continue;
        }

        auto const& [valObj, immutable] = keys[name];

        // even if it's mutable, if we don't mutate it that's a noop so just
        // pass it
        if (valTxn.has_value() && *valTxn == valObj)
            continue;

        if (immutable)
        {
            JLOG(ctx.j.warn())
                << "SetRemarks: attempt to mutate an immutable remark.";
            return tecIMMUTABLE;
        }

        if (isDeletion)
        {
            if (--count < 0)
            {
                JLOG(ctx.j.warn()) << "SetRemarks: insane remarks accounting.";
                return tecCLAIM;
            }
        }
    }

    if (count > 32)
    {
        JLOG(ctx.j.warn())
            << "SetRemarks: an object may have at most 32 remarks.";
        return tecTOO_MANY_REMARKS;
    }

    return tesSUCCESS;
}

TER
SetRemarks::doApply()
{
    Sandbox sb(&ctx_.view());

    auto const sle = sb.read(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    auto const objID = ctx_.tx[sfObjectID];
    auto sleO = sb.peek(keylet::unchecked(objID));
    if (!sleO)
        return tefINTERNAL;

    std::optional<AccountID> issuer = getRemarksIssuer(sleO);

    if (!issuer || *issuer != account_)
        return tefINTERNAL;

    auto const& remarksTxn = ctx_.tx.getFieldArray(sfRemarks);

    std::map<Blob, std::pair<Blob, bool>> remarksMap;

    if (sleO->isFieldPresent(sfRemarks))
    {
        auto const& remarksObj = sleO->getFieldArray(sfRemarks);
        for (auto const& remark : remarksObj)
        {
            uint32_t flags = remark.isFieldPresent(sfFlags)
                ? remark.getFieldU32(sfFlags)
                : 0;
            bool const immutable = (flags & tfImmutable) != 0;
            remarksMap[remark.getFieldVL(sfRemarkName)] = {
                remark.getFieldVL(sfRemarkValue),
                remark.isFieldPresent(sfFlags) && immutable};
        }
    }

    for (auto const& remark : remarksTxn)
    {
        std::optional<Blob> val;
        if (remark.isFieldPresent(sfRemarkValue))
            val = remark.getFieldVL(sfRemarkValue);
        Blob name = remark.getFieldVL(sfRemarkName);

        bool const isDeletion = !val.has_value();
        uint32_t flags =
            remark.isFieldPresent(sfFlags) ? remark.getFieldU32(sfFlags) : 0;
        bool const setImmutable = (flags & tfImmutable) != 0;

        if (isDeletion)
        {
            if (remarksMap.find(name) != remarksMap.end())
                remarksMap.erase(name);
            continue;
        }

        if (remarksMap.find(name) == remarksMap.end())
        {
            remarksMap[name] = std::make_pair(*val, setImmutable);
            continue;
        }

        remarksMap[name].first = *val;
        if (!remarksMap[name].second)
            remarksMap[name].second = setImmutable;
    }

    // canonically order
    std::vector<Blob> keys;
    for (auto const& [k, _] : remarksMap)
        keys.push_back(k);

    std::sort(keys.begin(), keys.end());

    STArray newRemarks{sfRemarks, static_cast<std::size_t>(keys.size())};
    for (auto const& k : keys)
    {
        STObject remark{sfRemark};

        remark.setFieldVL(sfRemarkName, k);
        remark.setFieldVL(sfRemarkValue, remarksMap[k].first);
        if (remarksMap[k].second)
            remark.setFieldU32(sfFlags, lsfImmutable);

        newRemarks.push_back(std::move(remark));
    }

    if (newRemarks.size() > 32)
        return tefINTERNAL;

    if (newRemarks.empty() && sleO->isFieldPresent(sfRemarks))
        sleO->makeFieldAbsent(sfRemarks);
    else
        sleO->setFieldArray(sfRemarks, std::move(newRemarks));

    sb.update(sleO);
    sb.apply(ctx_.rawView());

    return tesSUCCESS;
}

XRPAmount
SetRemarks::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount remarkFee{0};
    if (tx.isFieldPresent(sfRemarks))
    {
        int64_t remarkBytes = 0;
        auto const& remarks = tx.getFieldArray(sfRemarks);
        for (auto const& remark : remarks)
        {
            int64_t entryBytes = 0;
            if (remark.isFieldPresent(sfRemarkName))
            {
                entryBytes += remark.getFieldVL(sfRemarkName).size();
            }
            if (remark.isFieldPresent(sfRemarkValue))
            {
                entryBytes += remark.getFieldVL(sfRemarkValue).size();
            }

            // overflow
            if (remarkBytes + entryBytes < remarkBytes)
                return INITIAL_XRP;

            remarkBytes += entryBytes;
        }

        // one drop per byte
        remarkFee = XRPAmount{remarkBytes};
    }
    auto fee = Transactor::calculateBaseFee(view, tx);
    return fee + remarkFee;
}

}  // namespace ripple
