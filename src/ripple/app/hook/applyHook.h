#ifndef APPLY_HOOK_INCLUDED
#define APPLY_HOOK_INCLUDED 1
#include <ripple/app/hook/Enum.h>
#include <ripple/app/hook/Macro.h>
#include <ripple/app/hook/Misc.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/tx/impl/ApplyContext.h>
#include <ripple/basics/Blob.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/digest.h>
#include "quickjs-atom.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include <any>
#include <memory>
#include <optional>
#include <queue>
#include <vector>
#include <wasmedge/wasmedge.h>

extern "C" {
int
js_code_init_textdecoder(JSContext*, JSModuleDef* m);
}

namespace hook {
struct HookContext;
struct HookResult;
bool
isEmittedTxn(ripple::STTx const& tx);

// This map type acts as both a read and write cache for hook execution
// and is preserved across the execution of the set of hook chains
// being executed in the current transaction. It is committed to lgr
// only upon tesSuccess for the otxn.
class HookStateMap : public std::map<
                         ripple::AccountID,  // account that owns the state
                         std::tuple<
                             int64_t,  // remaining available ownercount
                             int64_t,  // total namespace count
                             std::map<
                                 ripple::uint256,  // namespace
                                 std::map<
                                     ripple::uint256,  // key
                                     std::pair<
                                         bool,  // is modified from ledger value
                                         ripple::Blob>>>>>  // the value
{
public:
    uint32_t modified_entry_count = 0;  // track the number of total modified
};

using namespace ripple;
std::vector<std::pair<AccountID, bool>>
getTransactionalStakeHolders(STTx const& tx, ReadView const& rv);
}  // namespace hook

namespace hook_api {

// for debugging if you want a lot of output change to 1
#define HOOK_DBG 0
#define DBG_PRINTF \
    if (HOOK_DBG)  \
    printf
#define DBG_FPRINTF \
    if (HOOK_DBG)   \
    fprintf

DECLARE_WASM_FUNCTION(int32_t, _g, uint32_t guard_id, uint32_t maxiter);

DECLARE_WASM_FUNCTION(
    int64_t,
    accept,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code);

DECLARE_JS_FUNCTION(int64_t, accept, JSValue error_msg, JSValue error_code);

DECLARE_WASM_FUNCTION(
    int64_t,
    rollback,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code);

DECLARE_JS_FUNCTION(int64_t, rollback, JSValue error_msg, JSValue error_code);

DECLARE_WASM_FUNCTION(
    int64_t,
    util_raddr,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

DECLARE_JS_FUNCTION(JSValue, util_raddr, JSValue acc_id);

DECLARE_WASM_FUNCTION(
    int64_t,
    util_accid,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

DECLARE_JS_FUNCTION(JSValue, util_accid, JSValue acc_id);

DECLARE_WASM_FUNCTION(
    int64_t,
    util_verify,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t sread_ptr,
    uint32_t sread_len,
    uint32_t kread_ptr,
    uint32_t kread_len);

DECLARE_JS_FUNCTION(
    JSValue,
    util_verify,
    JSValue data,
    JSValue sig,
    JSValue pubkey);

DECLARE_WASM_FUNCTION(
    int64_t,
    sto_validate,
    uint32_t tread_ptr,
    uint32_t tread_len);
DECLARE_JS_FUNCTION(JSValue, sto_validate, JSValue sto);
DECLARE_WASM_FUNCTION(
    int64_t,
    sto_subfield,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id);
DECLARE_JS_FUNCTION(JSValue, sto_subfield, JSValue sto, JSValue field_id);
DECLARE_WASM_FUNCTION(
    int64_t,
    sto_subarray,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t array_id);
DECLARE_JS_FUNCTION(JSValue, sto_subarray, JSValue sto, JSValue array_id);
DECLARE_WASM_FUNCTION(
    int64_t,
    sto_emplace,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t sread_ptr,
    uint32_t sread_len,
    uint32_t fread_ptr,
    uint32_t fread_len,
    uint32_t field_id);
DECLARE_JS_FUNCTION(
    JSValue,
    sto_emplace,
    JSValue sto,
    JSValue field_bytes,
    JSValue field_id);
DECLARE_WASM_FUNCTION(
    int64_t,
    sto_erase,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t field_id);
DECLARE_JS_FUNCTION(JSValue, sto_erase, JSValue sto, JSValue field_id);

DECLARE_WASM_FUNCTION(
    int64_t,
    util_sha512h,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

DECLARE_JS_FUNCTION(JSValue, util_sha512h, JSValue data);

DECLARE_WASM_FUNCTION(
    int64_t,
    util_keylet,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t keylet_type,
    uint32_t a,
    uint32_t b,
    uint32_t c,
    uint32_t d,
    uint32_t e,
    uint32_t f);

DECLARE_JS_FUNCTION(
    JSValue,
    util_keylet,
    JSValue keylet_type,
    JSValue keylet_data);

DECLARE_WASM_FUNCNARG(int64_t, etxn_burden);
DECLARE_JS_FUNCNARG(JSValue, etxn_burden);
DECLARE_WASM_FUNCTION(
    int64_t,
    etxn_details,
    uint32_t write_ptr,
    uint32_t write_len);
DECLARE_JS_FUNCNARG(JSValue, etxn_details);

DECLARE_WASM_FUNCTION(
    int64_t,
    etxn_fee_base,
    uint32_t read_ptr,
    uint32_t read_len);
DECLARE_JS_FUNCTION(JSValue, etxn_fee_base, JSValue txblob);

DECLARE_WASM_FUNCTION(int64_t, etxn_reserve, uint32_t count);
DECLARE_JS_FUNCTION(JSValue, etxn_reserve, JSValue count);

DECLARE_WASM_FUNCNARG(int64_t, etxn_generation);
DECLARE_JS_FUNCNARG(JSValue, etxn_generation);

DECLARE_WASM_FUNCTION(
    int64_t,
    etxn_nonce,
    uint32_t write_ptr,
    uint32_t write_len);

DECLARE_JS_FUNCNARG(JSValue, etxn_nonce);

DECLARE_WASM_FUNCTION(
    int64_t,
    emit,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

DECLARE_JS_FUNCTION(JSValue, emit, JSValue txn);

DECLARE_JS_FUNCTION(JSValue, prepare, JSValue tmpl);

DECLARE_JS_FUNCNARG(JSValue, otxn_json);

DECLARE_JS_FUNCTION(JSValue, slot_json, JSValue slotno);

DECLARE_JS_FUNCTION(JSValue, sto_to_json, JSValue sto_in);

DECLARE_JS_FUNCTION(JSValue, sto_from_json, JSValue json_in);

DECLARE_WASM_FUNCTION(int64_t, float_set, int32_t exponent, int64_t mantissa);
DECLARE_JS_FUNCTION(JSValue, float_set, JSValue e, JSValue m);

DECLARE_WASM_FUNCTION(int64_t, float_multiply, int64_t float1, int64_t float2);
DECLARE_JS_FUNCTION(JSValue, float_multiply, JSValue f1, JSValue f2);

DECLARE_WASM_FUNCTION(
    int64_t,
    float_mulratio,
    int64_t float1,
    uint32_t round_up,
    uint32_t numerator,
    uint32_t denominator);
DECLARE_JS_FUNCTION(
    JSValue,
    float_mulratio,
    JSValue f1,
    JSValue round_up,
    JSValue numerator,
    JSValue denominator);

DECLARE_WASM_FUNCTION(int64_t, float_negate, int64_t float1);
DECLARE_JS_FUNCTION(JSValue, float_negate, JSValue f1);

DECLARE_WASM_FUNCTION(
    int64_t,
    float_compare,
    int64_t float1,
    int64_t float2,
    uint32_t mode);
DECLARE_JS_FUNCTION(
    JSValue,
    float_compare,
    JSValue f1,
    JSValue f2,
    JSValue mode);

DECLARE_WASM_FUNCTION(int64_t, float_sum, int64_t float1, int64_t float2);
DECLARE_JS_FUNCTION(JSValue, float_sum, JSValue f1, JSValue f2);

DECLARE_WASM_FUNCTION(
    int64_t,
    float_sto,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t cread_ptr,
    uint32_t cread_len,
    uint32_t iread_ptr,
    uint32_t iread_len,
    int64_t float1,
    uint32_t field_code);
DECLARE_JS_FUNCTION(
    JSValue,
    float_sto,
    JSValue cur,
    JSValue isu,
    JSValue float1,
    JSValue field_code);

DECLARE_WASM_FUNCTION(
    int64_t,
    float_sto_set,
    uint32_t read_ptr,
    uint32_t read_len);
DECLARE_JS_FUNCTION(JSValue, float_sto_set, JSValue buf);

DECLARE_WASM_FUNCTION(int64_t, float_invert, int64_t float1);
DECLARE_JS_FUNCTION(JSValue, float_invert, JSValue f1);
DECLARE_WASM_FUNCTION(int64_t, float_divide, int64_t float1, int64_t float2);
DECLARE_JS_FUNCTION(JSValue, float_divide, JSValue f1, JSValue f2);
DECLARE_WASM_FUNCNARG(int64_t, float_one);
DECLARE_JS_FUNCNARG(JSValue, float_one);
DECLARE_WASM_FUNCTION(int64_t, float_mantissa, int64_t float1);
DECLARE_JS_FUNCTION(JSValue, float_mantissa, JSValue f1);
DECLARE_WASM_FUNCTION(int64_t, float_sign, int64_t float1);
DECLARE_JS_FUNCTION(JSValue, float_sign, JSValue f1);
DECLARE_WASM_FUNCTION(
    int64_t,
    float_int,
    int64_t float1,
    uint32_t decimal_places,
    uint32_t abs);
DECLARE_JS_FUNCTION(
    JSValue,
    float_int,
    JSValue f1,
    JSValue decimal_places,
    JSValue abs);
DECLARE_WASM_FUNCTION(int64_t, float_log, int64_t float1);
DECLARE_JS_FUNCTION(JSValue, float_log, JSValue float1);
DECLARE_WASM_FUNCTION(int64_t, float_root, int64_t float1, uint32_t n);
DECLARE_JS_FUNCTION(JSValue, float_root, JSValue f1, JSValue n);
DECLARE_WASM_FUNCTION(
    int64_t,
    hook_account,
    uint32_t write_ptr,
    uint32_t write_len);
DECLARE_JS_FUNCTION(JSValue, hook_account);

DECLARE_WASM_FUNCTION(
    int64_t,
    hook_hash,
    uint32_t write_ptr,
    uint32_t write_len,
    int32_t hook_no);
DECLARE_JS_FUNCTION(JSValue, hook_hash, JSValue hook_no);

DECLARE_WASM_FUNCNARG(int64_t, fee_base);
DECLARE_JS_FUNCNARG(JSValue, fee_base);
DECLARE_WASM_FUNCNARG(int64_t, ledger_seq);
DECLARE_JS_FUNCNARG(JSvalue, ledger_seq);
DECLARE_WASM_FUNCNARG(int64_t, ledger_last_time);
DECLARE_JS_FUNCNARG(JSValue, ledger_last_time);
DECLARE_WASM_FUNCTION(
    int64_t,
    ledger_last_hash,
    uint32_t write_ptr,
    uint32_t write_len);
DECLARE_JS_FUNCNARG(JSValue, ledger_last_hash);

DECLARE_WASM_FUNCTION(
    int64_t,
    ledger_nonce,
    uint32_t write_ptr,
    uint32_t write_len);
DECLARE_JS_FUNCNARG(JSValue, ledger_nonce);

DECLARE_WASM_FUNCTION(
    int64_t,
    ledger_keylet,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t lread_ptr,
    uint32_t lread_len,
    uint32_t hread_ptr,
    uint32_t hread_len);

DECLARE_JS_FUNCTION(JSValue, ledger_keylet, JSValue low, JSValue high);

DECLARE_WASM_FUNCTION(
    int64_t,
    hook_param_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t hread_ptr,
    uint32_t hread_len);

DECLARE_JS_FUNCTION(
    JSvalue,
    hook_param_set,
    JSValue val,
    JSValue key,
    JSValue hhash);

DECLARE_WASM_FUNCTION(
    int64_t,
    hook_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

DECLARE_JS_FUNCTION(JSValue, hook_param, JSValue key);

DECLARE_WASM_FUNCNARG(int64_t, hook_again);
DECLARE_JS_FUNCNARG(JSValue, hook_again);

DECLARE_WASM_FUNCTION(
    int64_t,
    hook_skip,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t flags);

DECLARE_JS_FUNCTION(JSValue, hook_skip, JSValue hhash, JSValue flags);

DECLARE_WASM_FUNCNARG(int64_t, hook_pos);

DECLARE_JS_FUNCNARG(JSValue, hook_pos);

DECLARE_WASM_FUNCTION(
    int64_t,
    slot,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t slot);
DECLARE_JS_FUNCTION(JSValue, slot, JSValue slot_no);

DECLARE_WASM_FUNCTION(int64_t, slot_clear, uint32_t slot);
DECLARE_JS_FUNCTION(JSValue, slot_clear, JSValue slot_no);

DECLARE_WASM_FUNCTION(int64_t, slot_count, uint32_t slot);
DECLARE_JS_FUNCTION(JSValue, slot_count, JSValeu slot_no);

DECLARE_WASM_FUNCTION(
    int64_t,
    slot_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t slot);
DECLARE_JS_FUNCTION(JSValue, slot_set, JSValue kl, JSValue slot_no);

DECLARE_WASM_FUNCTION(int64_t, slot_size, uint32_t slot);
DECLARE_JS_FUNCTION(JSValue, slot_size, JSValue slot_no);
DECLARE_WASM_FUNCTION(
    int64_t,
    slot_subarray,
    uint32_t parent_slot,
    uint32_t array_id,
    uint32_t new_slot);
DECLARE_JS_FUNCTION(
    JSValue,
    slot_subarray,
    JSValue parent_slot_no,
    JSValue array_id,
    JSValue new_slot_no);

DECLARE_WASM_FUNCTION(
    int64_t,
    slot_subfield,
    uint32_t parent_slot,
    uint32_t field_id,
    uint32_t new_slot);
DECLARE_JS_FUNCTION(
    JSValue,
    slot_subfield,
    JSValue parent_slot_no,
    JSValue field_id,
    JSValue new_slot_no);

DECLARE_WASM_FUNCTION(int64_t, slot_type, uint32_t slot_no, uint32_t flags);
DECLARE_JS_FUNCTION(JSValue, slot_type, JSValue slot_no, JSValue flags);
DECLARE_WASM_FUNCTION(int64_t, slot_float, uint32_t slot_no);
DECLARE_JS_FUNCTION(JSValue, slot_float, JSValue slot_no);

DECLARE_WASM_FUNCTION(
    int64_t,
    state_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len);
DECLARE_JS_FUNCTION(JSValue, state_set, JSValue val, JSValue key);

DECLARE_WASM_FUNCTION(
    int64_t,
    state_foreign_set,
    uint32_t read_ptr,
    uint32_t read_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t nread_ptr,
    uint32_t nread_len,
    uint32_t aread_ptr,
    uint32_t aread_len);

DECLARE_JS_FUNCTION(
    JSValue,
    state_foreign_set,
    JSValue val,
    JSValue key,
    JSValue ns,
    JSValue accid);

DECLARE_WASM_FUNCTION(
    int64_t,
    state,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t kread_ptr,
    uint32_t kread_len);
DECLARE_JS_FUNCTION(JSValue, state, JSValue key);

DECLARE_WASM_FUNCTION(
    int64_t,
    state_foreign,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t kread_ptr,
    uint32_t kread_len,
    uint32_t nread_ptr,
    uint32_t nread_len,
    uint32_t aread_ptr,
    uint32_t aread_len);
DECLARE_JS_FUNCTION(
    JSValue,
    state_foreign,
    JSValue key,
    JSValue ns,
    JSValue accid);

DECLARE_WASM_FUNCTION(
    int64_t,
    trace,
    uint32_t mread_ptr,
    uint32_t mread_len,
    uint32_t dread_ptr,
    uint32_t dread_len,
    uint32_t as_hex);

DECLARE_JS_FUNCTION(JSValue, trace, JSValue msg, JSValue data, JSValue as_hex);

DECLARE_WASM_FUNCTION(
    int64_t,
    trace_num,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t number);
DECLARE_WASM_FUNCTION(
    int64_t,
    trace_float,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t float1);

DECLARE_WASM_FUNCNARG(int64_t, otxn_burden);
DECLARE_JS_FUNCNARG(JSValue, otxn_burden);

DECLARE_WASM_FUNCTION(
    int64_t,
    otxn_field,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t field_id);

DECLARE_JS_FUNCTION(JSValue, otxn_field, JSValue field_id);

DECLARE_WASM_FUNCNARG(int64_t, otxn_generation);
DECLARE_JS_FUNCNARG(JSValue, otxn_generation);
DECLARE_WASM_FUNCTION(
    int64_t,
    otxn_id,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t flags);

DECLARE_JS_FUNCTION(JSValue, otxn_id, flags);

DECLARE_WASM_FUNCNARG(int64_t, otxn_type);

DECLARE_JS_FUNCNARG(int64_t, otxn_type);

DECLARE_WASM_FUNCTION(int64_t, otxn_slot, uint32_t slot_no);

DECLARE_JS_FUNCTION(JSValue, otxn_slot, JSValue slot_no);

DECLARE_WASM_FUNCTION(
    int64_t,
    otxn_param,
    uint32_t write_ptr,
    uint32_t write_len,
    uint32_t read_ptr,
    uint32_t read_len);

DECLARE_JS_FUNCTION(JSValue, otxn_param, JSValue param_key);

DECLARE_WASM_FUNCTION(int64_t, meta_slot, uint32_t slot_no);
DECLARE_JS_FUNCTION(JSValue, meta_slot, JSValue slot_no);

DECLARE_WASM_FUNCTION(
    int64_t,
    xpop_slot,
    uint32_t slot_no_tx,
    uint32_t slot_no_meta);
DECLARE_JS_FUNCTION(
    JSValue,
    xpop_slot,
    JSValue slot_no_tx,
    JSValue slot_no_meta);

} /* end namespace hook_api */

namespace hook {

bool
canHook(ripple::TxType txType, ripple::uint256 hookOn);

struct HookResult;

HookResult
apply(
    ripple::uint256 const& hookSetTxnID, /* this is the txid of the sethook,
                                            used for caching (one day) */
    ripple::uint256 const&
        hookHash, /* hash of the actual hook byte code, used for metadata */
    uint16_t hookApiVersion,
    ripple::uint256 const& hookNamespace,
    ripple::Blob const& wasm,
    std::map<
        std::vector<uint8_t>, /* param name  */
        std::vector<uint8_t>  /* param value */
        > const& hookParams,
    std::map<
        ripple::uint256, /* hook hash */
        std::map<std::vector<uint8_t>, std::vector<uint8_t>>> const&
        hookParamOverrides,
    HookStateMap& stateMap,
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& account, /* the account the hook is INSTALLED ON
                                         not always the otxn account */
    bool hasCallback,
    bool isCallback,
    bool isStrongTSH,
    uint32_t hookArgument,
    uint8_t hookChainPosition,
    // result of apply() if this is weak exec
    std::shared_ptr<STObject const> const& provisionalMeta,
    uint32_t instructionLimit);

struct HookContext;

uint32_t
computeHookStateOwnerCount(uint32_t hookStateCount);

int64_t
computeExecutionFee(uint64_t instructionCount);
int64_t
computeCreationFee(uint64_t byteCount);

struct HookResult
{
    ripple::uint256 const hookSetTxnID;
    ripple::uint256 const hookHash;
    ripple::Keylet const accountKeylet;
    ripple::Keylet const ownerDirKeylet;
    ripple::Keylet const hookKeylet;
    ripple::AccountID const account;
    ripple::AccountID const otxnAccount;
    ripple::uint256 const hookNamespace;

    std::queue<std::shared_ptr<ripple::Transaction>>
        emittedTxn{};  // etx stored here until accept/rollback
    HookStateMap& stateMap;
    uint16_t changedStateCount = 0;
    std::map<
        ripple::uint256,  // hook hash
        std::map<
            std::vector<uint8_t>,  // hook param name
            std::vector<uint8_t>   // hook param value
            >>
        hookParamOverrides;

    std::map<std::vector<uint8_t>, std::vector<uint8_t>> const& hookParams;
    std::set<ripple::uint256> hookSkips;
    hook_api::ExitType exitType = hook_api::ExitType::ROLLBACK;
    std::string exitReason{""};
    int64_t exitCode{-1};
    uint64_t instructionCount{0};
    bool hasCallback = false;  // true iff this hook wasm has a cbak function
    bool isCallback =
        false;  // true iff this hook execution is a callback in action
    bool isStrong = false;
    uint32_t hookArgument = 0;
    uint32_t overrideCount = 0;
    uint8_t hookChainPosition = 0;
    bool foreignStateSetDisabled = false;
    bool executeAgainAsWeak =
        false;  // hook_again allows strong pre-apply to nominate
                // additional weak post-apply execution
    std::shared_ptr<STObject const> provisionalMeta;
};

class HookExecutorBase;
// class HookExecutorWasm;
// class HookExecutorJs;

struct SlotEntry
{
    std::shared_ptr<const ripple::STObject> storage;
    const ripple::STBase* entry;  // raw pointer into the storage, that can be
                                  // freely pointed around inside
};

struct HookContext
{
    ripple::ApplyContext& applyCtx;
    // slots are used up by requesting objects from inside the hook
    // the map stores pairs consisting of a memory view and whatever shared or
    // unique ptr is required to keep the underlying object alive for the
    // duration of the hook's execution slot number -> { keylet or hash, {
    // pointer to current object, storage for that object } }
    std::map<uint32_t, SlotEntry> slot{};
    std::queue<uint32_t> slot_free{};
    uint32_t slot_counter{0};  // uint16 to avoid accidental overflow and to
                               // allow more slots in future
    uint16_t emit_nonce_counter{
        0};  // incremented whenever nonce is called to ensure unique nonces
    uint16_t ledger_nonce_counter{0};
    int64_t expected_etxn_count{-1};  // make this a 64bit int so the uint32
                                      // from the hookapi cant overflow it
    std::map<ripple::uint256, bool> nonce_used{};
    uint32_t generation =
        0;  // used for caching, only generated when txn_generation is called
    uint64_t burden =
        0;  // used for caching, only generated when txn_burden is called
    std::map<uint32_t, uint32_t>
        guard_map{};  // iteration guard map <id -> upto_iteration>
    HookResult result;
    std::optional<ripple::STObject>
        emitFailure;  // if this is a callback from a failed
                      // emitted txn then this optional becomes
                      // populated with the SLE
    const HookExecutorBase* module = 0;
};

bool
addHookNamespaceEntry(ripple::SLE& sleAccount, ripple::uint256 ns);

bool
removeHookNamespaceEntry(ripple::SLE& sleAccount, ripple::uint256 ns);

ripple::TER
setHookState(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key,
    ripple::Slice const& data);

// write hook execution metadata and remove emitted transaction ledger entries
ripple::TER
finalizeHookResult(
    hook::HookResult& hookResult,
    ripple::ApplyContext&,
    bool doEmit);

// write state map to ledger
ripple::TER
finalizeHookState(
    HookStateMap const&,
    ripple::ApplyContext&,
    ripple::uint256 const&);

// if the txn being executed was an emitted txn then this removes it from the
// emission directory
ripple::TER
removeEmissionEntry(ripple::ApplyContext& applyCtx);

bool /* retval of true means an error */
gatherHookParameters(
    std::shared_ptr<ripple::STLedgerEntry> const& hookDef,
    ripple::STObject const& hookObj,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& parameters,
    beast::Journal const& j_);

// RH TODO: call destruct for these on rippled shutdown
#define ADD_WASM_FUNCTION(F, ctx)                          \
    {                                                      \
        WasmEdge_FunctionInstanceContext* hf =             \
            WasmEdge_FunctionInstanceCreate(               \
                hook_api::WasmFunctionType##F,             \
                hook_api::WasmFunction##F,                 \
                (void*)(&ctx),                             \
                0);                                        \
        WasmEdge_ModuleInstanceAddFunction(                \
            importObj, hook_api::WasmFunctionName##F, hf); \
    }

#define ADD_JS_FUNCTION(F, ctx)                       \
    {                                                 \
        JSValue global_obj = JS_GetGlobalObject(ctx); \
        JS_SetPropertyStr(                            \
            ctx,                                      \
            global_obj,                               \
            #F,                                       \
            JS_NewCFunction(                          \
                ctx,                                  \
                hook_api::JSFunction##F,              \
                #F,                                   \
                hook_api::JSFunctionParamCount##F));  \
        JS_FreeValue(ctx, global_obj);                \
    }

#define HR_ACC() hookResult.account << "-" << hookResult.otxnAccount
#define HC_ACC() hookCtx.result.account << "-" << hookCtx.result.otxnAccount

class HookExecutorBase
{
protected:
    bool spent = false;  // an HookExecutor can only be used once

public:
    HookContext& hookCtx;

    HookExecutorBase(HookContext& ctx) : hookCtx(ctx)
    {
    }

    virtual void
    execute(
        const void* code,
        size_t len,
        bool callback,
        uint32_t param,
        uint32_t instructionLimit,
        beast::Journal const& j) = 0;

    static std::optional<std::string>
    validate(const void* code, size_t len)
    {
        // Base class doesn't implement validation
        return "validate() illegally called on HookExecutorBase class";
    }

    virtual ~HookExecutorBase() = default;
};

/**
 * HookExecutorWasm is effectively a two-part function:
 * The first part sets up the Hook Api inside the wasm import, ready for use
 * (this is done during object construction.)
 * The second part is actually executing webassembly instructions
 * this is done during execteWasm function.
 * The instance is single use.
 */

// create these once at boot and keep them
static WasmEdge_String exportName = WasmEdge_StringCreateByCString("env");
static WasmEdge_String tableName = WasmEdge_StringCreateByCString("table");
static auto* tableType = WasmEdge_TableTypeCreate(
    WasmEdge_RefType_FuncRef,
    {.HasMax = true, .Shared = false, .Min = 10, .Max = 20});
static auto* memType = WasmEdge_MemoryTypeCreate(
    {.HasMax = true, .Shared = false, .Min = 1, .Max = 1});
static WasmEdge_String memName = WasmEdge_StringCreateByCString("memory");
static WasmEdge_String cbakFunctionName =
    WasmEdge_StringCreateByCString("cbak");
static WasmEdge_String hookFunctionName =
    WasmEdge_StringCreateByCString("hook");

// see: lib/system/allocator.cpp
#define WasmEdge_kPageSize 65536ULL

class HookExecutorWasm : public HookExecutorBase
{
public:
    WasmEdge_ModuleInstanceContext* importObj;

    class WasmEdgeVM
    {
    public:
        WasmEdge_ConfigureContext* conf = NULL;
        WasmEdge_VMContext* ctx = NULL;

        WasmEdgeVM()
        {
            conf = WasmEdge_ConfigureCreate();
            if (!conf)
                return;
            WasmEdge_ConfigureStatisticsSetInstructionCounting(conf, true);
            ctx = WasmEdge_VMCreate(conf, NULL);
        }

        bool
        sane()
        {
            return ctx && conf;
        }

        ~WasmEdgeVM()
        {
            if (conf)
                WasmEdge_ConfigureDelete(conf);
            if (ctx)
                WasmEdge_VMDelete(ctx);
        }
    };

    // if an error occured return a string prefixed with `prefix` followed by
    // the error description
    static std::optional<std::string>
    getWasmError(std::string prefix, WasmEdge_Result& res)
    {
        if (WasmEdge_ResultOK(res))
            return {};

        const char* msg = WasmEdge_ResultGetMessage(res);
        return prefix + ": " + (msg ? msg : "unknown error");
    }

    /**
     * Validate that a web assembly blob can be loaded by wasmedge
     */
    static std::optional<std::string>
    validate(const void* wasm, size_t len)
    {
        WasmEdgeVM vm;

        if (!vm.sane())
            return "Could not create WASMEDGE instance";

        WasmEdge_Result res = WasmEdge_VMLoadWasmFromBuffer(
            vm.ctx, reinterpret_cast<const uint8_t*>(wasm), len);

        if (auto err = getWasmError("VMLoadWasmFromBuffer failed", res); err)
            return *err;

        res = WasmEdge_VMValidate(vm.ctx);

        if (auto err = getWasmError("VMValidate failed", res); err)
            return *err;

        return {};
    }

    /**
     * Execute web assembly byte code against the constructed Hook Context
     * Once execution has occured the exector is spent and cannot be used again
     * and should be destructed Information about the execution is populated
     * into hookCtx
     */
    void
    execute(
        const void* wasm,
        size_t len,
        bool callback,
        uint32_t hookArgument,
        uint32_t instructionLimit, /* this is unused in wasm, set to 0 */
        beast::Journal const& j) override
    {
        // HookExecutorWasm can only execute once
        assert(!spent);
        spent = true;

        JLOG(j.trace()) << "HookInfo[" << HC_ACC()
                        << "]: creating wasm instance";

        WasmEdge_LogOff();

        WasmEdgeVM vm;

        if (!vm.sane())
        {
            JLOG(j.warn()) << "HookError[" << HC_ACC()
                           << "]: Could not create WASMEDGE instance.";

            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            return;
        }

        WasmEdge_Result res =
            WasmEdge_VMRegisterModuleFromImport(vm.ctx, this->importObj);

        if (auto err = getWasmError("Import phase failed", res); err)
        {
            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            JLOG(j.trace()) << "HookError[" << HC_ACC() << "]: " << *err;
            return;
        }

        WasmEdge_Value params[1] = {
            WasmEdge_ValueGenI32((int64_t)hookArgument)};
        WasmEdge_Value returns[1];

        res = WasmEdge_VMRunWasmFromBuffer(
            vm.ctx,
            reinterpret_cast<const uint8_t*>(wasm),
            len,
            callback ? cbakFunctionName : hookFunctionName,
            params,
            1,
            returns,
            1);

        if (auto err = getWasmError("WASM VM error", res); err)
        {
            JLOG(j.warn()) << "HookError[" << HC_ACC() << "]: " << *err;
            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            return;
        }

        auto* statsCtx = WasmEdge_VMGetStatisticsContext(vm.ctx);
        hookCtx.result.instructionCount =
            WasmEdge_StatisticsGetInstrCount(statsCtx);

        // RH NOTE: stack unwind will clean up WasmEdgeVM
    }

    HookExecutorWasm(HookContext& ctx)
        : HookExecutorBase(ctx)
        , importObj(WasmEdge_ModuleInstanceCreate(exportName))
    {
        ctx.module = this;

        WasmEdge_LogSetDebugLevel();

        ADD_WASM_FUNCTION(_g, ctx);
        ADD_WASM_FUNCTION(accept, ctx);
        ADD_WASM_FUNCTION(rollback, ctx);
        ADD_WASM_FUNCTION(util_raddr, ctx);
        ADD_WASM_FUNCTION(util_accid, ctx);
        ADD_WASM_FUNCTION(util_verify, ctx);
        ADD_WASM_FUNCTION(util_sha512h, ctx);
        ADD_WASM_FUNCTION(sto_validate, ctx);
        ADD_WASM_FUNCTION(sto_subfield, ctx);
        ADD_WASM_FUNCTION(sto_subarray, ctx);
        ADD_WASM_FUNCTION(sto_emplace, ctx);
        ADD_WASM_FUNCTION(sto_erase, ctx);
        ADD_WASM_FUNCTION(util_keylet, ctx);

        ADD_WASM_FUNCTION(emit, ctx);
        ADD_WASM_FUNCTION(etxn_burden, ctx);
        ADD_WASM_FUNCTION(etxn_fee_base, ctx);
        ADD_WASM_FUNCTION(etxn_details, ctx);
        ADD_WASM_FUNCTION(etxn_reserve, ctx);
        ADD_WASM_FUNCTION(etxn_generation, ctx);
        ADD_WASM_FUNCTION(etxn_nonce, ctx);

        ADD_WASM_FUNCTION(float_set, ctx);
        ADD_WASM_FUNCTION(float_multiply, ctx);
        ADD_WASM_FUNCTION(float_mulratio, ctx);
        ADD_WASM_FUNCTION(float_negate, ctx);
        ADD_WASM_FUNCTION(float_compare, ctx);
        ADD_WASM_FUNCTION(float_sum, ctx);
        ADD_WASM_FUNCTION(float_sto, ctx);
        ADD_WASM_FUNCTION(float_sto_set, ctx);
        ADD_WASM_FUNCTION(float_invert, ctx);

        ADD_WASM_FUNCTION(float_divide, ctx);
        ADD_WASM_FUNCTION(float_one, ctx);
        ADD_WASM_FUNCTION(float_mantissa, ctx);
        ADD_WASM_FUNCTION(float_sign, ctx);
        ADD_WASM_FUNCTION(float_int, ctx);
        ADD_WASM_FUNCTION(float_log, ctx);
        ADD_WASM_FUNCTION(float_root, ctx);

        ADD_WASM_FUNCTION(otxn_burden, ctx);
        ADD_WASM_FUNCTION(otxn_generation, ctx);
        ADD_WASM_FUNCTION(otxn_field, ctx);
        ADD_WASM_FUNCTION(otxn_id, ctx);
        ADD_WASM_FUNCTION(otxn_type, ctx);
        ADD_WASM_FUNCTION(otxn_slot, ctx);
        ADD_WASM_FUNCTION(otxn_param, ctx);

        ADD_WASM_FUNCTION(hook_account, ctx);
        ADD_WASM_FUNCTION(hook_hash, ctx);
        ADD_WASM_FUNCTION(hook_again, ctx);
        ADD_WASM_FUNCTION(fee_base, ctx);
        ADD_WASM_FUNCTION(ledger_seq, ctx);
        ADD_WASM_FUNCTION(ledger_last_hash, ctx);
        ADD_WASM_FUNCTION(ledger_last_time, ctx);
        ADD_WASM_FUNCTION(ledger_nonce, ctx);
        ADD_WASM_FUNCTION(ledger_keylet, ctx);

        ADD_WASM_FUNCTION(hook_param, ctx);
        ADD_WASM_FUNCTION(hook_param_set, ctx);
        ADD_WASM_FUNCTION(hook_skip, ctx);
        ADD_WASM_FUNCTION(hook_pos, ctx);

        ADD_WASM_FUNCTION(state, ctx);
        ADD_WASM_FUNCTION(state_foreign, ctx);
        ADD_WASM_FUNCTION(state_set, ctx);
        ADD_WASM_FUNCTION(state_foreign_set, ctx);

        ADD_WASM_FUNCTION(slot, ctx);
        ADD_WASM_FUNCTION(slot_clear, ctx);
        ADD_WASM_FUNCTION(slot_count, ctx);
        ADD_WASM_FUNCTION(slot_set, ctx);
        ADD_WASM_FUNCTION(slot_size, ctx);
        ADD_WASM_FUNCTION(slot_subarray, ctx);
        ADD_WASM_FUNCTION(slot_subfield, ctx);
        ADD_WASM_FUNCTION(slot_type, ctx);
        ADD_WASM_FUNCTION(slot_float, ctx);

        ADD_WASM_FUNCTION(trace, ctx);
        ADD_WASM_FUNCTION(trace_num, ctx);
        ADD_WASM_FUNCTION(trace_float, ctx);

        ADD_WASM_FUNCTION(meta_slot, ctx);
        ADD_WASM_FUNCTION(xpop_slot, ctx);

        WasmEdge_TableInstanceContext* hostTable =
            WasmEdge_TableInstanceCreate(tableType);
        WasmEdge_ModuleInstanceAddTable(importObj, tableName, hostTable);
        WasmEdge_MemoryInstanceContext* hostMem =
            WasmEdge_MemoryInstanceCreate(memType);
        WasmEdge_ModuleInstanceAddMemory(importObj, memName, hostMem);
    }

    virtual ~HookExecutorWasm()
    {
        WasmEdge_ModuleInstanceDelete(importObj);
    };
};

class HookExecutorJS : public HookExecutorBase
{
public:
    class QuickJSVM
    {
    public:
        JSRuntime* rt = NULL;
        JSContext* ctx = NULL;

        QuickJSVM(void* hookCtx, uint32_t instructionLimit)
        {
            rt = JS_NewRuntime(instructionLimit);
            ctx = JS_NewContextRaw(rt);
            JS_AddIntrinsicBaseObjects(ctx);
            JS_AddIntrinsicEval(ctx);
            JS_AddIntrinsicStringNormalize(ctx);
            JS_AddIntrinsicRegExp(ctx);
            JS_AddIntrinsicJSON(ctx);
            JS_AddIntrinsicMapSet(ctx);
            JS_AddIntrinsicTypedArrays(ctx);
            JS_AddIntrinsicBigInt(ctx);
            //::js_init_module_textdecoder(ctx, "textdecoder");
            ::js_code_init_textdecoder(ctx, 0);

            JS_SetMaxStackSize(rt, 65535);
            JS_SetMemoryLimit(rt, 16 * 1024 * 1024);

            JS_SetRuntimeOpaque(rt, hookCtx);

            ADD_JS_FUNCTION(accept, ctx);
            ADD_JS_FUNCTION(rollback, ctx);
            ADD_JS_FUNCTION(util_raddr, ctx);
            ADD_JS_FUNCTION(util_accid, ctx);
            ADD_JS_FUNCTION(util_verify, ctx);
            ADD_JS_FUNCTION(util_sha512h, ctx);
            ADD_JS_FUNCTION(util_keylet, ctx);

            ADD_JS_FUNCTION(sto_validate, ctx);
            ADD_JS_FUNCTION(sto_subfield, ctx);
            ADD_JS_FUNCTION(sto_subarray, ctx);
            ADD_JS_FUNCTION(sto_emplace, ctx);
            ADD_JS_FUNCTION(sto_erase, ctx);

            ADD_JS_FUNCTION(emit, ctx);
            ADD_JS_FUNCTION(prepare, ctx);
            ADD_JS_FUNCTION(otxn_json, ctx);

            ADD_JS_FUNCTION(slot_json, ctx);
            ADD_JS_FUNCTION(sto_to_json, ctx);
            ADD_JS_FUNCTION(sto_from_json, ctx);

            ADD_JS_FUNCTION(etxn_burden, ctx);
            ADD_JS_FUNCTION(etxn_fee_base, ctx);
            ADD_JS_FUNCTION(etxn_details, ctx);
            ADD_JS_FUNCTION(etxn_reserve, ctx);
            ADD_JS_FUNCTION(etxn_generation, ctx);
            ADD_JS_FUNCTION(etxn_nonce, ctx);

            ADD_JS_FUNCTION(float_set, ctx);
            ADD_JS_FUNCTION(float_multiply, ctx);
            ADD_JS_FUNCTION(float_mulratio, ctx);
            ADD_JS_FUNCTION(float_negate, ctx);
            ADD_JS_FUNCTION(float_compare, ctx);
            ADD_JS_FUNCTION(float_sum, ctx);
            ADD_JS_FUNCTION(float_sto, ctx);
            ADD_JS_FUNCTION(float_sto_set, ctx);
            ADD_JS_FUNCTION(float_invert, ctx);

            ADD_JS_FUNCTION(float_divide, ctx);
            ADD_JS_FUNCTION(float_one, ctx);
            ADD_JS_FUNCTION(float_mantissa, ctx);
            ADD_JS_FUNCTION(float_sign, ctx);
            ADD_JS_FUNCTION(float_int, ctx);
            ADD_JS_FUNCTION(float_log, ctx);
            ADD_JS_FUNCTION(float_root, ctx);

            ADD_JS_FUNCTION(otxn_burden, ctx);
            ADD_JS_FUNCTION(otxn_generation, ctx);
            ADD_JS_FUNCTION(otxn_field, ctx);
            ADD_JS_FUNCTION(otxn_id, ctx);
            ADD_JS_FUNCTION(otxn_type, ctx);
            ADD_JS_FUNCTION(otxn_slot, ctx);
            ADD_JS_FUNCTION(otxn_param, ctx);

            ADD_JS_FUNCTION(hook_account, ctx);
            ADD_JS_FUNCTION(hook_hash, ctx);
            ADD_JS_FUNCTION(hook_again, ctx);
            ADD_JS_FUNCTION(fee_base, ctx);
            ADD_JS_FUNCTION(ledger_seq, ctx);
            ADD_JS_FUNCTION(ledger_last_hash, ctx);
            ADD_JS_FUNCTION(ledger_last_time, ctx);
            ADD_JS_FUNCTION(ledger_nonce, ctx);
            ADD_JS_FUNCTION(ledger_keylet, ctx);

            ADD_JS_FUNCTION(hook_param, ctx);
            ADD_JS_FUNCTION(hook_param_set, ctx);
            ADD_JS_FUNCTION(hook_skip, ctx);
            ADD_JS_FUNCTION(hook_pos, ctx);

            ADD_JS_FUNCTION(state, ctx);
            ADD_JS_FUNCTION(state_foreign, ctx);
            ADD_JS_FUNCTION(state_set, ctx);
            ADD_JS_FUNCTION(state_foreign_set, ctx);

            ADD_JS_FUNCTION(slot, ctx);
            ADD_JS_FUNCTION(slot_clear, ctx);
            ADD_JS_FUNCTION(slot_count, ctx);
            ADD_JS_FUNCTION(slot_set, ctx);
            ADD_JS_FUNCTION(slot_size, ctx);
            ADD_JS_FUNCTION(slot_subarray, ctx);
            ADD_JS_FUNCTION(slot_subfield, ctx);
            ADD_JS_FUNCTION(slot_type, ctx);
            ADD_JS_FUNCTION(slot_float, ctx);

            ADD_JS_FUNCTION(trace, ctx);
            /*
            ADD_JS_FUNCTION(trace_num, ctx);
            ADD_JS_FUNCTION(trace_float, ctx);

            */
            ADD_JS_FUNCTION(meta_slot, ctx);
            ADD_JS_FUNCTION(xpop_slot, ctx);
        }

        bool
        sane()
        {
            return ctx && rt;
        }

        ~QuickJSVM()
        {
            if (ctx)
                JS_FreeContext(ctx);
            if (rt)
                JS_FreeRuntime(rt);
            ctx = NULL;
            rt = NULL;
        }
    };

    /**
     * Helper function to handle a JS exception by extracting its message
     * and freeing any additional JSValues
     */
    template <typename... JSValues>
    static std::string
    handleException(
        JSContext* ctx,
        const char* defaultMsg,
        JSValues... valuesToFree)
    {
        // Get the actual exception object
        JSValue exception = JS_GetException(ctx);

        // Extract the error message property
        JSValue msgProp = JS_GetPropertyStr(ctx, exception, "message");
        const char* str = JS_ToCString(ctx, msgProp);

        std::string result = (str != nullptr) ? str : defaultMsg;

        if (str != nullptr)
            JS_FreeCString(ctx, str);

        JS_FreeValue(ctx, msgProp);
        JS_FreeValue(ctx, exception);

        // Free all additional values passed
        (JS_FreeValue(ctx, valuesToFree), ...);

        return result;
    }

    /**
     * Validate that a js blob can be loaded by quickjs
     */
    static std::optional<std::string>
    validate(const void* buf, size_t buf_len)
    {
        if (buf_len < 5)
            return "Could not create QUICKJS instance, bytecode too short.";

        std::optional<std::string> retval;

        // RHNOTE: hard instrction (internal program counter loop) limit of 1MM,
        // like wasm.
        QuickJSVM vm{NULL, 1000000};
        JSContext* ctx = vm.ctx;

        if (!vm.sane())
            return "Could not create QUICKJS instance";

        JSValue obj = JS_ReadObject(
            ctx, (uint8_t const*)buf, buf_len, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(obj) || JS_IsUndefined(obj))
        {
            return handleException(ctx, "invalid bytecode");
        }

        JSValue val = JS_EvalFunction(ctx, obj);
        if (JS_IsException(val))
        {
            return handleException(ctx, "bytecode eval failure", obj);
        }

        JS_FreeValue(ctx, val);

        const char* testCalls =
            "if (typeof(Hook) != \"function\" || "
            "(typeof(Callback) != \"function\" && typeof(Callback) != "
            "\"undefined\")) "
            "throw Error(\"Hook/Callback function required\")";

        val = JS_Eval(vm.ctx, testCalls, strlen(testCalls), "<qjsvm>", 0);

        if (JS_IsException(val))
        {
            std::string errMsg =
                handleException(ctx, "Hook/Callback validation failure", obj);
            return errMsg;
        }

        JS_FreeValue(ctx, val);
        // We don't manually free the bytecode object (obj) here because
        // JS_EvalFunction internally transforms it into a closure and takes
        // ownership of its internal structures.
        // JS_FreeValue(ctx, obj);

        return retval;
    }

    /**
     * Execute QuickJS bytecode against hook context.
     * Once execution has occured the exector is spent and cannot be used again
     * and should be destructed Information about the execution is populated
     * into hookCtx
     */
    void
    execute(
        const void* buf,
        size_t buf_len,
        bool callback,
        uint32_t hookArgument,
        uint32_t instructionLimit,
        beast::Journal const& j) override
    {
        // HookExecutorWasm can only execute once
        assert(!spent);
        spent = true;

        JLOG(j.trace()) << "HookInfo[" << HC_ACC()
                        << "]: creating quickjs instance";

        QuickJSVM vm{reinterpret_cast<void*>(&hookCtx), instructionLimit};
        JSContext* ctx = vm.ctx;

        if (!vm.sane())
        {
            JLOG(j.warn()) << "HookError[" << HC_ACC()
                           << "]: Could not create QUICKJS instance.";

            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        JSValue obj = JS_ReadObject(
            ctx, (uint8_t const*)buf, buf_len, JS_READ_OBJ_BYTECODE);

        if (JS_IsException(obj))
        {
            JS_FreeValue(ctx, obj);
            JLOG(j.warn())
                << "HookError[" << HC_ACC()
                << "]: Could not create QUICKJS instance (invalid bytecode).";
            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        JSValue val = JS_EvalFunction(ctx, obj);

        if (JS_IsException(val))
        {
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, obj);

            JLOG(j.warn()) << "HookError[" << HC_ACC()
                           << "]: Could not create QUICKJS instance (bytecode "
                              "eval failure).";
            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        JS_FreeValue(ctx, val);

        char expr[256];

        int expr_len = snprintf(
            expr, 256, "%s(%d)", callback ? "Callback" : "Hook", hookArgument);

        if (expr_len < 7 || expr_len == 256)
        {
            JLOG(j.warn())
                << "HookError[" << HC_ACC()
                << "]: Could not create QUICKJS instance (expr string).";

            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        val = JS_Eval(vm.ctx, expr, expr_len, "<qjsvm>", 0);

        int normal_exit = 0;

        JSValue exception_val = JS_GetException(ctx);
        if (!JS_IsUndefined(exception_val) && JS_IsError(ctx, exception_val))
        {
            int printed_something = 0;

            // Most exceptions should have a message field, try to fetch and
            // print that
            JSValue msg = JS_GetPropertyStr(ctx, exception_val, "message");
            if (!JS_IsUndefined(msg))
            {
                if (const char* str = JS_ToCString(ctx, msg); str)
                {
                    std::string m(str);
                    JS_FreeCString(ctx, str);

                    if ((m == "HookExit Accept" || m == "HookExit Rollback") &&
                        (hookCtx.result.exitType ==
                             hook_api::ExitType::ACCEPT ||
                         hookCtx.result.exitType ==
                             hook_api::ExitType::ROLLBACK))
                    {
                        normal_exit = 1;
                    }
                    else
                    {
                        JLOG(j.warn()) << "HookError[" << HC_ACC()
                                       << "]: JSException " << m;
                        printed_something++;
                    }
                }
            }
            JS_FreeValue(ctx, msg);

            // Accept/rollback are handled via an uncatchable exception
            // internally in quickjs so only print a backtrace if it isn't a
            // normal exit.
            if (!normal_exit)
            {
                if (!printed_something)
                {
                    JLOG(j.warn())
                        << "HookError[" << HC_ACC() << "]: [unknown exception]";
                }

                JSValue bt = JS_GetPropertyStr(ctx, exception_val, "stack");
                if (!normal_exit && !JS_IsUndefined(bt))
                {
                    if (const char* str = JS_ToCString(ctx, bt); str)
                    {
                        JLOG(j.warn())
                            << "HookError[" << HC_ACC() << "]: " << str;
                        JS_FreeCString(ctx, str);
                    }
                }

                JS_FreeValue(ctx, bt);
            }
        }
        JS_FreeValue(ctx, exception_val);

        if (normal_exit)
        {
            if (hookCtx.result.exitType == hook_api::ExitType::ACCEPT)
            {
                JLOG(j.warn())
                    << "HookInfo[" << HC_ACC() << "]: JSVM Exited with ACCEPT";
            }
            else
            {
                JLOG(j.warn()) << "HookInfo[" << HC_ACC()
                               << "]: JSVM Exited with ROLLBACK";
            }
            JLOG(j.warn()) << "HookInfo[" << HC_ACC()
                           << "]: Instruction Count: "
                           << JS_GetInstructionCount(ctx);
            hookCtx.result.instructionCount = JS_GetInstructionCount(ctx);
        }
        /*
            // RHTODO: place jsvm_error exit type logic appropriately
            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            hookCtx.result.instructionCount = 0; //?
        */
        JS_FreeValue(ctx, val);
    }

    HookExecutorJS(HookContext& ctx) : HookExecutorBase(ctx)
    {
        ctx.module = this;
    }

    virtual ~HookExecutorJS(){};
};

}  // namespace hook

#endif
