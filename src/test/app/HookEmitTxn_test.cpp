
#include <test/app/SetHook_wasm.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/tx/detail/SetHook.h>
#include <xrpl/protocol/jss.h>
#include "test/jtx/invoke.h"
#include "xrpl/protocol/XRPAmount.h"

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

#define HASH_WASM(x)                                                           \
    [[maybe_unused]] uint256 const x##_hash =                                  \
        ripple::sha512Half_s(ripple::Slice(x##_wasm.data(), x##_wasm.size())); \
    [[maybe_unused]] std::string const x##_hash_str = to_string(x##_hash);     \
    [[maybe_unused]] Keylet const x##_keylet = keylet::hookDefinition(x##_hash);

class HookEmitTxn_test : public beast::unit_test::suite
{
private:
    bool
    foundCreatedLedgerEntry(STArray const& affectedNodes, LedgerEntryType stype)
    {
        bool found = false;
        for (auto const& node : affectedNodes)
        {
            SField const& metaType = node.getFName();
            uint16_t nodeType = node.getFieldU16(sfLedgerEntryType);
            if (metaType == sfCreatedNode && nodeType == stype)
            {
                found = true;
                break;
            }
        }
        return found;
    }

    void
    testNFTokenOffer(FeatureBitset const& features)
    {
    }

    void
    testCron(FeatureBitset const& features)
    {
    }

    void
    testCheck(FeatureBitset const& features)
    {
    }

    void
    testHookDefinition(FeatureBitset const& features)
    {
    }

    void
    testEmittedTxn(FeatureBitset const& features)
    {
    }

    void
    testHook(FeatureBitset const& features)
    {
    }

    void
    testImportVLSequence(FeatureBitset const& features)
    {
    }

    void
    testNegativeUNL(FeatureBitset const& features)
    {
    }

    void
    testNFTokenPage(FeatureBitset const& features)
    {
    }

    void
    testUNLReport(FeatureBitset const& features)
    {
    }

    void
    testSignerList(FeatureBitset const& features)
    {
    }

    void
    testTicket(FeatureBitset const& features)
    {
    }

    void
    testURIToken(FeatureBitset const& features)
    {
    }

    void
    testAccountRoot(FeatureBitset const& features)
    {
        testcase("testAccountRoot");
        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, features};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.close();

        env(hook(alice, {{hso(emit_tx_wasm)}}, 0),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        STTx blobTx(ttPAYMENT, [&env, &bob](STObject& obj) {
            obj[sfAmount] = XRPAmount(env.current()->fees().accountReserve(10));
            obj[sfDestination] = bob.id();
            // common fields
            obj[sfSequence] = 0;
            obj[sfSigningPubKey] = Slice{};
            obj.makeFieldAbsent(sfFee);      // use etxn_fee_base to set fee
            obj.makeFieldAbsent(sfAccount);  // use hook_account to set account
        });
        Serializer rawTxn;
        blobTx.add(rawTxn);
        SerialIter sit(rawTxn.slice());

        env(invoke::invoke(alice),
            invoke::blob(strHex(rawTxn.slice())),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        auto const emissions = env.meta()->getFieldArray(sfHookEmissions)[0];
        auto const emittedTxnID = emissions.getFieldH256(sfEmittedTxnID);

        env.close();

        auto const [txn, meta] = env.closed()->txRead(emittedTxnID);
        BEAST_EXPECT(txn->getTxnType() == ttPAYMENT);
        BEAST_EXPECT(meta->getFieldU8(sfTransactionResult) == tesSUCCESS);
        auto const nodes = meta->getFieldArray(sfAffectedNodes);
        BEAST_EXPECT(foundCreatedLedgerEntry(nodes, ltACCOUNT_ROOT));
    }

    void
    testDirectoryNode(FeatureBitset const& features)
    {
    }

    void
    testAmendments(FeatureBitset const& features)
    {
    }

    void
    testLedgerHashes(FeatureBitset const& features)
    {
    }

    void
    testBridge(FeatureBitset const& features)
    {
    }

    void
    testOffer(FeatureBitset const& features)
    {
    }

    void
    testDepositPreauth(FeatureBitset const& features)
    {
    }

    void
    testXChainOwnedClaimID(FeatureBitset const& features)
    {
    }

    void
    testRippleState(FeatureBitset const& features)
    {
    }

    void
    testFeeSettings(FeatureBitset const& features)
    {
    }

    void
    testXChainOwnedCreateAccountClaimID(FeatureBitset const& features)
    {
    }

    void
    testEscrow(FeatureBitset const& features)
    {
    }

    void
    testHookState(FeatureBitset const& features)
    {
    }

    void
    testPayChannel(FeatureBitset const& features)
    {
    }

    void
    testAMM(FeatureBitset const& features)
    {
    }

    void
    testOracle(FeatureBitset const& features)
    {
    }

    void
    testMPTokenIssuance(FeatureBitset const& features)
    {
    }

    void
    testMPToken(FeatureBitset const& features)
    {
    }

    void
    testCredential(FeatureBitset const& features)
    {
    }

    void
    testPermissionedDomain(FeatureBitset const& features)
    {
    }

    void
    testDID(FeatureBitset const& features)
    {
    }

    void
    testCreateLedgerEntry(FeatureBitset const& features)
    {
#pragma push_macro("LEDGER_ENTRY")
#undef LEDGER_ENTRY

#define LEDGER_ENTRY(tag, value, name, ...) test##name(features);

#include <xrpl/protocol/detail/ledger_entries.macro>

#undef LEDGER_ENTRY
#pragma pop_macro("LEDGER_ENTRY")
    }

private:
    // This hook is used in HookEmitTxn_test.cpp
    TestHook emit_tx_wasm = wasm[
        R"[test.hook](
          #include <stdint.h>
          extern int32_t _g(uint32_t id, uint32_t maxiter);
          extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
          extern int64_t rollback(uint32_t read_ptr, uint32_t read_len,
                                  int64_t error_code);
          extern int64_t emit(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr,
                              uint32_t read_len);
          extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
          extern int64_t etxn_reserve(uint32_t);
          extern int64_t etxn_fee_base(uint32_t read_ptr, uint32_t read_len);
          extern int64_t etxn_details(uint32_t write_ptr, uint32_t write_len);
          extern int64_t ledger_seq(void);
          extern int64_t otxn_field(uint32_t write_ptr, uint32_t write_len,
                                    uint32_t field_id);
          extern int64_t trace(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr,
                               uint32_t read_len, int32_t as_hex);
          #define sfBlob ((7U << 16U) + 26U)
          
          #define SBUF(x) (uint32_t) x, sizeof(x)
          
          uint8_t blob[1000];
          
          #define FLIP_ENDIAN_32(value)                                                  \
            (uint32_t)(((value & 0xFFU) << 24) | ((value & 0xFF00U) << 8) |              \
                       ((value & 0xFF0000U) >> 8) | ((value & 0xFF000000U) >> 24))
          
          #define SET_UINT32(ptr, value) *((uint32_t *)(ptr)) = FLIP_ENDIAN_32(value);
          
          #define SET_NATIVE_AMOUNT(ptr, amount)                                         \
            do {                                                                         \
              uint8_t *b = (ptr);                                                        \
              *b++ = 0b01000000 + ((amount >> 56) & 0b00111111);                         \
              *b++ = (amount >> 48) & 0xFFU;                                             \
              *b++ = (amount >> 40) & 0xFFU;                                             \
              *b++ = (amount >> 32) & 0xFFU;                                             \
              *b++ = (amount >> 24) & 0xFFU;                                             \
              *b++ = (amount >> 16) & 0xFFU;                                             \
              *b++ = (amount >> 8) & 0xFFU;                                              \
              *b++ = (amount >> 0) & 0xFFU;                                              \
            } while (0)
          
          #define PREPARE_TXN(ptr, len)                                                  \
            do {                                                                         \
              uint8_t *nextPtr = (uint8_t *)(ptr);                                       \
              nextPtr += len;                                                            \
              etxn_reserve(1);                                                           \
              uint32_t fls = (uint32_t)ledger_seq() + 1;                                 \
              /** firstledgersequence = ledger_seq + 1 */                                \
              *nextPtr++ = 0x20U;                                                        \
              *nextPtr++ = 0x1AU;                                                        \
              txn_len += 2;                                                              \
              SET_UINT32((uint32_t)nextPtr, fls);                                        \
              nextPtr += 4;                                                              \
              txn_len += 4;                                                              \
              /** lastledgersequence = ledger_seq + 5 */                                 \
              *nextPtr++ = 0x20U;                                                        \
              *nextPtr++ = 0x1BU;                                                        \
              txn_len += 2;                                                              \
              SET_UINT32((uint32_t)nextPtr, fls + 4);                                    \
              nextPtr += 4;                                                              \
              txn_len += 4;                                                              \
              /** account = hook_account */                                              \
              *nextPtr++ = 0x81U;                                                        \
              *nextPtr++ = 0x14U;                                                        \
              txn_len += 2;                                                              \
              hook_account((uint32_t)nextPtr, 20);                                       \
              nextPtr += 20;                                                             \
              txn_len += 20;                                                             \
              /** emit details */                                                        \
              etxn_details((uint32_t)nextPtr, 116U);                                     \
              nextPtr += 116;                                                            \
              txn_len += 116;                                                            \
              /** fee */                                                                 \
              *nextPtr++ = 0x68U;                                                        \
              *nextPtr++ = 0x40U;                                                        \
              txn_len += 1;                                                              \
              SET_NATIVE_AMOUNT((uint32_t)--nextPtr, 0);                                 \
              txn_len += 8;                                                              \
              int64_t fee = etxn_fee_base((uint32_t)ptr, txn_len);                       \
              SET_NATIVE_AMOUNT((uint32_t)nextPtr, fee);                                 \
            } while (0)
          
          int64_t hook(uint32_t reserved) {
            _g(1, 1);
          
            int64_t blob_len = otxn_field(SBUF(blob), sfBlob);
            uint64_t prefix_len = (blob[0] < 193) ? 1 : (blob[0] < 241) ? 2 : 3;
          
          #define txn_ptr (blob + prefix_len)
            uint32_t txn_len = blob_len - prefix_len;
          
            PREPARE_TXN(txn_ptr, txn_len);
            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), txn_ptr, txn_len);
            if (emit_result < 0)
              return rollback(SBUF("Emit failed."), __LINE__);
            return accept(SBUF("Emit succeeded."), __LINE__);
          }
        )[test.hook]"];
    HASH_WASM(emit_tx);

public:
    void
    run() override
    {
        using namespace test::jtx;
        testCreateLedgerEntry(supported_amendments());
    }
};  // namespace ripple

BEAST_DEFINE_TESTSUITE_PRIO(HookEmitTxn, app, ripple, 2);
}  // namespace test
}  // namespace ripple
