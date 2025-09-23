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
#include "ripple/basics/base_uint.h"
#include <cfenv>

namespace hook {
namespace hook_float {

// power of 10 LUT for fast integer math
static int64_t power_of_ten[19] = {
    1LL,
    10LL,
    100LL,
    1000LL,
    10000LL,
    100000LL,
    1000000LL,
    10000000LL,
    100000000LL,
    1000000000LL,
    10000000000LL,
    100000000000LL,
    1000000000000LL,
    10000000000000LL,
    100000000000000LL,
    1000000000000000LL,  // 15
    10000000000000000LL,
    100000000000000000LL,
    1000000000000000000LL,
};

using namespace hook_api;
static int64_t const minMantissa = 1000000000000000ull;
static int64_t const maxMantissa = 9999999999999999ull;
static int32_t const minExponent = -96;
static int32_t const maxExponent = 80;

inline Expected<int32_t, HookReturnCode>
get_exponent(int64_t float1)
{
    if (float1 < 0)
        return Unexpected(INVALID_FLOAT);
    if (float1 == 0)
        return 0;
    uint64_t float_in = (uint64_t)float1;
    float_in >>= 54U;
    float_in &= 0xFFU;
    return ((int32_t)float_in) - 97;
}

inline Expected<uint64_t, HookReturnCode>
get_mantissa(int64_t float1)
{
    if (float1 < 0)
        return Unexpected(INVALID_FLOAT);
    if (float1 == 0)
        return 0;
    float1 -= ((((uint64_t)float1) >> 54U) << 54U);
    return float1;
}

inline bool
is_negative(int64_t float1)
{
    return ((float1 >> 62U) & 1ULL) == 0;
}

inline int64_t
invert_sign(int64_t float1)
{
    int64_t r = (int64_t)(((uint64_t)float1) ^ (1ULL << 62U));
    return r;
}

inline int64_t
set_sign(int64_t float1, bool set_negative)
{
    bool neg = is_negative(float1);
    if ((neg && set_negative) || (!neg && !set_negative))
        return float1;

    return invert_sign(float1);
}

inline Expected<uint64_t, HookReturnCode>
set_mantissa(int64_t float1, uint64_t mantissa)
{
    if (mantissa > maxMantissa)
        return Unexpected(MANTISSA_OVERSIZED);
    if (mantissa < minMantissa)
        return Unexpected(MANTISSA_UNDERSIZED);
    return float1 - get_mantissa(float1).value() + mantissa;
}

inline Expected<uint64_t, HookReturnCode>
set_exponent(int64_t float1, int32_t exponent)
{
    if (exponent > maxExponent)
        return Unexpected(EXPONENT_OVERSIZED);
    if (exponent < minExponent)
        return Unexpected(EXPONENT_UNDERSIZED);

    uint64_t exp = (exponent + 97);
    exp <<= 54U;
    float1 &= ~(0xFFLL << 54);
    float1 += (int64_t)exp;
    return float1;
}

inline Expected<uint64_t, HookReturnCode>
make_float(ripple::IOUAmount& amt)
{
    int64_t man_out = amt.mantissa();
    int64_t float_out = 0;
    bool neg = man_out < 0;
    if (neg)
        man_out *= -1;

    float_out = set_sign(float_out, neg);
    auto const mantissa = set_mantissa(float_out, (uint64_t)man_out);
    if (!mantissa)
        // TODO: This change requires the amendment.
        // return Unexpected(mantissa.error());
        float_out = mantissa.error();
    else
        float_out = mantissa.value();
    auto const exponent = set_exponent(float_out, amt.exponent());
    if (!exponent)
        return Unexpected(exponent.error());
    float_out = exponent.value();
    return float_out;
}

inline Expected<uint64_t, HookReturnCode>
make_float(uint64_t mantissa, int32_t exponent, bool neg)
{
    if (mantissa == 0)
        return 0;
    if (mantissa > maxMantissa)
        return Unexpected(MANTISSA_OVERSIZED);
    if (mantissa < minMantissa)
        return Unexpected(MANTISSA_UNDERSIZED);
    if (exponent > maxExponent)
        return Unexpected(EXPONENT_OVERSIZED);
    if (exponent < minExponent)
        return Unexpected(EXPONENT_UNDERSIZED);
    int64_t out = 0;

    auto const m = set_mantissa(out, mantissa);
    if (!m)
        return m.error();
    out = m.value();

    auto const e = set_exponent(out, exponent);
    if (!e)
        return e.error();
    out = e.value();

    out = set_sign(out, neg);
    return out;
}

/**
 * This function normalizes the mantissa and exponent passed, if it can.
 * It returns the XFL and mutates the supplied manitssa and exponent.
 * If a negative mantissa is provided then the returned XFL has the negative
 * flag set. If there is an overflow error return XFL_OVERFLOW. On underflow
 * returns canonical 0
 */
template <typename T>
inline Expected<uint64_t, HookReturnCode>
normalize_xfl(T& man, int32_t& exp, bool neg = false)
{
    if (man == 0)
        return 0;

    if (man == std::numeric_limits<int64_t>::min())
        man++;

    constexpr bool sman = std::is_same<T, int64_t>::value;
    static_assert(sman || std::is_same<T, uint64_t>());

    if constexpr (sman)
    {
        if (man < 0)
        {
            man *= -1LL;
            neg = true;
        }
    }

    // mantissa order
    std::feclearexcept(FE_ALL_EXCEPT);
    int32_t mo = log10(man);
    // defensively ensure log10 produces a sane result; we'll borrow the
    // overflow error code if it didn't
    if (std::fetestexcept(FE_INVALID))
        return Unexpected(XFL_OVERFLOW);

    int32_t adjust = 15 - mo;

    if (adjust > 0)
    {
        // defensive check
        if (adjust > 18)
            return 0;
        man *= power_of_ten[adjust];
        exp -= adjust;
    }
    else if (adjust < 0)
    {
        // defensive check
        if (-adjust > 18)
            return Unexpected(XFL_OVERFLOW);
        man /= power_of_ten[-adjust];
        exp -= adjust;
    }

    if (man == 0)
    {
        exp = 0;
        return 0;
    }

    // even after adjustment the mantissa can be outside the range by one place
    // improving the math above would probably alleviate the need for these
    // branches
    if (man < minMantissa)
    {
        if (man == minMantissa - 1LL)
            man += 1LL;
        else
        {
            man *= 10LL;
            exp--;
        }
    }

    if (man > maxMantissa)
    {
        if (man == maxMantissa + 1LL)
            man -= 1LL;
        else
        {
            man /= 10LL;
            exp++;
        }
    }

    if (exp < minExponent)
    {
        man = 0;
        exp = 0;
        return 0;
    }

    if (man == 0)
    {
        exp = 0;
        return 0;
    }

    if (exp > maxExponent)
        return Unexpected(XFL_OVERFLOW);

    auto const ret = make_float((uint64_t)man, exp, neg);
    if constexpr (sman)
    {
        if (neg)
            man *= -1LL;
    }

    if (!ret)
        return ret.error();

    return ret;
}

const int64_t float_one_internal =
    make_float(1000000000000000ull, -15, false).value();

}  // namespace hook_float

using namespace ripple;
using namespace hook_float;

Expected<const STBase*, HookReturnCode>
HookAPI::otxn_field(uint32_t field_id) const
{
    SField const& fieldType = ripple::SField::getField(field_id);

    if (fieldType == sfInvalid)
        return Unexpected(INVALID_FIELD);

    if (!hookCtx.applyCtx.tx.isFieldPresent(fieldType))
        return Unexpected(DOESNT_EXIST);

    auto const& field = hookCtx.emitFailure
        ? hookCtx.emitFailure->getField(fieldType)
        : const_cast<ripple::STTx&>(hookCtx.applyCtx.tx).getField(fieldType);

    return &field;
}

Expected<uint256, HookReturnCode>
HookAPI::otxn_id(uint32_t flags) const
{
    auto const& txID =
        (hookCtx.emitFailure && !flags
             ? hookCtx.applyCtx.tx.getFieldH256(sfTransactionHash)
             : hookCtx.applyCtx.tx.getTransactionID());

    return txID;
}

TxType
HookAPI::otxn_type() const
{
    if (hookCtx.emitFailure)
        return safe_cast<TxType>(
            hookCtx.emitFailure->getFieldU16(sfTransactionType));

    return hookCtx.applyCtx.tx.getTxnType();
}

Expected<uint32_t, HookReturnCode>
HookAPI::otxn_slot(uint32_t slot_into) const
{
    if (slot_into > hook_api::max_slots)
        return Unexpected(INVALID_ARGUMENT);

    // check if we can emplace the object to a slot
    if (slot_into == 0 && no_free_slots())
        return Unexpected(NO_FREE_SLOTS);

    if (slot_into == 0)
    {
        if (auto found = get_free_slot(); found)
            slot_into = *found;
        else
            return Unexpected(NO_FREE_SLOTS);
    }

    auto const& st_tx = std::make_shared<ripple::STObject>(
        hookCtx.emitFailure ? *(hookCtx.emitFailure)
                            : const_cast<ripple::STTx&>(hookCtx.applyCtx.tx)
                                  .downcast<ripple::STObject>());

    hookCtx.slot[slot_into] = hook::SlotEntry{.storage = st_tx, .entry = 0};

    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;
}

Expected<Blob, HookReturnCode>
HookAPI::otxn_param(Bytes const& param_name) const
{
    if (param_name.size() < 1)
        return Unexpected(TOO_SMALL);

    if (param_name.size() > 32)
        return Unexpected(TOO_BIG);

    if (!hookCtx.applyCtx.tx.isFieldPresent(sfHookParameters))
        return Unexpected(DOESNT_EXIST);

    auto const& params = hookCtx.applyCtx.tx.getFieldArray(sfHookParameters);

    for (auto const& param : params)
    {
        if (!param.isFieldPresent(sfHookParameterName) ||
            param.getFieldVL(sfHookParameterName) != param_name)
            continue;

        if (!param.isFieldPresent(sfHookParameterValue))
            return Unexpected(DOESNT_EXIST);

        auto const& val = param.getFieldVL(sfHookParameterValue);
        if (val.empty())
            return Unexpected(DOESNT_EXIST);

        return val;
    }

    return Unexpected(DOESNT_EXIST);
}

AccountID
HookAPI::hook_account() const
{
    return hookCtx.result.account;
}

Expected<ripple::uint256, HookReturnCode>
HookAPI::hook_hash(int32_t hook_no) const
{
    if (hook_no == -1)
        return hookCtx.result.hookHash;

    std::shared_ptr<SLE> hookSLE =
        hookCtx.applyCtx.view().peek(hookCtx.result.hookKeylet);
    if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
        return Unexpected(INTERNAL_ERROR);

    ripple::STArray const& hooks = hookSLE->getFieldArray(sfHooks);
    if (hook_no >= hooks.size())
        return Unexpected(DOESNT_EXIST);

    auto const& hook = hooks[hook_no];
    if (!hook.isFieldPresent(sfHookHash))
        return Unexpected(DOESNT_EXIST);

    return hook.getFieldH256(sfHookHash);
}

Expected<int64_t, HookReturnCode>
HookAPI::hook_again() const
{
    if (hookCtx.result.executeAgainAsWeak)
        return ALREADY_SET;

    if (hookCtx.result.isStrong)
    {
        hookCtx.result.executeAgainAsWeak = true;
        return 1;
    }

    return PREREQUISITE_NOT_MET;
}

Expected<Blob, HookReturnCode>
HookAPI::hook_param(Bytes const& paramName) const
{
    if (paramName.size() < 1)
        return Unexpected(TOO_SMALL);

    if (paramName.size() > 32)
        return Unexpected(TOO_BIG);

    // first check for overrides set by prior hooks in the chain
    auto const& overrides = hookCtx.result.hookParamOverrides;
    if (overrides.find(hookCtx.result.hookHash) != overrides.end())
    {
        auto const& params = overrides.at(hookCtx.result.hookHash);
        if (params.find(paramName) != params.end())
        {
            auto const& param = params.at(paramName);
            if (param.size() == 0)
                // allow overrides to "delete" parameters
                return Unexpected(DOESNT_EXIST);

            return param;
        }
    }

    // next check if there's a param set on this hook
    auto const& params = hookCtx.result.hookParams;
    if (params.find(paramName) != params.end())
    {
        auto const& param = params.at(paramName);
        if (param.size() == 0)
            return Unexpected(DOESNT_EXIST);

        return param;
    }

    return Unexpected(DOESNT_EXIST);
}

Expected<uint64_t, HookReturnCode>
HookAPI::hook_param_set(
    uint256 const& hash,
    Bytes const& paramName,
    Bytes const& paramValue) const
{
    if (paramName.size() < 1)
        return Unexpected(TOO_SMALL);

    if (paramName.size() > hook::maxHookParameterKeySize())
        return Unexpected(TOO_BIG);

    if (paramValue.size() > hook::maxHookParameterValueSize())
        return Unexpected(TOO_BIG);

    if (hookCtx.result.overrideCount >= hook_api::max_params)
        return Unexpected(TOO_MANY_PARAMS);

    hookCtx.result.overrideCount++;

    auto& overrides = hookCtx.result.hookParamOverrides;
    if (overrides.find(hash) == overrides.end())
    {
        overrides[hash] = std::map<Bytes, Bytes>{
            {std::move(paramName), std::move(paramValue)}};
    }
    else
        overrides[hash][std::move(paramName)] = std::move(paramValue);

    return paramValue.size();
}

Expected<uint64_t, HookReturnCode>
HookAPI::hook_skip(uint256 const& hash, uint32_t flags) const
{
    if (flags != 0 && flags != 1)
        return INVALID_ARGUMENT;

    auto& skips = hookCtx.result.hookSkips;

    if (flags == 1)
    {
        // delete flag
        if (skips.find(hash) == skips.end())
            return Unexpected(DOESNT_EXIST);
        skips.erase(hash);
        return 1;
    }

    // first check if it's already in the skips set
    if (skips.find(hash) != skips.end())
        return 1;

    // next check if it's even in this chain
    std::shared_ptr<SLE> hookSLE =
        hookCtx.applyCtx.view().peek(hookCtx.result.hookKeylet);

    if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
        return Unexpected(INTERNAL_ERROR);

    ripple::STArray const& hooks = hookSLE->getFieldArray(sfHooks);
    bool found = false;
    for (auto const& hookObj : hooks)
    {
        if (hookObj.isFieldPresent(sfHookHash))
        {
            if (hookObj.getFieldH256(sfHookHash) == hash)
            {
                found = true;
                break;
            }
        }
    }

    if (!found)
        return Unexpected(DOESNT_EXIST);

    // finally add it to the skips list
    hookCtx.result.hookSkips.emplace(hash);
    return 1;
}

uint8_t
HookAPI::hook_pos() const
{
    return hookCtx.result.hookChainPosition;
}

uint64_t
HookAPI::fee_base() const
{
    return hookCtx.applyCtx.view().fees().base.drops();
}

uint32_t
HookAPI::ledger_seq() const
{
    return hookCtx.applyCtx.view().info().seq;
}

uint256
HookAPI::ledger_last_hash() const
{
    return hookCtx.applyCtx.view().info().parentHash;
}

uint64_t
HookAPI::ledger_last_time() const
{
    return hookCtx.applyCtx.view()
        .info()
        .parentCloseTime.time_since_epoch()
        .count();
}

Expected<uint256, HookReturnCode>
HookAPI::ledger_nonce() const
{
    auto& view = hookCtx.applyCtx.view();
    if (hookCtx.ledger_nonce_counter > hook_api::max_nonce)
        return Unexpected(TOO_MANY_NONCES);

    auto hash = ripple::sha512Half(
        ripple::HashPrefix::hookNonce,
        view.info().seq,
        view.info().parentCloseTime.time_since_epoch().count(),
        view.info().parentHash,
        hookCtx.applyCtx.tx.getTransactionID(),
        hookCtx.ledger_nonce_counter++,
        hookCtx.result.account);

    return hash;
}

Expected<Keylet, HookReturnCode>
HookAPI::ledger_keylet(Keylet const& klLo, Keylet const& klHi) const
{
    // keylets must be the same type!
    if (klLo.type != klHi.type)
        return Unexpected(DOES_NOT_MATCH);

    std::optional<ripple::uint256> found =
        hookCtx.applyCtx.view().succ(klLo.key, klHi.key.next());

    if (!found)
        return Unexpected(DOESNT_EXIST);

    Keylet kl_out{klLo.type, *found};

    return kl_out;
}

Expected<std::shared_ptr<Transaction>, HookReturnCode>
HookAPI::emit(Slice const& txBlob) const
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");
    auto& view = applyCtx.view();

    if (hookCtx.expected_etxn_count < 0)
        return Unexpected(PREREQUISITE_NOT_MET);

    if (hookCtx.result.emittedTxn.size() >= hookCtx.expected_etxn_count)
        return Unexpected(TOO_MANY_EMITTED_TXN);

    std::shared_ptr<STTx const> stpTrans;
    try
    {
        SerialIter sit(txBlob);
        stpTrans = std::make_shared<STTx const>(sit);
    }
    catch (std::exception const& e)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC() << "]: Failed " << e.what();
        return Unexpected(EMISSION_FAILURE);
    }

    if (isPseudoTx(*stpTrans))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Attempted to emit pseudo txn.";
        return Unexpected(EMISSION_FAILURE);
    }

    ripple::TxType txType = stpTrans->getTxnType();

    ripple::uint256 const& hookCanEmit = hookCtx.result.hookCanEmit;
    if (!hook::canEmit(txType, hookCanEmit))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Hook cannot emit this txn.";
        return Unexpected(EMISSION_FAILURE);
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
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 1: sfSequence must be present and 0
    if (!stpTrans->isFieldPresent(sfSequence) ||
        stpTrans->getFieldU32(sfSequence) != 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSequence missing or non-zero";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 2: sfSigningPubKey must be present and 00...00
    if (!stpTrans->isFieldPresent(sfSigningPubKey))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigningPubKey missing";
        return Unexpected(EMISSION_FAILURE);
    }

    auto const pk = stpTrans->getSigningPubKey();
    if (pk.size() != 33 && pk.size() != 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigningPubKey present but wrong size";
        return Unexpected(EMISSION_FAILURE);
    }

    for (int i = 0; i < pk.size(); ++i)
        if (pk[i] != 0)
        {
            JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                            << "]: sfSigningPubKey present but non-zero.";
            return Unexpected(EMISSION_FAILURE);
        }

    // rule 2.a: no signers
    if (stpTrans->isFieldPresent(sfSigners))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfSigners not allowed in emitted txns.";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 2.b: ticketseq cannot be used
    if (stpTrans->isFieldPresent(sfTicketSequence))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfTicketSequence not allowed in emitted txns.";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 2.c sfAccountTxnID not allowed
    if (stpTrans->isFieldPresent(sfAccountTxnID))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfAccountTxnID not allowed in emitted txns.";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 3: sfEmitDetails must be present and valid
    if (!stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitDetails missing.";
        return Unexpected(EMISSION_FAILURE);
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
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 8: emit generation cannot exceed 10
    if (emitDetails.getFieldU32(sfEmitGeneration) >= 10)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitGeneration was 10 or more.";
        return Unexpected(EMISSION_FAILURE);
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
        return Unexpected(EMISSION_FAILURE);
    }

    uint64_t bur_proper = static_cast<uint64_t>(etxn_burden().value());
    if (bur != bur_proper)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitBurden provided in EmitDetails "
                        << "was not correct (" << bur << ") "
                        << "should be " << bur_proper;
        return Unexpected(EMISSION_FAILURE);
    }

    if (pTxnID != applyCtx.tx.getTransactionID())
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitParentTxnID provided in EmitDetails"
                        << "was not correct";
        return Unexpected(EMISSION_FAILURE);
    }

    if (hookCtx.nonce_used.find(nonce) == hookCtx.nonce_used.end())
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitNonce provided in EmitDetails was not "
                           "generated by nonce api";
        return Unexpected(EMISSION_FAILURE);
    }

    if (callback && *callback != hookCtx.result.account)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfEmitCallback account must be the account of "
                           "the emitting hook";
        return Unexpected(EMISSION_FAILURE);
    }

    if (hash != hookCtx.result.hookHash)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfEmitHookHash must be the hash of the emitting hook";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 4: sfTxnSignature must be absent
    if (stpTrans->isFieldPresent(sfTxnSignature))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfTxnSignature is present but should not be";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 5: LastLedgerSeq must be present and after current ledger
    if (!stpTrans->isFieldPresent(sfLastLedgerSequence))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfLastLedgerSequence missing";
        return Unexpected(EMISSION_FAILURE);
    }

    uint32_t tx_lls = stpTrans->getFieldU32(sfLastLedgerSequence);
    uint32_t ledgerSeq = view.info().seq;
    if (tx_lls < ledgerSeq + 1)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfLastLedgerSequence invalid (less than next ledger)";
        return Unexpected(EMISSION_FAILURE);
    }

    if (tx_lls > ledgerSeq + 5)
    {
        JLOG(j.trace())
            << "HookEmit[" << HC_ACC()
            << "]: sfLastLedgerSequence cannot be greater than current seq + 5";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 6
    if (!stpTrans->isFieldPresent(sfFirstLedgerSequence) ||
        stpTrans->getFieldU32(sfFirstLedgerSequence) > tx_lls)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: sfFirstLedgerSequence must be present and <= "
                           "LastLedgerSequence";
        return Unexpected(EMISSION_FAILURE);
    }

    // rule 7 check the emitted txn pays the appropriate fee
    int64_t minfee = etxn_fee_base(txBlob).value();

    if (minfee < 0)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee could not be calculated";
        return Unexpected(EMISSION_FAILURE);
    }

    if (!stpTrans->isFieldPresent(sfFee))
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee missing from emitted tx";
        return Unexpected(EMISSION_FAILURE);
    }

    int64_t fee = stpTrans->getFieldAmount(sfFee).xrp().drops();
    if (fee < minfee)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: Fee less than minimum required";
        return Unexpected(EMISSION_FAILURE);
    }

    std::string reason;
    auto tpTrans =
        std::make_shared<Transaction>(stpTrans, reason, applyCtx.app);
    if (tpTrans->getStatus() != NEW)
    {
        JLOG(j.trace()) << "HookEmit[" << HC_ACC()
                        << "]: tpTrans->getStatus() != NEW";
        return Unexpected(EMISSION_FAILURE);
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
        return Unexpected(EMISSION_FAILURE);
    }

    return tpTrans;
}

Expected<uint64_t, HookReturnCode>
HookAPI::etxn_burden() const
{
    if (hookCtx.expected_etxn_count <= -1)
        return Unexpected(PREREQUISITE_NOT_MET);

    uint64_t last_burden = static_cast<uint64_t>(otxn_burden());
    uint64_t burden =
        last_burden * static_cast<uint64_t>(hookCtx.expected_etxn_count);
    if (burden < last_burden)
        return Unexpected(FEE_TOO_LARGE);
    return burden;
}

Expected<uint64_t, HookReturnCode>
HookAPI::etxn_fee_base(ripple::Slice const& txBlob) const
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");

    if (hookCtx.expected_etxn_count <= -1)
        return Unexpected(PREREQUISITE_NOT_MET);

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
        return Unexpected(INVALID_TXN);
    }
}

Expected<uint64_t, HookReturnCode>
HookAPI::etxn_details(uint8_t* out_ptr) const
{
    if (hookCtx.expected_etxn_count <= -1)
        return PREREQUISITE_NOT_MET;

    uint32_t generation = etxn_generation();

    auto const burden_result = etxn_burden();

    if (!burden_result)
        return FEE_TOO_LARGE;

    int64_t burden = burden_result.value();

    uint8_t* out = out_ptr;

    *out++ = 0xEDU;  // begin sfEmitDetails                            /* upto =
                     // 0 | size =  1 */
    *out++ = 0x20U;  // sfEmitGeneration preamble                      /* upto =
                     // 1 | size =  6 */
    *out++ = 0x2EU;  // preamble cont
    *out++ = (generation >> 24U) & 0xFFU;
    *out++ = (generation >> 16U) & 0xFFU;
    *out++ = (generation >> 8U) & 0xFFU;
    *out++ = (generation >> 0U) & 0xFFU;
    *out++ = 0x3DU;  // sfEmitBurden preamble                           /* upto
                     // =   7 | size =  9 */
    *out++ = (burden >> 56U) & 0xFFU;
    *out++ = (burden >> 48U) & 0xFFU;
    *out++ = (burden >> 40U) & 0xFFU;
    *out++ = (burden >> 32U) & 0xFFU;
    *out++ = (burden >> 24U) & 0xFFU;
    *out++ = (burden >> 16U) & 0xFFU;
    *out++ = (burden >> 8U) & 0xFFU;
    *out++ = (burden >> 0U) & 0xFFU;
    *out++ = 0x5BU;  // sfEmitParentTxnID preamble                      /* upto
                     // =  16 | size = 33 */
    auto const& txID = hookCtx.applyCtx.tx.getTransactionID();
    memcpy(out, txID.data(), 32);
    out += 32;
    *out++ = 0x5CU;  // sfEmitNonce                                     /* upto
                     // =  49 | size = 33 */

    auto hash = etxn_nonce();
    if (!hash.has_value())
        return INTERNAL_ERROR;

    memcpy(out, hash->data(), 32);

    out += 32;
    *out++ = 0x5DU;  // sfEmitHookHash preamble                          /* upto
                     // =  82 | size = 33 */
    for (int i = 0; i < 32; ++i)
        *out++ = hookCtx.result.hookHash.data()[i];

    if (hookCtx.result.hasCallback)
    {
        *out++ = 0x8AU;  // sfEmitCallback preamble                         /*
                         // upto = 115 | size = 22 */
        *out++ = 0x14U;  // preamble cont

        memcpy(out, hookCtx.result.account.data(), 20);

        out += 20;
    }
    *out++ = 0xE1U;  // end object (sfEmitDetails)                     /* upto =
                     // 137 | size =  1 */
                     /* upto = 138 | --------- */
    int64_t outlen = out - out_ptr;

    return outlen;
}

Expected<uint64_t, HookReturnCode>
HookAPI::etxn_reserve(uint64_t count) const
{
    if (hookCtx.expected_etxn_count > -1)
        return Unexpected(ALREADY_SET);

    if (count < 1)
        return Unexpected(TOO_SMALL);

    if (count > hook_api::max_emit)
        return Unexpected(TOO_BIG);

    hookCtx.expected_etxn_count = count;

    return count;
}

uint32_t
HookAPI::etxn_generation() const
{
    return otxn_generation() + 1;
}

Expected<uint256, HookReturnCode>
HookAPI::etxn_nonce() const
{
    if (hookCtx.emit_nonce_counter > hook_api::max_nonce)
        return Unexpected(TOO_MANY_NONCES);

    // in some cases the same hook might execute multiple times
    // on one txn, therefore we need to pass this information to the nonce
    uint32_t flags = 0;
    flags |= hookCtx.result.isStrong ? 0b10U : 0;
    flags |= hookCtx.result.isCallback ? 0b01U : 0;
    flags |= (hookCtx.result.hookChainPosition << 2U);

    auto hash = ripple::sha512Half(
        ripple::HashPrefix::emitTxnNonce,
        hookCtx.applyCtx.tx.getTransactionID(),
        hookCtx.emit_nonce_counter++,
        hookCtx.result.account,
        hookCtx.result.hookHash,
        flags);

    hookCtx.nonce_used[hash] = true;

    return hash;
}

using namespace hook_float;

Expected<uint64_t, HookReturnCode>
HookAPI::float_set(int32_t exponent, int64_t mantissa) const
{
    if (mantissa == 0)
        return 0;

    auto normalized = hook_float::normalize_xfl(mantissa, exponent);

    // the above function will underflow into a canonical 0
    // but this api must report that underflow
    if (!normalized)
    {
        if (normalized.error() == XFL_OVERFLOW)
            return Unexpected(INVALID_FLOAT);
        return normalized.error();
    }
    if (normalized.value() == 0)
        return Unexpected(INVALID_FLOAT);

    return normalized;
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_multiply(uint64_t float1, uint64_t float2) const
{
    if (float1 == 0 || float2 == 0)
        return 0;

    uint64_t man1 = get_mantissa(float1).value();
    int32_t exp1 = get_exponent(float1).value();
    bool neg1 = is_negative(float1);
    uint64_t man2 = get_mantissa(float2).value();
    int32_t exp2 = get_exponent(float2).value();
    bool neg2 = is_negative(float2);

    auto const result =
        float_multiply_internal_parts(man1, exp1, neg1, man2, exp2, neg2);
    if (!result)
        return result.error();
    return result;
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_mulratio(
    uint64_t float1,
    uint32_t round_up,
    uint32_t numerator,
    uint32_t denominator) const
{
    if (float1 == 0)
        return 0;
    if (denominator == 0)
        return Unexpected(DIVISION_BY_ZERO);

    int64_t man1 = get_mantissa(float1).value();
    int32_t exp1 = get_exponent(float1).value();

    if (!mulratio_internal(man1, exp1, round_up > 0, numerator, denominator))
        return Unexpected(XFL_OVERFLOW);

    // defensive check
    if (man1 < 0)
        man1 *= -1LL;

    auto const result = make_float((uint64_t)man1, exp1, is_negative(float1));
    if (!result)
        return result.error();
    return result;
}

uint64_t
HookAPI::float_negate(uint64_t float1) const
{
    if (float1 == 0)
        return 0;
    return invert_sign(float1);
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_compare(uint64_t float1, uint64_t float2, uint32_t mode) const
{
    bool equal_flag = mode & compare_mode::EQUAL;
    bool less_flag = mode & compare_mode::LESS;
    bool greater_flag = mode & compare_mode::GREATER;
    bool not_equal = less_flag && greater_flag;

    if ((equal_flag && less_flag && greater_flag) || mode == 0)
        return Unexpected(INVALID_ARGUMENT);

    if (mode & (~0b111UL))
        return Unexpected(INVALID_ARGUMENT);

    try
    {
        int64_t man1 =
            (get_mantissa(float1)).value() * (is_negative(float1) ? -1LL : 1LL);
        int32_t exp1 = get_exponent(float1).value();
        ripple::IOUAmount amt1{man1, exp1};
        int64_t man2 =
            get_mantissa(float2).value() * (is_negative(float2) ? -1LL : 1LL);
        int32_t exp2 = get_exponent(float2).value();
        ripple::IOUAmount amt2{man2, exp2};

        if (not_equal && amt1 != amt2)
            return 1;

        if (equal_flag && amt1 == amt2)
            return 1;

        if (greater_flag && amt1 > amt2)
            return 1;

        if (less_flag && amt1 < amt2)
            return 1;

        return 0;
    }
    catch (std::overflow_error& e)
    {
        return Unexpected(XFL_OVERFLOW);
    }
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_sum(uint64_t float1, uint64_t float2) const
{
    if (float1 == 0)
        return float2;
    if (float2 == 0)
        return float1;

    int64_t man1 =
        get_mantissa(float1).value() * (is_negative(float1) ? -1LL : 1LL);
    int32_t exp1 = get_exponent(float1).value();
    int64_t man2 =
        get_mantissa(float2).value() * (is_negative(float2) ? -1LL : 1LL);
    int32_t exp2 = get_exponent(float2).value();

    try
    {
        ripple::IOUAmount amt1{man1, exp1};
        ripple::IOUAmount amt2{man2, exp2};

        amt1 += amt2;
        auto const result = make_float(amt1);
        if (!result)
        {
            // TODO: Should be (EXPONENT_UNDERSIZED || MANTISSA_UNDERSIZED)
            if (result.error() == EXPONENT_UNDERSIZED)
            {
                // this is an underflow e.g. as a result of subtracting an xfl
                // from itself and thus not an error, just return canonical 0
                return 0;
            }
            return Unexpected(result.error());
        }
        return result;
    }
    catch (std::overflow_error& e)
    {
        return Unexpected(XFL_OVERFLOW);
    }
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_invert(uint64_t float1) const
{
    if (float1 == 0)
        return Unexpected(DIVISION_BY_ZERO);
    if (float1 == float_one_internal)
        return float_one_internal;

    return float_divide_internal(float_one_internal, float1);
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_divide(uint64_t float1, uint64_t float2) const
{
    return float_divide_internal(float1, float2);
}

uint64_t
HookAPI::float_one() const
{
    return float_one_internal;
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_mantissa(uint64_t float1) const
{
    if (float1 == 0)
        return 0;
    return get_mantissa(float1);
}

uint64_t
HookAPI::float_sign(uint64_t float1) const
{
    if (float1 == 0)
        return 0;
    return is_negative(float1);
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_int(uint64_t float1, uint32_t decimal_places, uint32_t absolute)
    const
{
    if (float1 == 0)
        return 0;
    uint64_t man1 = get_mantissa(float1).value();
    int32_t exp1 = get_exponent(float1).value();
    bool neg1 = is_negative(float1);

    if (decimal_places > 15)
        return Unexpected(INVALID_ARGUMENT);

    if (neg1)
    {
        if (!absolute)
            return Unexpected(CANT_RETURN_NEGATIVE);
    }

    int32_t shift = -(exp1 + decimal_places);

    if (shift > 15)
        return 0;

    if (shift < 0)
        return Unexpected(TOO_BIG);

    if (shift > 0)
        man1 /= power_of_ten[shift];

    return man1;
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_log(uint64_t float1) const
{
    if (float1 == 0)
        return Unexpected(INVALID_ARGUMENT);

    uint64_t man1 = get_mantissa(float1).value();
    int32_t exp1 = get_exponent(float1).value();
    if (is_negative(float1))
        return Unexpected(COMPLEX_NOT_SUPPORTED);

    double inp = (double)(man1);
    double result = log10(inp) + exp1;

    return double_to_xfl(result);
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_root(uint64_t float1, uint32_t n) const
{
    if (float1 == 0)
        return 0;

    if (n < 2)
        return Unexpected(INVALID_ARGUMENT);

    uint64_t man1 = get_mantissa(float1).value();
    int32_t exp1 = get_exponent(float1).value();
    if (is_negative(float1))
        return Unexpected(COMPLEX_NOT_SUPPORTED);

    double inp = (double)(man1)*pow(10, exp1);
    double result = pow(inp, ((double)1.0f) / ((double)(n)));

    return double_to_xfl(result);
}

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

// private

inline int32_t
HookAPI::no_free_slots() const
{
    return hook_api::max_slots - hookCtx.slot.size() <= 0;
}

inline std::optional<int32_t>
HookAPI::get_free_slot() const
{
    // allocate a slot
    int32_t slot_into = 0;
    if (hookCtx.slot_free.size() > 0)
    {
        slot_into = hookCtx.slot_free.front();
        hookCtx.slot_free.pop();
        return slot_into;
    }

    // no slots were available in the queue so increment slot counter until we
    // find a free slot usually this will be the next available but the hook
    // developer may have allocated any slot ahead of when the counter gets
    // there
    do
    {
        slot_into = ++hookCtx.slot_counter;
    } while (hookCtx.slot.find(slot_into) != hookCtx.slot.end() &&
             // this condition should always be met, if for some reason, somehow
             // it is not then we will return the final slot every time.
             hookCtx.slot_counter <= hook_api::max_slots);

    if (hookCtx.slot_counter > hook_api::max_slots)
        return {};

    return slot_into;
}

inline Expected<uint64_t, HookReturnCode>
HookAPI::float_multiply_internal_parts(
    uint64_t man1,
    int32_t exp1,
    bool neg1,
    uint64_t man2,
    int32_t exp2,
    bool neg2) const
{
    using namespace boost::multiprecision;
    cpp_int mult = cpp_int(man1) * cpp_int(man2);
    mult /= power_of_ten[15];
    uint64_t man_out = static_cast<uint64_t>(mult);
    if (mult > man_out)
        return Unexpected(XFL_OVERFLOW);

    int32_t exp_out = exp1 + exp2 + 15;
    bool neg_out = (neg1 && !neg2) || (!neg1 && neg2);
    auto const ret = normalize_xfl(man_out, exp_out, neg_out);

    if (!ret)
    {
        if (ret.error() == EXPONENT_UNDERSIZED)
            return 0;
        if (ret.error() == EXPONENT_OVERSIZED)
            return Unexpected(XFL_OVERFLOW);
        return ret.error();
    }
    return ret;
}

inline Expected<uint64_t, HookReturnCode>
HookAPI::mulratio_internal(
    int64_t& man1,
    int32_t& exp1,
    bool round_up,
    uint32_t numerator,
    uint32_t denominator) const
{
    try
    {
        ripple::IOUAmount amt{man1, exp1};
        ripple::IOUAmount out = ripple::mulRatio(
            amt, numerator, denominator, round_up != 0);  // already normalized
        man1 = out.mantissa();
        exp1 = out.exponent();
        return 1;
    }
    catch (std::overflow_error& e)
    {
        return Unexpected(XFL_OVERFLOW);
    }
}

inline Expected<uint64_t, HookReturnCode>
HookAPI::float_divide_internal(uint64_t float1, uint64_t float2) const
{
    bool const hasFix = hookCtx.applyCtx.view().rules().enabled(fixFloatDivide);
    if (float2 == 0)
        return DIVISION_BY_ZERO;
    if (float1 == 0)
        return 0;

    // special case: division by 1
    // RH TODO: add more special cases (division by power of 10)
    if (float2 == float_one_internal)
        return float1;

    uint64_t man1 = get_mantissa(float1).value();
    int32_t exp1 = get_exponent(float1).value();
    bool neg1 = is_negative(float1);
    uint64_t man2 = get_mantissa(float2).value();
    int32_t exp2 = get_exponent(float2).value();
    bool neg2 = is_negative(float2);

    auto tmp1 = normalize_xfl(man1, exp1);
    auto tmp2 = normalize_xfl(man2, exp2);

    if (!tmp1 || !tmp2)
        return Unexpected(INVALID_FLOAT);

    if (tmp1.value() == 0)
        return 0;

    while (man2 > man1)
    {
        man2 /= 10;
        exp2++;
    }

    if (man2 == 0)
        return Unexpected(DIVISION_BY_ZERO);

    while (man2 < man1)
    {
        if (man2 * 10 > man1)
            break;
        man2 *= 10;
        exp2--;
    }

    uint64_t man3 = 0;
    int32_t exp3 = exp1 - exp2;

    while (man2 > 0)
    {
        int i = 0;
        if (hasFix)
        {
            for (; man1 >= man2; man1 -= man2, ++i)
                ;
        }
        else
        {
            for (; man1 > man2; man1 -= man2, ++i)
                ;
        }

        man3 *= 10;
        man3 += i;
        man2 /= 10;
        if (man2 == 0)
            break;
        exp3--;
    }

    bool neg3 = !((neg1 && neg2) || (!neg1 && !neg2));

    return normalize_xfl(man3, exp3, neg3);
}

inline Expected<uint64_t, HookReturnCode>
HookAPI::double_to_xfl(double x) const
{
    if ((x) == 0)
        return 0;
    bool neg = x < 0;
    double absresult = neg ? -x : x;

    // first compute the base 10 order of the float
    int32_t exp_out = (int32_t)log10(absresult);

    // next adjust it into the valid mantissa range (this means dividing by its
    // order and multiplying by 10**15)
    absresult *= pow(10, -exp_out + 15);

    // after adjustment the value may still fall below the minMantissa
    int64_t result = (int64_t)absresult;
    if (result < minMantissa)
    {
        if (result == minMantissa - 1LL)
            result += 1LL;
        else
        {
            result *= 10LL;
            exp_out--;
        }
    }

    // likewise the value can fall above the maxMantissa
    if (result > maxMantissa)
    {
        if (result == maxMantissa + 1LL)
            result -= 1LL;
        else
        {
            result /= 10LL;
            exp_out++;
        }
    }

    exp_out -= 15;
    auto const ret = make_float(result, exp_out, neg);

    if (!ret)
    {
        // TODO: Should be (EXPONENT_UNDERSIZED || MANTISSA_UNDERSIZED)
        if (ret.error() == EXPONENT_UNDERSIZED)
            return 0;
        return Unexpected(ret.error());
    }

    return ret;
}

}  // namespace hook
