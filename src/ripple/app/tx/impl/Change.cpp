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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/tx/impl/Change.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/Sandbox.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <string_view>
#include <ripple/app/hook/Guard.h> 
#include <ripple/protocol/AccountID.h>
#include <ripple/app/hook/applyHook.h>

namespace ripple {

NotTEC
Change::preflight(PreflightContext const& ctx)
{
    auto const ret = preflight0(ctx);
    if (!isTesSuccess(ret))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount(sfFee);
    if (!fee.native() || fee != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: invalid fee";
        return temBAD_FEE;
    }

    if (!ctx.tx.getSigningPubKey().empty() || !ctx.tx.getSignature().empty() ||
        ctx.tx.isFieldPresent(sfSigners))
    {
        JLOG(ctx.j.warn()) << "Change: Bad signature";
        return temBAD_SIGNATURE;
    }

    if (ctx.tx.getFieldU32(sfSequence) != 0 ||
        ctx.tx.isFieldPresent(sfPreviousTxnID))
    {
        JLOG(ctx.j.warn()) << "Change: Bad sequence";
        return temBAD_SEQUENCE;
    }

    if (ctx.tx.getTxnType() == ttUNL_MODIFY &&
        !ctx.rules.enabled(featureNegativeUNL))
    {
        JLOG(ctx.j.warn()) << "Change: NegativeUNL not enabled";
        return temDISABLED;
    }

    if (ctx.tx.getTxnType() == ttUNL_REPORT &&
        !ctx.rules.enabled(featureXahauGenesis))
    {
        JLOG(ctx.j.warn()) << "Change: UNLReport is not enabled.";
        return temDISABLED;
    }

    return tesSUCCESS;
}

TER
Change::preclaim(PreclaimContext const& ctx)
{
    // If tapOPEN_LEDGER is resurrected into ApplyFlags,
    // this block can be moved to preflight.
    if (ctx.view.open())
    {
        JLOG(ctx.j.warn()) << "Change transaction against open ledger";
        return temINVALID;
    }

    switch (ctx.tx.getTxnType())
    {
        case ttFEE:
            if (ctx.view.rules().enabled(featureXRPFees))
            {
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // required.
                if (!ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but once the XRPFees feature is enabled, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFee) ||
                    ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    ctx.tx.isFieldPresent(sfReserveBase) ||
                    ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
            }
            else
            {
                // The ttFEE transaction format formerly defined these fields
                // as required. When the XRPFees feature was implemented, they
                // were changed to be optional. Until the feature has been
                // enabled, they are required.
                if (!ctx.tx.isFieldPresent(sfBaseFee) ||
                    !ctx.tx.isFieldPresent(sfReferenceFeeUnits) ||
                    !ctx.tx.isFieldPresent(sfReserveBase) ||
                    !ctx.tx.isFieldPresent(sfReserveIncrement))
                    return temMALFORMED;
                // The ttFEE transaction format defines these fields as
                // optional, but without the XRPFees feature, they are
                // forbidden.
                if (ctx.tx.isFieldPresent(sfBaseFeeDrops) ||
                    ctx.tx.isFieldPresent(sfReserveBaseDrops) ||
                    ctx.tx.isFieldPresent(sfReserveIncrementDrops))
                    return temDISABLED;
            }
            return tesSUCCESS;
        case ttAMENDMENT:
        case ttUNL_MODIFY:
        case ttUNL_REPORT:
        case ttEMIT_FAILURE:
            return tesSUCCESS;
        default:
            return temUNKNOWN;
    }
}

TER
Change::doApply()
{
    switch (ctx_.tx.getTxnType())
    {
        case ttAMENDMENT:
            return applyAmendment();
        case ttFEE:
            return applyFee();
        case ttUNL_MODIFY:
            return applyUNLModify();
        case ttEMIT_FAILURE:
            return applyEmitFailure();
        case ttUNL_REPORT:
            return applyUNLReport();
        default:
            assert(0);
            return tefFAILURE;
    }
}

TER
Change::applyUNLReport()
{
    auto sle = view().peek(keylet::UNLReport());

    bool const created = !sle;

    if (created)
        sle = std::make_shared<SLE>(keylet::UNLReport());

    auto const av = ctx_.tx.getFieldArray(sfActiveValidators);

    sle->setFieldArray(sfActiveValidators, av);

    if (created)
        view().insert(sle);
    else
        view().update(sle);

    return tesSUCCESS;
}

void
Change::preCompute()
{
    assert(account_ == beast::zero);
}


void
Change::activateXahauGenesis()
{
    JLOG(j_.warn()) << "featureXahauGenesis amendment activation code starting";

    constexpr XRPAmount GENESIS { 1'000'000 * DROPS_PER_XRP };

    constexpr XRPAmount INFRA   { 10'000'000 * DROPS_PER_XRP};
    constexpr XRPAmount EXCHANGE { 2'000'000 * DROPS_PER_XRP};

    const static std::vector<std::pair<std::string, XRPAmount>>
    initial_distribution =
    {
        {"rMYm3TY5D3rXYVAz6Zr2PDqEcjsTYbNiAT", INFRA},
    };

    const static std::vector<std::pair<uint256, std::vector<uint8_t>>>
    genesis_hooks =
    {
        { ripple::uint256("0000000000000000000000000000000000000000000000000000000000000001"),
          {0x0}
        },
    };


    Sandbox sb(&view());

    // Step 1: mint genesis distribution
    for (auto const& [account, amount] : initial_distribution)
    {
        auto accid_raw = parseBase58<AccountID>(account);
        if (!accid_raw)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis could not parse an r-address: " << account << ", bailing.";
            return;
        }

        auto accid = *accid_raw;

        auto const kl = keylet::account(accid);

        auto sle = sb.peek(kl);
        auto const exists = !!sle;

        STAmount bal = exists ? sle->getFieldAmount(sfBalance) + STAmount{amount} : STAmount{amount};
        if (bal <= beast::zero)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis tried to set <= 0 balance on " <<  account << ", bailing";
            return;
        }

        // the account should not exist but if it does then handle it properly
        if (!exists)
        {
            sle = std::make_shared<SLE>(kl);
            sle->setAccountID(sfAccount, accid);

            std::uint32_t const seqno{
                sb.rules().enabled(featureDeletableAccounts) ? sb.seq()
                                                             : 1};
            sle->setFieldU32(sfSequence, seqno);

        }

        sle->setFieldAmount(sfBalance, bal);

        if (exists)
            sb.update(sle);
        else
            sb.insert(sle);

    };

    // Step 2: burn genesis funds to (almost) zero
    static auto const accid = calcAccountID(
        generateKeyPair(KeyType::secp256k1, generateSeed("masterpassphrase"))
            .first);

    auto const kl = keylet::account(accid);
    auto sle = sb.peek(kl);
    if (!sle)
    {
        JLOG(j_.warn())
            << "featureXahauGenesis genesis account doesn't exist!!";

        return;
    }

    sle->setFieldAmount(sfBalance, GENESIS);

    // Step 3: blackhole genesis
    sle->setAccountID(sfRegularKey, noAccount());
    sle->setFieldU32(sfFlags, lsfDisableMaster);
       

    // Step 4: install genesis hooks

    sle->setFieldU32(sfOwnerCount, sle->getFieldU32(sfOwnerCount) + genesis_hooks.size());
    sb.update(sle);

    if (sb.exists(keylet::hook(accid)))
    {
        JLOG(j_.warn())
            << "featureXahauGenesis genesis account already has hooks object in ledger, bailing";
        return;
    }

    {
        ripple::STArray hooks{sfHooks, genesis_hooks.size()};
        int hookCount = 0;

        for (auto const& [hookOn, wasmBytes] : genesis_hooks)
        {

            std::ostringstream loggerStream;
            auto result =
                validateGuards(
                    wasmBytes,   // wasm to verify
                    loggerStream,
                    "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"
                );

            if (!result)
            {
                std::string s = loggerStream.str();

                char* data = s.data();
                size_t len = s.size();

                char* last = data;
                size_t i = 0;
                for (; i < len; ++i)
                {
                    if (data[i] == '\n')
                    {
                        data[i] = '\0';
                        j_.warn() << last;
                        last = data + i;
                    }
                }

                if (last < data + i)
                    j_.warn() << last;

                JLOG(j_.warn())
                    << "featureXahauGenesis initial hook failed to validate guards, bailing";
                    
                return;
            }

            std::optional<std::string> result2 =
                hook::HookExecutor::validateWasm(wasmBytes.data(), (size_t)wasmBytes.size());

            if (result2)
            {
                JLOG(j_.warn())
                    << "featureXahauGenesis tried to set a hook with invalid code. VM error: "
                    << *result2 << ", bailing";
                return;
            }

            auto hookHash = ripple::sha512Half_s(ripple::Slice(wasmBytes.data(), wasmBytes.size()));
            auto const kl = keylet::hookDefinition(hookHash);
            if (view().exists(kl))
            {
                JLOG(j_.warn())
                    << "featureXahauGenesis genesis hookDefinition already exists !!! bailing";
                return;
            }

            auto hookDef = std::make_shared<SLE>(kl);

            hookDef->setFieldH256(sfHookHash, hookHash);
            hookDef->setFieldH256(sfHookOn, hookOn);
            hookDef->setFieldH256(sfHookNamespace, UINT256_BIT[hookCount++]);
            hookDef->setFieldArray(sfHookParameters, STArray{});
            hookDef->setFieldU8(sfHookApiVersion, 0);
            hookDef->setFieldVL(sfCreateCode, wasmBytes);
            hookDef->setFieldH256(sfHookSetTxnID,  ctx_.tx.getTransactionID());
            hookDef->setFieldU64(sfReferenceCount, 1);
            hookDef->setFieldAmount(sfFee,
                    XRPAmount {hook::computeExecutionFee(result->first)});
            if (result->second > 0)
                hookDef->setFieldAmount(sfHookCallbackFee, 
                    XRPAmount {hook::computeExecutionFee(result->second)});

            sb.insert(hookDef);

            STObject hookObj {sfHook};
            hookObj.setFieldH256(sfHookHash, hookHash);
            hooks.push_back(hookObj);

        }


        auto sle = std::make_shared<SLE>(keylet::hook(accid));
        sle->setFieldArray(sfHooks, hooks);
        sle->setAccountID(sfAccount, accid);

        auto const page = sb.dirInsert(
            keylet::ownerDir(accid),
            keylet::hook(accid),
            describeOwnerDir(accid));

        if (!page)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis genesis directory full when trying to insert hooks object, bailing";
            return;
        }
        sle->setFieldU64(sfOwnerNode, *page);
        sb.insert(sle);
    }

    JLOG(j_.warn()) << "featureXahauGenesis amendment executed successfully";
    
    sb.apply(ctx_.rawView());
}

void
Change::activateTrustLinesToSelfFix()
{
    JLOG(j_.warn()) << "fixTrustLinesToSelf amendment activation code starting";

    auto removeTrustLineToSelf = [this](Sandbox& sb, uint256 id) {
        auto tl = sb.peek(keylet::child(id));

        if (tl == nullptr)
        {
            JLOG(j_.warn()) << id << ": Unable to locate trustline";
            return true;
        }

        if (tl->getType() != ltRIPPLE_STATE)
        {
            JLOG(j_.warn()) << id << ": Unexpected type "
                            << static_cast<std::uint16_t>(tl->getType());
            return true;
        }

        auto const& lo = tl->getFieldAmount(sfLowLimit);
        auto const& hi = tl->getFieldAmount(sfHighLimit);

        if (lo != hi)
        {
            JLOG(j_.warn()) << id << ": Trustline doesn't meet requirements";
            return true;
        }

        if (auto const page = tl->getFieldU64(sfLowNode); !sb.dirRemove(
                keylet::ownerDir(lo.getIssuer()), page, tl->key(), false))
        {
            JLOG(j_.error()) << id << ": failed to remove low entry from "
                             << toBase58(lo.getIssuer()) << ":" << page
                             << " owner directory";
            return false;
        }

        if (auto const page = tl->getFieldU64(sfHighNode); !sb.dirRemove(
                keylet::ownerDir(hi.getIssuer()), page, tl->key(), false))
        {
            JLOG(j_.error()) << id << ": failed to remove high entry from "
                             << toBase58(hi.getIssuer()) << ":" << page
                             << " owner directory";
            return false;
        }

        if (tl->getFlags() & lsfLowReserve)
            adjustOwnerCount(
                sb, sb.peek(keylet::account(lo.getIssuer())), -1, j_);

        if (tl->getFlags() & lsfHighReserve)
            adjustOwnerCount(
                sb, sb.peek(keylet::account(hi.getIssuer())), -1, j_);

        sb.erase(tl);

        JLOG(j_.warn()) << "Successfully deleted trustline " << id;

        return true;
    };

    using namespace std::literals;

    Sandbox sb(&view());

    if (removeTrustLineToSelf(
            sb,
            uint256{
                "2F8F21EFCAFD7ACFB07D5BB04F0D2E18587820C7611305BB674A64EAB0FA71E1"sv}) &&
        removeTrustLineToSelf(
            sb,
            uint256{
                "326035D5C0560A9DA8636545DD5A1B0DFCFF63E68D491B5522B767BB00564B1A"sv}))
    {
        JLOG(j_.warn()) << "fixTrustLinesToSelf amendment activation code "
                           "executed successfully";
        sb.apply(ctx_.rawView());
    }
}

TER
Change::applyAmendment()
{
    uint256 amendment(ctx_.tx.getFieldH256(sfAmendment));

    auto const k = keylet::amendments();

    SLE::pointer amendmentObject = view().peek(k);

    if (!amendmentObject)
    {
        amendmentObject = std::make_shared<SLE>(k);
        view().insert(amendmentObject);
    }

    STVector256 amendments = amendmentObject->getFieldV256(sfAmendments);

    if (std::find(amendments.begin(), amendments.end(), amendment) !=
        amendments.end())
        return tefALREADY;

    auto flags = ctx_.tx.getFlags();

    const bool gotMajority = (flags & tfGotMajority) != 0;
    const bool lostMajority = (flags & tfLostMajority) != 0;

    if (gotMajority && lostMajority)
        return temINVALID_FLAG;

    STArray newMajorities(sfMajorities);

    bool found = false;
    if (amendmentObject->isFieldPresent(sfMajorities))
    {
        const STArray& oldMajorities =
            amendmentObject->getFieldArray(sfMajorities);
        for (auto const& majority : oldMajorities)
        {
            if (majority.getFieldH256(sfAmendment) == amendment)
            {
                if (gotMajority)
                    return tefALREADY;
                found = true;
            }
            else
            {
                // pass through
                newMajorities.push_back(majority);
            }
        }
    }

    if (!found && lostMajority)
        return tefALREADY;

    if (gotMajority)
    {
        // This amendment now has a majority
        newMajorities.push_back(STObject(sfMajority));
        auto& entry = newMajorities.back();
        entry.emplace_back(STUInt256(sfAmendment, amendment));
        entry.emplace_back(STUInt32(
            sfCloseTime, view().parentCloseTime().time_since_epoch().count()));

        if (!ctx_.app.getAmendmentTable().isSupported(amendment))
        {
            JLOG(j_.warn()) << "Unsupported amendment " << amendment
                            << " received a majority.";
        }
    }
    else if (!lostMajority)
    {
        // No flags, enable amendment
        amendments.push_back(amendment);
        amendmentObject->setFieldV256(sfAmendments, amendments);

        if (amendment == fixTrustLinesToSelf)
            activateTrustLinesToSelfFix();
        else if (amendment == featureXahauGenesis)
            activateXahauGenesis();

        ctx_.app.getAmendmentTable().enable(amendment);

        if (!ctx_.app.getAmendmentTable().isSupported(amendment))
        {
            JLOG(j_.error()) << "Unsupported amendment " << amendment
                             << " activated: server blocked.";
            ctx_.app.getOPs().setAmendmentBlocked();
        }
    }

    if (newMajorities.empty())
        amendmentObject->makeFieldAbsent(sfMajorities);
    else
        amendmentObject->setFieldArray(sfMajorities, newMajorities);

    view().update(amendmentObject);

    return tesSUCCESS;
}

TER
Change::applyFee()
{
    auto const k = keylet::fees();

    SLE::pointer feeObject = view().peek(k);

    if (!feeObject)
    {
        feeObject = std::make_shared<SLE>(k);
        view().insert(feeObject);
    }
    auto set = [](SLE::pointer& feeObject, STTx const& tx, auto const& field) {
        feeObject->at(field) = tx[field];
    };
    if (view().rules().enabled(featureXRPFees))
    {
        set(feeObject, ctx_.tx, sfBaseFeeDrops);
        set(feeObject, ctx_.tx, sfReserveBaseDrops);
        set(feeObject, ctx_.tx, sfReserveIncrementDrops);
        // Ensure the old fields are removed
        feeObject->makeFieldAbsent(sfBaseFee);
        feeObject->makeFieldAbsent(sfReferenceFeeUnits);
        feeObject->makeFieldAbsent(sfReserveBase);
        feeObject->makeFieldAbsent(sfReserveIncrement);
    }
    else
    {
        set(feeObject, ctx_.tx, sfBaseFee);
        set(feeObject, ctx_.tx, sfReferenceFeeUnits);
        set(feeObject, ctx_.tx, sfReserveBase);
        set(feeObject, ctx_.tx, sfReserveIncrement);
    }

    view().update(feeObject);

    JLOG(j_.warn()) << "Fees have been changed";
    return tesSUCCESS;
}

TER
Change::applyEmitFailure()
{
    uint256 txnID(ctx_.tx.getFieldH256(sfTransactionHash));
    do
    {
        JLOG(j_.warn())
            << "HookEmit[" << txnID << "]: ttEmitFailure removing emitted txn";

        auto key = keylet::emittedTxn(txnID);

        auto const& sle = view().peek(key);

        if (!sle)
        {
            // RH NOTE: This will now be the normal execution path, the alternative will only occur if something
            // went really wrong with the hook callback
//            JLOG(j_.warn())
//                << "HookError[" << txnID << "]: ttEmitFailure (Change) tried to remove already removed emittedtxn";
            break;
        }

        if (!view().dirRemove(
                keylet::emittedDir(),
                sle->getFieldU64(sfOwnerNode),
                key,
                false))
        {
            JLOG(j_.fatal())
                << "HookError[" << txnID << "]: ttEmitFailure (Change) tefBAD_LEDGER";
            return tefBAD_LEDGER;
        }

        view().erase(sle);
    } while (0);
    return tesSUCCESS;
}

TER
Change::applyUNLModify()
{
    if (!isFlagLedger(view().seq()))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, not a flag ledger, seq="
                        << view().seq();
        return tefFAILURE;
    }

    if (!ctx_.tx.isFieldPresent(sfUNLModifyDisabling) ||
        ctx_.tx.getFieldU8(sfUNLModifyDisabling) > 1 ||
        !ctx_.tx.isFieldPresent(sfLedgerSequence) ||
        !ctx_.tx.isFieldPresent(sfUNLModifyValidator))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, wrong Tx format.";
        return tefFAILURE;
    }

    bool const disabling = ctx_.tx.getFieldU8(sfUNLModifyDisabling);
    auto const seq = ctx_.tx.getFieldU32(sfLedgerSequence);
    if (seq != view().seq())
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, wrong ledger seq=" << seq;
        return tefFAILURE;
    }

    Blob const validator = ctx_.tx.getFieldVL(sfUNLModifyValidator);
    if (!publicKeyType(makeSlice(validator)))
    {
        JLOG(j_.warn()) << "N-UNL: applyUNLModify, bad validator key";
        return tefFAILURE;
    }

    JLOG(j_.info()) << "N-UNL: applyUNLModify, "
                    << (disabling ? "ToDisable" : "ToReEnable")
                    << " seq=" << seq
                    << " validator data:" << strHex(validator);

    auto const k = keylet::negativeUNL();
    SLE::pointer negUnlObject = view().peek(k);
    if (!negUnlObject)
    {
        negUnlObject = std::make_shared<SLE>(k);
        view().insert(negUnlObject);
    }

    bool const found = [&] {
        if (negUnlObject->isFieldPresent(sfDisabledValidators))
        {
            auto const& negUnl =
                negUnlObject->getFieldArray(sfDisabledValidators);
            for (auto const& v : negUnl)
            {
                if (v.isFieldPresent(sfPublicKey) &&
                    v.getFieldVL(sfPublicKey) == validator)
                    return true;
            }
        }
        return false;
    }();

    if (disabling)
    {
        // cannot have more than one toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, already has ToDisable";
            return tefFAILURE;
        }

        // cannot be the same as toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToReEnable) == validator)
            {
                JLOG(j_.warn())
                    << "N-UNL: applyUNLModify, ToDisable is same as ToReEnable";
                return tefFAILURE;
            }
        }

        // cannot be in negative UNL already
        if (found)
        {
            JLOG(j_.warn())
                << "N-UNL: applyUNLModify, ToDisable already in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToDisable, validator);
    }
    else
    {
        // cannot have more than one toReEnable
        if (negUnlObject->isFieldPresent(sfValidatorToReEnable))
        {
            JLOG(j_.warn()) << "N-UNL: applyUNLModify, already has ToReEnable";
            return tefFAILURE;
        }

        // cannot be the same as toDisable
        if (negUnlObject->isFieldPresent(sfValidatorToDisable))
        {
            if (negUnlObject->getFieldVL(sfValidatorToDisable) == validator)
            {
                JLOG(j_.warn())
                    << "N-UNL: applyUNLModify, ToReEnable is same as ToDisable";
                return tefFAILURE;
            }
        }

        // must be in negative UNL
        if (!found)
        {
            JLOG(j_.warn())
                << "N-UNL: applyUNLModify, ToReEnable is not in negative UNL";
            return tefFAILURE;
        }

        negUnlObject->setFieldVL(sfValidatorToReEnable, validator);
    }

    view().update(negUnlObject);
    return tesSUCCESS;
}

}  // namespace ripple
