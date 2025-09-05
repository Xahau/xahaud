#include <ripple/app/hook/Enum.h>
#include <ripple/app/tx/impl/ApplyContext.h>
#include <ripple/basics/Blob.h>
#include <ripple/basics/Expected.h>
#include <ripple/basics/Slice.h>
#include <ripple/protocol/STTx.h>

#include <ripple/app/misc/Transaction.h>
#include <ripple/basics/base_uint.h>
#include <ripple/protocol/TxFormats.h>
#include <cstdint>

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
    // util_raddr
    // util_accid
    // util_verify
    // util_sha512h

    /// sto APIs
    // sto_validate
    // sto_subfield
    // sto_subarray
    // sto_emplace
    // sto_erase
    // util_keylet

    /// etxn APIs
    Expected<std::shared_ptr<Transaction>, HookReturnCode>
    emit(Slice txBlob);

    Expected<uint64_t, HookReturnCode>
    etxn_burden() const;

    Expected<uint64_t, HookReturnCode>
    etxn_fee_base(Slice txBlob) const;
    // etxn_details
    // etxn_reserve
    uint32_t
    etxn_generation() const;
    // etxn_nonce

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

    // float_sto
    // float_sto_set
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
    otxn_param(Bytes param_name) const;

    /// hook APIs
    // hook_account
    // hook_hash
    // hook_again
    // hook_param
    // hook_param_set
    // hook_skip
    // hook_pos

    /// ledger APIs
    // fee_base
    // ledger_seq
    // ledger_last_hash
    // ledger_last_time
    // ledger_nonce
    // ledger_keylet

    /// state APIs
    // state
    // state_foreign
    // state_set
    // state_foreign_set

    /// slot APIs
    // slot
    // slot_clear
    // slot_count
    // slot_set
    // slot_size
    // slot_subarray
    // slot_subfield
    // slot_type
    // slot_float

    /// trace APIs
    // trace
    // trace_num
    // trace_float

    // meta_slot
    // xpop_slot

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
};

}  // namespace hook
