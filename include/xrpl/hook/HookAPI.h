#ifndef HOOK_API_INCLUDED
#define HOOK_API_INCLUDED 1

#include <xrpld/app/misc/Transaction.h>
#include <xrpl/hook/Enum.h>

namespace hook {
using namespace ripple;
using HookReturnCode = hook_api::hook_return_code;

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

    Expected<int64_t, HookReturnCode>
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
    enum parse_error {
        pe_unexpected_end = -1,
        pe_unknown_type_early = -2,  // detected early
        pe_unknown_type_late = -3,   // end of function
        pe_excessive_nesting = -4,
        pe_excessive_size = -5
    };

    inline Expected<
        int32_t,
        parse_error>
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
