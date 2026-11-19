//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <test/app/SetHook_wasm.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

// Like BEAST_EXPECT, but returns from the function if the condition fails
#define EB_REQUIRE(x)        \
    do                       \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    } while (0)

// A node that acquires a ledger from the network never ran the emitting hook
// nor TxQ::accept for it, so its HashRouter lacks SF_EMITTED for the emitted
// txns in that ledger's emitted directory. buildLedger on such a parent must
// still apply them.
class EmittedBuild_test : public beast::unit_test::suite
{
    // Emits a Payment from alice's emit_tx hook and returns the closed ledger
    // whose emitted directory holds it.
    std::shared_ptr<Ledger const>
    emitPayment(
        jtx::Env& env,
        jtx::Account const& alice,
        jtx::Account const& bob)
    {
        using namespace jtx;
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(hook(alice, {{hso(emit_tx_wasm)}}, 0),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        auto jv = pay(alice, bob, XRP(1));
        jv.removeMember(jss::Account);
        jv.removeMember(jss::Fee);
        jv[jss::Sequence] = 0;
        jv[jss::SigningPubKey] = "";
        STParsedJSONObject parsed("tx", jv);
        if (!BEAST_EXPECT(parsed.object))
            return nullptr;
        Serializer s;
        parsed.object->add(s);

        env(invoke::invoke(alice),
            invoke::blob(strHex(s.slice())),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();
        return env.app().getLedgerMaster().getClosedLedger();
    }

    static std::shared_ptr<SLE const>
    emittedEntry(Ledger const& ledger)
    {
        for (auto const& sle : ledger.sles)
            if (sle->getType() == ltEMITTED_TXN)
                return sle;
        return nullptr;
    }

    static std::shared_ptr<STTx const>
    emittedTxn(SLE const& sle)
    {
        auto const& et =
            const_cast<SLE&>(sle).getField(sfEmittedTxn).downcast<STObject>();
        Serializer s;
        et.add(s);
        SerialIter sit(s.slice());
        return std::make_shared<STTx const>(sit);
    }

    // The ttEMIT_FAILURE TxQ::accept injects for an expired emitted txn
    static std::shared_ptr<STTx const>
    emitFailure(STTx const& emitted, std::uint32_t seq)
    {
        auto const txnHash = emitted.getTransactionID();
        auto const emitDetails = const_cast<STTx&>(emitted)
                                     .getField(sfEmitDetails)
                                     .downcast<STObject>();
        return std::make_shared<STTx const>(
            ttEMIT_FAILURE, [seq, txnHash, emitDetails](auto& obj) {
                obj[sfLedgerSequence] = seq;
                obj[sfTransactionHash] = txnHash;
                obj.set(STObject(emitDetails));
            });
    }

    // metadata result of `id` in `ledger`, or nullopt if absent
    static std::optional<TER>
    txResult(Ledger const& ledger, uint256 const& id)
    {
        auto const [txn, meta] = ledger.txRead(id);
        if (!txn || !meta)
            return std::nullopt;
        return TER::fromInt(meta->getFieldU8(sfTransactionResult));
    }

    static XRPAmount
    balance(Ledger const& ledger, jtx::Account const& acc)
    {
        return ledger.read(keylet::account(acc.id()))
            ->getFieldAmount(sfBalance)
            .xrp();
    }

    // buildLedger on `parent` with the app of `env` and the txns `txs`
    std::shared_ptr<Ledger>
    build(
        jtx::Env& env,
        std::shared_ptr<Ledger const> const& parent,
        std::vector<std::shared_ptr<STTx const>> const& txs,
        std::set<TxID>& failed)
    {
        CanonicalTXSet set(parent->info().hash);
        for (auto const& tx : txs)
            set.insert(tx);
        // The built ledgers share the parent's node store, so the Env that
        // made the parent (A) must outlive them.
        return buildLedger(
            parent,
            parent->info().closeTime + parent->info().closeTimeResolution,
            true,
            parent->info().closeTimeResolution,
            env.app(),
            set,
            failed,
            env.journal);
    }

    void
    testDue(FeatureBitset features)
    {
        testcase("due emitted txn applies on an acquired parent");
        using namespace jtx;
        Env envA{*this, features};
        Env envB{*this, features};
        Account const alice{"alice"}, bob{"bob"};

        auto const parent = emitPayment(envA, alice, bob);
        EB_REQUIRE(parent);
        auto const entry = emittedEntry(*parent);
        EB_REQUIRE(entry);
        auto const tx = emittedTxn(*entry);
        auto const id = tx->getTransactionID();

        // B never saw it
        EB_REQUIRE(!(envB.app().getHashRouter().getFlags(id) & SF_EMITTED));

        std::set<TxID> failed;
        auto const built = build(envB, parent, {tx}, failed);
        EB_REQUIRE(built);
        BEAST_EXPECT(failed.empty());
        BEAST_EXPECT(built->txExists(id));
        BEAST_EXPECT(txResult(*built, id) == TER{tesSUCCESS});
        BEAST_EXPECT(!built->exists(keylet::emittedTxn(id)));
        BEAST_EXPECT(balance(*built, bob) == balance(*parent, bob) + XRP(1));

        // B now holds SF_EMITTED for it and the directory entry is gone:
        // building on top with the same txn must not apply it again
        BEAST_EXPECT(envB.app().getHashRouter().getFlags(id) & SF_EMITTED);
        std::set<TxID> failed2;
        auto const next = build(envB, built, {tx}, failed2);
        EB_REQUIRE(next);
        BEAST_EXPECT(failed2.count(id) == 1);
        BEAST_EXPECT(!next->txExists(id));
    }

    void
    testExpired(FeatureBitset features)
    {
        testcase("expired emitted txn: emit failure applies");
        using namespace jtx;
        Env envA{*this, features};
        Env envB{*this, features};
        Account const alice{"alice"}, bob{"bob"};

        auto parent = emitPayment(envA, alice, bob);
        EB_REQUIRE(parent);
        auto const entry = emittedEntry(*parent);
        EB_REQUIRE(entry);
        auto const tx = emittedTxn(*entry);
        auto const id = tx->getTransactionID();
        auto const lls = tx->getFieldU32(sfLastLedgerSequence);

        // let it expire by building empty ledgers on B until the parent is
        // at LastLedgerSequence; the entry is still there
        std::set<TxID> failed;
        while (parent->seq() < lls)
        {
            parent = build(envB, parent, {}, failed);
            EB_REQUIRE(parent);
        }
        EB_REQUIRE(parent->exists(keylet::emittedTxn(id)));

        auto const ef = emitFailure(*tx, parent->seq() + 1);
        auto const built = build(envB, parent, {ef}, failed);
        EB_REQUIRE(built);
        BEAST_EXPECT(failed.empty());
        BEAST_EXPECT(built->txExists(ef->getTransactionID()));
        BEAST_EXPECT(
            txResult(*built, ef->getTransactionID()) == TER{tesSUCCESS});
        BEAST_EXPECT(!built->txExists(id));
        BEAST_EXPECT(!built->exists(keylet::emittedTxn(id)));
    }

    void
    testUnflagged(FeatureBitset features)
    {
        testcase(
            "unflagged emitted-looking txn is rejected before the directory "
            "check");
        using namespace jtx;
        Env envA{*this, features};
        Env envB{*this, features};
        Account const alice{"alice"}, bob{"bob"};

        auto const parent = emitPayment(envA, alice, bob);
        EB_REQUIRE(parent);
        auto const entry = emittedEntry(*parent);
        EB_REQUIRE(entry);

        // same emitted txn, different amount: carries sfEmitDetails but its
        // id is not in the emitted directory, so no SF_EMITTED flag is ever
        // set for it and it is rejected before the directory check
        auto const orig = emittedTxn(*entry);
        auto const forged =
            std::make_shared<STTx const>(ttPAYMENT, [&](STObject& obj) {
                obj = *orig;
                obj.setFieldAmount(sfAmount, XRP(2));
            });
        auto const forgedID = forged->getTransactionID();
        EB_REQUIRE(forgedID != orig->getTransactionID());
        EB_REQUIRE(forged->isFieldPresent(sfEmitDetails));

        std::set<TxID> failed;
        auto const built = build(envB, parent, {forged}, failed);
        EB_REQUIRE(built);
        BEAST_EXPECT(!built->txExists(forgedID));
        // the real entry is untouched
        BEAST_EXPECT(
            built->exists(keylet::emittedTxn(orig->getTransactionID())));
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

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const features = supported_amendments();
        EB_REQUIRE(!emit_tx_wasm.empty());
        testDue(features);
        testExpired(features);
        testUnflagged(features);
    }
};

BEAST_DEFINE_TESTSUITE(EmittedBuild, app, ripple);
}  // namespace test
}  // namespace ripple
