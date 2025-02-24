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

#include <ripple/app/hook/Guard.h>
#include <ripple/app/hook/applyHook.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/tx/impl/Change.h>
#include <ripple/app/tx/impl/SetSignerList.h>
#include <ripple/app/tx/impl/XahauGenesis.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/Sandbox.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <string_view>

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

    if (ctx.tx.getTxnType() == ttUNL_REPORT)
    {
        if (!ctx.rules.enabled(featureXahauGenesis))
        {
            JLOG(ctx.j.warn()) << "Change: UNLReport is not enabled.";
            return temDISABLED;
        }

        if (!ctx.tx.isFieldPresent(sfActiveValidator) &&
            !ctx.tx.isFieldPresent(sfImportVLKey))
        {
            JLOG(ctx.j.warn()) << "Change: UNLReport must specify at least one "
                                  "of sfImportVLKey, sfActiveValidator";
            return temMALFORMED;
        }

        // if we do specify import_vl_keys in config then we won't approve keys
        // that aren't on our list
        if (ctx.tx.isFieldPresent(sfImportVLKey) &&
            !ctx.app.config().IMPORT_VL_KEYS.empty())
        {
            auto const& inner = const_cast<ripple::STTx&>(ctx.tx)
                                    .getField(sfImportVLKey)
                                    .downcast<STObject>();
            auto const pk = inner.getFieldVL(sfPublicKey);
            std::string const strPk = strHex(makeSlice(pk));
            if (ctx.app.config().IMPORT_VL_KEYS.find(strPk) ==
                ctx.app.config().IMPORT_VL_KEYS.end())
                return telIMPORT_VL_KEY_NOT_RECOGNISED;
        }
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

    auto const seq = view().info().seq;

    bool const created = !sle;

    if (created)
        sle = std::make_shared<SLE>(keylet::UNLReport());

    bool const reset = sle->isFieldPresent(sfPreviousTxnLgrSeq) &&
        sle->getFieldU32(sfPreviousTxnLgrSeq) < seq;

    auto canonicalize = [&](SField const& arrayType,
                            SField const& objType) -> std::vector<STObject> {
        auto const existing = reset || !sle->isFieldPresent(arrayType)
            ? STArray(arrayType)
            : sle->getFieldArray(arrayType);

        // canonically order using std::set
        std::map<PublicKey, AccountID> ordered;
        for (auto const& obj : existing)
        {
            auto pk = obj.getFieldVL(sfPublicKey);
            if (!publicKeyType(makeSlice(pk)))
                continue;

            PublicKey p(makeSlice(pk));
            ordered.emplace(
                p,
                obj.isFieldPresent(sfAccount) ? obj.getAccountID(sfAccount)
                                              : calcAccountID(p));
        };

        if (ctx_.tx.isFieldPresent(objType))
        {
            auto pk = const_cast<ripple::STTx&>(ctx_.tx)
                          .getField(objType)
                          .downcast<STObject>()
                          .getFieldVL(sfPublicKey);

            if (publicKeyType(makeSlice(pk)))
            {
                PublicKey p(makeSlice(pk));
                ordered.emplace(p, calcAccountID(p));
            }
        }

        std::vector<STObject> out;
        out.reserve(ordered.size());
        for (auto const& [k, a] : ordered)
        {
            out.emplace_back(objType);
            out.back().setFieldVL(sfPublicKey, k);
            out.back().setAccountID(sfAccount, a);
        }

        return out;
    };

    bool const hasAV = ctx_.tx.isFieldPresent(sfActiveValidator);
    bool const hasVL = ctx_.tx.isFieldPresent(sfImportVLKey);

    // update
    if (hasAV)
        sle->setFieldArray(
            sfActiveValidators,
            STArray(
                canonicalize(sfActiveValidators, sfActiveValidator),
                sfActiveValidators));

    if (hasVL)
        sle->setFieldArray(
            sfImportVLKeys,
            STArray(
                canonicalize(sfImportVLKeys, sfImportVLKey), sfImportVLKeys));

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

struct L2Table
{
    std::string account;
    AccountID id;
    std::vector<std::string> members;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> params;
};

inline std::tuple<
    std::vector<std::pair<std::string, XRPAmount>>,  // non-goverance
                                                     // distribution
    std::vector<std::pair<std::string, XRPAmount>>,  // L1 distribution
    std::vector<L2Table>,                            // L2 membership
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>>
normalizeXahauGenesis(
    std::vector<std::pair<std::string, XRPAmount>> const& ngentries,
    std::vector<std::pair<std::string, XRPAmount>> const& l1entries,
    std::vector<std::pair<std::string, std::vector<std::string>>> l2entries,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> params,
    beast::Journal const& j)
{
    auto getID = [](std::string const& rn)
        -> std::optional<std::pair<std::string, AccountID>> {
        if (rn.c_str()[0] == 'r')
        {
            auto parsed = parseBase58<AccountID>(rn);
            if (!parsed)
                return {};
            return {{toBase58(*parsed), *parsed}};
        }

        if (rn.c_str()[0] == 'n')
        {
            auto const parsed =
                parseBase58<PublicKey>(TokenType::NodePublic, rn);
            if (!parsed)
                return {};
            AccountID id = calcAccountID(*parsed);
            return {{toBase58(id), id}};
        }

        return {};
    };

    std::set<AccountID> NGAccs;
    std::vector<std::pair<std::string, XRPAmount>> NGAmounts;
    for (auto const& [rn, x] : ngentries)
    {
        if (auto parsed = getID(rn); parsed)
        {
            std::string& idStr = parsed->first;
            AccountID& id = parsed->second;

            NGAmounts.emplace_back(idStr, x);
            JLOG(j.warn()) << "featureXahauGenesis: "
                           << "initial non-governance distribution: " << rn
                           << " =>accid: " << idStr;

            NGAccs.emplace(id);
            continue;
        }

        JLOG(j.warn())
            << "featureXahauGenesis could not parse ngentries address: " << rn;
    }

    std::set<AccountID> L1Seats;
    std::vector<std::pair<std::string, XRPAmount>> amounts;
    uint8_t mc = 0;
    for (auto const& [rn, x] : l1entries)
    {
        if (auto parsed = getID(rn); parsed)
        {
            std::string& idStr = parsed->first;
            AccountID& id = parsed->second;

            if (L1Seats.find(id) != L1Seats.end())
            {
                JLOG(j.warn()) << "featureXahauGenesis L1Seat: " << idStr
                               << " appears more than once in l1entries.\n";
                continue;
            }

            if (NGAccs.find(id) != NGAccs.end())
            {
                JLOG(j.warn()) << "featureXahauGenesis L1Seat: " << idStr
                               << " appears in non-governance accounts and "
                                  "l1entries. skipping l1.\n";
                continue;
            }

            amounts.emplace_back(idStr, x);
            JLOG(j.warn()) << "featureXahauGenesis: "
                           << "initial validator: " << rn
                           << " =>accid: " << idStr;

            // initial member enumeration
            params.emplace_back(
                std::vector<uint8_t>{'I', 'S', mc++},
                std::vector<uint8_t>(id.data(), id.data() + 20));

            L1Seats.emplace(id);
            continue;
        }

        JLOG(j.warn())
            << "featureXahauGenesis could not parse l1entries address: " << rn;
    }

    // initial member count
    params.emplace_back(
        std::vector<uint8_t>{'I', 'M', 'C'}, std::vector<uint8_t>{mc});

    std::vector<L2Table> tables;
    for (auto const& [table, members] : l2entries)
    {
        if (auto parsed = getID(table); parsed)
        {
            if (L1Seats.find(parsed->second) == L1Seats.end())
            {
                JLOG(j.warn()) << "featureXahauGenesis L2Table does not sit at "
                                  "an L1 seat. skipping.";
                continue;
            }

            L2Table t;
            t.account = parsed->first;
            t.id = parsed->second;
            uint8_t mc = 0;
            for (auto const& m : members)
            {
                if (auto parsed = getID(m); parsed)
                {
                    t.members.push_back(parsed->first);
                    t.params.emplace_back(
                        std::vector<uint8_t>{'I', 'S', mc++},
                        std::vector<uint8_t>(
                            parsed->second.data(), parsed->second.data() + 20));
                }
                else
                    JLOG(j.warn())
                        << "featureXahauGenesis L2Table member: " << m
                        << " unable to be parsed. skipping.";
            }

            t.params.emplace_back(
                std::vector<uint8_t>{'I', 'M', 'C'}, std::vector<uint8_t>{mc});
            tables.push_back(t);
        }
        else
            JLOG(j.warn())
                << "featureXahauGenesis could not parse L2 table address: "
                << table;
    }

    return {NGAmounts, amounts, tables, params};
};

void
Change::activateXahauGenesis()
{
    JLOG(j_.warn()) << "featureXahauGenesis amendment activation code starting";

    using namespace XahauGenesis;

    bool const isTest =
        (ctx_.tx.getFlags() & tfTestSuite) && ctx_.app.config().standalone();

    // RH NOTE: we'll only configure xahau governance structure on networks that
    // begin with 2133... so production xahau: 21337 and its testnet 21338
    // with 21330-21336 and 21339 also valid and reserved for dev nets etc.
    // all other Network IDs will be conventionally configured.
    if ((ctx_.app.config().NETWORK_ID / 10) != 2133 && !isTest)
        return;

    auto [ng_entries, l1_entries, l2_entries, gov_params] =
        normalizeXahauGenesis(
            isTest ? TestNonGovernanceDistribution : NonGovernanceDistribution,
            isTest ? TestL1Membership : L1Membership,
            isTest ? TestL2Membership : L2Membership,
            GovernanceParameters,
            j_);

    std::vector<std::tuple<
        uint256,               // hook on
        std::vector<uint8_t>,  // hook code
        std::vector<std::pair<
            std::vector<uint8_t>,     // param name
            std::vector<uint8_t>>>>>  // param value
        genesis_hooks = {
            {GovernanceHookOn, GovernanceHook, gov_params},
            {RewardHookOn, RewardHook, {}}};

    Sandbox sb(&view());

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

    // running total of the amount of xrp we will burn from the genesis, less
    // the initial distributions
    auto destroyedXRP = sle->getFieldAmount(sfBalance).xrp() - GenesisAmount;

    // Step 1: burn genesis funds to (almost) zero
    sle->setFieldAmount(sfBalance, GenesisAmount);

    // Step 2: mint genesis distribution
    auto mint = [&](std::string const& account, XRPAmount const& amount) {
        auto accid_raw = parseBase58<AccountID>(account);
        if (!accid_raw)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis could not parse an r-address: "
                << account;
            return;
        }

        auto accid = *accid_raw;

        auto const kl = keylet::account(accid);

        auto sle = sb.peek(kl);
        auto const exists = !!sle;

        STAmount newBal = STAmount{amount};
        STAmount existingBal =
            exists ? sle->getFieldAmount(sfBalance) : STAmount{XRPAmount{0}};
        STAmount adjustment = newBal - existingBal;

        // the account should not exist but if it does then handle it properly
        if (!exists)
        {
            sle = std::make_shared<SLE>(kl);
            sle->setAccountID(sfAccount, accid);
            std::uint32_t const seqno{
                sb.info().parentCloseTime.time_since_epoch().count()};
            sle->setFieldU32(sfSequence, seqno);
        }

        sle->setFieldAmount(sfBalance, newBal);

        destroyedXRP -= adjustment.xrp();

        if (exists)
            sb.update(sle);
        else
            sb.insert(sle);
    };

    // non governance distributions
    for (auto const& [account, amount] : ng_entries)
        mint(account, amount);

    // l1 seat distributions
    for (auto const& [account, amount] : l1_entries)
        mint(account, amount);

    // Step 3: blackhole genesis
    sle->setAccountID(sfRegularKey, noAccount());
    sle->setFieldU32(sfFlags, lsfDisableMaster);

    // if somehow there's a signerlist we need to delete it
    if (sb.exists(keylet::signers(accid)))
        SetSignerList::removeFromLedger(ctx_.app, sb, accid, j_);

    // Step 4: install genesis hooks
    sle->setFieldU32(
        sfOwnerCount, sle->getFieldU32(sfOwnerCount) + genesis_hooks.size());
    sb.update(sle);

    if (sb.exists(keylet::hook(accid)))
    {
        JLOG(j_.warn()) << "featureXahauGenesis genesis account already has "
                           "hooks object in ledger, bailing";
        return;
    }

    {
        ripple::STArray hooks{sfHooks, static_cast<int>(genesis_hooks.size())};
        int hookCount = 0;

        for (auto const& [hookOn, wasmBytes, params] : genesis_hooks)
        {
            std::ostringstream loggerStream;
            auto result = validateGuards(
                wasmBytes,  // wasm to verify
                loggerStream,
                "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                (ctx_.view().rules().enabled(featureHooksUpdate1) ? 1 : 0) +
                    (ctx_.view().rules().enabled(fix20250131) ? 2 : 0));

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

                JLOG(j_.warn()) << "featureXahauGenesis initial hook failed to "
                                   "validate guards, bailing";

                return;
            }

            std::optional<std::string> result2 =
                hook::HookExecutor::validateWasm(
                    wasmBytes.data(), (size_t)wasmBytes.size());

            if (result2)
            {
                JLOG(j_.warn()) << "featureXahauGenesis tried to set a hook "
                                   "with invalid code. VM error: "
                                << *result2 << ", bailing";
                return;
            }

            auto const hookHash = ripple::sha512Half_s(
                ripple::Slice(wasmBytes.data(), wasmBytes.size()));

            auto const kl = keylet::hookDefinition(hookHash);
            if (view().exists(kl))
            {
                JLOG(j_.warn()) << "featureXahauGenesis genesis hookDefinition "
                                   "already exists !!! bailing";
                return;
            }

            auto hookDef = std::make_shared<SLE>(kl);

            hookDef->setFieldH256(sfHookHash, hookHash);
            hookDef->setFieldH256(sfHookOn, hookOn);
            hookDef->setFieldH256(
                sfHookNamespace,
                ripple::uint256("0000000000000000000000000000000000000000000000"
                                "000000000000000000"));

            hookDef->setFieldU16(sfHookApiVersion, 0);
            hookDef->setFieldVL(sfCreateCode, wasmBytes);
            hookDef->setFieldH256(sfHookSetTxnID, ctx_.tx.getTransactionID());
            // governance hook is referenced by the l2tables
            hookDef->setFieldU64(
                sfReferenceCount,
                (hookCount++ == 0 ? l2_entries.size() : 0) + 1);
            hookDef->setFieldAmount(
                sfFee, XRPAmount{hook::computeExecutionFee(result->first)});
            if (result->second > 0)
                hookDef->setFieldAmount(
                    sfHookCallbackFee,
                    XRPAmount{hook::computeExecutionFee(result->second)});

            sb.insert(hookDef);

            STObject hookObj{sfHook};
            hookObj.setFieldH256(sfHookHash, hookHash);
            // parameters
            {
                std::vector<STObject> vec;
                for (auto const& [k, v] : params)
                {
                    STObject param(sfHookParameter);
                    param.setFieldVL(sfHookParameterName, k);
                    param.setFieldVL(sfHookParameterValue, v);
                    vec.emplace_back(std::move(param));
                };

                hookObj.setFieldArray(
                    sfHookParameters, STArray(vec, sfHookParameters));
            }

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
            JLOG(j_.warn()) << "featureXahauGenesis genesis directory full "
                               "when trying to insert hooks object, bailing";
            return;
        }
        sle->setFieldU64(sfOwnerNode, *page);
        sb.insert(sle);
    }

    // install hooks on layer 2 tables
    auto const governHash = ripple::sha512Half_s(ripple::Slice(
        XahauGenesis::GovernanceHook.data(),
        XahauGenesis::GovernanceHook.size()));
    for (auto const& t : l2_entries)
    {
        JLOG(j_.trace()) << "featureXahauGenesis: installing L2 table at: "
                         << t.account << " with " << t.members.size()
                         << " members\n";

        auto const hookKL = keylet::hook(t.id);
        if (sb.exists(hookKL))
        {
            JLOG(j_.warn()) << "featureXahauGenesis layer2 table account "
                               "already has hooks object in ledger, bailing";
            return;
        }

        ripple::STArray hooks{sfHooks, 1};
        STObject hookObj{sfHook};
        hookObj.setFieldH256(sfHookHash, governHash);
        // parameters
        {
            std::vector<STObject> vec;
            for (auto const& [k, v] : t.params)
            {
                STObject param(sfHookParameter);
                param.setFieldVL(sfHookParameterName, k);
                param.setFieldVL(sfHookParameterValue, v);
                vec.emplace_back(std::move(param));
            };

            hookObj.setFieldArray(
                sfHookParameters, STArray(vec, sfHookParameters));
        }

        hooks.push_back(hookObj);

        auto sle = std::make_shared<SLE>(hookKL);
        sle->setFieldArray(sfHooks, hooks);
        sle->setAccountID(sfAccount, t.id);

        auto const page = sb.dirInsert(
            keylet::ownerDir(t.id), keylet::hook(t.id), describeOwnerDir(t.id));

        if (!page)
        {
            JLOG(j_.warn())
                << "featureXahauGenesis layer2 table directory full when "
                   "trying to insert hooks object, bailing";
            return;
        }
        sle->setFieldU64(sfOwnerNode, *page);
        sb.insert(sle);

        // blackhole the l2 account
        {
            auto const kl = keylet::account(t.id);
            auto sle = sb.peek(kl);

            sle->setAccountID(sfRegularKey, noAccount());
            sle->setFieldU32(sfFlags, lsfDisableMaster);
            sle->setFieldU32(sfOwnerCount, sle->getFieldU32(sfOwnerCount) + 1);
            sb.update(sle);
        }
    }

    JLOG(j_.warn()) << "featureXahauGenesis amendment executed successfully";

    if (destroyedXRP < beast::zero)
    {
        JLOG(j_.warn()) << "featureXahauGenesis: destroyed XRP tally was "
                           "negative, bailing.";
        return;
    }

    // record the start ledger
    auto sleFees = sb.peek(keylet::fees());
    sleFees->setFieldU32(sfXahauActivationLgrSeq, sb.info().seq);
    sb.update(sleFees);

    sb.apply(ctx_.rawView());
    ctx_.rawView().rawDestroyXRP(destroyedXRP);
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
        JLOG(j_.warn()) << "HookEmit[" << txnID
                        << "]: ttEmitFailure removing emitted txn";

        auto key = keylet::emittedTxn(txnID);

        auto const& sle = view().peek(key);

        if (!sle)
        {
            // RH NOTE: This will now be the normal execution path, the
            // alternative will only occur if something went really wrong with
            // the hook callback
            //            JLOG(j_.warn())
            //                << "HookError[" << txnID << "]: ttEmitFailure
            //                (Change) tried to remove already removed
            //                emittedtxn";
            break;
        }

        if (!view().dirRemove(
                keylet::emittedDir(),
                sle->getFieldU64(sfOwnerNode),
                key,
                false))
        {
            JLOG(j_.fatal()) << "HookError[" << txnID
                             << "]: ttEmitFailure (Change) tefBAD_LEDGER";
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
