// Implementation of decoupled Hook APIs for emit and related helpers.

#include <xrpld/app/hook/HookAPI.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/ledger/TransactionMaster.h>
#include <xrpld/app/tx/detail/Import.h>
#include <xrpl/protocol/STParsedJSON.h>
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

/// control APIs
// _g
// accept
// rollback

/// util APIs
Expected<std::string, HookReturnCode>
HookAPI::util_raddr(Bytes const& accountID) const
{
    if (accountID.size() != 20)
        return Unexpected(INVALID_ARGUMENT);

    return encodeBase58Token(
        TokenType::AccountID, accountID.data(), accountID.size());
}

Expected<Bytes, HookReturnCode>
HookAPI::util_accid(std::string raddress) const
{
    auto const result = decodeBase58Token(raddress, TokenType::AccountID);
    if (result.empty())
        return Unexpected(INVALID_ARGUMENT);
    return Bytes(result.data(), result.data() + result.size());
}

Expected<bool, HookReturnCode>
HookAPI::util_verify(Slice const& data, Slice const& sig, Slice const& key)
    const
{
    if (key.size() != 33)
        return Unexpected(INVALID_KEY);

    if (data.size() == 0)
        return Unexpected(TOO_SMALL);

    if (sig.size() < 30)
        return Unexpected(TOO_SMALL);

    if (!publicKeyType(key))
        return Unexpected(INVALID_KEY);

    ripple::PublicKey pubkey{key};
    return ripple::verify(pubkey, data, sig, false);
}

uint256
HookAPI::util_sha512h(Slice const& data) const
{
    return ripple::sha512Half(data);
}

// util_keylet

/// sto APIs
Expected<bool, HookReturnCode>
HookAPI::sto_validate(Bytes const& data) const
{
    if (data.size() < 2)
        return Unexpected(TOO_SMALL);

    unsigned char* start = const_cast<unsigned char*>(data.data());
    unsigned char* upto = start;
    unsigned char* end = start + data.size();

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        auto const length = get_stobject_length(
            upto,
            end,
            type,
            field,
            payload_start,
            payload_length,
            hookCtx.applyCtx.view().rules(),
            0);
        if (!length)
            return 0;
        upto += length.value();
    }

    return upto == end ? 1 : 0;
}

Expected<std::pair<uint32_t, uint32_t>, HookReturnCode>
HookAPI::sto_subfield(Bytes const& data, uint32_t field_id) const
{
    if (data.size() < 2)
        return Unexpected(TOO_SMALL);

    unsigned char* start = const_cast<unsigned char*>(data.data());
    unsigned char* upto = start;
    unsigned char* end = start + data.size();

    DBG_PRINTF(
        "sto_subfield called, looking for field %u type %u\n",
        field_id & 0xFFFF,
        (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF((j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");

    //    if ((*upto & 0xF0) == 0xE0)
    //        upto++;

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        auto const length = get_stobject_length(
            upto,
            end,
            type,
            field,
            payload_start,
            payload_length,
            hookCtx.applyCtx.view().rules(),
            0);
        if (!length)
            return Unexpected(PARSE_ERROR);
        if ((type << 16) + field == field_id)
        {
            DBG_PRINTF(
                "sto_subfield returned for field %u type %u\n",
                field_id & 0xFFFF,
                (field_id >> 16));
            for (int j = -5; j < 5; ++j)
                DBG_PRINTF((j == 0 ? " [%02X] " : "  %02X  "), *(upto + j));
            DBG_PRINTF("\n");

            if (type == 0xF)  // we return arrays fully formed
                return std::make_pair(upto - start, length.value());

            // return pointers to all other objects as payloads
            return std::make_pair(upto - start + payload_start, payload_length);
        }
        upto += length.value();
    }

    if (upto != end)
        return Unexpected(PARSE_ERROR);

    return Unexpected(DOESNT_EXIST);
}

Expected<std::pair<uint32_t, uint32_t>, HookReturnCode>
HookAPI::sto_subarray(Bytes const& data, uint32_t index_id) const
{
    if (data.size() < 2)
        return Unexpected(TOO_SMALL);

    unsigned char* start = const_cast<unsigned char*>(data.data());
    unsigned char* upto = start;
    unsigned char* end = start + data.size();

    // unwrap the array if it is wrapped,
    // by removing a byte from the start and end
    // why here 0xF0?
    // STI_ARRAY = 0xF0
    // eg) Signers field value = 0x03 => 0xF3
    // eg) Amounts field value = 0x5C => 0xF0, 0x5C
    if ((*upto & 0xF0U) == 0xF0U)
    {
        if (hookCtx.applyCtx.view().rules().enabled(fixHookAPI20251128) &&
            *upto == 0xF0U)
        {
            // field value > 15
            upto++;
            upto++;
            end--;
        }
        else
        {
            // field value <= 15
            upto++;
            end--;
        }
    }

    if (upto >= end)
        return Unexpected(PARSE_ERROR);

    /*
    DBG_PRINTF("sto_subarray called, looking for index %u\n", index_id);
    for (int j = -5; j < 5; ++j)
        printf(( j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");
    */
    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        auto const length = get_stobject_length(
            upto,
            end,
            type,
            field,
            payload_start,
            payload_length,
            hookCtx.applyCtx.view().rules(),
            0);
        if (!length)
            return Unexpected(PARSE_ERROR);

        if (i == index_id)
        {
            DBG_PRINTF("sto_subarray returned for index %u\n", index_id);
            for (int j = -5; j < 5; ++j)
                DBG_PRINTF(
                    (j == 0 ? " [%02X] " : "  %02X  "),
                    *(upto + j + length.value()));
            DBG_PRINTF("\n");

            return std::make_pair(upto - start, length.value());
        }
        upto += length.value();
    }

    if (upto != end)
        return Unexpected(PARSE_ERROR);

    return Unexpected(DOESNT_EXIST);
}

Expected<Bytes, HookReturnCode>
HookAPI::sto_emplace(
    Bytes const& source_object,
    std::optional<Bytes> const& field_object,
    uint32_t field_id) const
{
    // RH TODO: put these constants somewhere (votable?)
    if (source_object.size() > 1024 * 16)
        return Unexpected(TOO_BIG);

    if (source_object.size() < 2)
        return Unexpected(TOO_SMALL);

    if (!field_object.has_value())
    {
        // this is a delete operation
    }
    else
    {
        if (field_object->size() > 4096)
            return Unexpected(TOO_BIG);

        if (field_object->size() < 2)
            return Unexpected(TOO_SMALL);
    }

    if (field_object.has_value() &&
        hookCtx.applyCtx.view().rules().enabled(fixHookAPI20251128))
    {
        // inject field should be valid sto object and it's field id should
        // match the field_id
        unsigned char* inject_start = (unsigned char*)(field_object->data());
        unsigned char* inject_end =
            (unsigned char*)(field_object->data() + field_object->size());
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        auto const length = get_stobject_length(
            inject_start,
            inject_end,
            type,
            field,
            payload_start,
            payload_length,
            hookCtx.applyCtx.view().rules(),
            0);
        if (!length)
            return Unexpected(PARSE_ERROR);
        if ((type << 16) + field != field_id)
        {
            return Unexpected(PARSE_ERROR);
        }
    }

    std::vector<uint8_t> out(
        (size_t)(source_object.size() +
                 (field_object ? field_object->size() : 0)),
        (uint8_t)0);
    uint8_t* write_ptr = out.data();

    // we must inject the field at the canonical location....
    // so find that location
    unsigned char* start = (unsigned char*)(source_object.data());
    unsigned char* upto = start;
    unsigned char* end = start + source_object.size();
    unsigned char* inject_start = end;
    unsigned char* inject_end = end;

    DBG_PRINTF(
        "sto_emplace called, looking for field %u type %u\n",
        field_id & 0xFFFF,
        (field_id >> 16));
    for (int j = -5; j < 5; ++j)
        DBG_PRINTF((j == 0 ? " >%02X< " : "  %02X  "), *(start + j));
    DBG_PRINTF("\n");

    for (int i = 0; i < 1024 && upto < end; ++i)
    {
        int type = -1, field = -1, payload_start = -1, payload_length = -1;
        auto const length = get_stobject_length(
            upto,
            end,
            type,
            field,
            payload_start,
            payload_length,
            hookCtx.applyCtx.view().rules(),
            0);
        if (!length)
            return Unexpected(PARSE_ERROR);
        if ((type << 16) + field == field_id)
        {
            inject_start = upto;
            inject_end = upto + length.value();
            break;
        }
        else if ((type << 16) + field > field_id)
        {
            inject_start = upto;
            inject_end = upto;
            break;
        }
        upto += length.value();
    }

    // if the scan loop ends past the end of the source object
    // then the source object is invalid/corrupt, so we must
    // return an error
    if (upto > end)
        return Unexpected(PARSE_ERROR);

    // upto is injection point
    int64_t bytes_written = 0;

    // part 1
    if (inject_start - start > 0)
    {
        size_t len = inject_start - start;
        memcpy(write_ptr, start, len);
        bytes_written += len;
    }

    if (field_object && field_object->size() > 0)
    {
        // write the field (or don't if it's a delete operation)
        memcpy(
            write_ptr + bytes_written,
            field_object->data(),
            field_object->size());
        bytes_written += field_object->size();
    }

    // part 2
    if (end - inject_end > 0)
    {
        size_t len = end - inject_end;
        memcpy(write_ptr + bytes_written, inject_end, len);
        bytes_written += len;
    }

    out.resize(bytes_written);
    return out;
}

// sto_erase

/// etxn APIs
Expected<Bytes, HookReturnCode>
HookAPI::prepare(Slice const& txBlob) const
{
    auto& applyCtx = hookCtx.applyCtx;
    auto j = applyCtx.app.journal("View");

    if (hookCtx.expected_etxn_count < 0)
        return Unexpected(PREREQUISITE_NOT_MET);

    Json::Value json;

    // std::shared_ptr<STObject const> stpTrans;
    try
    {
        SerialIter sitTrans{txBlob};
        json =
            STObject(std::ref(sitTrans), sfGeneric).getJson(JsonOptions::none);
    }
    catch (std::exception& e)
    {
        JLOG(j.trace()) << "HookInfo[" << HC_ACC() << "]: prepare Failed "
                        << e.what() << "\n";
        return Unexpected(INVALID_ARGUMENT);
    }

    // add a dummy fee
    json[jss::Fee] = "0";

    // force key to empty
    json[jss::SigningPubKey] =
        "000000000000000000000000000000000000000000000000000000000000000000";

    // force sequence to 0
    json[jss::Sequence] = Json::Value(0u);

    std::string raddr = encodeBase58Token(
        TokenType::AccountID, hookCtx.result.account.data(), 20);

    json[jss::Account] = raddr;

    uint32_t seq = applyCtx.view().info().seq;
    if (!json.isMember(jss::FirstLedgerSequence))
        json[jss::FirstLedgerSequence] = Json::Value(seq + 1);

    if (!json.isMember(jss::LastLedgerSequence))
        json[jss::LastLedgerSequence] = Json::Value(seq + 5);

    uint8_t details[512];
    if (!json.isMember(jss::EmitDetails))
    {
        auto ret = etxn_details(details);
        if (!ret || ret.value() < 2)
            return Unexpected(INTERNAL_ERROR);

        // truncate the head and tail (emit details object markers)
        Slice s(
            reinterpret_cast<void const*>(details + 1),
            (size_t)(ret.value() - 2));

        // std::cout << "emitdets: " << strHex(s) << "\n";
        try
        {
            SerialIter sit{s};
            STObject st{sit, sfEmitDetails};
            json[jss::EmitDetails] = st.getJson(JsonOptions::none);
        }
        catch (std::exception const& ex)
        {
            JLOG(j.warn()) << "HookInfo[" << HC_ACC() << "]: Exception in "
                           << __func__ << ": " << ex.what();
            return Unexpected(INTERNAL_ERROR);
        }
    }

    // {
    //     const std::string flat = Json::FastWriter().write(json);
    //     std::cout << "intermediate: `" << flat << "`\n";
    // }

    Blob tx_blob;
    {
        STParsedJSONObject parsed(std::string(jss::tx_json), json);
        if (!parsed.object.has_value())
            return Unexpected(INVALID_ARGUMENT);

        STObject& obj = *(parsed.object);

        // serialize it
        Serializer s;
        obj.add(s);
        tx_blob = s.getData();
    }

    // run it through the fee estimate, this doubles as a txn sanity check
    auto fee = etxn_fee_base(Slice(tx_blob.data(), tx_blob.size()));
    if (!fee)
        return Unexpected(INVALID_ARGUMENT);

    json[jss::Fee] = to_string(fee.value());

    {
        STParsedJSONObject parsed(std::string(jss::tx_json), json);
        if (!parsed.object.has_value())
            return Unexpected(INVALID_ARGUMENT);

        STObject& obj = *(parsed.object);

        // serialize it
        Serializer s;
        obj.add(s);
        tx_blob = s.getData();
    }

    return tx_blob;
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
                        << transHuman(preflightResult.ter);
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

        if (!hookCtx.applyCtx.view().rules().enabled(fixHookAPI20251128))
            return Transactor::calculateBaseFee(
                       *(applyCtx.app.openLedger().current()), *stpTrans)
                .drops();

        return invoke_calculateBaseFee(
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
        return Unexpected(PREREQUISITE_NOT_MET);

    uint32_t generation = etxn_generation();

    auto const burden_result = etxn_burden();

    if (!burden_result)
        return Unexpected(FEE_TOO_LARGE);

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

/// float APIs

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

    return float_multiply_internal_parts(man1, exp1, neg1, man2, exp2, neg2);
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

Expected<Bytes, HookReturnCode>
HookAPI::float_sto(
    std::optional<Currency> currency,
    std::optional<AccountID> issuer,
    uint64_t float1,
    uint32_t field_code,
    uint32_t write_len) const
{
    uint16_t field = field_code & 0xFFFFU;
    uint16_t type = field_code >> 16U;

    bool is_xrp = field_code == 0;
    bool is_short =
        field_code == 0xFFFFFFFFU;  // non-xrp value but do not output header or
                                    // tail, just amount

    int bytes_needed = 8 +
        (field == 0 && type == 0
             ? 0
             : (field == 0xFFFFU && type == 0xFFFFU
                    ? 0
                    : (field < 16 && type < 16
                           ? 1
                           : (field >= 16 && type < 16
                                  ? 2
                                  : (field < 16 && type >= 16 ? 2 : 3)))));

    if (issuer && !currency)
        return Unexpected(INVALID_ARGUMENT);

    if (!issuer && currency)
        return Unexpected(INVALID_ARGUMENT);

    if (issuer)
    {
        if (is_xrp)
            return Unexpected(INVALID_ARGUMENT);
        if (is_short)
            return Unexpected(INVALID_ARGUMENT);

        bytes_needed += 40;
    }
    else if (!is_xrp && !is_short)
        return Unexpected(INVALID_ARGUMENT);

    if (bytes_needed > write_len)
        return Unexpected(TOO_SMALL);

    Bytes vec(bytes_needed);
    uint8_t* write_ptr = vec.data();

    if (is_xrp || is_short)
    {
        // do nothing
    }
    else if (field < 16 && type < 16)
    {
        *write_ptr++ = (((uint8_t)type) << 4U) + ((uint8_t)field);
    }
    else if (field >= 16 && type < 16)
    {
        *write_ptr++ = (((uint8_t)type) << 4U);
        *write_ptr++ = ((uint8_t)field);
    }
    else if (field < 16 && type >= 16)
    {
        *write_ptr++ = (((uint8_t)field) << 4U);
        *write_ptr++ = ((uint8_t)type);
    }
    else
    {
        *write_ptr++ = 0;
        *write_ptr++ = ((uint8_t)type);
        *write_ptr++ = ((uint8_t)field);
    }

    uint64_t man = get_mantissa(float1).value();
    int32_t exp = get_exponent(float1).value();
    bool neg = is_negative(float1);
    uint8_t out[8];
    if (is_xrp)
    {
        int32_t shift = -(exp);

        if (shift > 15)
            // https://github.com/Xahau/xahaud/issues/586
            return Unexpected(XFL_OVERFLOW);

        if (shift < 0)
            return Unexpected(XFL_OVERFLOW);

        if (shift > 0)
            man /= power_of_ten[shift];

        out[0] = (neg ? 0b00000000U : 0b01000000U);
        out[0] += (uint8_t)((man >> 56U) & 0b111111U);
        out[1] = (uint8_t)((man >> 48U) & 0xFF);
        out[2] = (uint8_t)((man >> 40U) & 0xFF);
        out[3] = (uint8_t)((man >> 32U) & 0xFF);
        out[4] = (uint8_t)((man >> 24U) & 0xFF);
        out[5] = (uint8_t)((man >> 16U) & 0xFF);
        out[6] = (uint8_t)((man >> 8U) & 0xFF);
        out[7] = (uint8_t)((man >> 0U) & 0xFF);
    }
    else if (man == 0)
    {
        out[0] = 0b10000000U;
        for (int i = 1; i < 8; ++i)
            out[i] = 0;
    }
    else
    {
        exp += 97;

        /// encode the rippled floating point sto format

        out[0] = (neg ? 0b10000000U : 0b11000000U);
        out[0] += (uint8_t)(exp >> 2U);
        out[1] = ((uint8_t)(exp & 0b11U)) << 6U;
        out[1] += (((uint8_t)(man >> 48U)) & 0b111111U);
        out[2] = (uint8_t)((man >> 40U) & 0xFFU);
        out[3] = (uint8_t)((man >> 32U) & 0xFFU);
        out[4] = (uint8_t)((man >> 24U) & 0xFFU);
        out[5] = (uint8_t)((man >> 16U) & 0xFFU);
        out[6] = (uint8_t)((man >> 8U) & 0xFFU);
        out[7] = (uint8_t)((man >> 0U) & 0xFFU);
    }

    std::memcpy(write_ptr, out, 8);
    write_ptr += 8;

    if (!is_xrp && !is_short)
    {
        std::memcpy(write_ptr, currency->data(), 20);
        write_ptr += 20;
        std::memcpy(write_ptr, issuer->data(), 20);
    }

    return vec;
}

Expected<uint64_t, HookReturnCode>
HookAPI::float_sto_set(Bytes const& data) const
{
    uint8_t* upto = const_cast<uint8_t*>(data.data());
    uint8_t length = data.size();

    if (length > 8)
    {
        uint8_t hi = upto[0] >> 4U;
        uint8_t lo = upto[0] & 0xFU;

        if (hi == 0 && lo == 0)
        {
            // typecode >= 16 && fieldcode >= 16
            if (length < 11)
                return Unexpected(NOT_AN_OBJECT);
            upto += 3;
            length -= 3;
        }
        else if (hi == 0 || lo == 0)
        {
            // typecode >= 16 && fieldcode < 16
            if (length < 10)
                return Unexpected(NOT_AN_OBJECT);
            upto += 2;
            length -= 2;
        }
        else
        {
            // typecode < 16 && fieldcode < 16
            upto++;
            length--;
        }
    }

    if (length < 8)
        return Unexpected(NOT_AN_OBJECT);

    bool is_xrp = (((*upto) & 0b10000000U) == 0);
    bool is_negative = (((*upto) & 0b01000000U) == 0);

    int32_t exponent = 0;

    if (is_xrp)
    {
        // exponent remains 0
        upto++;
    }
    else
    {
        exponent = (((*upto++) & 0b00111111U)) << 2U;
        exponent += ((*upto) >> 6U);
        exponent -= 97;
    }

    uint64_t mantissa = (((uint64_t)(*upto++)) & 0b00111111U) << 48U;
    mantissa += ((uint64_t)*upto++) << 40U;
    mantissa += ((uint64_t)*upto++) << 32U;
    mantissa += ((uint64_t)*upto++) << 24U;
    mantissa += ((uint64_t)*upto++) << 16U;
    mantissa += ((uint64_t)*upto++) << 8U;
    mantissa += ((uint64_t)*upto++);

    if (mantissa == 0)
        return 0;

    return hook_float::normalize_xfl(mantissa, exponent, is_negative);
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

/// otxn APIs

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

/// hook APIs

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
        return Unexpected(ALREADY_SET);

    if (hookCtx.result.isStrong)
    {
        hookCtx.result.executeAgainAsWeak = true;
        return 1;
    }

    return Unexpected(PREREQUISITE_NOT_MET);
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
        return Unexpected(INVALID_ARGUMENT);

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

/// ledger APIs

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

/// state APIs

// state

Expected<Bytes, HookReturnCode>
HookAPI::state_foreign(
    uint256 const& key,
    uint256 const& ns,
    AccountID const& account) const
{
    // first check if the requested state was previously cached this session
    auto cacheEntryLookup = lookup_state_cache(account, ns, key);
    if (cacheEntryLookup)
    {
        auto const& cacheEntry = cacheEntryLookup->get();

        return cacheEntry.second;
    }

    auto hsSLE =
        hookCtx.applyCtx.view().peek(keylet::hookState(account, key, ns));

    if (!hsSLE)
        return Unexpected(DOESNT_EXIST);

    Blob b = hsSLE->getFieldVL(sfHookStateData);

    // it exists add it to cache and return it
    if (!set_state_cache(account, ns, key, b, false).has_value())
        return Unexpected(INTERNAL_ERROR);  // should never happen

    return b;
}

// state_set

Expected<uint64_t, HookReturnCode>
HookAPI::state_foreign_set(
    uint256 const& key,
    uint256 const& ns,
    AccountID const& account,
    Bytes& data) const
{
    // local modifications are always allowed
    if (account == hookCtx.result.account)
    {
        if (auto ret = set_state_cache(account, ns, key, data, true);
            !ret.has_value())
            return Unexpected(ret.error());

        return data.size();
    }

    // execution to here means it's actually a foreign set
    if (hookCtx.result.foreignStateSetDisabled)
        return Unexpected(PREVIOUS_FAILURE_PREVENTS_RETRY);

    // first check if we've already modified this state
    auto cacheEntry = lookup_state_cache(account, ns, key);
    if (cacheEntry && cacheEntry->get().first)
    {
        // if a cache entry already exists and it has already been modified
        // don't check grants again
        if (auto ret = set_state_cache(account, ns, key, data, true);
            !ret.has_value())
            return Unexpected(ret.error());

        return data.size();
    }

    // cache miss or cache was present but entry was not marked as previously
    // modified therefore before continuing we need to check grants
    auto const sle =
        hookCtx.applyCtx.view().read(ripple::keylet::hook(account));
    if (!sle)
        return Unexpected(INTERNAL_ERROR);

    bool found_auth = false;

    // we do this by iterating the hooks installed on the foreign account and in
    // turn their grants and namespaces
    auto const& hooks = sle->getFieldArray(sfHooks);
    for (auto const& hookObj : hooks)
    {
        // skip blank entries
        if (!hookObj.isFieldPresent(sfHookHash))
            continue;

        if (!hookObj.isFieldPresent(sfHookGrants))
            continue;

        auto const& hookGrants = hookObj.getFieldArray(sfHookGrants);

        if (hookGrants.size() < 1)
            continue;

        // the grant allows the hook to modify the granter's namespace only
        if (hookObj.isFieldPresent(sfHookNamespace))
        {
            if (hookObj.getFieldH256(sfHookNamespace) != ns)
                continue;
        }
        else
        {
            // fetch the hook definition
            auto const def =
                hookCtx.applyCtx.view().read(ripple::keylet::hookDefinition(
                    hookObj.getFieldH256(sfHookHash)));
            if (!def)  // should never happen except in a rare race condition
                continue;
            if (def->getFieldH256(sfHookNamespace) != ns)
                continue;
        }

        // this is expensive search so we'll disallow after one failed attempt
        for (auto const& hookGrantObj : hookGrants)
        {
            bool hasAuthorizedField = hookGrantObj.isFieldPresent(sfAuthorize);

            if (hookGrantObj.getFieldH256(sfHookHash) ==
                    hookCtx.result.hookHash &&
                (!hasAuthorizedField ||
                 hookGrantObj.getAccountID(sfAuthorize) ==
                     hookCtx.result.account))
            {
                found_auth = true;
                break;
            }
        }

        if (found_auth)
            break;
    }

    if (!found_auth)
    {
        // hook only gets one attempt
        hookCtx.result.foreignStateSetDisabled = true;
        return Unexpected(NOT_AUTHORIZED);
    }

    if (auto ret = set_state_cache(account, ns, key, data, true);
        !ret.has_value())
        return Unexpected(ret.error());

    return data.size();
}

/// slot APIs

Expected<const STBase*, HookReturnCode>
HookAPI::slot(uint32_t slot_no) const
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (hookCtx.slot[slot_no].entry == 0)
        return Unexpected(INTERNAL_ERROR);

    return hookCtx.slot[slot_no].entry;
}

Expected<uint64_t, HookReturnCode>
HookAPI::slot_clear(uint32_t slot_no) const
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    hookCtx.slot.erase(slot_no);
    hookCtx.slot_free.push(slot_no);
    return 1;
}

Expected<uint64_t, HookReturnCode>
HookAPI::slot_count(uint32_t slot_no) const
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (hookCtx.slot[slot_no].entry == 0)
        return Unexpected(INTERNAL_ERROR);

    if (hookCtx.slot[slot_no].entry->getSType() != STI_ARRAY)
        return Unexpected(NOT_AN_ARRAY);

    return hookCtx.slot[slot_no].entry->downcast<ripple::STArray>().size();
}

Expected<uint32_t, HookReturnCode>
HookAPI::slot_set(Bytes const& data, uint32_t slot_no) const
{
    if ((data.size() != 32 && data.size() != 34) ||
        slot_no > hook_api::max_slots)
        return Unexpected(INVALID_ARGUMENT);

    if (slot_no == 0 && no_free_slots())
        return Unexpected(NO_FREE_SLOTS);

    std::optional<std::shared_ptr<const ripple::STObject>> slot_value =
        std::nullopt;

    if (data.size() == 34)
    {
        std::optional<ripple::Keylet> kl = unserialize_keylet(data);
        if (!kl)
            return Unexpected(DOESNT_EXIST);

        if (kl->key == beast::zero)
            return Unexpected(DOESNT_EXIST);

        auto const sle = hookCtx.applyCtx.view().read(*kl);
        if (!sle)
            return Unexpected(DOESNT_EXIST);

        slot_value = sle;
    }
    else if (data.size() == 32)
    {
        uint256 hash = uint256::fromVoid(data.data());

        ripple::error_code_i ec{ripple::error_code_i::rpcUNKNOWN};

        auto hTx = hookCtx.applyCtx.app.getMasterTransaction().fetch(hash, ec);

        if (auto const* p = std::get_if<std::pair<
                std::shared_ptr<ripple::Transaction>,
                std::shared_ptr<ripple::TxMeta>>>(&hTx))
            slot_value = p->first->getSTransaction();
        else
            return Unexpected(DOESNT_EXIST);
    }
    else
        return Unexpected(INVALID_ARGUMENT);

    if (!slot_value.has_value())
        return Unexpected(DOESNT_EXIST);

    if (slot_no == 0)
    {
        if (auto found = get_free_slot(); found)
            slot_no = *found;
        else
            return Unexpected(NO_FREE_SLOTS);
    }

    hookCtx.slot[slot_no] = hook::SlotEntry{.storage = *slot_value, .entry = 0};
    hookCtx.slot[slot_no].entry = &(*hookCtx.slot[slot_no].storage);

    return slot_no;
}

Expected<uint64_t, HookReturnCode>
HookAPI::slot_size(uint32_t slot_no) const
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (hookCtx.slot[slot_no].entry == 0)
        return Unexpected(INTERNAL_ERROR);

    // RH TODO: this is a very expensive way of computing size, cache it
    Serializer s;
    hookCtx.slot[slot_no].entry->add(s);
    return s.getDataLength();
}

Expected<uint32_t, HookReturnCode>
HookAPI::slot_subarray(
    uint32_t parent_slot,
    uint32_t array_id,
    uint32_t new_slot) const
{
    if (hookCtx.slot.find(parent_slot) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (hookCtx.slot[parent_slot].entry == 0)
        return Unexpected(INTERNAL_ERROR);

    if (hookCtx.slot[parent_slot].entry->getSType() != STI_ARRAY)
        return Unexpected(NOT_AN_ARRAY);

    if (new_slot == 0 && no_free_slots())
        return Unexpected(NO_FREE_SLOTS);

    if (new_slot > hook_api::max_slots)
        return Unexpected(INVALID_ARGUMENT);

    bool copied = false;
    try
    {
        ripple::STArray& parent_obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[parent_slot].entry)
                .downcast<ripple::STArray>();

        if (parent_obj.size() <= array_id)
            return Unexpected(DOESNT_EXIST);

        if (new_slot == 0)
        {
            if (auto found = get_free_slot(); found)
                new_slot = *found;
            else
                return Unexpected(NO_FREE_SLOTS);
        }

        // copy
        if (new_slot != parent_slot)
        {
            copied = true;
            hookCtx.slot[new_slot] = hookCtx.slot[parent_slot];
        }
        hookCtx.slot[new_slot].entry = &(parent_obj[array_id]);
        return new_slot;
    }
    catch (const std::bad_cast& e)
    {
        if (copied)
        {
            hookCtx.slot.erase(new_slot);
            hookCtx.slot_free.push(new_slot);
        }
        return Unexpected(NOT_AN_ARRAY);
    }

    return new_slot;
}

Expected<uint32_t, HookReturnCode>
HookAPI::slot_subfield(
    uint32_t parent_slot,
    uint32_t field_id,
    uint32_t new_slot) const
{
    if (hookCtx.slot.find(parent_slot) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (new_slot == 0 && no_free_slots())
        return Unexpected(NO_FREE_SLOTS);

    if (new_slot > hook_api::max_slots)
        return Unexpected(INVALID_ARGUMENT);

    SField const& fieldCode = ripple::SField::getField(field_id);

    if (fieldCode == sfInvalid)
        return Unexpected(INVALID_FIELD);

    if (hookCtx.slot[parent_slot].entry == 0)
        return Unexpected(INTERNAL_ERROR);

    bool copied = false;

    try
    {
        ripple::STObject& parent_obj =
            const_cast<ripple::STBase&>(*hookCtx.slot[parent_slot].entry)
                .downcast<ripple::STObject>();

        if (!parent_obj.isFieldPresent(fieldCode))
            return Unexpected(DOESNT_EXIST);

        if (new_slot == 0)
        {
            if (auto found = get_free_slot(); found)
                new_slot = *found;
            else
                return Unexpected(NO_FREE_SLOTS);
        }

        // copy
        if (new_slot != parent_slot)
        {
            copied = true;
            hookCtx.slot[new_slot] = hookCtx.slot[parent_slot];
        }

        hookCtx.slot[new_slot].entry = &(parent_obj.getField(fieldCode));
        return new_slot;
    }
    catch (const std::bad_cast& e)
    {
        if (copied)
        {
            hookCtx.slot.erase(new_slot);
            hookCtx.slot_free.push(new_slot);
        }
        return Unexpected(NOT_AN_OBJECT);
    }
}

Expected<std::variant<STBase, STAmount>, HookReturnCode>
HookAPI::slot_type(uint32_t slot_no, uint32_t flags) const
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (hookCtx.slot[slot_no].entry == 0)
        return Unexpected(INTERNAL_ERROR);
    try
    {
        ripple::STBase& obj = const_cast<ripple::STBase&>(
            *hookCtx.slot[slot_no].entry);  //.downcast<ripple::STBase>();
        if (flags == 0)
            return obj;

        // this flag is for use with an amount field to determine if the amount
        // is native (xrp)
        if (flags == 1)
        {
            if (obj.getSType() != STI_AMOUNT)
                return Unexpected(NOT_AN_AMOUNT);
            return const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry)
                .downcast<ripple::STAmount>();
        }

        return Unexpected(INVALID_ARGUMENT);
    }
    catch (const std::bad_cast& e)
    {
        return Unexpected(INTERNAL_ERROR);
    }
}

Expected<uint64_t, HookReturnCode>
HookAPI::slot_float(uint32_t slot_no) const
{
    if (hookCtx.slot.find(slot_no) == hookCtx.slot.end())
        return Unexpected(DOESNT_EXIST);

    if (hookCtx.slot[slot_no].entry == 0)
        return Unexpected(INTERNAL_ERROR);

    try
    {
        ripple::STAmount& st_amt =
            const_cast<ripple::STBase&>(*hookCtx.slot[slot_no].entry)
                .downcast<ripple::STAmount>();

        int64_t normalized = 0;
        if (st_amt.native())
        {
            ripple::XRPAmount amt = st_amt.xrp();
            int64_t drops = amt.drops();
            int32_t exp = -6;
            // normalize
            auto const ret = hook_float::normalize_xfl(drops, exp);
            if (!ret)
            {
                if (ret.error() == EXPONENT_UNDERSIZED)
                    return 0;
                return Unexpected(ret.error());
            }
            normalized = ret.value();
        }
        else
        {
            ripple::IOUAmount amt = st_amt.iou();
            auto const ret = make_float(amt);
            if (!ret)
            {
                if (ret.error() == EXPONENT_UNDERSIZED)
                    return 0;
                return Unexpected(ret.error());
            }
            normalized = ret.value();
        }

        if (normalized == EXPONENT_UNDERSIZED)
            /* exponent undersized (underflow) */
            return 0;  // return 0 in this case
        return normalized;
    }
    catch (const std::bad_cast& e)
    {
        return Unexpected(NOT_AN_AMOUNT);
    }
}

/// trace APIs
// trace
// trace_num
// trace_float

Expected<uint32_t, HookReturnCode>
HookAPI::meta_slot(uint32_t slot_into) const
{
    if (!hookCtx.result.provisionalMeta)
        return Unexpected(PREREQUISITE_NOT_MET);

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

    hookCtx.slot[slot_into] =
        hook::SlotEntry{.storage = hookCtx.result.provisionalMeta, .entry = 0};

    hookCtx.slot[slot_into].entry = &(*hookCtx.slot[slot_into].storage);

    return slot_into;
}

Expected<std::pair<uint32_t, uint32_t>, HookReturnCode>
HookAPI::xpop_slot(uint32_t slot_into_tx, uint32_t slot_into_meta) const
{
    if (hookCtx.applyCtx.tx.getFieldU16(sfTransactionType) != ttIMPORT)
        return Unexpected(PREREQUISITE_NOT_MET);

    if (slot_into_tx > hook_api::max_slots ||
        slot_into_meta > hook_api::max_slots)
        return Unexpected(INVALID_ARGUMENT);

    size_t free_count = hook_api::max_slots - hookCtx.slot.size();

    size_t needed_count = slot_into_tx == 0 && slot_into_meta == 0 ? 2
        : slot_into_tx != 0 && slot_into_meta != 0                 ? 0
                                                                   : 1;

    if (free_count < needed_count)
        return Unexpected(NO_FREE_SLOTS);

    // if they supply the same slot number for both (other than 0)
    // they will produce a collision
    if (needed_count == 0 && slot_into_tx == slot_into_meta)
        return Unexpected(INVALID_ARGUMENT);

    if (slot_into_tx == 0)
    {
        if (no_free_slots())
            return Unexpected(NO_FREE_SLOTS);

        if (auto found = get_free_slot(); found)
            slot_into_tx = *found;
        else
            return Unexpected(NO_FREE_SLOTS);
    }

    if (slot_into_meta == 0)
    {
        if (no_free_slots())
            return Unexpected(NO_FREE_SLOTS);

        if (auto found = get_free_slot(); found)
            slot_into_meta = *found;
        else
            return Unexpected(NO_FREE_SLOTS);
    }

    auto [tx, meta] =
        Import::getInnerTxn(hookCtx.applyCtx.tx, hookCtx.applyCtx.journal);

    if (!tx || !meta)
        return Unexpected(INVALID_TXN);

    hookCtx.slot[slot_into_tx] =
        hook::SlotEntry{.storage = std::move(tx), .entry = 0};

    hookCtx.slot[slot_into_tx].entry = &(*hookCtx.slot[slot_into_tx].storage);

    hookCtx.slot[slot_into_meta] =
        hook::SlotEntry{.storage = std::move(meta), .entry = 0};

    hookCtx.slot[slot_into_meta].entry =
        &(*hookCtx.slot[slot_into_meta].storage);

    return std::make_pair(slot_into_tx, slot_into_meta);
}

/// private

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
        return Unexpected(ret.error());
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
        return Unexpected(DIVISION_BY_ZERO);
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

std::optional<ripple::Keylet>
HookAPI::unserialize_keylet(Bytes const& data) const
{
    if (data.size() != 34)
        return {};

    uint16_t ktype = ((uint16_t)data[0] << 8) + ((uint16_t)data[1]);

    return ripple::Keylet{
        static_cast<LedgerEntryType>(ktype),
        ripple::uint256::fromVoid(data.data() + 2)};
}

inline std::optional<
    std::reference_wrapper<std::pair<bool, ripple::Blob> const>>
HookAPI::lookup_state_cache(
    AccountID const& acc,
    uint256 const& ns,
    uint256 const& key) const
{
    auto& stateMap = hookCtx.result.stateMap;
    if (stateMap.find(acc) == stateMap.end())
        return std::nullopt;

    auto& stateMapAcc = std::get<3>(stateMap[acc]);
    if (stateMapAcc.find(ns) == stateMapAcc.end())
        return std::nullopt;

    auto& stateMapNs = stateMapAcc[ns];

    auto const& ret = stateMapNs.find(key);

    if (ret == stateMapNs.end())
        return std::nullopt;

    return std::cref(ret->second);
}

// update the state cache
inline Expected<uint64_t, HookReturnCode>
HookAPI::set_state_cache(
    AccountID const& acc,
    uint256 const& ns,
    uint256 const& key,
    Bytes const& data,
    bool modified) const
{
    auto& stateMap = hookCtx.result.stateMap;
    auto& view = hookCtx.applyCtx.view();

    if (modified && stateMap.modified_entry_count >= max_state_modifications)
        return Unexpected(TOO_MANY_STATE_MODIFICATIONS);

    bool const createNamespace = view.rules().enabled(fixXahauV1) &&
        !view.exists(keylet::hookStateDir(acc, ns));

    if (stateMap.find(acc) == stateMap.end())
    {
        // if this is the first time this account has been interacted with
        // we will compute how many available reserve positions there are
        auto const& fees = hookCtx.applyCtx.view().fees();

        auto const accSLE = view.read(ripple::keylet::account(acc));

        if (!accSLE)
            return Unexpected(DOESNT_EXIST);

        STAmount bal = accSLE->getFieldAmount(sfBalance);

        uint16_t const hookStateScale = accSLE->isFieldPresent(sfHookStateScale)
            ? accSLE->getFieldU16(sfHookStateScale)
            : 1;

        int64_t availableForReserves = bal.xrp().drops() -
            fees.accountReserve(accSLE->getFieldU32(sfOwnerCount)).drops();

        int64_t increment = fees.increment.drops();

        if (increment <= 0)
            increment = 1;

        availableForReserves /= increment;

        if (availableForReserves < hookStateScale && modified)
            return Unexpected(RESERVE_INSUFFICIENT);

        int64_t namespaceCount = accSLE->isFieldPresent(sfHookNamespaces)
            ? accSLE->getFieldV256(sfHookNamespaces).size()
            : 0;

        if (createNamespace)
        {
            // overflow should never ever happen but check anyway
            if (namespaceCount + 1 < namespaceCount)
                return Unexpected(INTERNAL_ERROR);

            if (++namespaceCount > hook::maxNamespaces())
                return Unexpected(TOO_MANY_NAMESPACES);
        }

        stateMap.modified_entry_count++;

        // sanity check
        if (view.rules().enabled(featureExtendedHookState) &&
            availableForReserves < hookStateScale)
            return Unexpected(INTERNAL_ERROR);

        stateMap[acc] = {
            availableForReserves - hookStateScale,
            namespaceCount,
            hookStateScale,
            {{ns, {{key, {modified, data}}}}}};
        return 1;
    }

    auto& availableForReserves = std::get<0>(stateMap[acc]);
    auto& namespaceCount = std::get<1>(stateMap[acc]);
    auto& hookStateScale = std::get<2>(stateMap[acc]);
    auto& stateMapAcc = std::get<3>(stateMap[acc]);
    bool const canReserveNew = availableForReserves >= hookStateScale;

    if (stateMapAcc.find(ns) == stateMapAcc.end())
    {
        if (modified)
        {
            if (!canReserveNew)
                return Unexpected(RESERVE_INSUFFICIENT);

            if (createNamespace)
            {
                // overflow should never ever happen but check anyway
                if (namespaceCount + 1 < namespaceCount)
                    return Unexpected(INTERNAL_ERROR);

                if (namespaceCount + 1 > hook::maxNamespaces())
                    return Unexpected(TOO_MANY_NAMESPACES);

                namespaceCount++;
            }

            if (view.rules().enabled(featureExtendedHookState) &&
                availableForReserves < hookStateScale)
                return Unexpected(INTERNAL_ERROR);

            availableForReserves -= hookStateScale;
            stateMap.modified_entry_count++;
        }

        stateMapAcc[ns] = {{key, {modified, data}}};

        return 1;
    }

    auto& stateMapNs = stateMapAcc[ns];
    if (stateMapNs.find(key) == stateMapNs.end())
    {
        if (modified)
        {
            if (!canReserveNew)
                return Unexpected(RESERVE_INSUFFICIENT);

            if (view.rules().enabled(featureExtendedHookState) &&
                availableForReserves < hookStateScale)
                return Unexpected(INTERNAL_ERROR);

            availableForReserves -= hookStateScale;
            stateMap.modified_entry_count++;
        }

        stateMapNs[key] = {modified, data};
        hookCtx.result.changedStateCount++;
        return 1;
    }

    if (modified)
    {
        if (!stateMapNs[key].first)
            hookCtx.result.changedStateCount++;

        stateMap.modified_entry_count++;
        stateMapNs[key].first = true;
    }

    stateMapNs[key].second = data;
    return 1;
}

// RH NOTE this is a light-weight stobject parsing function for drilling into a
// provided serialzied object however it could probably be replaced by an
// existing class or routine or set of routines in XRPLD Returns object length
// including header bytes (and footer bytes in the event of array or object)
// negative indicates error
inline Expected<
    int32_t,
    HookAPI::parse_error>
HookAPI::get_stobject_length(
    unsigned char* start,   // in - begin iterator
    unsigned char* maxptr,  // in - end iterator
    int& type,              // out - populated by serialized type code
    int& field,             // out - populated by serialized field code
    int& payload_start,  // out - the start of actual payload data for this type
    int& payload_length,  // out - the length of actual payload data for this
                          // type
    Rules const& rules,
    int recursion_depth)  // used internally
    const
{
    if (recursion_depth > 10)
        return Unexpected(pe_excessive_nesting);

    uint16_t max_sti_type = rules.enabled(featureHookAPISerializedType240)
        ? STI_CURRENCY
        : STI_VECTOR256;

    if (type > max_sti_type)
        return pe_unknown_type_early;

    unsigned char* end = maxptr;
    unsigned char* upto = start;
    int high = *upto >> 4;
    int low = *upto & 0xF;

    upto++;
    if (upto >= end)
        return Unexpected(pe_unexpected_end);
    if (high > 0 && low > 0)
    {
        // common type common field
        type = high;
        field = low;
    }
    else if (high > 0)
    {
        // common type, uncommon field
        type = high;
        field = *upto++;
    }
    else if (low > 0)
    {
        // common field, uncommon type
        field = low;
        type = *upto++;
    }
    else
    {
        // uncommon type and field
        type = *upto++;
        if (upto >= end)
            return Unexpected(pe_unexpected_end);
        field = *upto++;
    }

    DBG_PRINTF(
        "%d get_st_object found field %d type %d\n",
        recursion_depth,
        field,
        type);

    if (upto >= end)
        return Unexpected(pe_unexpected_end);

    // RH TODO: link this to rippled's internal STObject constants
    // E.g.:
    /*
    int field_code = (safe_cast<int>(type) << 16) | field;
    auto const& fieldObj = ripple::SField::getField;
    */

    // type 10~13 are reserved
    if (type < 1 || max_sti_type < type || (10 <= type && type <= 13))
        return Unexpected(pe_unknown_type_early);

    // not supported types
    if (type == STI_NUMBER || type == STI_UINT96 || type == STI_UINT192 ||
        type == STI_UINT384 || type == STI_UINT512)
        return pe_unknown_type_early;

    bool is_vl =
        (type == STI_ACCOUNT || type == STI_VL ||
         (type == STI_PATHSET &&
          !rules.enabled(featureHookAPISerializedType240)) ||
         type == STI_VECTOR256);

    int length = -1;
    if (is_vl)
    {
        length = *upto++;
        if (upto >= end)
            return Unexpected(pe_unexpected_end);

        if (length < 193)
        {
            // do nothing
        }
        else if (length > 192 && length < 241)
        {
            length -= 193;
            length *= 256;
            length += *upto++ + 193;
            if (upto > end)
                return Unexpected(pe_unexpected_end);
        }
        else
        {
            int b2 = *upto++;
            if (upto >= end)
                return Unexpected(pe_unexpected_end);
            length -= 241;
            length *= 65536;
            length += 12481 + (b2 * 256) + *upto++;
            if (upto >= end)
                return Unexpected(pe_unexpected_end);
        }
    }
    else if (
        (type >= STI_UINT16 && type <= STI_UINT256) || type == STI_UINT8 ||
        type == STI_UINT160 || type == STI_CURRENCY)
    {
        switch (type)
        {
            case STI_UINT16:
                length = 2;
                break;
            case STI_UINT32:
                length = 4;
                break;
            case STI_UINT64:
                length = 8;
                break;
            case STI_UINT128:
                length = 16;
                break;
            case STI_UINT256:
                length = 32;
                break;
            case STI_UINT8:
                length = 1;
                break;
            case STI_UINT160:
                length = 20;
                break;
            case STI_CURRENCY:
                length = 20;
                break;
            default:
                return -1;
        }
    }
    else if (type == STI_AMOUNT) /* AMOUNT */
    {
        length = (*upto >> 6 == 1) ? 8 : 48;
        if (upto >= end)
            return Unexpected(pe_unexpected_end);
    }
    else if (
        type == STI_PATHSET && rules.enabled(featureHookAPISerializedType240))
    {
        length = 0;
        while (upto + length < end)
        {
            // iterate Path step
            while (*(upto + length) & 0x01 || *(upto + length) & 0x10 ||
                   *(upto + length) & 0x20)
            {
                int flag = *(upto + length++);
                // flag shoud be 0x01 or 0x10 or 0x20 or those union
                if (flag == 0 || flag & ~(0x01 | 0x10 | 0x20))
                    return pe_unexpected_end;
                if (flag & 0x01)  // account
                    length += 20;
                if (flag & 0x10)  // currency
                    length += 20;
                if (flag & 0x20)  // issuer
                    length += 20;

                int next_flag = *(upto + length);
                if (next_flag == 0x00 || next_flag == 0xff)
                    // end of Path step
                    break;
            }

            // continue or end of Paths
            int lastflag = *(upto + length++);
            if (lastflag == 0xff)
                continue;  // continue byte
            else if (lastflag == 0x00)
                break;  // end byte
            else
                return pe_unexpected_end;
        }
        if (upto >= end)
            return pe_unexpected_end;
    }
    else if (type == STI_ISSUE)
    {
        auto zero20 = std::array<char, 20>{0};
        // if first 20 byte is all zeros return 20
        // else return 40
        if (memcmp(upto, zero20.data(), 20) == 0)
            length = 20;
        else
            length = 40;
    }
    else if (type == STI_XCHAIN_BRIDGE)
    {
        auto zero20 = std::array<char, 20>{0};
        // Lock Chain
        length = 1;    // Door Account1 prefix length
        length += 20;  // Door Account1 length
        // Door Issue1
        if (memcmp(upto + length, zero20.data(), 20) == 0)
            length += 20;  // only Currency
        else
            length += 40;  // Currency and Issue

        // Issuing Chain
        length += 1;   // Door Account2 prefix length
        length += 20;  // Door Account2 length
        // Door Issue2
        if (memcmp(upto + length, zero20.data(), 20) == 0)
            length += 20;  // only Currency
        else
            length += 40;  // Currency and Issue
    }

    if (length > -1)
    {
        payload_start = upto - start;
        payload_length = length;
        DBG_PRINTF(
            "%d get_stobject_length field: %d Type: %d VL: %s Len: %d "
            "Payload_Start: %d Payload_Len: %d\n",
            recursion_depth,
            field,
            type,
            (is_vl ? "yes" : "no"),
            length,
            payload_start,
            payload_length);
        return length + (upto - start);
    }

    if (type == STI_OBJECT || type == STI_ARRAY)
    {
        payload_start = upto - start;

        for (int i = 0; i < 1024; ++i)
        {
            int subfield = -1, subtype = -1, payload_start_ = -1,
                payload_length_ = -1;
            auto const sublength = get_stobject_length(
                upto,
                end,
                subtype,
                subfield,
                payload_start_,
                payload_length_,
                hookCtx.applyCtx.view().rules(),
                recursion_depth + 1);
            DBG_PRINTF(
                "%d get_stobject_length i %d %d-%d, upto %d sublength %d\n",
                recursion_depth,
                i,
                subtype,
                subfield,
                upto - start,
                sublength);
            if (!sublength)
                return Unexpected(pe_unexpected_end);
            upto += sublength.value();
            if (upto >= end)
                return Unexpected(pe_unexpected_end);

            if ((*upto == 0xE1U && type == 0xEU) ||  // STI_OBJECT Maker
                (*upto == 0xF1U && type == 0xFU))    // STI_ARRAY Maker
            {
                payload_length = upto - start - payload_start;
                upto++;
                return (upto - start);
            }
        }
        return Unexpected(pe_excessive_size);
    }

    return Unexpected(pe_unknown_type_late);
};

}  // namespace hook
