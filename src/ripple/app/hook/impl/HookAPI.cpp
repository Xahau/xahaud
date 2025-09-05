// Implementation of decoupled Hook APIs for emit and related helpers.

#include <ripple/app/hook/HookAPI.h>
#include <ripple/app/hook/applyHook.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/tx/apply.h>
#include <ripple/app/tx/impl/ApplyContext.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/tokens.h>

namespace hook {

using namespace ripple;

uint64_t
HookAPI::otxn_burden() const
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");

    if (hookCtx.burden)
        return hookCtx.burden;

    auto const& tx = applyCtx.tx;
    if (!tx.isFieldPresent(sfEmitDetails))
        return 1;

    auto const& pd = const_cast<ripple::STTx&>(tx)
                         .getField(sfEmitDetails)
                         .downcast<STObject>();

    if (!pd.isFieldPresent(sfEmitBurden))
    {
        JLOG(j.warn())
            << "HookError[" << HC_ACC()
            << "]: found sfEmitDetails but sfEmitBurden was not present";
        return 1;
    }

    uint64_t burden = pd.getFieldU64(sfEmitBurden);
    burden &= ((1ULL << 63) - 1);
    hookCtx.burden = burden;
    return static_cast<int64_t>(burden);
}

uint32_t
HookAPI::otxn_generation() const
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");

    if (hookCtx.generation)
        return hookCtx.generation;

    auto const& tx = applyCtx.tx;
    if (!tx.isFieldPresent(sfEmitDetails))
        return 0;

    auto const& pd = const_cast<ripple::STTx&>(tx)
                         .getField(sfEmitDetails)
                         .downcast<STObject>();

    if (!pd.isFieldPresent(sfEmitGeneration))
    {
        JLOG(j.warn())
            << "HookError[" << HC_ACC()
            << "]: found sfEmitDetails but sfEmitGeneration was not present";
        return 0;
    }

    hookCtx.generation = pd.getFieldU32(sfEmitGeneration);
    return hookCtx.generation;
}

uint32_t
HookAPI::etxn_generation() const
{
    return otxn_generation() + 1;
}

Expected<uint64_t, HookReturnCode>
HookAPI::etxn_burden() const
{
    if (hookCtx.expected_etxn_count <= -1)
        return Unexpected(hook_api::PREREQUISITE_NOT_MET);

    uint64_t last_burden = static_cast<uint64_t>(otxn_burden());
    uint64_t burden =
        last_burden * static_cast<uint64_t>(hookCtx.expected_etxn_count);
    if (burden < last_burden)
        return Unexpected(hook_api::FEE_TOO_LARGE);
    return burden;
}

Expected<uint64_t, HookReturnCode>
HookAPI::etxn_fee_base(ripple::Slice txBlob) const
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");

    if (hookCtx.expected_etxn_count <= -1)
        return Unexpected(hook_api::PREREQUISITE_NOT_MET);

    try
    {
        SerialIter sitTrans(txBlob);
        std::unique_ptr<STTx const> stpTrans =
            std::make_unique<STTx const>(std::ref(sitTrans));
        return Transactor::calculateBaseFee(
                   *(applyCtx.app.openLedger().current()), *stpTrans)
            .drops();
    }
    catch (std::exception const& e)
    {
        JLOG(j.trace()) << "HookInfo[" << HC_ACC()
                        << "]: etxn_fee_base exception: " << e.what();
        return Unexpected(hook_api::INVALID_TXN);
    }
}

Expected<std::shared_ptr<Transaction>, HookReturnCode>
HookAPI::emit(ripple::Slice txBlob)
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");
    auto& view = applyCtx.view();

    if (hookCtx.expected_etxn_count < 0)
        return Unexpected(hook_api::PREREQUISITE_NOT_MET);

    if (hookCtx.result.emittedTxn.size() >= hookCtx.expected_etxn_count)
        return Unexpected(hook_api::TOO_MANY_EMITTED_TXN);

    std::shared_ptr<STTx const> stpTrans;
    try
    {
        SerialIter sit(txBlob);
        stpTrans = std::make_shared<STTx const>(sit);
    }
    catch (std::exception const& e)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC() << "]: Failed " << e.what();
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (isPseudoTx(*stpTrans))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Attempted to emit pseudo txn.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    ripple::TxType txType = stpTrans->getTxnType();

    ripple::uint256 const& hookCanEmit = hookCtx.result.hookCanEmit;
    if (!hook::canEmit(txType, hookCanEmit))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Hook cannot emit this txn.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // check the emitted txn is valid
    /* Emitted TXN rules
     * 0. Account must match the hook account
     * 1. Sequence: 0
     * 2. PubSigningKey: 000000000000000
     * 3. sfEmitDetails present and valid
     * 4. No sfTxnSignature
     * 5. LastLedgerSeq > current ledger, > firstledgerseq & LastLedgerSeq < seq
     * + 5
     * 6. FirstLedgerSeq > current ledger
     * 7. Fee must be correctly high
     * 8. The generation cannot be higher than 10
     */

    // rule 0: account must match the hook account
    if (!stpTrans->isFieldPresent(sfAccount) ||
        stpTrans->getAccountID(sfAccount) != hookCtx.result.account)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfAccount does not match hook account";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 1: sfSequence must be present and 0
    if (!stpTrans->isFieldPresent(sfSequence) ||
        stpTrans->getFieldU32(sfSequence) != 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSequence missing or non-zero";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 2: sfSigningPubKey must be present and 00...00
    if (!stpTrans->isFieldPresent(sfSigningPubKey))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigningPubKey missing";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    auto const pk = stpTrans->getSigningPubKey();
    if (pk.size() != 33 && pk.size() != 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigningPubKey present but wrong size";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    for (int i = 0; i < pk.size(); ++i)
        if (pk[i] != 0)
        {
            JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                            << "]: sfSigningPubKey present but non-zero.";
            return Unexpected(hook_api::EMISSION_FAILURE);
        }

    // rule 2.a: no signers
    if (stpTrans->isFieldPresent(sfSigners))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigners not allowed in emitted txns.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 2.b: ticketseq cannot be used
    if (stpTrans->isFieldPresent(sfTicketSequence))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfTicketSequence not allowed in emitted txns.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 2.c sfAccountTxnID not allowed
    if (stpTrans->isFieldPresent(sfAccountTxnID))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfAccountTxnID not allowed in emitted txns.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 3: sfEmitDetails must be present and valid
    if (!stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitDetails missing.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    auto const& emitDetails = const_cast<ripple::STTx&>(*stpTrans)
                                  .getField(sfEmitDetails)
                                  .downcast<STObject>();

    if (!emitDetails.isFieldPresent(sfEmitGeneration) ||
        !emitDetails.isFieldPresent(sfEmitBurden) ||
        !emitDetails.isFieldPresent(sfEmitParentTxnID) ||
        !emitDetails.isFieldPresent(sfEmitNonce) ||
        !emitDetails.isFieldPresent(sfEmitHookHash))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitDetails malformed.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 8: emit generation cannot exceed 10
    if (emitDetails.getFieldU32(sfEmitGeneration) >= 10)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitGeneration was 10 or more.";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    auto const gen = emitDetails.getFieldU32(sfEmitGeneration);
    auto const bur = emitDetails.getFieldU64(sfEmitBurden);
    auto const pTxnID = emitDetails.getFieldH256(sfEmitParentTxnID);
    auto const nonce = emitDetails.getFieldH256(sfEmitNonce);

    std::optional<ripple::AccountID> callback;
    if (emitDetails.isFieldPresent(sfEmitCallback))
        callback = emitDetails.getAccountID(sfEmitCallback);

    auto const& hash = emitDetails.getFieldH256(sfEmitHookHash);

    uint32_t gen_proper = static_cast<uint32_t>(etxn_generation());

    if (gen != gen_proper)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitGeneration provided in EmitDetails "
                        << "not correct (" << gen << ") "
                        << "should be " << gen_proper;
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    uint64_t bur_proper = static_cast<uint64_t>(etxn_burden().value());
    if (bur != bur_proper)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitBurden provided in EmitDetails "
                        << "was not correct (" << bur << ") "
                        << "should be " << bur_proper;
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (pTxnID != applyCtx.tx.getTransactionID())
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitParentTxnID provided in EmitDetails"
                        << "was not correct";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (hookCtx.nonce_used.find(nonce) == hookCtx.nonce_used.end())
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitNonce provided in EmitDetails was not "
                           "generated by nonce api";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (callback && *callback != hookCtx.result.account)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitCallback account must be the account of "
                           "the emitting hook";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (hash != hookCtx.result.hookHash)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfEmitHookHash must be the hash of the emitting hook";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 4: sfTxnSignature must be absent
    if (stpTrans->isFieldPresent(sfTxnSignature))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfTxnSignature is present but should not be";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 5: LastLedgerSeq must be present and after current ledger
    if (!stpTrans->isFieldPresent(sfLastLedgerSequence))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfLastLedgerSequence missing";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    uint32_t tx_lls = stpTrans->getFieldU32(sfLastLedgerSequence);
    uint32_t ledgerSeq = view.info().seq;
    if (tx_lls < ledgerSeq + 1)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfLastLedgerSequence invalid (less than next ledger)";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (tx_lls > ledgerSeq + 5)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfLastLedgerSequence cannot be greater than current seq + 5";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 6
    if (!stpTrans->isFieldPresent(sfFirstLedgerSequence) ||
        stpTrans->getFieldU32(sfFirstLedgerSequence) > tx_lls)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfFirstLedgerSequence must be present and <= "
                           "LastLedgerSequence";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // rule 7 check the emitted txn pays the appropriate fee
    int64_t minfee = etxn_fee_base(txBlob).value();

    if (minfee < 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee could not be calculated";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    if (!stpTrans->isFieldPresent(sfFee))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee missing from emitted tx";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    int64_t fee = stpTrans->getFieldAmount(sfFee).xrp().drops();
    if (fee < minfee)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee less than minimum required";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    std::string reason;
    auto tpTrans =
        std::make_shared<Transaction>(stpTrans, reason, applyCtx.app);
    if (tpTrans->getStatus() != NEW)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: tpTrans->getStatus() != NEW";
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    // preflight the transaction
    auto preflightResult = ripple::preflight(
        applyCtx.app,
        view.rules(),
        *stpTrans,
        ripple::ApplyFlags::tapPREFLIGHT_EMIT,
        j);

    if (!isTesSuccess(preflightResult.ter))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Transaction preflight failure: "
                        << preflightResult.ter;
        return Unexpected(hook_api::EMISSION_FAILURE);
    }

    return tpTrans;
}

}  // namespace hook
