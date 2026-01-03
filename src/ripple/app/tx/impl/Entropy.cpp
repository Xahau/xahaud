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

#include <ripple/app/tx/impl/Entropy.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/st.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

TxConsequences
Entropy::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
Entropy::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    return preflight2(ctx);
}

TER
Entropy::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureRNG))
        return temDISABLED;

//    auto const seq = ctx.view.info().seq;

//    auto const txLgrSeq = ctx.tx[sfLedgerSequence];

    // due to circulation we'll accept up to one ledger old entropy txns
//    if (seq != txLgrSeq)
//    {
//        JLOG(ctx.j.warn()) << "Entropy: wrong ledger txseq=" << txLgrSeq << " lgrseq=" << seq << " acc:" << ctx.tx.getAccountID(sfAccount);
//        return tefFAILURE;
//    }

    // account must be a valid UV 
    if (!inUNLReport(ctx.view, ctx.tx.getAccountID(sfAccount), ctx.j))
    {
        JLOG(ctx.j.warn())
            << "Entropy: Txn Account isn't in the UNLReport.";
        return tefFAILURE;
    }

    return tesSUCCESS;
}

TER
Entropy::doApply()
{

    auto const seq = view().info().seq;

//    if (seq != ctx_.tx.getFieldU32(sfLedgerSequence))
//    {
//        return tefFAILURE;
//    }

    auto sle = view().peek(keylet::random());

    bool const created = !sle;

    if (created)
    {
        sle = std::make_shared<SLE>(keylet::random());
    }

    auto lastSeq = created ? 0 : sle->getFieldU32(sfLedgerSequence);

    if (lastSeq < seq)
    {
        // update the ledger sequence of the object
        sle->setFieldU32(sfLedgerSequence, seq);

        // reset entropy count to zero... this will probably be
        // one after the below executes but its possible the digest
        // doesn't match and the entropy count isn't incremented
        sle->setFieldU16(sfEntropyCount, 0);

        // swap the random data out ready for this round of entropy collection
        sle->setFieldH256(sfLastRandomData, sle->getFieldH256(sfRandomData));
        sle->setFieldH256(sfRandomData, beast::zero);
    }

    uint256 nextDigest = ctx_.tx.getFieldH256(sfNextRandomDigest);
    uint256 currentEntropy = ctx_.tx.getFieldH256(sfRandomData);
    uint256 currentDigest = sha512Half(currentEntropy);

    AccountID const validator = ctx_.tx.getAccountID(sfAccount);

    // iterate the digest array to find the entry if it exists
    STArray digestEntries = sle->getFieldArray(sfRandomDigests);
    std::map<AccountID, STObject> entries;

    for (auto& entry : digestEntries)
    {
        // we'll automatically clean up really old entries by just omitting them from
        // the map here
        if (entry.getFieldU32(sfLedgerSequence) < seq - 5)
            continue;

        entries.emplace(entry.getAccountID(sfValidator), std::move(entry));
    }

    if (auto it = entries.find(validator); it != entries.end())
    {
        auto& entry = it->second;

        // ensure the precommitted digest matches the provided entropy
        if (entry.getFieldH256(sfNextRandomDigest) != currentDigest)
        {
            if (entry.getFieldU32(sfLedgerSequence) != seq - 1)
            {
                // this is a skip-ahead or missed last txn somehow, so ignore, but no warning.
            }
            else
            {
                // this is a clear violation so warn (and ignore the entropy)
                JLOG(j_.warn()) << "!!! Validator " << validator << " supplied entropy that "
                    << "does not match precommitment value !!!";
            }
        }
        else
        {

            // contribute the new entropy to the random data field
            sle->setFieldH256(sfRandomData, sha512Half(validator, sle->getFieldH256(sfRandomData), currentEntropy));

            // increment entropy count
            sle->setFieldU16(sfEntropyCount, sle->getFieldU16(sfEntropyCount) + 1);
        }

        // update the digest entry
        entry.setFieldH256(sfNextRandomDigest, nextDigest);
        entry.setFieldU32(sfLedgerSequence, seq);
    }
    else
    {
        // this validator doesn't have an entry so create one
        STObject entry{sfRandomDigestEntry};
        entry.setAccountID(sfValidator, validator);
        entry.setFieldH256(sfNextRandomDigest, nextDigest);
        entry.setFieldU32(sfLedgerSequence, seq);
        entries.emplace(validator, std::move(entry));
    }

    // update the array
    STArray newEntries(sfRandomDigests);
    newEntries.reserve(entries.size());
    for (auto& [_, entry] : entries)
        newEntries.push_back(std::move(entry));

    sle->setFieldArray(sfRandomDigests, std::move(newEntries));

    // send it off to the ledger
    if (!created)
        view().update(sle);
    else
        view().insert(sle);

    return tesSUCCESS;
}


XRPAmount
Entropy::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount extraFee{0};

    if (tx.isFieldPresent(sfBlob))
        extraFee +=
            XRPAmount{static_cast<XRPAmount>(tx.getFieldVL(sfBlob).size())};

    // old code (prior to fixXahauV1)
    if (!view.rules().enabled(fixXahauV1))
    {
        if (tx.isFieldPresent(sfHookParameters))
        {
            uint64_t paramBytes = 0;
            auto const& params = tx.getFieldArray(sfHookParameters);
            for (auto const& param : params)
            {
                paramBytes +=
                    (param.isFieldPresent(sfHookParameterName)
                         ? param.getFieldVL(sfHookParameterName).size()
                         : 0) +
                    (param.isFieldPresent(sfHookParameterValue)
                         ? param.getFieldVL(sfHookParameterValue).size()
                         : 0);
            }
            extraFee += XRPAmount{static_cast<XRPAmount>(paramBytes)};
        }
    }

    return Transactor::calculateBaseFee(view, tx) + extraFee;
}

// if this validator is on the UNLReport then return a signed ttENTROPY transaction
// to be added to the txq.
std::shared_ptr<STTx const>
makeEntropyTxn(OpenView& view, Application& app, beast::Journal const& j_)
{
    // Inject an RNG psuedo if we're on the UNL
    if (!view.rules().enabled(featureRNG))
        return {};

    JLOG(j_.debug())
        << "ENTROPY processing: started";

    auto const seq = view.info().seq;

    static uint32_t lastSeq = 0;

    // we only generate once per ledger
    if (lastSeq == seq)
        return {};

    lastSeq = seq;

    // if we're not a validator we do nothing here
    if (app.getValidationPublicKey().empty())
        return {};

    PublicKey pkSigning = app.getValidationPublicKey();

    auto const pk = app.validatorManifests().getMasterKey(pkSigning);

    if (!inUNLReport(view, app, pk, j_))
        return {};

    // build and sign a txn

    AccountID acc = calcAccountID(pk);

    static auto getRnd = []() -> uint256 {
        static std::ifstream rng("/dev/urandom", std::ios::binary);
        uint256 out;
        if (rng && rng.read(reinterpret_cast<char*>(out.data()), 32))
            return out;
        std::random_device rd;
        for (auto& word : out)
            word = rd();
        return out;
    };


    static std::optional<uint256> prevRnd;

    uint256 nextRnd = getRnd();

    // create txn
    auto rngTx = std::make_shared<STTx>(ttENTROPY, [&](auto& obj) {
        obj.setFieldU32(sfLastLedgerSequence, seq);
        obj.setAccountID(sfAccount, acc);
        obj.setFieldU32(sfSequence, 0);
        if (prevRnd.has_value())
            obj.setFieldH256(sfRandomData, *prevRnd);
        obj.setFieldH256(sfNextRandomDigest, sha512Half(nextRnd));
        obj.setFieldVL(sfSigningPubKey, pkSigning.slice());
        obj.setFieldH256(sfParentHash, view.info().parentHash);
        obj.setFieldU32(sfFlags, tfFullyCanonicalSig);

        if (app.config().NETWORK_ID > 1024)
            obj.setFieldU32(sfNetworkID, app.config().NETWORK_ID);
    });

    prevRnd = nextRnd;

    // sign the txn using our ephemeral key
    rngTx->sign(pkSigning, app.getValidationSecretKey());

    JLOG(j_.debug()) << "ENTROPY txn: " << rngTx->getFullText();

    return rngTx;
}

}  // namespace ripple
