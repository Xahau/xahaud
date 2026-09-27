#ifndef HOOK_API_INCLUDED
#define HOOK_API_INCLUDED 1

#include <xrpld/app/misc/Transaction.h>
#include <xrpl/hook/Enum.h>
#include <cfenv>

namespace hook {

using HookReturnCode = hook_api::hook_return_code;

namespace hook_float {
using enum hook_api::hook_return_code;

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
    using enum hook_api::hook_return_code;
    if (float1 < 0)
        return Unexpected(INVALID_FLOAT);
    if (float1 == 0)
        return 0u;
    uint64_t float_in = (uint64_t)float1;
    float_in >>= 54U;
    float_in &= 0xFFU;
    return static_cast<int32_t>(float_in) - 97;
}

inline Expected<uint64_t, HookReturnCode>
get_mantissa(int64_t float1)
{
    using enum hook_api::hook_return_code;
    if (float1 < 0)
        return Unexpected(INVALID_FLOAT);
    if (float1 == 0)
        return 0ULL;
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
    using enum hook_api::hook_return_code;
    if (mantissa > maxMantissa)
        return Unexpected(MANTISSA_OVERSIZED);
    if (mantissa < minMantissa)
        return Unexpected(MANTISSA_UNDERSIZED);
    return float1 - get_mantissa(float1).value() + mantissa;
}

inline Expected<uint64_t, HookReturnCode>
set_exponent(int64_t float1, int32_t exponent)
{
    using enum hook_api::hook_return_code;
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
    using enum hook_api::hook_return_code;
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
        float_out = (int64_t)mantissa.error();
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
    using enum hook_api::hook_return_code;
    if (mantissa == 0)
        return 0ULL;
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
        return Unexpected(m.error());  // LCOV_EXCL_LINE
    out = m.value();

    auto const e = set_exponent(out, exponent);
    if (!e)
        return Unexpected(e.error());  // LCOV_EXCL_LINE
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
        return 0ULL;

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
        return Unexpected(XFL_OVERFLOW);  // LCOV_EXCL_LINE

    int32_t adjust = 15 - mo;

    if (adjust > 0)
    {
        // defensive check
        if (adjust > 18)
            return 0ULL;  // LCOV_EXCL_LINE
        man *= power_of_ten[adjust];
        exp -= adjust;
    }
    else if (adjust < 0)
    {
        // defensive check
        if (-adjust > 18)
            return Unexpected(XFL_OVERFLOW);  // LCOV_EXCL_LINE
        man /= power_of_ten[-adjust];
        exp -= adjust;
    }

    if (man == 0)
    {
        exp = 0;
        return 0ULL;
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
        return 0ULL;
    }

    if (man == 0)
    {
        exp = 0;
        return 0ULL;
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
        return Unexpected(ret.error());

    return ret;
}

const int64_t float_one_internal =
    make_float(1000000000000000ull, -15, false).value();

}  // namespace hook_float

using namespace ripple;

using Bytes = std::vector<std::uint8_t>;

struct HookContext;  // defined in applyHook.h

class HookAPI
{
public:
    explicit HookAPI(HookContext& ctx) : hookCtx(ctx)
    {
    }

    /// control APIs
    // _g
    // accept
    // rollback

    /// util APIs
    Expected<std::string, HookReturnCode>
    util_raddr(Bytes const& accountID) const;

    Expected<Bytes, HookReturnCode>
    util_accid(std::string raddress) const;

    Expected<bool, HookReturnCode>
    util_verify(Slice const& data, Slice const& sig, Slice const& key) const;

    uint256
    util_sha512h(Slice const& data) const;

    // util_keylet()

    /// sto APIs
    Expected<bool, HookReturnCode>
    sto_validate(Bytes const& data) const;

    Expected<std::pair<uint32_t, uint32_t>, HookReturnCode>
    sto_subfield(Bytes const& data, uint32_t field_id) const;

    Expected<std::pair<uint32_t, uint32_t>, HookReturnCode>
    sto_subarray(Bytes const& data, uint32_t index_id) const;

    Expected<Bytes, HookReturnCode>
    sto_emplace(
        Bytes const& source_object,
        std::optional<Bytes> const& field_object,
        uint32_t field_id) const;

    // sto_erase(): same as sto_emplace with field_object = nullopt

    /// etxn APIs
    Expected<Bytes, HookReturnCode>
    prepare(Slice const& txBlob) const;

    Expected<std::shared_ptr<Transaction>, HookReturnCode>
    emit(Slice const& txBlob) const;

    Expected<uint64_t, HookReturnCode>
    etxn_burden() const;

    Expected<uint64_t, HookReturnCode>
    etxn_fee_base(Slice const& txBlob) const;

    Expected<uint64_t, HookReturnCode>
    etxn_details(uint8_t* out_ptr) const;

    Expected<uint64_t, HookReturnCode>
    etxn_reserve(uint64_t count) const;

    uint32_t
    etxn_generation() const;

    Expected<uint256, HookReturnCode>
    etxn_nonce() const;

    /// float APIs
    Expected<uint64_t, HookReturnCode>
    float_set(int32_t exponent, int64_t mantissa) const;

    Expected<uint64_t, HookReturnCode>
    float_multiply(uint64_t float1, uint64_t float2) const;

    Expected<uint64_t, HookReturnCode>
    float_mulratio(
        uint64_t float1,
        uint32_t round_up,
        uint32_t numerator,
        uint32_t denominator) const;

    uint64_t
    float_negate(uint64_t float1) const;

    Expected<uint64_t, HookReturnCode>
    float_compare(uint64_t float1, uint64_t float2, uint32_t mode) const;

    Expected<uint64_t, HookReturnCode>
    float_sum(uint64_t float1, uint64_t float2) const;

    Expected<Bytes, HookReturnCode>
    float_sto(
        std::optional<Currency> currency,
        std::optional<AccountID> issuer,
        uint64_t float1,
        uint32_t field_code,
        uint32_t write_len) const;

    Expected<uint64_t, HookReturnCode>
    float_sto_set(Bytes const& data) const;

    Expected<uint64_t, HookReturnCode>
    float_invert(uint64_t float1) const;

    Expected<uint64_t, HookReturnCode>
    float_divide(uint64_t float1, uint64_t float2) const;

    uint64_t
    float_one() const;

    Expected<uint64_t, HookReturnCode>
    float_mantissa(uint64_t float1) const;

    uint64_t
    float_sign(uint64_t float1) const;

    Expected<uint64_t, HookReturnCode>
    float_int(uint64_t float1, uint32_t decimal_places, uint32_t absolute)
        const;

    Expected<uint64_t, HookReturnCode>
    float_log(uint64_t float1) const;

    Expected<uint64_t, HookReturnCode>
    float_root(uint64_t float1, uint32_t n) const;

    /// otxn APIs
    uint64_t
    otxn_burden() const;

    uint32_t
    otxn_generation() const;

    Expected<const STBase*, HookReturnCode>
    otxn_field(uint32_t field_id) const;

    Expected<uint256, HookReturnCode>
    otxn_id(uint32_t flags) const;

    TxType
    otxn_type() const;

    Expected<uint32_t, HookReturnCode>
    otxn_slot(uint32_t slot_into) const;

    Expected<Blob, HookReturnCode>
    otxn_param(Bytes const& param_name) const;

    /// hook APIs
    AccountID
    hook_account() const;

    Expected<ripple::uint256, HookReturnCode>
    hook_hash(int32_t hook_no) const;

    Expected<uint64_t, HookReturnCode>
    hook_again() const;

    Expected<Blob, HookReturnCode>
    hook_param(Bytes const& paramName) const;

    Expected<uint64_t, HookReturnCode>
    hook_param_set(
        uint256 const& hash,
        Bytes const& paramName,
        Bytes const& paramValue) const;

    Expected<uint64_t, HookReturnCode>
    hook_skip(uint256 const& hash, uint32_t flags) const;

    uint8_t
    hook_pos() const;

    /// ledger APIs
    uint64_t
    fee_base() const;

    uint32_t
    ledger_seq() const;

    uint256
    ledger_last_hash() const;

    uint64_t
    ledger_last_time() const;

    Expected<uint256, HookReturnCode>
    ledger_nonce() const;

    Expected<Keylet, HookReturnCode>
    ledger_keylet(Keylet const& klLo, Keylet const& klHi) const;

    /// state APIs

    // state(): same as state_foreign with ns = 0 and account = hook_account()

    Expected<Bytes, HookReturnCode>
    state_foreign(
        uint256 const& key,
        uint256 const& ns,
        AccountID const& account) const;

    // state_set(): same as state_foreign_set with ns = 0 and account =

    Expected<uint64_t, HookReturnCode>
    state_foreign_set(
        uint256 const& key,
        uint256 const& ns,
        AccountID const& account,
        Bytes& data) const;

    /// slot APIs
    Expected<const STBase*, HookReturnCode>
    slot(uint32_t slot_no) const;

    Expected<uint64_t, HookReturnCode>
    slot_clear(uint32_t slot_no) const;

    Expected<uint64_t, HookReturnCode>
    slot_count(uint32_t slot_no) const;

    Expected<uint32_t, HookReturnCode>
    slot_set(Bytes const& data, uint32_t slot_no) const;

    Expected<uint64_t, HookReturnCode>
    slot_size(uint32_t slot_no) const;

    Expected<uint32_t, HookReturnCode>
    slot_subarray(uint32_t parent_slot, uint32_t array_id, uint32_t new_slot)
        const;

    Expected<uint32_t, HookReturnCode>
    slot_subfield(uint32_t parent_slot, uint32_t field_id, uint32_t new_slot)
        const;

    Expected<std::variant<STBase, STAmount>, HookReturnCode>
    slot_type(uint32_t slot_no, uint32_t flags) const;

    Expected<uint64_t, HookReturnCode>
    slot_float(uint32_t slot_no) const;

    /// trace APIs
    // trace
    // trace_num
    // trace_float

    Expected<uint32_t, HookReturnCode>
    meta_slot(uint32_t slot_into) const;

    Expected<std::pair<uint32_t, uint32_t>, HookReturnCode>
    xpop_slot(uint32_t slot_into_tx, uint32_t slot_into_meta) const;

private:
    HookContext& hookCtx;

    inline int32_t
    no_free_slots() const;

    inline std::optional<int32_t>
    get_free_slot() const;

    inline Expected<uint64_t, HookReturnCode>
    float_multiply_internal_parts(
        uint64_t man1,
        int32_t exp1,
        bool neg1,
        uint64_t man2,
        int32_t exp2,
        bool neg2) const;

    inline Expected<uint64_t, HookReturnCode>
    mulratio_internal(
        int64_t& man1,
        int32_t& exp1,
        bool round_up,
        uint32_t numerator,
        uint32_t denominator) const;

    inline Expected<uint64_t, HookReturnCode>
    float_divide_internal(uint64_t float1, uint64_t float2) const;

    inline Expected<uint64_t, HookReturnCode>
    double_to_xfl(double x) const;

    std::optional<ripple::Keylet>
    unserialize_keylet(Bytes const& data) const;

    // update the state cache
    inline std::optional<
        std::reference_wrapper<std::pair<bool, ripple::Blob> const>>
    lookup_state_cache(
        AccountID const& acc,
        uint256 const& ns,
        uint256 const& key) const;

    // check the state cache
    inline Expected<uint64_t, HookReturnCode>
    set_state_cache(
        AccountID const& acc,
        uint256 const& ns,
        uint256 const& key,
        Bytes const& data,
        bool modified) const;

    // these are only used by get_stobject_length below
    enum class STOParseErrorCode {
        pe_unexpected_end = -1,
        pe_unknown_type_early = -2,  // detected early
        pe_unknown_type_late = -3,   // end of function
        pe_excessive_nesting = -4,
        pe_excessive_size = -5
    };

    inline Expected<
        uint32_t,
        STOParseErrorCode>
    get_stobject_length(
        unsigned char* start,   // in - begin iterator
        unsigned char* maxptr,  // in - end iterator
        int& type,              // out - populated by serialized type code
        int& field,             // out - populated by serialized field code
        int& payload_start,     // out - the start of actual payload data for
                                // this type
        int& payload_length,    // out - the length of actual payload data for
                                // this type
        Rules const& rules,
        int recursion_depth = 0)  // used internally
        const;
};

}  // namespace hook

#endif  // HOOK_API_INCLUDED
