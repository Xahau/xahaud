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

Expected<std::shared_ptr<Transaction>, HookReturnCode>
HookAPI::emit(Slice txBlob)
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
HookAPI::etxn_fee_base(ripple::Slice txBlob) const
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

uint32_t
HookAPI::etxn_generation() const
{
    return otxn_generation() + 1;
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
